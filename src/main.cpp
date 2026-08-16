// ============================================================================
// ESP32 BTC Miner v2.4 — Complete port from NerdMiner_v2
// ============================================================================
// FIX v2.3: Stack overflow resolvido — mining roda em FreeRTOS task com 16KB
// FIX v2.4: Difficulty negotiation corrigida — usa pool's set_difficulty como
//   threshold de submissao (igual NerdMiner_v2). DEFAULT_DIFFICULTY=0.00015.
//   Envia mining.suggest_difficulty(0.00015) pro pool aceitar shares leves.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <stdio.h>

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
const char* STRATUM_HOST = "pool.nerdminer.io";
const int STRATUM_PORT = 3333;
const char* WORKER_ID = "1FRpCfmiwAGVkCLt2FjVuuoAjhSaE2j4QN";
const char* WORKER_PASS = "x";

// ============================================================================
// PORTADO DE: src/mining.h  +  src/stratum.h  +  src/version.h
// ============================================================================

#define TARGET_BUFFER_SIZE 64
#define MAX_MERKLE_BRANCHES 32
#define BUFFER_JSON_DOC 6144
#define BUFFER 512
#define DEFAULT_DIFFICULTY  0.00015
#define KEEPALIVE_TIME_ms       30000
#define POOLINACTIVITY_TIME_ms  60000
#define CURRENT_VERSION "V1.8.3"

typedef struct{
  String sub_details;
  String extranonce1;
  String extranonce2;
  int extranonce2_size;
  char wName[100];
  char wPass[100];
} mining_subscribe;

typedef struct{
  String job_id;
  String prev_block_hash;
  String coinb1;
  String coinb2;
  JsonArray merkle_branch;
  String version;
  String nbits;
  String ntime;
  bool clean_jobs;
} mining_job;

typedef struct{
  uint8_t bytearray_target[32];
  uint8_t bytearray_blockheader[80];
  uint8_t merkle_result[32];
} miner_data;

typedef enum {
  STRATUM_PARSE_ERROR,
  STRATUM_SUCCESS,
  STRATUM_UNKNOWN,
  MINING_NOTIFY,
  MINING_SET_DIFFICULTY
} stratum_method;

// ============================================================================
// GLOBAL (evita stack overflow)
// ============================================================================

StaticJsonDocument<BUFFER_JSON_DOC> g_doc;
unsigned long g_stratum_id = 0;

#define MAX_SUBMIT_IDS 64
unsigned long submitIdList[MAX_SUBMIT_IDS];
int submitIdCount = 0;
unsigned long sharesSubmitted = 0;
static char s_payload[512];

// --- Funcoes utils ---
unsigned long getNextId(unsigned long id) {
  if (id == ULONG_MAX) { id = 1; return id; }
  return ++id;
}

bool verifyPayload(String* line) {
  if (line->length() == 0) return false;
  line->trim();
  if (line->isEmpty()) return false;
  return true;
}

bool checkError(const StaticJsonDocument<BUFFER_JSON_DOC> doc) {
  if (!doc.containsKey("error")) return false;
  if (doc["error"].size() == 0) return false;
  Serial.printf("ERROR: %d | reason: %s \n", (const int) doc["error"][0], (const char*) doc["error"][1]);
  return true;
}

void trackSubmitId(unsigned long id) {
  if (submitIdCount < MAX_SUBMIT_IDS) {
    submitIdList[submitIdCount++] = id;
  } else {
    for (int i = 0; i < MAX_SUBMIT_IDS - 1; i++) submitIdList[i] = submitIdList[i + 1];
    submitIdList[MAX_SUBMIT_IDS - 1] = id;
  }
}

bool isRealSubmitId(unsigned long id) {
  for (int i = 0; i < submitIdCount; i++) {
    if (submitIdList[i] == id) return true;
  }
  return false;
}

uint8_t hex(char ch) {
  uint8_t r = (ch > 57) ? (ch - 55) : (ch - 48);
  return r & 0x0F;
}

int to_byte_array(const char *in, size_t in_size, uint8_t *out) {
  int count = 0;
  if (in_size % 2) {
    while (*in && out) {
      *out = hex(*in++);
      if (!*in) return count;
      *out = (*out << 4) | hex(*in++);
      out++; count++;
    }
    return count;
  } else {
    while (*in && out) {
      *out++ = (hex(*in++) << 4) | hex(*in++);
      count++;
    }
    return count;
  }
}

