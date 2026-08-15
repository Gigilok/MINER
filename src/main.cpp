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

// --- POOL (BRAIINS) ---
const char* STRATUM_HOST = "stratum.braiins.com";
const int STRATUM_PORT = 3333;
const char* WORKER_ID = "cliquefeira.esp32";
const char* WORKER_PASS = "anything123";

enum UI_State { STATE_SCANNING, STATE_SELECT_SSID, STATE_INPUT_PASSWORD, STATE_CONNECTING, STATE_MINING };
UI_State currentState = STATE_SCANNING;

String ssid = "";
String password = "";
String poolStatus = "Desconectado";

WiFiClient client;
unsigned long hashesTotal = 0;
int acceptedShares = 0;
double hashrate = 0.0;

char chars[] = " abcdefghijklmnopqrstuvwxyz0123456789@!#$%&*";
int charIndex = 0;
int scanNetworksCount = 0;
int selectedNetworkIndex = 0;

// --- VARIÁVEIS STRATUM ---
double current_difficulty = 1.0;
String extranonce1 = "";
int extranonce2_size = 4;
String current_job_id = "";
String current_prevhash = "";
String current_coinb1 = "";
String current_coinb2 = "";
String merkle_branches[12];
int num_branches = 0;
String current_version_hex = "";
String current_nbits_hex = "";
String current_ntime_hex = "";

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

// --- SHA256 (Motor do NerdMiner) ---
void doubleSHA256(const uint8_t* data, size_t len, uint8_t* out) {
    uint8_t hash1[32];
    mbedtls_sha256(data, len, hash1, 0);
    mbedtls_sha256(hash1, 32, out, 0);
}

String reverseHex(String hex) {
  String reversed = "";
  for (int i = hex.length() - 2; i >= 0; i -= 2) {
    reversed += hex.substring(i, i+2);
  }
  return reversed;
}

void hexToBytes(const String& hex, uint8_t* bytes) {
    for (int i = 0; i < hex.length() / 2; i++) {
        char c = hex.charAt(i * 2);
        int high = (c >= 'a') ? (c - 'a' + 10) : (c - '0');
        c = hex.charAt(i * 2 + 1);
        int low = (c >= 'a') ? (c - 'a' + 10) : (c - '0');
        bytes[i] = (high << 4) | low;
    }
}

// Checagem de Dificuldade (Se o hash é menor que o target)
bool checkHash(uint8_t* hash, double difficulty) {
    if (difficulty <= 0) difficulty = 1.0;
    // Target máximo do Bitcoin: 0x00000000FFFF0000... (em Little Endian)
    if (hash[31] > 0 || hash[30] > 0 || hash[29] > 0 || hash[28] > 0) return false;
    
    double max_val = 0xFFFF;
    double target_val = max_val / difficulty;
    uint32_t target_int = (uint32_t)target_val;
    
    uint32_t hash_val = ((uint32_t)hash[27] << 8) | hash[26];
    if (hash_val < target_int) return true;
    if (hash_val > target_int) return false;
    
    for (int i = 25; i >= 0; i--) {
        if (hash[i] > 0) return false;
    }
    return true;
}

// --- TELA ---
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
    for(int i=0; i<password.length(); i++) display.print("*");
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
    display.setCursor(0, 25); display.print("H/s: "); display.print(hashrate, 1);
    display.setCursor(0, 35); display.print("Hashes: "); display.print(hashesTotal);
    display.setCursor(0, 45); display.print("Acertos: "); display.print(acceptedShares);
    display.setCursor(0, 55); display.print("BCK+DOWN: Resetar");
  }
  display.display();
}

// --- STRATUM ---
void submitShare(uint32_t nonce, String extranonce2) {
  String nonceHex = String(nonce, HEX);
  while(nonceHex.length() < 8) nonceHex = "0" + nonceHex;
  
  String payload = "{\"id\":4,\"method\":\"mining.submit\",\"params\":[\"" + String(WORKER_ID) + "\",\"" + current_job_id + "\",\"" + extranonce2 + "\",\"" + current_ntime_hex + "\",\"" + nonceHex + "\"]}\n";
  client.print(payload);
}

void processStratum(String line) {
  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, line);
  if (error) return;

  if (doc.containsKey("method")) {
    const char* method = doc["method"];
    
    if (strcmp(method, "mining.set_difficulty") == 0) {
      current_difficulty = doc["params"][0].as<double>();
    }
    
    if (strcmp(method, "mining.notify") == 0) {
      current_job_id = doc["params"][0].as<String>();
      current_prevhash = doc["params"][1].as<String>();
      current_coinb1 = doc["params"][2].as<String>();
      current_coinb2 = doc["params"][3].as<String>();
      num_branches = 0;
      for (int i = 0; i < 12; i++) {
        if (!doc["params"][4][i].isNull()) {
          merkle_branches[num_branches++] = doc["params"][4][i].as<String>();
        }
      }
      current_version_hex = doc["params"][5].as<String>();
      current_nbits_hex = doc["params"][6].as<String>();
      current_ntime_hex = doc["params"][7].as<String>();
    }
  } else {
    if (doc.containsKey("result") && doc["id"] == 2) {
      if (doc["result"] == true) poolStatus = "Pool: LOGADO OK";
      else poolStatus = "USER ERRADO!";
    }
    if (doc.containsKey("result") && doc["result"] == true && doc["id"] == 4) {
      acceptedShares++;
      buzzerAlertSuccess();
    }
  }
}

