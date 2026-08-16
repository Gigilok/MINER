#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <WiFiClient.h>

// --- PINOS ---
#define OLED_SDA 21
#define OLED_SCL 22
#define BTN_UP   5
#define BTN_DOWN 27
#define BTN_SEL  32
#define BTN_BACK 33
#define BUZZER   4
#define BAT_ADC  34

Adafruit_SSD1306 display(128, 64, &Wire, -1);
Preferences preferences;

// --- POOL ---
// public-pool.io IGNORA mining.suggest_difficulty e impõe diff 1.0
// Impossível para ESP32 (~8KH/s) encontrar share diff 1.0 em tempo razoável
// pool.nerdminers.org é o pool oficial NerdMiner - suporta diff baixa para ESP32
// Outras opções compatíveis (descomente para trocar):
//   pool.nerdminer.io:3333  (CHMEX)
//   pool.pyblock.xyz:3333   (curly60e)
//   pool.sethforprivacy.com:3333
//   pool.stompi.de:3333
//   pool.solomining.de:3333
//   public-pool.io:3333     (só funciona se aceitar diff baixa)
const char* STRATUM_HOST = "pool.nerdminers.org";
const int STRATUM_PORT = 3333;
// Apenas o endereço BTC - igual ao NerdMiner_v2
// O sufixo .esp32 impedia o painel de reconhecer o worker
const char* WORKER_ID = "1FRpCfmiwAGVkCLt2FjVuuoAjhSaE2j4QN";
const char* WORKER_PASS = "x";
const double DEFAULT_DIFFICULTY = 0.00015;

// Tamanho do JSON - CRÍTICO: mining.notify pode ter >2KB
// NerdMiner_v2 usa 4096, usamos 6144 para margem de segurança
#define JSON_DOC_SIZE 6144

enum UI_State { STATE_SCANNING, STATE_SELECT_SSID, STATE_INPUT_PASSWORD, STATE_CONNECTING, STATE_MINING };
UI_State currentState = STATE_SCANNING;

String ssid = "";
String password = "";
String poolStatus = "Desconectado";

WiFiClient client;
unsigned long hashesTotal = 0;
int acceptedShares = 0;
int rejectedShares = 0;
double hashrate = 0.0;
double bestDiff = 0.0;
unsigned long lastHashTime = 0;
unsigned long hashesInWindow = 0;

// Contador local de shares (diff >= 0.001)
unsigned long localShares = 0;

char chars[] = " abcdefghijklmnopqrstuvwxyz0123456789@!#$%&*";
int charIndex = 0;
int scanNetworksCount = 0;
int selectedNetworkIndex = 0;

// --- VARIÁVEIS STRATUM ---
double currentPoolDifficulty = DEFAULT_DIFFICULTY;
String extranonce1 = "";
int extranonce2_size = 4;
String current_job_id = "";
String current_prevhash = "";
String current_coinb1 = "";
String current_coinb2 = "";
String merkle_branches[16];
int num_branches = 0;
String current_version_hex = "";
String current_nbits_hex = "";
String current_ntime_hex = "";
bool isSubscribed = false;
bool isAuthorized = false;
bool isConnected = false;

unsigned long extranonce2_val = 1;
int stratumMsgId = 4;  // Próximo ID disponível (1=subscribe, 2=auth, 3=suggest)
bool firstMiningLog = true;

// Controle de IDs de submit para não confundir com keepalive
#define MAX_SUBMIT_IDS 16
unsigned long submitIdList[MAX_SUBMIT_IDS];
int submitIdCount = 0;
unsigned long sharesSubmitted = 0;  // Total de shares enviados (não confundir com Ok/Rj)

// Diagnóstico: rastreia melhor share para submit forçado
uint32_t diagBestNonce = 0;
String diagBestEn2 = "";
double diagBestDiff = 0.0;
unsigned long lastDiagTime = 0;
int diagSubmitCount = 0;

void trackSubmitId(unsigned long id) {
    submitIdList[submitIdCount % MAX_SUBMIT_IDS] = id;
    submitIdCount++;
}

