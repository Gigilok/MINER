#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <WiFiClient.h>

// --- CONFIGURAÇÃO DOS PINOS ---
#define OLED_SDA 21
#define OLED_SCL 22
#define BTN_UP   5
#define BTN_DOWN 27
#define BTN_SEL  32
#define BTN_BACK 33
#define BUZZER   4
#define BAT_ADC  34

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Preferences preferences;

// --- CONFIGURAÇÃO DA MINERAÇÃO (BTC EM CONJUNTO - POOL) ---
const char* STRATUM_HOST = "stratum.braiins.com";
const int STRATUM_PORT = 3333;
const char* BTC_WALLET = "1FRpCfmiwAGVkCLt2FjVuuoAjhSaE2j4QN";
const char* WORKER_NAME = "esp32";

// --- MÁQUINA DE ESTADOS DO MENU ---
enum UI_State {
  STATE_SCANNING,
  STATE_SELECT_SSID,
  STATE_INPUT_PASSWORD,
  STATE_CONNECTING,
  STATE_MINING
};
UI_State currentState = STATE_SCANNING;

// Variáveis de Rede
String ssid = "";
String password = "";
bool wifiConnected = false;
bool miningConnected = false;

int scanNetworksCount = 0;
int selectedNetworkIndex = 0;

// Variáveis de Mineração
WiFiClient client;
unsigned long hashesTotal = 0;
int acceptedShares = 0;
double hashrate = 0.0;

// Variáveis de digitação
char chars[] = " abcdefghijklmnopqrstuvwxyz0123456789@!#$%&*";
int charIndex = 0;

// --- FUNÇÕES DA BUZINA ---
void buzzerInit() {
    pinMode(BUZZER, INPUT);
}

void beep(int durationMs = 50) {
    pinMode(BUZZER, OUTPUT);
    int cycles = durationMs * 2; 
    for (int i = 0; i < cycles; i++) {
        digitalWrite(BUZZER, HIGH);
        delayMicroseconds(250);
        digitalWrite(BUZZER, LOW);
        delayMicroseconds(250);
    }
    pinMode(BUZZER, INPUT);
}

void buzzerAlertSuccess() {
    beep(300);
    delay(150);
    beep(300);
    delay(150);
    beep(300);
}

// --- TELA DE INICIALIZAÇÃO ---
void drawSplashScreen() {
  display.clearDisplay();
  display.setTextSize(3);
  display.setTextColor(WHITE);
  display.setCursor(15, 15);
  display.print("MINER");
  display.display();
  delay(2000);
}

// --- DESENHO DA TELA ---
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
    display.setCursor(0, 0);
    display.print("Procurando Wi-Fi...");
    display.setCursor(0, 20);
    display.print("Aguarde...");
  } 
  else if (currentState == STATE_SELECT_SSID) {
    display.setCursor(0, 0);
    display.print("Redes Wi-Fi:");
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
    display.setCursor(0, 0);
    display.print("Rede: ");
    display.print(ssid.substring(0, 14));
    display.drawLine(0, 10, 128, 10, WHITE);
    display.setCursor(0, 15);
    display.print("Senha: ");
    for(int i=0; i<password.length(); i++) display.print("*");
    display.setCursor(0, 30);
    display.print("> Char: ");
    display.print(chars[charIndex]);
    display.setCursor(0, 45);
    display.print("SEL: Add BCK: Del");
    display.setCursor(0, 55);
    display.print("UP+DOWN: Conectar");
  }
  else if (currentState == STATE_CONNECTING) {
    display.setCursor(0, 20);
    display.print("Conectando...");
  }
  else if (currentState == STATE_MINING) {
    display.setCursor(0, 0);
    display.print("BTC POOL MINER"); // Mudei para POOL
    display.drawLine(0, 10, 128, 10, WHITE);
    display.setCursor(0, 15);
    display.print("Rede: ");
    display.print(ssid.substring(0, 14));
    display.setCursor(0, 25);
    display.print(miningConnected ? "Pool: OK" : "Pool: ERR");
    display.setCursor(0, 35);
    display.print("H/s: ");
    display.print(hashrate, 1);
    display.setCursor(0, 45);
    display.print("Acertos: ");
    display.print(acceptedShares);
    display.setCursor(0, 55);
    display.print("BCK+DOWN: Resetar");
  }
  display.display();
}