void swap_endian_words(const char * hex_words, uint8_t * output) {
  size_t hex_length = strlen(hex_words);
  size_t binary_length = hex_length / 2;
  for (size_t i = 0; i < binary_length; i += 4) {
    for (int j = 0; j < 4; j++) {
      unsigned int byte_val;
      sscanf(hex_words + (i + j) * 2, "%2x", &byte_val);
      output[i + (3 - j)] = byte_val;
    }
  }
}

static const double truediffone = 26959535291011309493156476344723991336010898738574164086137773096960.0;

double le256todouble(const void *target) {
  const uint64_t *data64;
  double dcut64;
  data64 = (const uint64_t *)((const uint8_t*)target + 24);
  dcut64 = *data64 * 6277101735386680763835789423207666416102355444464034512896.0;
  data64 = (const uint64_t *)((const uint8_t*)target + 16);
  dcut64 += *data64 * 340282366920938463463374607431768211456.0;
  data64 = (const uint64_t *)((const uint8_t*)target + 8);
  dcut64 += *data64 * 18446744073709551616.0;
  data64 = (const uint64_t *)(target);
  dcut64 += *data64;
  return dcut64;
}

double diff_from_target(void *target) {
  double d64, dcut64;
  d64 = truediffone;
  dcut64 = le256todouble(target);
  if (!dcut64) dcut64 = 1;
  return d64 / dcut64;
}

// ============================================================================
// STRATUM
// ============================================================================

mining_subscribe init_mining_subscribe(void) {
  mining_subscribe new_mSub;
  new_mSub.extranonce1 = "";
  new_mSub.extranonce2 = "";
  new_mSub.extranonce2_size = 0;
  new_mSub.sub_details = "";
  return new_mSub;
}

bool tx_mining_subscribe(WiFiClient& client, mining_subscribe& mSubscribe) {
  g_stratum_id = 1;
  sprintf(s_payload, "{\"id\": %u, \"method\": \"mining.subscribe\", \"params\": [\"NerdMinerV2/%s\"]}\n", g_stratum_id, CURRENT_VERSION);
  Serial.printf("[WORKER] ==> Mining subscribe\n");
  Serial.print("  Sending  : "); Serial.println(s_payload);
  client.print(s_payload);
  delay(200);
  String line = client.readStringUntil('\n');
  if (line.length() == 0) return false;
  line.trim();
  Serial.print("  Receiving: "); Serial.println(line);
  DeserializationError error = deserializeJson(g_doc, line);
  if (error) { Serial.print("JSON err: "); Serial.println(error.c_str()); return false; }
  if (checkError(g_doc)) return false;
  if (!g_doc.containsKey("result")) return false;
  mSubscribe.sub_details = String((const char*) g_doc["result"][0][0][1]);
  mSubscribe.extranonce1 = String((const char*) g_doc["result"][1]);
  mSubscribe.extranonce2_size = g_doc["result"][2];
  Serial.print("    sub_details: "); Serial.println(mSubscribe.sub_details);
  Serial.print("    extranonce1: "); Serial.println(mSubscribe.extranonce1);
  Serial.print("    extranonce2_size: "); Serial.println(mSubscribe.extranonce2_size);
  if (mSubscribe.extranonce1.length() == 0) {
    Serial.println("[WORKER] >>>>>>>>> Work aborted - no extranonce1");
    g_doc.clear(); g_doc.garbageCollect();
    return false;
  }
  g_doc.clear(); g_doc.garbageCollect();
  return true;
}

bool tx_mining_auth(WiFiClient& client, const char * user, const char * pass) {
  g_stratum_id = getNextId(g_stratum_id);
  sprintf(s_payload, "{\"params\": [\"%s\", \"%s\"], \"id\": %u, \"method\": \"mining.authorize\"}\n",
    user, pass, g_stratum_id);
  Serial.printf("[WORKER] ==> Authorize work\n");
  Serial.print("  Sending  : "); Serial.println(s_payload);
  client.print(s_payload);
  delay(200);
  g_doc.clear(); g_doc.garbageCollect();
  return true;
}