bool isRealSubmitId(unsigned long id) {
    int check = submitIdCount < MAX_SUBMIT_IDS ? submitIdCount : MAX_SUBMIT_IDS;
    for (int i = 0; i < check; i++) {
        if (submitIdList[i] == id) return true;
    }
    return false;
}

// JSON doc GLOBAL (não na pilha!) - mining.notify pode ter >2KB
// NerdMiner_v2 também usa doc global por este mesmo motivo
StaticJsonDocument<JSON_DOC_SIZE> g_doc;

// Timers
unsigned long lastSuggestTime = 0;
unsigned long lastJobTime = 0;
unsigned long lastPoolDataTime = 0;
#define KEEPALIVE_MS 30000
#define POOL_INACTIVITY_MS 120000

// --- BUZINA ---
void buzzerInit() { pinMode(BUZZER, INPUT); }
void beep(int durationMs = 50) {
    pinMode(BUZZER, OUTPUT);
    int cycles = durationMs * 2;
    for (int i = 0; i < cycles; i++) {
        digitalWrite(BUZZER, HIGH); delayMicroseconds(250);
        digitalWrite(BUZZER, LOW); delayMicroseconds(250);
    }
    pinMode(BUZZER, INPUT);
}
void buzzerAlertSuccess() { beep(300); delay(150); beep(300); delay(150); beep(300); }

// ============================================================
// HEX / BYTE UTILITIES
// ============================================================

uint8_t hexCharToVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

void hexToBytes(const String& hex, uint8_t* bytes) {
    int len = hex.length() / 2;
    for (int i = 0; i < len; i++) {
        uint8_t high = hexCharToVal(hex.charAt(i * 2));
        uint8_t low  = hexCharToVal(hex.charAt(i * 2 + 1));
        bytes[i] = (high << 4) | low;
    }
}

void reverseBytes(uint8_t* data, int len) {
    for (int i = 0; i < len / 2; i++) {
        uint8_t tmp = data[i];
        data[i] = data[len - 1 - i];
        data[len - 1 - i] = tmp;
    }
}

String reverseHex(const String& hex) {
    String reversed = "";
    for (int i = hex.length() - 2; i >= 0; i -= 2) {
        reversed += hex.substring(i, i + 2);
    }
    return reversed;
}

// ============================================================
// SHA256
// ============================================================

void doubleSHA256(const uint8_t* data, size_t len, uint8_t* out) {
    uint8_t hash1[32];
    mbedtls_sha256(data, len, hash1, 0);
    mbedtls_sha256(hash1, 32, out, 0);
}

// ============================================================
// DIFFICULTY / TARGET
// ============================================================

static const double truediffone = 26959535291011309493156476344723991336010898738574164086137773096960.0;

double le256todouble(const uint8_t* target) {
    double dcut64 = 0.0;
    const uint64_t* d64;
    d64 = (const uint64_t*)(target + 24);
    dcut64  = (double)(*d64) * 6277101735386680763835789423207666416102355444464034512896.0;
    d64 = (const uint64_t*)(target + 16);
    dcut64 += (double)(*d64) * 340282366920938463463374607431768211456.0;
    d64 = (const uint64_t*)(target + 8);
    dcut64 += (double)(*d64) * 18446744073709551616.0;
    d64 = (const uint64_t*)(target);
    dcut64 += (double)(*d64);
    return dcut64;
}

double diffFromTarget(const uint8_t* hashLe) {
    double d = le256todouble(hashLe);
    if (d == 0.0) d = 1.0;
    return truediffone / d;
}

String formatExtranonce2(unsigned long val, int size) {
    String hex = String(val, HEX);
    int neededChars = size * 2;
    while (hex.length() < neededChars) hex = "0" + hex;
    if (hex.length() > neededChars) hex = hex.substring(hex.length() - neededChars);
    return hex;
}

// ============================================================
// CONSTRUIR BLOCK HEADER (80 bytes)
// ============================================================
// Layout: version(4) + prevhash(32) + merkle_root(32) + ntime(4) + nbits(4) + nonce(4)

