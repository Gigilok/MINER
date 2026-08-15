#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <WiFiClient.h>
#include <esp_task_wdt.h>

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

// --- POOL (public-pool.io - suporta dificuldade baixa para ESP32) ---
// IMPORTANTE: Troque o WORKER_ID abaixo pelo seu endereço de carteira Bitcoin (BC1q... ou 1A1z...)
// Sem uma carteira BTC real, os shares minerados não vão para lugar nenhum.
// Crie uma carteira em: blockchain.com, wallet.com, ou use a Trust Wallet.
const char* STRATUM_HOST = "public-pool.io";
const int STRATUM_PORT = 3333;
const char* WORKER_ID = "1FRpCfmiwAGVkCLt2FjVuuoAjhSaE2j4QN.esp32";  // sua carteira BTC + nome do worker
const char* WORKER_PASS = "x";
const double DEFAULT_DIFFICULTY = 0.00015;
const unsigned long KEEPALIVE_MS = 30000;

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
unsigned long lastKeepAlive = 0;
unsigned long hashesInWindow = 0;
unsigned long windowStartTime = 0;

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

// Target em little-endian (256 bits)
uint8_t target_le[32] = {0};

// Extranonce2 tracker
unsigned long extranonce2_val = 1;

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

// Converte um caractere hex para valor numérico (suporta A-F e a-f)
uint8_t hexCharToVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

// Converte string hex para array de bytes (suporta maiúsculas e minúsculas)
void hexToBytes(const String& hex, uint8_t* bytes) {
    int len = hex.length() / 2;
    for (int i = 0; i < len; i++) {
        uint8_t high = hexCharToVal(hex.charAt(i * 2));
        uint8_t low  = hexCharToVal(hex.charAt(i * 2 + 1));
        bytes[i] = (high << 4) | low;
    }
}

// Inverte a ordem dos bytes de um array in-place
void reverseBytes(uint8_t* data, int len) {
    for (int i = 0; i < len / 2; i++) {
        uint8_t tmp = data[i];
        data[i] = data[len - 1 - i];
        data[len - 1 - i] = tmp;
    }
}

// Reverte uma string hex (troca pares de bytes) - usa para campos do header
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
    mbedtls_sha256(data, len, hash1, 0);  // 0 = SHA-256 (não SHA-224)
    mbedtls_sha256(hash1, 32, out, 0);
}

// ============================================================
// DIFFICULTY / TARGET
// ============================================================

static const double truediffone = 26959535291011309493156476344723991336010898738574164086137773096960.0;

// Converte array LE de 256 bits para double
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

// Calcula a dificuldade a partir de um hash (em LE)
double diffFromTarget(const uint8_t* hashLe) {
    double d = le256todouble(hashLe);
    if (d == 0.0) d = 1.0;
    return truediffone / d;
}

// Converte nbits (string hex do pool) para target de 32 bytes em LE
void nbitsToTarget(const String& nbitsHex, uint8_t* targetOut) {
    // target = nbits[2:] + '00' * (nbits[0:2] - 3), preenchido com zeros à esquerda
    int sizeNibbles = nbitsHex.length();
    int exponent = 0;
    // Primeiro byte (2 hex chars) é o expoente
    String expStr = nbitsHex.substring(0, 2);
    exponent = (int)strtol(expStr.c_str(), NULL, 16);
    // Coeficiente é o resto
    String coeff = nbitsHex.substring(2);

    // Zerar tudo
    memset(targetOut, 0, 32);

    // Número de zeros antes do coeficiente
    int zeroBytes = exponent - 3;
    if (zeroBytes < 0) zeroBytes = 0;
    if (zeroBytes > 29) zeroBytes = 29;

    // Preenche o coeficiente nos bytes corretos
    int coeffLen = coeff.length() / 2;
    // Trunca se necessário para caber
    if (zeroBytes + coeffLen > 32) {
        coeffLen = 32 - zeroBytes;
    }
    for (int i = 0; i < coeffLen; i++) {
        uint8_t b = 0;
        if (i * 2 < coeff.length()) {
            b = hexCharToVal(coeff.charAt(i * 2)) << 4;
        }
        if (i * 2 + 1 < coeff.length()) {
            b |= hexCharToVal(coeff.charAt(i * 2 + 1));
        }
        targetOut[zeroBytes + coeffLen - 1 - i] = b;
    }
    // target está em big-endian agora; inverter para little-endian
    reverseBytes(targetOut, 32);
}

// Gera string hex do extranonce2 com tamanho correto
String formatExtranonce2(unsigned long val, int size) {
    String hex = String(val, HEX);
    int neededChars = size * 2;
    while (hex.length() < neededChars) hex = "0" + hex;
    if (hex.length() > neededChars) hex = hex.substring(hex.length() - neededChars);
    return hex;
}