stratum_method parse_mining_method(String line) {
  if (line.length() == 0) return STRATUM_PARSE_ERROR;
  line.trim();
  if (line.isEmpty()) return STRATUM_PARSE_ERROR;
  Serial.print("  Receiving: "); Serial.println(line);
  DeserializationError error = deserializeJson(g_doc, line);
  if (error) { Serial.print("JSON err: "); Serial.println(error.c_str()); return STRATUM_PARSE_ERROR; }
  if (checkError(g_doc)) return STRATUM_PARSE_ERROR;
  if (!g_doc.containsKey("method")) {
    return g_doc["error"].isNull() ? STRATUM_SUCCESS : STRATUM_UNKNOWN;
  }
  if (strcmp("mining.notify", (const char*) g_doc["method"]) == 0) return MINING_NOTIFY;
  if (strcmp("mining.set_difficulty", (const char*) g_doc["method"]) == 0) return MINING_SET_DIFFICULTY;
  return STRATUM_UNKNOWN;
}

bool parse_mining_notify(String line, mining_job& mJob) {
  Serial.println("    Parsing Method [MINING NOTIFY]");
  if (line.length() == 0) return false;
  line.trim();
  DeserializationError error = deserializeJson(g_doc, line);
  if (error) { Serial.print("JSON err: "); Serial.println(error.c_str()); return false; }
  if (!g_doc.containsKey("params")) return false;
  mJob.job_id = String((const char*) g_doc["params"][0]);
  mJob.prev_block_hash = String((const char*) g_doc["params"][1]);
  mJob.coinb1 = String((const char*) g_doc["params"][2]);
  mJob.coinb2 = String((const char*) g_doc["params"][3]);
  mJob.merkle_branch = g_doc["params"][4];
  mJob.version = String((const char*) g_doc["params"][5]);
  mJob.nbits = String((const char*) g_doc["params"][6]);
  mJob.ntime = String((const char*) g_doc["params"][7]);
  mJob.clean_jobs = g_doc["params"][8];
  g_doc.clear(); g_doc.garbageCollect();
  return true;
}

bool parse_mining_set_difficulty(String line, double& difficulty) {
  Serial.println("    Parsing Method [SET DIFFICULTY]");
  if (line.length() == 0) return false;
  line.trim();
  DeserializationError error = deserializeJson(g_doc, line);
  if (error) return false;
  if (!g_doc.containsKey("params")) return false;
  Serial.print("    difficulty: "); Serial.println((double)g_doc["params"][0], 12);
  difficulty = (double)g_doc["params"][0];
  g_doc.clear(); g_doc.garbageCollect();
  return true;
}

bool tx_mining_submit(WiFiClient& client, mining_subscribe& mWorker, mining_job& mJob, unsigned long nonce, unsigned long &submit_id) {
  char nonceHex[9];
  sprintf(nonceHex, "%08x", nonce);
  g_stratum_id = getNextId(g_stratum_id);
  submit_id = g_stratum_id;
  sprintf(s_payload, "{\"id\":%u,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}\n",
    g_stratum_id, mWorker.wName, mJob.job_id.c_str(),
    mWorker.extranonce2.c_str(), mJob.ntime.c_str(), nonceHex);
  Serial.print("  Sending  : "); Serial.print(s_payload);
  return client.print(s_payload);
}

bool tx_suggest_difficulty(WiFiClient& client, double difficulty) {
  g_stratum_id = getNextId(g_stratum_id);
  sprintf(s_payload, "{\"id\":%u,\"method\":\"mining.suggest_difficulty\",\"params\":[%.10g]}\n", g_stratum_id, difficulty);
  Serial.print("  Sending  : "); Serial.print(s_payload);
  return client.print(s_payload);
}

unsigned long parse_extract_id(const String &line) {
  if (line.length() == 0) return 0;
  DeserializationError error = deserializeJson(g_doc, line);
  if (error) return 0;
  if (!g_doc.containsKey("id")) return 0;
  unsigned long id = (unsigned long)g_doc["id"];
  g_doc.clear(); g_doc.garbageCollect();
  return id;
}

// ============================================================================
// SHA256
// ============================================================================

void doubleSHA256(const uint8_t* data, size_t len, uint8_t* out) {
  uint8_t hash1[32];
  mbedtls_sha256(data, len, hash1, 0);
  mbedtls_sha256(hash1, 32, out, 0);
}

// ============================================================================
// VARIÁVEIS GLOBAIS (acessadas por UI e mining task)
// ============================================================================

String ssid = "";
String password = "";
String poolStatus = "Desconectado";

WiFiClient client;
volatile unsigned long hashesTotal = 0;
volatile int acceptedShares = 0;
volatile int rejectedShares = 0;
volatile double hashrate = 0.0;
volatile double bestDiff = 0.0;
volatile unsigned long localShares = 0;