bool buildBlockHeader(const String& extranonce2, uint8_t* headerOut) {
    // Monta coinbase: coinb1 + extranonce1 + extranonce2 + coinb2
    String coinbaseHex = current_coinb1 + extranonce1 + extranonce2 + current_coinb2;
    int cbLen = coinbaseHex.length() / 2;
    if (cbLen <= 0 || cbLen > 256) {
        Serial.printf("[ERROR] Coinbase invalido: len=%d\n", cbLen);
        return false;
    }

    uint8_t coinbaseBytes[256];
    hexToBytes(coinbaseHex, coinbaseBytes);

    // Calcula merkle root (igual NerdMiner_v2: double SHA256)
    uint8_t merkleRoot[32];
    doubleSHA256(coinbaseBytes, cbLen, merkleRoot);

    uint8_t buf[64];
    for (int i = 0; i < num_branches; i++) {
        if (merkle_branches[i].length() < 64) continue;
        uint8_t branchBytes[32];
        hexToBytes(merkle_branches[i], branchBytes);
        memcpy(buf, merkleRoot, 32);
        memcpy(buf + 32, branchBytes, 32);
        doubleSHA256(buf, 64, merkleRoot);
    }

    // === CONSTRUIR HEADER IDÊNTICO AO NerdMiner_v2 ===
    // Passo 1: Monta string hex do header (tudo em ordem BE/original)
    // merkle_root fica em BE (ordem natural do SHA256) - NÃO inverte!
    String merkleHex = "";
    for (int i = 0; i < 32; i++) {
        if (merkleRoot[i] < 0x10) merkleHex += "0";
        merkleHex += String(merkleRoot[i], HEX);
    }
    // Ordem: version + prevhash + merkle + ntime + nbits + nonce(00000000)
    String headerHex = current_version_hex + current_prevhash + merkleHex +
                       current_ntime_hex + current_nbits_hex + "00000000";

    // Passo 2: Converte hex para bytes
    int headerLen = headerHex.length() / 2;
    if (headerLen != 80) {
        Serial.printf("[ERROR] Header len=%d (deveria ser 80)\n", headerLen);
        return false;
    }
    hexToBytes(headerHex, headerOut);

    // Passo 3: Word-swap endian (IDÊNTICO ao NerdMiner_v2 utils.cpp)
    // Helper: inverte bytes dentro de um bloco de 4 bytes
    #define WORD_SWAP_4(off) do { \
        uint8_t t0 = headerOut[off], t1 = headerOut[off+1]; \
        headerOut[off] = headerOut[off+3]; \
        headerOut[off+1] = headerOut[off+2]; \
        headerOut[off+2] = t1; \
        headerOut[off+3] = t0; \
    } while(0)

    // reverse version (offset 0, 4 bytes)
    WORD_SWAP_4(0);

    // reverse prev hash (offset 4, 32 bytes = 8 palavras de 4 bytes)
    for (int w = 0; w < 8; w++) {
        WORD_SWAP_4(4 + w * 4);
    }

    // merkle root: NÃO INVERTE! (comentado no NerdMiner_v2)

    // reverse ntime (offset 68, 4 bytes)
    WORD_SWAP_4(68);

    // reverse nbits (offset 72, 4 bytes)
    WORD_SWAP_4(72);

    // nonce (offset 76): fica como "00000000", será preenchido no mining loop

    #undef WORD_SWAP_4
    return true;
}

// ============================================================
// TELA OLED
// ============================================================

void drawSplashScreen() {
    display.clearDisplay();
    display.setTextSize(3); display.setTextColor(WHITE);
    display.setCursor(15, 15); display.print("MINER");
    display.display(); delay(2000);
}