// ============================================================
// CONSTRUIR COINBASE + MERKLE ROOT + BLOCK HEADER
// ============================================================

// Retorna true se construiu com sucesso
bool buildBlockHeader(const String& extranonce2, uint8_t* headerOut, int* coinbaseLen) {
    // 1. Constrói coinbase hex
    String coinbaseHex = current_coinb1 + extranonce1 + extranonce2 + current_coinb2;
    int cbLen = coinbaseHex.length() / 2;
    if (cbLen <= 0 || cbLen > 256) return false;
    *coinbaseLen = cbLen;

    uint8_t coinbaseBytes[256];
    hexToBytes(coinbaseHex, coinbaseBytes);

    // 2. Double SHA256 da coinbase → merkle root inicial (em BE)
    uint8_t merkleRoot[32];
    doubleSHA256(coinbaseBytes, cbLen, merkleRoot);

    // 3. Árvore de Merkle: hash cada branch com o root atual
    //    IMPORTANTE: tudo permanece em BE (ordem natural do SHA256)
    //    NÃO revertemos os branches
    uint8_t buf[64];
    for (int i = 0; i < num_branches; i++) {
        if (merkle_branches[i].length() < 64) continue; // branch deve ter 32 bytes (64 hex chars)
        uint8_t branchBytes[32];
        hexToBytes(merkle_branches[i], branchBytes);  // ← SEM reverseHex!

        memcpy(buf, merkleRoot, 32);       // root em BE
        memcpy(buf + 32, branchBytes, 32);  // branch em BE (consistente!)
        doubleSHA256(buf, 64, merkleRoot);  // resultado em BE
    }

    // 4. Monta o block header de 80 bytes
    //    Todos os campos ficam em Little-Endian no header

    // Version (4 bytes) → reverso de BE para LE
    hexToBytes(reverseHex(current_version_hex), &headerOut[0]);

    // Previous block hash (32 bytes) → reverso de BE para LE
    hexToBytes(reverseHex(current_prevhash), &headerOut[4]);

    // Merkle root (32 bytes) → SHA256 retorna em BE, revertemos para LE
    uint8_t merkleLe[32];
    memcpy(merkleLe, merkleRoot, 32);
    reverseBytes(merkleLe, 32);
    memcpy(&headerOut[36], merkleLe, 32);

    // ntime (4 bytes) → reverso de BE para LE
    hexToBytes(reverseHex(current_ntime_hex), &headerOut[68]);

    // nbits (4 bytes) → reverso de BE para LE
    hexToBytes(reverseHex(current_nbits_hex), &headerOut[72]);

    // nonce (4 bytes) → preenchido depois, offset 76-79
    memset(&headerOut[76], 0, 4);

    return true;
}

// ============================================================
// TELA OLED
// ============================================================

void drawSplashScreen() {
    display.clearDisplay();
    display.setTextSize(3);
    display.setTextColor(WHITE);
    display.setCursor(15, 15);
    display.print("MINER");
    display.display();
    delay(2000);
}

void drawUI() {
    display.clearDisplay();
    int batRaw = analogRead(BAT_ADC);
    int batPercent = map(batRaw, 0, 4095, 0, 100);

    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(85, 0);
    display.print("B:");
    display.print(batPercent);
    display.print("%");

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
        display.setCursor(0, 0); display.print("BTC REAL MINER");
        display.drawLine(0, 10, 128, 10, WHITE);
        display.setCursor(0, 15); display.print(poolStatus);
        display.setCursor(0, 25); display.print("H/s: ");
        if (hashrate >= 1000.0) {
            display.print(hashrate / 1000.0, 2);
            display.print("k");
        } else {
            display.print((unsigned long)hashrate);
        }
        display.setCursor(0, 35); display.print("Hashes: "); display.print(hashesTotal / 1000); display.print("k");
        display.setCursor(0, 45);
        display.print("Ok:"); display.print(acceptedShares);
        display.print(" Rj:"); display.print(rejectedShares);
        display.setCursor(0, 55);
        display.print("Best:"); display.print(bestDiff, 0);
        display.print(" BCK+DN:RST");
    }
    display.display();
}

// ============================================================
// STRATUM PROTOCOL
// ============================================================

int stratumMsgId = 1;

void submitShare(uint32_t nonce, const String& extranonce2) {
    String nonceHex = String(nonce, HEX);
    while (nonceHex.length() < 8) nonceHex = "0" + nonceHex;

    String payload = "{\"id\":" + String(stratumMsgId++) +
        ",\"method\":\"mining.submit\",\"params\":[\"" +
        String(WORKER_ID) + "\",\"" +
        current_job_id + "\",\"" +
        extranonce2 + "\",\"" +
        current_ntime_hex + "\",\"" +
        nonceHex + "\"]}\n";
    client.print(payload);
}