char chars[] = " abcdefghijklmnopqrstuvwxyz0123456789@!#$%&*";
int charIndex = 0;
int scanNetworksCount = 0;
int selectedNetworkIndex = 0;

// Mining state
mining_subscribe mWorker;
mining_job mJob;
miner_data mMiner;
double currentPoolDifficulty = DEFAULT_DIFFICULTY;
bool isSubscribed = false;
bool isAuthorized = false;
bool hasJob = false;
unsigned long lastPoolDataTime = 0;
unsigned long lastJobTime = 0;
bool firstHeaderLog = true;
bool miningActive = false;

// UI state
enum UI_State { STATE_MENU, STATE_WIFI_SCAN, STATE_WIFI_PASS, STATE_MINING };
int currentState = STATE_MENU;

// Buzzer
void beep(int ms) { digitalWrite(BUZZER, HIGH); delay(ms); digitalWrite(BUZZER, LOW); }
void buzzerSuccess() { beep(80); delay(80); beep(80); delay(80); beep(200); }

// ============================================================================
// calculateMiningData — portado de NerdMiner_v2 utils.cpp
// ============================================================================

miner_data calculateMiningData(mining_subscribe& mWorker, mining_job& mJob) {
  miner_data mMiner;

  // target
  static char s_target_buf[TARGET_BUFFER_SIZE + 1];
  memset(s_target_buf, '0', TARGET_BUFFER_SIZE);
  int zeros = (int) strtol(mJob.nbits.substring(0, 2).c_str(), 0, 16) - 3;
  memcpy(s_target_buf + zeros - 2, mJob.nbits.substring(2).c_str(), mJob.nbits.length() - 2);
  s_target_buf[TARGET_BUFFER_SIZE] = 0;
  Serial.print("    target: "); Serial.println(s_target_buf);

  size_t size_target = to_byte_array(s_target_buf, 32, mMiner.bytearray_target);
  for (size_t j = 0; j < 8; j++) {
    mMiner.bytearray_target[j] ^= mMiner.bytearray_target[size_target - 1 - j];
    mMiner.bytearray_target[size_target - 1 - j] ^= mMiner.bytearray_target[j];
    mMiner.bytearray_target[j] ^= mMiner.bytearray_target[size_target - 1 - j];
  }

  // extranonce2
  if (mWorker.extranonce2_size == 2)
    mWorker.extranonce2 = "0001";
  else if (mWorker.extranonce2_size == 4)
    mWorker.extranonce2 = "00000001";
  else if (mWorker.extranonce2_size == 8)
    mWorker.extranonce2 = "0000000000000001";
  else {
    Serial.println("Unknown extranonce2");
    mWorker.extranonce2 = "00000001";
  }

  // coinbase
  static char s_coinbase_buf[512];
  snprintf(s_coinbase_buf, sizeof(s_coinbase_buf), "%s%s%s%s",
    mJob.coinb1.c_str(), mWorker.extranonce1.c_str(),
    mWorker.extranonce2.c_str(), mJob.coinb2.c_str());
  Serial.print("    coinbase: "); Serial.println(s_coinbase_buf);
  size_t str_len = strlen(s_coinbase_buf) / 2;
  static uint8_t s_bytearray[256];
  size_t res = to_byte_array(s_coinbase_buf, str_len * 2, s_bytearray);

  // double SHA256 of coinbase
  static uint8_t s_interResult[32];
  static uint8_t s_shaResult[32];
  mbedtls_sha256_context s_ctx;
  mbedtls_sha256_init(&s_ctx);
  mbedtls_sha256_starts_ret(&s_ctx, 0);
  mbedtls_sha256_update_ret(&s_ctx, s_bytearray, str_len);
  mbedtls_sha256_finish_ret(&s_ctx, s_interResult);
  mbedtls_sha256_starts_ret(&s_ctx, 0);
  mbedtls_sha256_update_ret(&s_ctx, s_interResult, 32);
  mbedtls_sha256_finish_ret(&s_ctx, s_shaResult);
  mbedtls_sha256_free(&s_ctx);

  memcpy(mMiner.merkle_result, s_shaResult, 32);

  // merkle tree
  static uint8_t s_merkle_concat[64];
  for (size_t k = 0; k < mJob.merkle_branch.size(); k++) {
    const char* merkle_element = (const char*) mJob.merkle_branch[k];
    static uint8_t s_bytearray_m[32];
    size_t res2 = to_byte_array(merkle_element, 64, s_bytearray_m);
    for (size_t i = 0; i < 32; i++) {
      s_merkle_concat[i] = mMiner.merkle_result[i];
      s_merkle_concat[32 + i] = s_bytearray_m[i];
    }
    mbedtls_sha256_starts_ret(&s_ctx, 0);
    mbedtls_sha256_update_ret(&s_ctx, s_merkle_concat, 64);
    mbedtls_sha256_finish_ret(&s_ctx, s_interResult);
    mbedtls_sha256_starts_ret(&s_ctx, 0);
    mbedtls_sha256_update_ret(&s_ctx, s_interResult, 32);
    mbedtls_sha256_finish_ret(&s_ctx, mMiner.merkle_result);
  }

  // merkle root hex
  static char s_merkle_root[65];
  for (int i = 0; i < 32; i++) {
    Serial.printf("%02x", mMiner.merkle_result[i]);
    snprintf(&s_merkle_root[i * 2], 3, "%02x", mMiner.merkle_result[i]);
  }
  s_merkle_root[65] = 0;
  Serial.println("");

  // blockheader
  String blockheader = mJob.version + mJob.prev_block_hash + String(s_merkle_root) + mJob.ntime + mJob.nbits + "00000000";
  str_len = blockheader.length() / 2;
  res = to_byte_array(blockheader.c_str(), str_len * 2, mMiner.bytearray_blockheader);

  // reverse version (byte-reverse 4 bytes)
  uint8_t buff;
  size_t boffset = 0, bsize = 4;
  for (size_t j = boffset; j < boffset + (bsize / 2); j++) {
    buff = mMiner.bytearray_blockheader[j];
    mMiner.bytearray_blockheader[j] = mMiner.bytearray_blockheader[2 * boffset + bsize - 1 - j];
    mMiner.bytearray_blockheader[2 * boffset + bsize - 1 - j] = buff;
  }

  // reverse prev hash (4-byte word swap)
  boffset = 4; bsize = 32; size_t bword = 4;
  for (size_t i = 1; i <= bsize / bword; i++) {
    for (size_t j = boffset; j < boffset + bword / 2; j++) {
      buff = mMiner.bytearray_blockheader[j];
      mMiner.bytearray_blockheader[j] = mMiner.bytearray_blockheader[2 * boffset + bword - 1 - j];
      mMiner.bytearray_blockheader[2 * boffset + bword - 1 - j] = buff;
    }
    boffset += bword;
  }

  // merkle root: NOT swapped (commented out in NerdMiner_v2)

  // reverse ntime
  boffset = 68; bsize = 4;
  for (size_t j = boffset; j < boffset + (bsize / 2); j++) {
    buff = mMiner.bytearray_blockheader[j];
    mMiner.bytearray_blockheader[j] = mMiner.bytearray_blockheader[2 * boffset + bsize - 1 - j];
    mMiner.bytearray_blockheader[2 * boffset + bsize - 1 - j] = buff;
  }

  // reverse nbits
  boffset = 72; bsize = 4;
  for (size_t j = boffset; j < boffset + (bsize / 2); j++) {
    buff = mMiner.bytearray_blockheader[j];
    mMiner.bytearray_blockheader[j] = mMiner.bytearray_blockheader[2 * boffset + bsize - 1 - j];
    mMiner.bytearray_blockheader[2 * boffset + bsize - 1 - j] = buff;
  }

  return mMiner;
}