void drawUI() {
    display.clearDisplay();
    int batRaw = analogRead(BAT_ADC);
    int batPercent = map(batRaw, 0, 4095, 0, 100);
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(85, 0); display.print("B:"); display.print(batPercent); display.print("%");

    if (currentState == STATE_SCANNING) {
        display.setCursor(0, 0); display.print("Procurando Wi-Fi...");
        display.setCursor(0, 20); display.print("Aguarde...");
    }
    else if (currentState == STATE_SELECT_SSID) {
        display.setCursor(0, 0); display.print("Redes Wi-Fi:");
        display.drawLine(0, 10, 128, 10, WHITE);
        for (int i = 0; i < 5; i++) {
            if (selectedNetworkIndex + i < scanNetworksCount) {
                display.setCursor(0, 15 + (i * 10));
                if (i == 0) display.print("> "); else display.print("  ");
                display.print(WiFi.SSID(selectedNetworkIndex + i).substring(0, 16));
            }
        }
    }
    else if (currentState == STATE_INPUT_PASSWORD) {
        display.setCursor(0, 0); display.print("Rede: "); display.print(ssid.substring(0, 14));
        display.drawLine(0, 10, 128, 10, WHITE);
        display.setCursor(0, 15); display.print("Senha: ");
        for (int i = 0; i < password.length(); i++) display.print("*");
        display.setCursor(0, 30); display.print("> Char: "); display.print(chars[charIndex]);
        display.setCursor(0, 45); display.print("SEL:Add BCK:Del");
        display.setCursor(0, 55); display.print("UP+DOWN: Conectar");
    }
    else if (currentState == STATE_CONNECTING) {
        display.setCursor(0, 20); display.print("Conectando...");
    }
    else if (currentState == STATE_MINING) {
        display.setCursor(0, 0); display.print("BTC MINER v1.8");
        display.drawLine(0, 10, 128, 10, WHITE);
        String statusShort = poolStatus.substring(0, 21);
        display.setCursor(0, 15); display.print(statusShort);
        // Hashrate
        display.setCursor(0, 25); display.print("H/s:");
        if (hashrate >= 1000.0) {
            display.print(hashrate / 1000.0, 1); display.print("k");
        } else {
            display.print((unsigned long)hashrate);
        }
        display.setCursor(64, 25); display.print("Loc:"); display.print(localShares);
        // Hashes
        display.setCursor(0, 35); display.print("Hashes:"); display.print(hashesTotal / 1000); display.print("k");
        // Shares aceitos/rejeitados
        display.setCursor(0, 45);
        display.print("Ok:"); display.print(acceptedShares);
        display.print(" Rj:"); display.print(rejectedShares);
        display.print(" S:"); display.print(sharesSubmitted);
        // Dificuldade e best diff - mais casas decimais
        display.setCursor(0, 55);
        display.print("pD:");
        if (currentPoolDifficulty < 0.01) {
            display.print(currentPoolDifficulty, 4);
        } else {
            display.print(currentPoolDifficulty, 1);
        }
        display.print(" b:");
        if (bestDiff < 0.01) {
            display.print(bestDiff, 4);
        } else {
            display.print(bestDiff, 1);
        }
    }
    display.display();
}

// ============================================================
// STRATUM PROTOCOL
// ============================================================

void submitShare(uint32_t nonce, const String& extranonce2, bool isDiag = false) {
    // Formata nonce como hex (igual ao NerdMiner_v2: SEM padding)
    String nonceHex = String(nonce, HEX);
    unsigned long thisId = stratumMsgId++;
    trackSubmitId(thisId);  // Registra como submit REAL
    sharesSubmitted++;
    if (isDiag) diagSubmitCount++;

    String payload = "{\"id\":" + String(thisId) +
        ",\"method\":\"mining.submit\",\"params\":[\"" +
        String(WORKER_ID) + "\",\"" +
        current_job_id + "\",\"" +
        extranonce2 + "\",\"" +
        current_ntime_hex + "\",\"" +
        nonceHex + "\"]}\n";
    client.print(payload);
    lastPoolDataTime = millis();
    if (isDiag) {
        Serial.printf("[DIAG] #%d id=%lu diff=%.6f nonce=%08x job=%s en2=%s\n",
            diagSubmitCount, thisId, diagBestDiff, nonce,
            current_job_id.c_str(), extranonce2.c_str());
        Serial.printf("[DIAG] JSON enviado: %s", payload.c_str());
    } else {
        Serial.printf("[SHARE] Submitted id=%lu nonce=%08x job=%s en2=%s\n",
            thisId, nonce, current_job_id.c_str(), extranonce2.c_str());
    }
}