void miningLoop() {
  while (client.available()) {
    String line = client.readStringUntil('\n');
    processStratum(line);
  }

  if (poolStatus == "Pool: LOGADO OK" && current_job_id.length() > 0) {
    unsigned long startTime = millis();
    unsigned long hashesThisLoop = 0;
    
    // 1. Gera o extranonce2 aleatório para este job
    uint32_t ex2_rand = esp_random();
    char ex2_buf[9];
    sprintf(ex2_buf, "%08x", ex2_rand);
    String extranonce2 = String(ex2_buf);
    
    // 2. Constrói a Coinbase Transaction
    String coinbase_hex = current_coinb1 + extranonce1 + extranonce2 + current_coinb2;
    uint8_t coinbase_bytes[coinbase_hex.length() / 2];
    hexToBytes(coinbase_hex, coinbase_bytes);
    
    // 3. Calcula a Merkle Root
    uint8_t merkle_root[32];
    doubleSHA256(coinbase_bytes, coinbase_hex.length() / 2, merkle_root);
    
    for (int i = 0; i < num_branches; i++) {
      String reversed_branch = reverseHex(merkle_branches[i]);
      uint8_t branch[32];
      hexToBytes(reversed_branch, branch);
      
      uint8_t buf[64];
      memcpy(buf, merkle_root, 32);
      memcpy(buf + 32, branch, 32);
      doubleSHA256(buf, 64, merkle_root);
    }
    
    // 4. Constrói o Block Header de 80 bytes
    uint8_t blockheader[80];
    
    // Version (Little Endian)
    String rev_version = reverseHex(current_version_hex);
    hexToBytes(rev_version, &blockheader[0]);
    
    // PrevHash (Little Endian)
    String rev_prevhash = reverseHex(current_prevhash);
    hexToBytes(rev_prevhash, &blockheader[4]);
    
    // Merkle Root (Já está em Little Endano)
    memcpy(&blockheader[36], merkle_root, 32);
    
    // nTime (Little Endian)
    String rev_ntime = reverseHex(current_ntime_hex);
    hexToBytes(rev_ntime, &blockheader[68]);
    
    // nBits (Little Endian)
    String rev_nbits = reverseHex(current_nbits_hex);
    hexToBytes(rev_nbits, &blockheader[72]);
    
    // 5. Loop de Hashing (Mineração real por 1 segundo)
    uint32_t nonce = esp_random();
    
    while (millis() - startTime < 1000) {
      // Coloca o Nonce nos últimos 4 bytes do header (offset 76)
      memcpy(&blockheader[76], &nonce, 4);
      
      // Faz o duplo SHA256
      uint8_t hash[32];
      doubleSHA256(blockheader, 80, hash);
      
      hashesThisLoop++;
      
      // Checa se achou um Share ou Bloco!
      if (checkHash(hash, current_difficulty)) {
        submitShare(nonce, extranonce2);
      }
      
      nonce++;
    }
    
    hashesTotal += hashesThisLoop;
    hashrate = (double)hashesThisLoop;
  }
}

void connectToStratum() {
  poolStatus = "Conectando Pool...";
  if (!client.connect(STRATUM_HOST, STRATUM_PORT)) {
    poolStatus = "Pool: ERR (Bloqueio)";
    return;
  }
  poolStatus = "Enviando Login...";
  String subscribe = "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"esp32-miner/1.0\"]}\n";
  client.print(subscribe);
  String auth = "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"" + String(WORKER_ID) + "\",\"" + String(WORKER_PASS) + "\"]}\n";
  client.print(auth);
  beep(100);
}

// --- BOTÕES ---
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

void setup() {
  Serial.begin(115200);
  pinMode(BTN_UP, INPUT_PULLUP); pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SEL, INPUT_PULLUP); pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(BAT_ADC, INPUT); buzzerInit();
  
  Wire.begin(OLED_SDA, OLED_SCL);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { for(;;); }
  drawSplashScreen();
  
  preferences.begin("wifi", false);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("pass", "");
  preferences.end();

  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), password.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(500); attempts++; }
    if (WiFi.status() == WL_CONNECTED) { currentState = STATE_MINING; connectToStratum(); }
    else { currentState = STATE_SCANNING; }
  } else { currentState = STATE_SCANNING; }
  beep(100);
}

void loop() {
  if (currentState == STATE_SCANNING) {
    drawUI();
    scanNetworksCount = WiFi.scanNetworks();
    if (scanNetworksCount == 0) { delay(1000); } 
    else { currentState = STATE_SELECT_SSID; selectedNetworkIndex = 0; }
  }
  if (currentState == STATE_MINING) { miningLoop(); }
  handleButtons();
  
  static unsigned long lastScreenUpdate = 0;
  if (millis() - lastScreenUpdate > 250) { drawUI(); lastScreenUpdate = millis(); }
}