// ============================================================================
// CONEXAO STRATUM
// ============================================================================

void connectToStratum() {
  poolStatus = "Conectando Pool...";
  isSubscribed = false;
  isAuthorized = false;
  hasJob = false;
  client.stop();
  submitIdCount = 0;
  sharesSubmitted = 0;
  currentPoolDifficulty = DEFAULT_DIFFICULTY;
  bestDiff = 0.0;
  localShares = 0;
  lastJobTime = 0;
  lastPoolDataTime = 0;
  firstHeaderLog = true;

  if (!client.connect(STRATUM_HOST, STRATUM_PORT)) {
    poolStatus = "ERRO: Conexao";
    Serial.println("[STRATUM] Connection FAILED");
    return;
  }
  Serial.printf("[STRATUM] Connected to %s:%d\n", STRATUM_HOST, STRATUM_PORT);
  poolStatus = "Subscribing...";
  lastPoolDataTime = millis();

  mWorker = init_mining_subscribe();
  if (!tx_mining_subscribe(client, mWorker)) {
    Serial.println("[STRATUM] Subscribe FAILED");
    poolStatus = "ERRO: Subscribe";
    client.stop();
    return;
  }
  isSubscribed = true;

  strncpy(mWorker.wName, WORKER_ID, sizeof(mWorker.wName) - 1);
  strncpy(mWorker.wPass, WORKER_PASS, sizeof(mWorker.wPass) - 1);
  tx_mining_auth(client, mWorker.wName, mWorker.wPass);

  tx_suggest_difficulty(client, DEFAULT_DIFFICULTY);

  delay(300);
  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (line.length() < 2) continue;
    stratum_method method = parse_mining_method(line);
    if (method == STRATUM_SUCCESS) {
      isAuthorized = true;
      poolStatus = "Autorizado!";
      Serial.println("[STRATUM] Authorized OK!");
    } else if (method == MINING_SET_DIFFICULTY) {
      double poolDiff = 0;
      if (parse_mining_set_difficulty(line, poolDiff)) {
        currentPoolDifficulty = poolDiff;
        Serial.printf("[DIFF] Pool setou diff=%.6f — USANDO como threshold\n", poolDiff);
      }
    } else if (method == MINING_NOTIFY) {
      if (parse_mining_notify(line, mJob)) {
        hasJob = true;
        lastJobTime = millis();
      }
    }
    lastPoolDataTime = millis();
  }
  poolStatus = "Aguardando job...";
  beep(100);
}