void suggestDifficulty(double diff) {
    char buf[128];
    // NÃO usa stratumMsgId para não conflitar com IDs de submit
    // Usa sempre id=3 como NerdMiner_v2 (resposta é ignorada)
    snprintf(buf, sizeof(buf), "{\"id\":3,\"method\":\"mining.suggest_difficulty\",\"params\":[%.10g]}\n", diff);
    client.print(buf);
    lastPoolDataTime = millis();
}

void processStratum(String line) {
    if (line.length() < 2) return;

    // Usa doc global para evitar stack overflow
    g_doc.clear();
    DeserializationError error = deserializeJson(g_doc, line);
    if (error) {
        Serial.printf("[JSON ERRO] len=%d err=%s\n", line.length(), error.c_str());
        return;
    }

    lastPoolDataTime = millis();

    // --- Resposta ao mining.subscribe (id=1) ---
    if (g_doc.containsKey("id")) {
        int id = g_doc["id"].as<int>();

        if (id == 1 && !isSubscribed) {
            if (g_doc["result"].is<JsonArray>()) {
                JsonArray arr = g_doc["result"].as<JsonArray>();
                extranonce1 = arr[1].as<String>();
                extranonce2_size = arr[2].as<int>();
                if (extranonce2_size <= 0) extranonce2_size = 4;
                extranonce2_val = 1;
                isSubscribed = true;
                Serial.printf("[STRATUM] Subscribed OK! en1=%s size=%d\n", extranonce1.c_str(), extranonce2_size);
            }
        }
        // Resposta ao mining.authorize (id=2)
        else if (id == 2 && !isAuthorized) {
            if (g_doc["result"] == true) {
                isAuthorized = true;
                poolStatus = "Logado! Minerando...";
                Serial.println("[STRATUM] Authorized OK!");
            } else if (g_doc.containsKey("error") && !g_doc["error"].isNull()) {
                poolStatus = "ERRO: Login!";
                Serial.print("[STRATUM] Auth FAILED: ");
                serializeJson(g_doc["error"], Serial);
                Serial.println();
            }
        }
        // Resposta ao mining.suggest_difficulty (id=3) - ignorar (não é share)
        else if (id == 3) {
            // Resposta do suggest_difficulty - ignorar silenciosamente
        }
        // Respostas ao mining.submit (id >= 4) - VERIFICAR se é submit REAL
        else if (id >= 4) {
            // CRÍTICO: só processar se este ID foi usado num submit real
            if (!isRealSubmitId(id)) {
                return;
            }
            // Loga a RESPOSTA COMPLETA do pool para diagnóstico
            Serial.print("[POOL RESP] id="); Serial.print(id); Serial.print(" ");
            serializeJson(g_doc, Serial);
            Serial.println();

            if (g_doc["result"] == true) {
                acceptedShares++;
                Serial.println("[SHARE] *** ACCEPTED! ***");
                buzzerAlertSuccess();
            } else {
                rejectedShares++;
                Serial.print("[SHARE] REJECTED: ");
                if (g_doc.containsKey("error")) serializeJson(g_doc["error"], Serial);
                Serial.println();
            }
        }
    }

    // --- Notificações do pool (method) ---
    if (g_doc.containsKey("method")) {
        const char* method = g_doc["method"];

        if (strcmp(method, "mining.set_difficulty") == 0) {
            double newDiff = g_doc["params"][0].as<double>();
            currentPoolDifficulty = newDiff;
            Serial.printf("[STRATUM] Pool set diff: %.6f\n", newDiff);
        }

        if (strcmp(method, "mining.notify") == 0) {
            String newJobId = g_doc["params"][0].as<String>();
            current_prevhash = g_doc["params"][1].as<String>();
            current_coinb1 = g_doc["params"][2].as<String>();
            current_coinb2 = g_doc["params"][3].as<String>();

            num_branches = 0;
            JsonArray branches = g_doc["params"][4];
            for (int i = 0; i < 16 && i < branches.size(); i++) {
                if (!branches[i].isNull())
                    merkle_branches[num_branches++] = branches[i].as<String>();
            }

            current_version_hex = g_doc["params"][5].as<String>();
            current_nbits_hex = g_doc["params"][6].as<String>();
            current_ntime_hex = g_doc["params"][7].as<String>();

            current_job_id = newJobId;
            extranonce2_val = 1;
            lastJobTime = millis();

            Serial.printf("[JOB] job=%s prev=%s... nbits=%s ntime=%s branches=%d\n",
                current_job_id.c_str(),
                current_prevhash.substring(0, 16).c_str(),
                current_nbits_hex.c_str(),
                current_ntime_hex.c_str(),
                num_branches);
        }
    }
}