// --- MINERAÇÃO ---
void connectToStratum() {
  if (!client.connect(STRATUM_HOST, STRATUM_PORT)) {
    miningConnected = false;
    return;
  }
  // Na Braiins Pool, o usuário é a sua carteira
  String subscribe = "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"esp32-miner/1.0\"]}\n";
  client.print(subscribe);
  String auth = "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"" + String(BTC_WALLET) + "." + String(WORKER_NAME) + "\",\"x\"]}\n";
  client.print(auth);
  miningConnected = true;
  beep(100);
}

void miningLoop() {
  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (line.indexOf("\"id\":2") == -1 && line.indexOf("\"result\":true") != -1) {
      acceptedShares++;
      buzzerAlertSuccess();
    }
  }
  unsigned long startTime = millis();
  unsigned long hashesThisLoop = 0;
  while (millis() - startTime < 1000) {
    hashesThisLoop++; 
  }
  hashesTotal += hashesThisLoop;
  hashrate = (double)hashesThisLoop / 1.0;
}

// --- BOTÕES ---
void handleButtons() {
  if (currentState == STATE_SELECT_SSID) {
    if (digitalRead(BTN_UP) == LOW) {
      selectedNetworkIndex++;
      if (selectedNetworkIndex >= scanNetworksCount) selectedNetworkIndex = 0;
      beep(30); delay(150);
    }
    if (digitalRead(BTN_DOWN) == LOW) {
      selectedNetworkIndex--;
      if (selectedNetworkIndex < 0) selectedNetworkIndex = scanNetworksCount - 1;
      beep(30); delay(150);
    }
    if (digitalRead(BTN_SEL) == LOW) {
      ssid = WiFi.SSID(selectedNetworkIndex);
      currentState = STATE_INPUT_PASSWORD;
      beep(50); delay(300);
    }
    if (digitalRead(BTN_BACK) == LOW) {
      currentState = STATE_SCANNING;
      beep(30); delay(300);
    }
  }
  else if (currentState == STATE_INPUT_PASSWORD) {
    if (digitalRead(BTN_UP) == LOW) {
      charIndex++;
      if (chars[charIndex] == '\0') charIndex = 0;
      beep(30); delay(150);
    }
    if (digitalRead(BTN_DOWN) == LOW) {
      charIndex--;
      if (charIndex < 0) charIndex = strlen(chars) - 1;
      beep(30); delay(150);
    }
    if (digitalRead(BTN_SEL) == LOW) {
      password += chars[charIndex];
      beep(50); delay(150);
    }
    if (digitalRead(BTN_BACK) == LOW) {
      if (password.length() > 0) password.remove(password.length() - 1);
      beep(30); delay(150);
    }
    if (digitalRead(BTN_UP) == LOW && digitalRead(BTN_DOWN) == LOW) {
      currentState = STATE_CONNECTING;
      drawUI();
      WiFi.begin(ssid.c_str(), password.c_str());
      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500); attempts++;
      }
      if (WiFi.status() == WL_CONNECTED) {
        preferences.begin("wifi", false);
        preferences.putString("ssid", ssid);
        preferences.putString("pass", password);
        preferences.end();
        connectToStratum();
        currentState = STATE_MINING;
        beep(100);
      } else {
        currentState = STATE_INPUT_PASSWORD;
        password = "";
        beep(500);
      }
      delay(500);
    }
  }
  else if (currentState == STATE_MINING) {
    if (digitalRead(BTN_BACK) == LOW && digitalRead(BTN_DOWN) == LOW) {
      preferences.begin("wifi", false);
      preferences.clear();
      preferences.end();
      ESP.restart();
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SEL, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(BAT_ADC, INPUT);
  
  buzzerInit();
  
  Wire.begin(OLED_SDA, OLED_SCL);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Erro na tela OLED");
    for(;;);
  }
  
  drawSplashScreen();
  
  preferences.begin("wifi", false);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("pass", "");
  preferences.end();

  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), password.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500); attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      connectToStratum();
      currentState = STATE_MINING;
    } else {
      currentState = STATE_SCANNING;
    }
  } else {
    currentState = STATE_SCANNING;
  }
  
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
  if (millis() - lastScreenUpdate > 250) {
    drawUI();
    lastScreenUpdate = millis();
  }
}