// ============================================================================
// MINING LOOP
// ============================================================================

void miningLoop() {
  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (line.length() < 2) continue;
    lastPoolDataTime = millis();

    stratum_method method = parse_mining_method(line);

    if (method == STRATUM_SUCCESS) {
      unsigned long resp_id = parse_extract_id(line);
      if (isRealSubmitId(resp_id)) {
        deserializeJson(g_doc, line);
        Serial.print("[POOL RESP] id="); Serial.print(resp_id); Serial.print(" ");
        serializeJson(g_doc, Serial); Serial.println();
        if (g_doc["result"] == true) {
          acceptedShares++;
          Serial.println("[SHARE] *** ACCEPTED! ***");
          buzzerSuccess();
        } else {
          rejectedShares++;
          Serial.print("[SHARE] REJECTED: ");
          if (g_doc.containsKey("error")) serializeJson(g_doc["error"], Serial);
          Serial.println();
        }
        g_doc.clear(); g_doc.garbageCollect();
      }
    }
    else if (method == MINING_NOTIFY) {
      if (parse_mining_notify(line, mJob)) {
        hasJob = true;
        lastJobTime = millis();
        lastPoolDataTime = millis();
        Serial.println("[JOB] New mining.notify, recalculating...");
        mMiner = calculateMiningData(mWorker, mJob);
        firstHeaderLog = true;
        Serial.printf("[JOB] job=%s branches=%d nbits=%s ntime=%s\n",
          mJob.job_id.c_str(), mJob.merkle_branch.size(),
          mJob.nbits.c_str(), mJob.ntime.c_str());
      } else {
        Serial.println("[JOB] Parse error, reconnecting...");
        client.stop();
        isSubscribed = false; isAuthorized = false; hasJob = false;
        delay(2000);
        connectToStratum();
        return;
      }
    }
    else if (method == MINING_SET_DIFFICULTY) {
      double poolDiff = 0;
      if (parse_mining_set_difficulty(line, poolDiff)) {
        currentPoolDifficulty = poolDiff;
        Serial.printf("[DIFF] Pool setou diff=%.6f — USANDO como threshold\n", poolDiff);
      }
    }
  }

  unsigned long now = millis();

  if (!client.connected()) {
    Serial.println("[STRATUM] Connection lost! Reconnecting...");
    poolStatus = "Reconectando...";
    client.stop();
    isSubscribed = false; isAuthorized = false; hasJob = false;
    delay(3000);
    connectToStratum();
    return;
  }

  if (now - lastPoolDataTime > POOLINACTIVITY_TIME_ms) {
    Serial.println("[STRATUM] Pool timeout. Reconnecting...");
    poolStatus = "Timeout Pool...";
    client.stop();
    isSubscribed = false; isAuthorized = false; hasJob = false;
    delay(2000);
    connectToStratum();
    return;
  }

  if (now - lastPoolDataTime > KEEPALIVE_TIME_ms) {
    Serial.println("  Sending  : KeepAlive suggest_difficulty");
    tx_suggest_difficulty(client, DEFAULT_DIFFICULTY);
    lastPoolDataTime = now;
  }

  if (!hasJob || !isSubscribed) {
    if (!hasJob && isSubscribed) poolStatus = "Aguardando job...";
    return;
  }

  // MINERACAO
  if (firstHeaderLog) {
    firstHeaderLog = false;
    Serial.print("[DEBUG] Header (80b): ");
    for (int i = 0; i < 80; i++) {
      if (mMiner.bytearray_blockheader[i] < 0x10) Serial.print("0");
      Serial.print(mMiner.bytearray_blockheader[i], HEX);
    }
    Serial.println();
  }

  unsigned long loopStart = millis();
  uint32_t nonce = esp_random();
  unsigned int localH = 0;

  while (millis() - loopStart < 50) {
    memcpy(&mMiner.bytearray_blockheader[76], &nonce, 4);

    uint8_t hashBe[32];
    doubleSHA256(mMiner.bytearray_blockheader, 80, hashBe);
    localH++;

    double hashDiff = diff_from_target(hashBe);

    if (hashDiff > bestDiff) bestDiff = hashDiff;

    if (hashDiff >= 0.000005) {
      localShares++;
      if (localShares <= 3 || localShares % 50 == 0) {
        Serial.printf("[LOCAL] diff=%.6f nonce=%08x (best:%.6f)\n",
          hashDiff, nonce, bestDiff);
      }
    }

    if (hashDiff > currentPoolDifficulty) {
      Serial.printf("[FOUND] diff=%.6f nonce=%08x job=%s\n",
        hashDiff, nonce, mJob.job_id.c_str());
      Serial.print("[FOUND] Hash BE: ");
      for (int i = 0; i < 32; i++) {
        if (hashBe[i] < 0x10) Serial.print("0");
        Serial.print(hashBe[i], HEX);
      }
      Serial.println();
      unsigned long submit_id = 0;
      tx_mining_submit(client, mWorker, mJob, nonce, submit_id);
      trackSubmitId(submit_id);
      sharesSubmitted++;
      lastPoolDataTime = millis();
      Serial.printf("[SHARE] Submitted id=%lu nonce=%08x en2=%s\n",
        submit_id, nonce, mWorker.extranonce2.c_str());
    }
    nonce++;
  }

  hashesTotal += localH;

  static unsigned long lastHashTime = 0;
  static unsigned long hashesInWindow = 0;
  hashesInWindow += localH;
  unsigned long now2 = millis();
  if (now2 - lastHashTime >= 1000) {
    double elapsed = (double)(now2 - lastHashTime) / 1000.0;
    hashrate = (double)hashesInWindow / elapsed;
    hashesInWindow = 0;
    lastHashTime = now2;
    static unsigned long lastHRLog = 0;
    if (now2 - lastHRLog >= 5000) {
      Serial.printf("[STATS] H/s=%.0f total=%luk local=%lu best=%.6f pD=%.6f Ok=%d Rj=%d\n",
        hashrate, hashesTotal / 1000, localShares, bestDiff,
        currentPoolDifficulty, acceptedShares, rejectedShares);
      lastHRLog = now2;
    }
  }

  if (hashrate > 100) {
    poolStatus = "Minerando " + String((unsigned long)hashrate) + " H/s";
  }
}

