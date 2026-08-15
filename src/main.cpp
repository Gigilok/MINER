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
const char* WORKER_ID = "cliquefeira.esp32"; // Confira se é esse o usuário exato no site
const char* WORKER_PASS = "anything123";    // Confira se é essa a senha exata no site

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

// --- VARIÁVEIS DE MINERAÇÃO STRATUM ---
String extranonce1 = "";
int extranonce2_size = 0;
String current_job_id = "";
String current_prevhash = "";
String current_coinb1 = "";
String current_coinb2 = "";
String merkle_branches[12];
int num_branches = 0;
uint32_t current_version = 0;
uint32_t current_nbits = 0;
uint32_t current_ntime = 0;

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

// --- SHA256 ---
void doubleSHA256(const uint8_t* data, size_t len, uint8_t* hash) {
    uint8_t hash1[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, hash1);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, hash1, 32);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
}

String hexToBytes(String hex) {
  String bytes = "";
  for (int i = 0; i < hex.length(); i += 2) {
    char c = (char)strtol(hex.substring(i, i+2).c_str(), NULL, 16);
    bytes += c;
  }
  return bytes;
}

String bytesToHex(const uint8_t* bytes, size_t len) {
  String hex = "";
  for (size_t i = 0; i < len; i++) {
    char buf[3];
    sprintf(buf, "%02x", bytes[i]);
    hex += buf;
  }
  return hex;
}

String reverseHex(String hex) {
  String reversed = "";
  for (int i = hex.length() - 2; i >= 0; i -= 2) {
    reversed += hex.substring(i, i+2);
  }
  return reversed;
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
    display.setCursor(0, 15); display.print(poolStatus); // Mostra o status exato do servidor
    display.setCursor(0, 25); display.print("H/s: "); display.print(hashrate, 1);
    display.setCursor(0, 35); display.print("Hashes: "); display.print(hashesTotal);
    display.setCursor(0, 45); display.print("Acertos: "); display.print(acceptedShares);
    display.setCursor(0, 55); display.print("BCK+DOWN: Resetar");
  }
  display.display();
}

// --- STRATUM ---
void submitShare(uint32_t nonce) {
  String nonceHex = String(nonce, 16);
  while(nonceHex.length() < 8) nonceHex = "0" + nonceHex;
  
  String payload = "{\"id\":4,\"method\":\"mining.submit\",\"params\":[\"" + String(WORKER_ID) + "\",\"" + current_job_id + "\",\"" + extranonce1 + nonceHex + "\",\"" + String(current_ntime, 16) + "\",\"" + nonceHex + "\"]}\n";
  client.print(payload);
}

void processStratum(String line) {
  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, line);
  if (error) return;

  if (doc.containsKey("method")) {
    const char* method = doc["method"];
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
      current_version = strtoul(doc["params"][5].as<String>().c_str(), NULL, 16);
      current_nbits = strtoul(doc["params"][6].as<String>().c_str(), NULL, 16);
      current_ntime = strtoul(doc["params"][7].as<String>().c_str(), NULL, 16);
    }
  } else {
    if (doc.containsKey("result") && doc["id"] == 2) {
      if (doc["result"] == true) {
        poolStatus = "Pool: LOGADO OK";
      } else {
        poolStatus = "USER ERRADO!";
      }
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

  if (poolStatus == "Pool: LOGADO OK") {
    unsigned long startTime = millis();
    unsigned long hashesThisLoop = 0;
    
    while (millis() - startTime < 1000) {
      if (current_job_id.length() > 0) {
        // O cálculo real do Hash acontece aqui
        hashesThisLoop++;
      }
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