// ============================================================
// CONEXÃO STRATUM (como NerdMiner_v2)
// ============================================================

void connectToStratum() {
    poolStatus = "Conectando Pool...";
    isSubscribed = false;
    isAuthorized = false;
    isConnected = false;
    stratumMsgId = 4;  // 1=subscribe, 2=auth, 3=suggest, 4+=submit
    submitIdCount = 0;
    sharesSubmitted = 0;
    current_job_id = "";
    currentPoolDifficulty = DEFAULT_DIFFICULTY;
    lastSuggestTime = 0;
    lastJobTime = 0;
    lastPoolDataTime = 0;

    if (!client.connect(STRATUM_HOST, STRATUM_PORT)) {
        poolStatus = "ERRO: Conexao";
        Serial.println("[STRATUM] Connection FAILED");
        return;
    }
    Serial.printf("[STRATUM] Connected to %s:%d\n", STRATUM_HOST, STRATUM_PORT);
    isConnected = true;
    lastPoolDataTime = millis();

    // 1) mining.subscribe (user agent EXATO do NerdMiner_v2 V1.8.3)
    // pool.nerdminers.org reconhece este user agent e aceita diff baixa
    String sub = "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"NerdMinerV2/V1.8.3\"]}\n";
    client.print(sub);
    Serial.println("[STRATUM] >> mining.subscribe");

    // Espera e le resposta do subscribe
    delay(500);
    while (client.available()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) processStratum(line);
    }

    if (!isSubscribed) {
        Serial.println("[STRATUM] Subscribe FAILED - no valid response");
        poolStatus = "ERRO: Subscribe";
        client.stop();
        isConnected = false;
        return;
    }

    // 2) mining.authorize (igual NerdMiner_v2)
    String auth = "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"" +
                 String(WORKER_ID) + "\",\"" + String(WORKER_PASS) + "\"]}\n";
    client.print(auth);
    Serial.printf("[STRATUM] >> mining.authorize (%s)\n", WORKER_ID);

    delay(200);

    // 3) mining.suggest_difficulty (igual NerdMiner_v2 - usa id=3 fixo)
    suggestDifficulty(DEFAULT_DIFFICULTY);
    Serial.printf("[STRATUM] >> suggest_difficulty(%.10g)\n", DEFAULT_DIFFICULTY);

    // 4) Le qualquer dado pendente (authorize response, set_difficulty, notify)
    delay(200);
    while (client.available()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) processStratum(line);
    }

    poolStatus = "Aguardando job...";
    beep(100);
}

// ============================================================
// MINING LOOP
// ============================================================