void suggestDifficulty(double diff) {
    String payload = "{\"id\":null,\"method\":\"mining.suggest_difficulty\",\"params\":[" + String(diff, 6) + "]}\n";
    client.print(payload);
}

void processStratum(String line) {
    if (line.length() < 2) return;

    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, line);
    if (error) return;

    // --- Resposta ao mining.subscribe ---
    if (doc.containsKey("id") && doc["id"] == 1) {
        if (doc["result"].is<JsonArray>()) {
            JsonArray arr = doc["result"].as<JsonArray>();
            // arr[0] = subscriptions, arr[1] = extranonce1, arr[2] = extranonce2_size
            extranonce1 = arr[1].as<String>();
            extranonce2_size = arr[2].as<int>();
            if (extranonce2_size <= 0) extranonce2_size = 4;
            extranonce2_val = 1; // reset
            isSubscribed = true;
            Serial.printf("[STRATUM] Subscribed. extranonce1=%s, size=%d\n",
                           extranonce1.c_str(), extranonce2_size);
        }
    }

    // --- Resposta ao mining.authorize ---
    if (doc.containsKey("id") && doc["id"] == 2) {
        if (doc["result"] == true) {
            isAuthorized = true;
            poolStatus = "Logado! Minerando...";
            Serial.println("[STRATUM] Authorized OK!");
        } else {
            poolStatus = "ERRO: Login!";
            Serial.println("[STRATUM] Authorization FAILED!");
        }
    }

    // --- Resposta ao mining.submit ---
    if (doc.containsKey("id")) {
        int id = doc["id"].as<int>();
        if (id >= 3) {  // submit responses have id >= 3
            if (doc["result"] == true) {
                acceptedShares++;
                Serial.println("[SHARE] ACCEPTED!");
                buzzerAlertSuccess();
            } else {
                rejectedShares++;
                Serial.print("[SHARE] REJECTED: ");
                if (doc.containsKey("error")) {
                    serializeJson(doc["error"], Serial);
                }
                Serial.println();
            }
        }
    }

    // --- method: mining.set_difficulty ---
    if (doc.containsKey("method")) {
        const char* method = doc["method"];

        if (strcmp(method, "mining.set_difficulty") == 0) {
            currentPoolDifficulty = doc["params"][0].as<double>();
            Serial.printf("[STRATUM] Pool difficulty: %f\n", currentPoolDifficulty);
        }

        if (strcmp(method, "mining.notify") == 0) {
            current_job_id = doc["params"][0].as<String>();
            current_prevhash = doc["params"][1].as<String>();
            current_coinb1 = doc["params"][2].as<String>();
            current_coinb2 = doc["params"][3].as<String>();

            // Merkle branches
            num_branches = 0;
            JsonArray branches = doc["params"][4];
            for (int i = 0; i < 16 && i < branches.size(); i++) {
                if (!branches[i].isNull()) {
                    merkle_branches[num_branches++] = branches[i].as<String>();
                }
            }

            current_version_hex = doc["params"][5].as<String>();
            current_nbits_hex = doc["params"][6].as<String>();
            current_ntime_hex = doc["params"][7].as<String>();

            // Calcula o target a partir de nbits
            nbitsToTarget(current_nbits_hex, target_le);

            // Reseta extranonce2 para novo job
            extranonce2_val = 1;

            Serial.printf("[STRATUM] New job: %s (branches=%d)\n",
                           current_job_id.c_str(), num_branches);
        }
    }
}

void connectToStratum() {
    poolStatus = "Conectando Pool...";
    isSubscribed = false;
    isAuthorized = false;
    stratumMsgId = 1;

    if (!client.connect(STRATUM_HOST, STRATUM_PORT)) {
        poolStatus = "ERRO: Conexao Pool";
        Serial.println("[STRATUM] Connection FAILED");
        return;
    }

    Serial.printf("[STRATUM] Connected to %s:%d\n", STRATUM_HOST, STRATUM_PORT);
    poolStatus = "Enviando Login...";

    // mining.subscribe
    String sub = "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"esp32-miner/1.0.0\"]}\n";
    client.print(sub);

    // mining.authorize
    String auth = "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"" +
                 String(WORKER_ID) + "\",\"" + String(WORKER_PASS) + "\"]}\n";
    client.print(auth);

    // Suggest difficulty
    suggestDifficulty(DEFAULT_DIFFICULTY);

    lastKeepAlive = millis();
    beep(100);
}

// ============================================================
// MINING LOOP
// ============================================================