// ============================================================================
// MINING TASK (FreeRTOS) — 16KB stack para evitar stack overflow
// ============================================================================

void miningTaskFunc(void *param) {
  Serial.println("[TASK] Mining task started (16KB stack)");
  for (;;) {
    if ((UI_State)currentState == STATE_MINING && miningActive) {
      if (!isSubscribed || !hasJob) {
        connectToStratum();
      } else {
        miningLoop();
      }
    } else {
      if (isSubscribed) {
        client.stop();
        isSubscribed = false; isAuthorized = false; hasJob = false;
        miningActive = false;
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// ============================================================================
// DISPLAY
// ============================================================================

void drawMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("ESP32 BTC Miner v2.4");
  display.println("");
  display.println("Select:");
  display.println("1. Start Mining");
  display.println("");
  display.print("SSID: ");
  if (ssid.length() > 16) display.println(ssid.substring(0, 16));
  else display.println(ssid);
  display.display();
}

void drawMiningScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("BTC Miner v2.4");
  display.println("----------------------");
  display.printf("H/s: %.0f\n", hashrate);
  display.printf("Best: %.6f\n", bestDiff);
  display.printf("Pool D: %.6f\n", currentPoolDifficulty);
  display.printf("Ok: %d  Rj: %d\n", acceptedShares, rejectedShares);
  display.printf("Total: %lukH\n", hashesTotal / 1000);
  display.display();
}

void drawWifiScan() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Scanning WiFi...");
  display.display();
}

void drawWifiPass() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Enter Password:");
  display.println(ssid);
  display.println("");
  display.print("> ");
  display.println(password);
  display.println("");
  display.println("SEL=OK  BACK=DEL");
  display.display();
}