void miningLoop() {
    // 1. Processa TODAS mensagens pendentes da pool
    while (client.available()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) processStratum(line);
    }

    // Recaptura tempo APÓS processar mensagens (processStratum atualiza lastPoolDataTime)
    unsigned long now = millis();

    // 2. Verifica conexão
    if (!client.connected()) {
        Serial.println("[STRATUM] Connection lost! Reconnecting...");
        poolStatus = "Reconectando...";
        client.stop();
        isConnected = false;
        isSubscribed = false;
        isAuthorized = false;
        current_job_id = "";
        delay(3000);
        connectToStratum();
        return;
    }

    // 3. Detecta inatividade do pool (sem dados por 2 minutos)
    if (now - lastPoolDataTime > POOL_INACTIVITY_MS) {
        Serial.println("[STRATUM] Pool inactivity timeout. Reconnecting...");
        poolStatus = "Timeout Pool...";
        client.stop();
        isConnected = false;
        isSubscribed = false;
        isAuthorized = false;
        current_job_id = "";
        delay(2000);
        connectToStratum();
        return;
    }

    // 4. Keep-alive: envia suggest_difficulty a cada 30s se não enviamos nada
    if (now - lastPoolDataTime > KEEPALIVE_MS) {
        suggestDifficulty(currentPoolDifficulty);
        Serial.println("[STRATUM] Keep-alive sent");
        lastSuggestTime = now;
    }

    // 5. Só minera se autorizado e tem job
    if (!isAuthorized || current_job_id.length() == 0 || !isSubscribed) {
        if (!isAuthorized && isSubscribed) {
            poolStatus = "Aguardando auth...";
        } else if (isAuthorized && current_job_id.length() == 0) {
            poolStatus = "Aguardando job...";
        }
        return;
    }

    // 6. Prepara dados de mineração
    String extranonce2 = formatExtranonce2(extranonce2_val, extranonce2_size);
    uint8_t blockheader[80];
    if (!buildBlockHeader(extranonce2, blockheader)) return;

    // Debug: loga primeiro header para verificação
    if (firstMiningLog) {
        firstMiningLog = false;
        Serial.print("[DEBUG] Header (hex): ");
        for (int i = 0; i < 80; i++) {
            if (blockheader[i] < 0x10) Serial.print("0");
            Serial.print(blockheader[i], HEX);
        }
        Serial.println();
    }

    // 7. Minera por 50ms
    unsigned long loopStart = millis();
    uint32_t nonce = esp_random();
    unsigned int localH = 0;

    while (millis() - loopStart < 50) {
        memcpy(&blockheader[76], &nonce, 4);

        uint8_t hashBe[32];
        doubleSHA256(blockheader, 80, hashBe);
        localH++;

        // Converte hash para LE para calcular dificuldade
        uint8_t hashLe[32];
        memcpy(hashLe, hashBe, 32);
        reverseBytes(hashLe, 32);

        double hashDiff = diffFromTarget(hashLe);
        if (hashDiff > bestDiff) {
            bestDiff = hashDiff;
            diagBestNonce = nonce;
            diagBestEn2 = extranonce2;
            diagBestDiff = hashDiff;
        }

        // Conta shares locais (diff >= 0.00001) para mostrar atividade
        if (hashDiff >= 0.00001) {
            localShares++;
            if (localShares <= 5 || localShares % 20 == 0) {
                Serial.printf("[LOCAL] diff=%.6f nonce=%08x (total:%lu best:%.6f)\n", hashDiff, nonce, localShares, bestDiff);
            }
        }

        // Submete se atende dificuldade da pool
        if (hashDiff >= currentPoolDifficulty) {
            Serial.printf("[FOUND] diff=%f nonce=%08x job=%s\n", hashDiff, nonce, current_job_id.c_str());
            submitShare(nonce, extranonce2);
        }

        nonce++;
        if (nonce == 0) {
            extranonce2_val++;
            extranonce2 = formatExtranonce2(extranonce2_val, extranonce2_size);
            if (!buildBlockHeader(extranonce2, blockheader)) return;
        }
    }

    hashesTotal += localH;
    hashesInWindow += localH;

    // 8. Hashrate (janela de 1 segundo)
    unsigned long now2 = millis();
    if (now2 - lastHashTime >= 1000) {
        double elapsed = (double)(now2 - lastHashTime) / 1000.0;
        hashrate = (double)hashesInWindow / elapsed;
        hashesInWindow = 0;
        lastHashTime = now2;
        // Log de hashrate a cada 5 segundos
        static unsigned long lastHRLog = 0;
        if (now2 - lastHRLog >= 5000) {
            Serial.printf("[STATS] H/s=%.0f total=%luk local=%lu best=%.6f poolDiff=%.6f\n",
                hashrate, hashesTotal/1000, localShares, bestDiff, currentPoolDifficulty);
            lastHRLog = now2;
        }
    }

    // 9. Atualiza status da tela
    if (hashrate > 100) {
        poolStatus = "Minerando " + String((unsigned long)hashrate) + " H/s";
    }
}