void miningLoop() {
    // 1. Processa mensagens do pool
    while (client.available()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            processStratum(line);
        }
    }

    // 2. Keep-alive (envia suggest_difficulty a cada 30s para não perder conexão)
    if (millis() - lastKeepAlive > KEEPALIVE_MS && client.connected()) {
        suggestDifficulty(currentPoolDifficulty);
        lastKeepAlive = millis();
    }

    // 3. Reconexão automática
    if (!client.connected()) {
        poolStatus = "Reconectando...";
        current_job_id = "";
        isSubscribed = false;
        isAuthorized = false;
        connectToStratum();
        delay(3000);
        return;
    }

    // 4. Só minera se está logado e tem job
    // NOTA: extranonce1 pode ser vazio (""), isso é valido em algumas pools
    if (!isAuthorized || current_job_id.length() == 0 || !isSubscribed) {
        return;
    }

    // 5. Prepara extranonce2 com tamanho correto
    String extranonce2 = formatExtranonce2(extranonce2_val, extranonce2_size);

    // 6. Constrói o block header
    uint8_t blockheader[80];
    int coinbaseLen = 0;
    if (!buildBlockHeader(extranonce2, blockheader, &coinbaseLen)) {
        return; // erro construindo header
    }

    // 7. Minera por ~100ms (deixa o loop do FreeRTOS cuidar do WiFi)
    unsigned long loopStart = millis();
    uint32_t nonce = esp_random();
    unsigned int localHashes = 0;

    while (millis() - loopStart < 100) {
        // Escreve nonce no header (LE nativo do ESP32)
        memcpy(&blockheader[76], &nonce, 4);

        // Double SHA256
        uint8_t hashBe[32]; // SHA256 retorna em big-endian
        doubleSHA256(blockheader, 80, hashBe);
        localHashes++;

        // Converte hash para little-endian para comparação de dificuldade
        uint8_t hashLe[32];
        memcpy(hashLe, hashBe, 32);
        reverseBytes(hashLe, 32);

        // Verifica se a dificuldade do hash é maior que a dificuldade da pool
        double hashDiff = diffFromTarget(hashLe);
        if (hashDiff > bestDiff) {
            bestDiff = hashDiff;
        }

        if (hashDiff >= currentPoolDifficulty) {
            Serial.printf("[FOUND] diff=%f nonce=%08x\n", hashDiff, nonce);
            submitShare(nonce, extranonce2);
        }

        nonce++;

        // Se o nonce deu wrap, incrementa extranonce2
        if (nonce == 0) {
            extranonce2_val++;
            extranonce2 = formatExtranonce2(extranonce2_val, extranonce2_size);
            if (!buildBlockHeader(extranonce2, blockheader, &coinbaseLen)) {
                return;
            }
            Serial.printf("[MINER] Extranonce2 rolled to: %s\n", extranonce2.c_str());
        }
    }

    hashesTotal += localHashes;
    hashesInWindow += localHashes;

    // 8. Calcula hashrate a cada 1 segundo
    unsigned long now = millis();
    if (now - lastHashTime >= 1000) {
        double elapsed = (double)(now - lastHashTime) / 1000.0;
        hashrate = (double)hashesInWindow / elapsed;
        hashesInWindow = 0;
        lastHashTime = now;
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
    Serial.println("\n\n=== ESP32 Bitcoin Miner v1.1 ===");

    pinMode(BTN_UP, INPUT_PULLUP); pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_SEL, INPUT_PULLUP); pinMode(BTN_BACK, INPUT_PULLUP);
    pinMode(BAT_ADC, INPUT); buzzerInit();

    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED not found!");
        for (;;);
    }
    drawSplashScreen();

    // Desabilita WDT do core 0 para não interferir na mineração
    disableCore0WDT();

    // Carrega credenciais salvas
    preferences.begin("wifi", false);
    ssid = preferences.getString("ssid", "");
    password = preferences.getString("pass", "");
    preferences.end();

    if (ssid.length() > 0) {
        Serial.printf("[WIFI] Connecting to saved: %s\n", ssid.c_str());
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
            Serial.println("[WIFI] Failed to connect");
            currentState = STATE_SCANNING;
        }
    } else {
        currentState = STATE_SCANNING;
    }

    windowStartTime = millis();
    lastHashTime = millis();
    beep(100);
}

void loop() {
    if (currentState == STATE_SCANNING) {
        drawUI();
        scanNetworksCount = WiFi.scanNetworks();
        if (scanNetworksCount == 0) {
            delay(1000);
        } else {
            currentState = STATE_SELECT_SSID;
            selectedNetworkIndex = 0;
        }
    }
    if (currentState == STATE_MINING) {
        miningLoop();
    }
    handleButtons();

    static unsigned long lastScreenUpdate = 0;
    if (millis() - lastScreenUpdate > 250) { drawUI(); lastScreenUpdate = millis(); }
}