// ============================================================================
// UI / INPUT
// ============================================================================

void handleButtonPress() {
  if (currentState == STATE_MENU) {
    if (digitalRead(BTN_SEL) == LOW) {
      currentState = STATE_WIFI_SCAN;
      scanNetworksCount = WiFi.scanNetworks();
      delay(300);
    }
  }
  else if (currentState == STATE_WIFI_SCAN) {
    if (digitalRead(BTN_UP) == LOW) {
      selectedNetworkIndex = (selectedNetworkIndex + 1) % scanNetworksCount;
      delay(200);
    }
    if (digitalRead(BTN_DOWN) == LOW) {
      selectedNetworkIndex = (selectedNetworkIndex - 1 + scanNetworksCount) % scanNetworksCount;
      delay(200);
    }
    if (digitalRead(BTN_SEL) == LOW) {
      ssid = WiFi.SSID(selectedNetworkIndex);
      password = "";
      charIndex = 0;
      currentState = STATE_WIFI_PASS;
      delay(300);
    }
    if (digitalRead(BTN_BACK) == LOW) {
      currentState = STATE_MENU;
      delay(300);
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Select WiFi:");
    int startIdx = max(0, selectedNetworkIndex - 3);
    for (int i = startIdx; i < min((int)scanNetworksCount, startIdx + 5); i++) {
      if (i == selectedNetworkIndex) display.print(">");
      display.printf("%d: %s (%d)\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    }
    display.display();
  }
  else if (currentState == STATE_WIFI_PASS) {
    if (digitalRead(BTN_UP) == LOW) {
      charIndex = (charIndex + 1) % (int)strlen(chars);
      delay(150);
    }
    if (digitalRead(BTN_DOWN) == LOW) {
      charIndex = (charIndex - 1 + (int)strlen(chars)) % (int)strlen(chars);
      delay(150);
    }
    if (digitalRead(BTN_SEL) == LOW) {
      password += chars[charIndex];
      charIndex = 0;
      delay(200);
    }
    if (digitalRead(BTN_BACK) == LOW) {
      if (password.length() > 0) {
        password.remove(password.length() - 1);
      } else {
        currentState = STATE_WIFI_SCAN;
      }
      delay(200);
    }
    drawWifiPass();
  }
  else if (currentState == STATE_MINING) {
    if (digitalRead(BTN_BACK) == LOW) {
      miningActive = false;
      client.stop();
      isSubscribed = false; isAuthorized = false; hasJob = false;
      currentState = STATE_MENU;
      delay(500);
    }
  }
}

// ============================================================================
// SETUP / LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SEL, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(BUZZER, OUTPUT);
  pinMode(BAT_ADC, INPUT);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 failed"));
    for (;;);
  }

  xTaskCreateUniversal(miningTaskFunc, "miningTask", 16384, NULL, 1, NULL, 1);

  drawMenu();
  beep(50);
}

void loop() {
  handleButtonPress();

  if (currentState == STATE_MENU) {
    drawMenu();
    delay(200);
  }
  else if (currentState == STATE_WIFI_SCAN) {
    drawWifiScan();
  }
  else if (currentState == STATE_WIFI_PASS) {
    // already drawn in handleButtonPress
  }
  else if (currentState == STATE_MINING) {
    if (!miningActive) {
      miningActive = true;
      acceptedShares = 0;
      rejectedShares = 0;
      hashesTotal = 0;
      bestDiff = 0.0;
      localShares = 0;
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid.c_str(), password.c_str());
      Serial.printf("[WIFI] Connecting to %s\n", ssid.c_str());
      int wifiTries = 0;
      while (WiFi.status() != WL_CONNECTED && wifiTries < 40) {
        delay(500);
        Serial.print(".");
        wifiTries++;
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        poolStatus = "Conectando Pool...";
      } else {
        Serial.println("\n[WIFI] FAILED");
        poolStatus = "WiFi FALHOU";
        miningActive = false;
        currentState = STATE_MENU;
      }
    }
    drawMiningScreen();
    delay(200);
  }
}