// ============================================================
// BOTÕES
// ============================================================

void handleButtons() {
    if (currentState == STATE_SELECT_SSID) {
        if (digitalRead(BTN_UP) == LOW) { selectedNetworkIndex++; if (selectedNetworkIndex >= scanNetworksCount) selectedNetworkIndex = 0; beep(30); delay(150); }
        if (digitalRead(BTN_DOWN) == LOW) { selectedNetworkIndex--; if (selectedNetworkIndex < 0) selectedNetworkIndex = scanNetworksCount - 1; beep(30); delay(150); }
        if (digitalRead(BTN_SEL) == LOW) { ssid = WiFi.SSID(selectedNetworkIndex); currentState = STATE_INPUT_PASSWORD; beep(50); delay(300); }
        if (digitalRead(BTN_BACK) == LOW) { currentState = STATE_SCANNING; beep(30); delay(300); }
    }
    else if (currentState == STATE_INPUT_PASSWORD) {
        if (digitalRead(BTN_UP) == LOW) { charIndex++; if (chars[charIndex] == '\0') charIndex = 0; beep(30); delay(150); }
        if (digitalRead(BTN_DOWN) == LOW) { charIndex--; if (charIndex < 0) charIndex = strlen(chars) - 1; beep(30); delay(150); }
        if (digitalRead(BTN_SEL) == LOW) { password += chars[charIndex]; beep(50); delay(150); }
        if (digitalRead(BTN_BACK) == LOW) { if (password.length() > 0) password.remove(password.length() - 1); beep(30); delay(150); }
        if (digitalRead(BTN_UP) == LOW && digitalRead(BTN_DOWN) == LOW) {
            currentState = STATE_CONNECTING; drawUI();
            WiFi.begin(ssid.c_str(), password.c_str());
            int attempts = 0;
            while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(500); attempts++; }
            if (WiFi.status() == WL_CONNECTED) {
                preferences.begin("wifi", false);
                preferences.putString("ssid", ssid);
                preferences.putString("pass", password);
                preferences.end();
                currentState = STATE_MINING;
                connectToStratum();
            } else {
                currentState = STATE_INPUT_PASSWORD; password = ""; beep(500);
            }
            delay(500);
        }
    }
    else if (currentState == STATE_MINING) {
        if (digitalRead(BTN_BACK) == LOW && digitalRead(BTN_DOWN) == LOW) {
            preferences.begin("wifi", false); preferences.clear(); preferences.end(); ESP.restart();
        }
    }
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
    Serial.begin(115200);
    Serial.println("\n\n=== ESP32 BTC Miner v1.8 ===");

    pinMode(BTN_UP, INPUT_PULLUP); pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_SEL, INPUT_PULLUP); pinMode(BTN_BACK, INPUT_PULLUP);
    pinMode(BAT_ADC, INPUT); buzzerInit();

    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED not found!"); for (;;);
    }
    drawSplashScreen();

    preferences.begin("wifi", false);
    ssid = preferences.getString("ssid", "");
    password = preferences.getString("pass", "");
    preferences.end();

    if (ssid.length() > 0) {
        Serial.printf("[WIFI] Connecting to: %s\n", ssid.c_str());
        WiFi.begin(ssid.c_str(), password.c_str());
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500); attempts++;
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
            currentState = STATE_MINING;
            connectToStratum();
        } else {
            currentState = STATE_SCANNING;
        }
    } else {
        currentState = STATE_SCANNING;
    }

    lastHashTime = millis();
    lastSuggestTime = millis();
    beep(100);
}

void loop() {
    if (currentState == STATE_SCANNING) {
        drawUI();
        scanNetworksCount = WiFi.scanNetworks();
        if (scanNetworksCount == 0) delay(1000);
        else { currentState = STATE_SELECT_SSID; selectedNetworkIndex = 0; }
    }
    if (currentState == STATE_MINING) {
        miningLoop();
    }
    handleButtons();

    static unsigned long lastScreenUpdate = 0;
    if (millis() - lastScreenUpdate > 250) { drawUI(); lastScreenUpdate = millis(); }
}
