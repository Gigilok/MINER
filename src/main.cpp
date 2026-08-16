// ============================================================================
// ESP32 BTC Miner v2.2 — Complete port from NerdMiner_v2
// ============================================================================
// TODA a lógica de mineração portada integralmente de:
//   https://github.com/BitMaker-hub/NerdMiner_v2
// Arquivos portados para este único .cpp:
//   src/utils.cpp  → calculateMiningData, to_byte_array, hex,
//                     le256todouble, diff_from_target, isSha256Valid,
//                     checkValid, suffix_string, reverse_bytes
//   src/stratum.cpp → tx_mining_subscribe, parse_mining_subscribe,
//                     init_mining_subscribe, tx_mining_auth,
//                     parse_mining_method, parse_mining_notify,
//                     tx_mining_submit, parse_mining_set_difficulty,
//                     tx_suggest_difficulty, parse_extract_id
//   src/mining.h   → miner_data struct, DEFAULT_DIFFICULTY,
//                     KEEPALIVE_TIME_ms, POOLINACTIVITY_TIME_ms,
//                     TARGET_BUFFER_SIZE
//   src/stratum.h   → mining_subscribe, mining_job, stratum_method,
//                     BUFFER_JSON_DOC, BUFFER
//   src/version.h   → CURRENT_VERSION
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
const char* WORKER_ID = "bc1qatm8zewhemwlvpmlenk2rurxkptlt847eh3y79";
const char* WORKER_PASS = "x";

// ============================================================================
// PORTADO DE: src/mining.h  +  src/stratum.h  +  src/version.h
// ============================================================================

#define TARGET_BUFFER_SIZE 64
#define MAX_MERKLE_BRANCHES 32
#define BUFFER_JSON_DOC 4096
#define BUFFER 1024
#define DEFAULT_DIFFICULTY  0.00015
#define KEEPALIVE_TIME_ms       30000
#define POOLINACTIVITY_TIME_ms  60000
#define CURRENT_VERSION "V1.8.3"

typedef struct{
  uint8_t bytearray_target[32];
  uint8_t bytearray_pooltarget[32];
  uint8_t merkle_result[32];
  uint8_t bytearray_blockheader[128];
} miner_data;

typedef struct {
    String sub_details;
    String extranonce1;
    String extranonce2;
    int extranonce2_size;
    char wName[80];
    char wPass[20];
} mining_subscribe;

typedef struct {
    String job_id;
    String prev_block_hash;
    String coinb1;
    String coinb2;
    String nbits;
    JsonArray merkle_branch;
    String version;
    uint32_t target;
    String ntime;
    bool clean_jobs;
} mining_job;

typedef enum {
    STRATUM_SUCCESS,
    STRATUM_UNKNOWN,
    STRATUM_PARSE_ERROR,
    MINING_NOTIFY,
    MINING_SET_DIFFICULTY
} stratum_method;

// ============================================================================
// PORTADO DE: src/utils.cpp  (funções exatas do NerdMiner_v2)
// ============================================================================

// Função hex() EXATA do NerdMiner_v2
uint8_t hex(char ch) {
    uint8_t r = (ch > 57) ? (ch - 55) : (ch - 48);
    return r & 0x0F;
}

// Função to_byte_array() EXATA do NerdMiner_v2
int to_byte_array(const char *in, size_t in_size, uint8_t *out) {
    int count = 0;
    if (in_size % 2) {
        while (*in && out) {
            *out = hex(*in++);
            if (!*in)
                return count;
            *out = (*out << 4) | hex(*in++);
            *out++;
            count++;
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

// Função reverse_bytes() EXATA do NerdMiner_v2
void reverse_bytes(uint8_t * data, size_t len) {
    for (int i = 0; i < len / 2; ++i) {
        uint8_t temp = data[i];
        data[i] = data[len - 1 - i];
        data[len - 1 - i] = temp;
    }
}

// le256todouble() EXATA do NerdMiner_v2
static const double truediffone = 26959535291011309493156476344723991336010898738574164086137773096960.0;

double le256todouble(const void *target) 
{
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

// diff_from_target() EXATA do NerdMiner_v2
// IMPORTANTE: recebe hash em BIG-ENDIAN (ordem natural do SHA256)
double diff_from_target(void *target)
{
        double d64, dcut64;

        d64 = truediffone;
        dcut64 = le256todouble(target);
        if (unlikely(!dcut64))
                dcut64 = 1;
        return d64 / dcut64;
}

// isSha256Valid() EXATA do NerdMiner_v2
bool isSha256Valid(const void* sha256)
{
    for(uint8_t i=0; i < 8; ++i)
    {
        if ( ((const uint32_t*)sha256)[i] != 0 ) 
            return true;
    }
    return false;
}

// checkValid() EXATA do NerdMiner_v2
bool checkValid(unsigned char* hash, unsigned char* target) {
  bool valid = true;
  unsigned char diff_target[32];
  memcpy(diff_target, &target, 32);
  reverse_bytes(diff_target, 32);

  for(uint8_t i=31; i>=0; i--) {
    if(hash[i] > diff_target[i]) {
      valid = false;
      break;
    }
  }
  return valid;
}

// suffix_string() EXATA do NerdMiner_v2 (para display)
void suffix_string(double val, char *buf, size_t bufsiz, int sigdigits)
{
        const double kilo = 1000;
        const double mega = 1000000;
        const double giga = 1000000000;
        const double tera = 1000000000000;
        const double peta = 1000000000000000;
        const double exa  = 1000000000000000000;
        const double min_diff = 0.001;
    const byte maxNdigits = 2;
        char suffix[2] = {0,0};
        bool decimal = true;
        double dval;

        if (val >= exa) {
                val /= peta;
                dval = val / kilo;
        suffix[0] = 'E';
        if (dval > 999.99)
            dval = 999.99;
        } else if (val >= peta) {
                val /= tera;
                dval = val / kilo;
                suffix[0] = 'P';
        } else if (val >= tera) {
                val /= giga;
                dval = val / kilo;
                suffix[0] = 'T';
        } else if (val >= giga) {
                val /= mega;
                dval = val / kilo;
                suffix[0] = 'G';
        } else if (val >= mega) {
                val /= kilo;
                dval = val / kilo;
                suffix[0] = 'M';
        } else if (val >= kilo) {
                dval = val / kilo;
                suffix[0] = 'K';
        } else {
                dval = val;
                if (dval < min_diff)
                        dval = 0.0;
        }
    
    int frac = 3;
    if (suffix[0] != 0)
    {
        if (dval > 99.999)
            frac = 1;
        else if (dval > 9.999)
            frac = 2;
    } else
    {
        if (dval > 99.999)
            frac = 2;
        else if (dval > 9.999)
            frac = 3;
        else
            frac = 4;
    }

        if (!sigdigits) {
                if (decimal)
        {
            if (frac == 4)
                            snprintf(buf, bufsiz, "%.4f%s", dval, suffix);
            else if (frac == 3)
                            snprintf(buf, bufsiz, "%.3f%s", dval, suffix);
            else if (frac == 2)
                            snprintf(buf, bufsiz, "%.2f%s", dval, suffix);
            else
                            snprintf(buf, bufsiz, "%.1f%s", dval, suffix);
        } else
                        snprintf(buf, bufsiz, "%d%s", (unsigned int)dval, suffix);
        } else {
                int ndigits = sigdigits - 1 - (dval > 0.0 ? floor(log10(dval)) : 0);
                snprintf(buf, bufsiz, "%*.*f%s", sigdigits + 1, ndigits, dval, suffix);
        }
}

// ============================================================================
// calculateMiningData() — PORTADA INTEGRALMENTE de NerdMiner_v2 utils.cpp
// Esta é a função mais crítica: constrói o block header de 80 bytes
// ============================================================================

miner_data calculateMiningData(mining_subscribe& mWorker, mining_job mJob)
{
  miner_data mMiner;
  memset(&mMiner, 0, sizeof(mMiner));

  // calculate target - target = (nbits[2:]+'00'*(int(nbits[:2],16) - 3)).zfill(64)
  char target[TARGET_BUFFER_SIZE+1];
  memset(target, '0', TARGET_BUFFER_SIZE);
  int zeros = (int) strtol(mJob.nbits.substring(0, 2).c_str(), 0, 16) - 3;
  memcpy(target + zeros - 2, mJob.nbits.substring(2).c_str(), mJob.nbits.length() - 2);
  target[TARGET_BUFFER_SIZE] = 0;
  Serial.print("    target: "); Serial.println(target);
  
  // bytearray target
  size_t size_target = to_byte_array(target, 32, mMiner.bytearray_target);

  // Reverse first 8 bytes of target (XOR swap, EXATO do NerdMiner_v2)
  for (size_t j = 0; j < 8; j++) {
    mMiner.bytearray_target[j] ^= mMiner.bytearray_target[size_target - 1 - j];
    mMiner.bytearray_target[size_target - 1 - j] ^= mMiner.bytearray_target[j];
    mMiner.bytearray_target[j] ^= mMiner.bytearray_target[size_target - 1 - j];
  }

  // get extranonce2 - EXATO do NerdMiner_v2
  if (mWorker.extranonce2_size == 2)
      mWorker.extranonce2 = "0001";
  else if (mWorker.extranonce2_size == 4)
      mWorker.extranonce2 = "00000001";
  else if (mWorker.extranonce2_size == 8)
      mWorker.extranonce2 = "0000000000000001";
  else
  {
      Serial.println("Unknown extranonce2");
      mWorker.extranonce2 = "00000001";
  }
  
  // get coinbase - EXATO do NerdMiner_v2
  static char coinbase_buffer[512];
  snprintf(coinbase_buffer, sizeof(coinbase_buffer), "%s%s%s%s", 
           mJob.coinb1.c_str(), mWorker.extranonce1.c_str(), 
           mWorker.extranonce2.c_str(), mJob.coinb2.c_str());
  Serial.print("    coinbase: "); Serial.println(coinbase_buffer);
  size_t str_len = strlen(coinbase_buffer)/2;
  uint8_t bytearray[256];
  size_t res = to_byte_array(coinbase_buffer, str_len*2, bytearray);

  // Double SHA256 do coinbase - EXATO do NerdMiner_v2
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  byte interResult[32];
  byte shaResult[32];

  mbedtls_sha256_starts_ret(&ctx,0);
  mbedtls_sha256_update_ret(&ctx, bytearray, str_len);
  mbedtls_sha256_finish_ret(&ctx, interResult);

  mbedtls_sha256_starts_ret(&ctx,0);
  mbedtls_sha256_update_ret(&ctx, interResult, 32);
  mbedtls_sha256_finish_ret(&ctx, shaResult);
  mbedtls_sha256_free(&ctx);

  // copy coinbase hash
  memcpy(mMiner.merkle_result, shaResult, sizeof(shaResult));
  
  // Merkle tree - EXATO do NerdMiner_v2
  byte merkle_concatenated[32 * 2];
  for (size_t k=0; k < mJob.merkle_branch.size(); k++) {
      const char* merkle_element = (const char*) mJob.merkle_branch[k];
      uint8_t bytearray_m[32];
      size_t res_m = to_byte_array(merkle_element, 64, bytearray_m);

      for (size_t i = 0; i < 32; i++) {
        merkle_concatenated[i] = mMiner.merkle_result[i];
        merkle_concatenated[32 + i] = bytearray_m[i];
      }

      mbedtls_sha256_context ctx2;
      mbedtls_sha256_init(&ctx2);
      mbedtls_sha256_starts_ret(&ctx2,0);
      mbedtls_sha256_update_ret(&ctx2, merkle_concatenated, 64);
      mbedtls_sha256_finish_ret(&ctx2, interResult);

      mbedtls_sha256_starts_ret(&ctx2,0);
      mbedtls_sha256_update_ret(&ctx2, interResult, 32);
      mbedtls_sha256_finish_ret(&ctx2, mMiner.merkle_result);
      mbedtls_sha256_free(&ctx2);
  }
  
  // merkle root
  Serial.print("    merkle sha         : ");
  char merkle_root[65];
  for (int i = 0; i < 32; i++) {
    Serial.printf("%02x", mMiner.merkle_result[i]);
    snprintf(&merkle_root[i*2], 3, "%02x", mMiner.merkle_result[i]);
  }
  merkle_root[65] = 0;
  Serial.println("");

  // calculate blockheader - EXATO do NerdMiner_v2
  // j.block_header = ''.join([j.version, j.prevhash, merkle_root, j.ntime, j.nbits])
  String blockheader = mJob.version + mJob.prev_block_hash + String(merkle_root) + mJob.ntime + mJob.nbits + "00000000";
  str_len = blockheader.length()/2;
  
  res = to_byte_array(blockheader.c_str(), str_len*2, mMiner.bytearray_blockheader);

  // === ENDIAN SWAPS — EXATOS do NerdMiner_v2 calculateMiningData ===
  uint8_t buff;
  size_t bword, bsize, boffset;

  // reverse version (4 bytes, word-swap)
  boffset = 0;
  bsize = 4;
  for (size_t j = boffset; j < boffset + (bsize/2); j++) {
      buff = mMiner.bytearray_blockheader[j];
      mMiner.bytearray_blockheader[j] = mMiner.bytearray_blockheader[2 * boffset + bsize - 1 - j];
      mMiner.bytearray_blockheader[2 * boffset + bsize - 1 - j] = buff;
  }

  // reverse prev hash (4-byte word swap) — EXATO do NerdMiner_v2
  boffset = 4;
  bword = 4;
  bsize = 32;
  for (size_t i = 1; i <= bsize / bword; i++) {
      for (size_t j = boffset; j < boffset + bword / 2; j++) {
          buff = mMiner.bytearray_blockheader[j];
          mMiner.bytearray_blockheader[j] = mMiner.bytearray_blockheader[2 * boffset + bword - 1 - j];
          mMiner.bytearray_blockheader[2 * boffset + bword - 1 - j] = buff;
      }
      boffset += bword;
  }

  // merkle root: NÃO INVERTE! (comentado no NerdMiner_v2)

  // reverse ntime (4 bytes, word-swap)
  boffset = 68;
  bsize = 4;
  for (size_t j = boffset; j < boffset + (bsize/2); j++) {
      buff = mMiner.bytearray_blockheader[j];
      mMiner.bytearray_blockheader[j] = mMiner.bytearray_blockheader[2 * boffset + bsize - 1 - j];
      mMiner.bytearray_blockheader[2 * boffset + bsize - 1 - j] = buff;
  }

  // reverse difficulty/nbits (4 bytes, word-swap)
  boffset = 72;
  bsize = 4;
  for (size_t j = boffset; j < boffset + (bsize/2); j++) {
      buff = mMiner.bytearray_blockheader[j];
      mMiner.bytearray_blockheader[j] = mMiner.bytearray_blockheader[2 * boffset + bsize - 1 - j];
      mMiner.bytearray_blockheader[2 * boffset + bsize - 1 - j] = buff;
  }

  // Debug: dump blockheader
  Serial.print("    blockheader hex: "); 
  for (size_t i = 0; i < 80; i++)
      Serial.printf("%02x", mMiner.bytearray_blockheader[i]);
  Serial.println("");

  return mMiner;
}

// ============================================================================
// PORTADO DE: src/stratum.cpp  (funções EXATAS do NerdMiner_v2)
// ============================================================================

StaticJsonDocument<BUFFER_JSON_DOC> g_doc;  // Global para evitar stack overflow
unsigned long g_stratum_id = 1;  // ID global para mensagens Stratum

// Forward declarations
unsigned long getNextId(unsigned long id);
bool verifyPayload (String* line);
bool checkError(const StaticJsonDocument<BUFFER_JSON_DOC> doc);
mining_subscribe init_mining_subscribe(void);
bool tx_mining_subscribe(WiFiClient& client, mining_subscribe& mSubscribe);
bool parse_mining_subscribe(String line, mining_subscribe& mSubscribe);
bool tx_mining_auth(WiFiClient& client, const char * user, const char * pass);
stratum_method parse_mining_method(String line);
bool parse_mining_notify(String line, mining_job& mJob);
bool tx_mining_submit(WiFiClient& client, mining_subscribe mWorker, mining_job mJob, unsigned long nonce, unsigned long &submit_id);
bool parse_mining_set_difficulty(String line, double& difficulty);
bool tx_suggest_difficulty(WiFiClient& client, double difficulty);
unsigned long parse_extract_id(const String &line);

unsigned long getNextId(unsigned long id) {
    if (id == ULONG_MAX) {
      id = 1;
      return id;
    }
    return ++id;
}

bool verifyPayload (String* line){
  if(line->length() == 0) return false;
  line->trim();
  if(line->isEmpty()) return false;
  return true;
}

bool checkError(const StaticJsonDocument<BUFFER_JSON_DOC> doc) {
  if (!doc.containsKey("error")) return false;
  if (doc["error"].size() == 0) return false;
  Serial.printf("ERROR: %d | reason: %s \n", (const int) doc["error"][0], (const char*) doc["error"][1]);
  return true;  
}

mining_subscribe init_mining_subscribe(void)
{
    mining_subscribe new_mSub;
    new_mSub.extranonce1 = "";
    new_mSub.extranonce2 = "";
    new_mSub.extranonce2_size = 0;
    new_mSub.sub_details = "";
    return new_mSub;
}

// STEP 1: mining.subscribe — EXATO do NerdMiner_v2
bool tx_mining_subscribe(WiFiClient& client, mining_subscribe& mSubscribe)
{
    char payload[BUFFER] = {0};
    g_stratum_id = 1;
    sprintf(payload, "{\"id\": %u, \"method\": \"mining.subscribe\", \"params\": [\"NerdMinerV2/%s\"]}\n", g_stratum_id, CURRENT_VERSION);
    
    Serial.printf("[WORKER] ==> Mining subscribe\n");
    Serial.print("  Sending  : "); Serial.println(payload);
    client.print(payload);
    
    delay(200);
    
    String line = client.readStringUntil('\n');
    if(!parse_mining_subscribe(line, mSubscribe)) return false;

    Serial.print("    sub_details: "); Serial.println(mSubscribe.sub_details);
    Serial.print("    extranonce1: "); Serial.println(mSubscribe.extranonce1);
    Serial.print("    extranonce2_size: "); Serial.println(mSubscribe.extranonce2_size);

    if((mSubscribe.extranonce1.length() == 0) ) { 
        Serial.printf("[WORKER] >>>>>>>>> Work aborted\n"); 
        Serial.printf("extranonce1 length: %u \n", mSubscribe.extranonce1.length());
        g_doc.clear();
        g_doc.garbageCollect();
        return false; 
    }
    return true;
}

bool parse_mining_subscribe(String line, mining_subscribe& mSubscribe)
{
    if(!verifyPayload(&line)) return false;
    Serial.print("  Receiving: "); Serial.println(line);
   
    DeserializationError error = deserializeJson(g_doc, line);
    if (error || checkError(g_doc)) return false;
    if (!g_doc.containsKey("result")) return false;

    mSubscribe.sub_details = String((const char*) g_doc["result"][0][0][1]);
    mSubscribe.extranonce1 = String((const char*) g_doc["result"][1]);
    mSubscribe.extranonce2_size = g_doc["result"][2];
    return true;
}

// STEP 2: mining.authorize — EXATO do NerdMiner_v2
bool tx_mining_auth(WiFiClient& client, const char * user, const char * pass)
{
    char payload[BUFFER] = {0};
    g_stratum_id = getNextId(g_stratum_id);
    sprintf(payload, "{\"params\": [\"%s\", \"%s\"], \"id\": %u, \"method\": \"mining.authorize\"}\n", 
      user, pass, g_stratum_id);
    
    Serial.printf("[WORKER] ==> Authorize work\n");
    Serial.print("  Sending  : "); Serial.println(payload);
    client.print(payload);
    delay(200);
    return true;
}

// parse_mining_method — EXATO do NerdMiner_v2
stratum_method parse_mining_method(String line)
{
    if(!verifyPayload(&line)) return STRATUM_PARSE_ERROR;
    Serial.print("  Receiving: "); Serial.println(line);
    
    DeserializationError error = deserializeJson(g_doc, line);
    if (error || checkError(g_doc)) return STRATUM_PARSE_ERROR;

    if (!g_doc.containsKey("method")) {
        if (g_doc["error"].isNull())
            return STRATUM_SUCCESS;
        else
            return STRATUM_UNKNOWN;
    }
    stratum_method result = STRATUM_UNKNOWN;
    if (strcmp("mining.notify", (const char*) g_doc["method"]) == 0)
        result = MINING_NOTIFY;
    else if (strcmp("mining.set_difficulty", (const char*) g_doc["method"]) == 0)
        result = MINING_SET_DIFFICULTY;
    return result;
}

// parse_mining_notify — EXATO do NerdMiner_v2
bool parse_mining_notify(String line, mining_job& mJob)
{
    Serial.println("    Parsing Method [MINING NOTIFY]");
    if(!verifyPayload(&line)) return false;
   
    DeserializationError error = deserializeJson(g_doc, line);
    if (error) return false;
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

    if (checkError(g_doc)) {
      Serial.printf("[WORKER] >>>>>>>>> Work aborted\n"); 
      return false;
    }
    return true;
}

// STEP 3: mining.submit — EXATO do NerdMiner_v2
bool tx_mining_submit(WiFiClient& client, mining_subscribe mWorker, mining_job mJob, unsigned long nonce, unsigned long &submit_id)
{
    char payload[BUFFER] = {0};
    g_stratum_id = getNextId(g_stratum_id);
    submit_id = g_stratum_id;
    sprintf(payload, "{\"id\":%u,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}\n",
        g_stratum_id,
        mWorker.wName,
        mJob.job_id.c_str(),
        mWorker.extranonce2.c_str(),
        mJob.ntime.c_str(),
        String(nonce, HEX).c_str()
        );
    Serial.print("  Sending  : "); Serial.print(payload);
    client.print(payload);
    return true;
}

// parse_mining_set_difficulty — EXATO do NerdMiner_v2
bool parse_mining_set_difficulty(String line, double& difficulty)
{
    Serial.println("    Parsing Method [SET DIFFICULTY]");
    if(!verifyPayload(&line)) return false;
   
    DeserializationError error = deserializeJson(g_doc, line);
    if (error) return false;
    if (!g_doc.containsKey("params")) return false;

    Serial.print("    difficulty: "); Serial.println((double)g_doc["params"][0],12);
    difficulty = (double)g_doc["params"][0];
    return true;
}

// tx_suggest_difficulty — EXATO do NerdMiner_v2
bool tx_suggest_difficulty(WiFiClient& client, double difficulty)
{
    char payload[BUFFER] = {0};
    g_stratum_id = getNextId(g_stratum_id);
    sprintf(payload, "{\"id\":%d,\"method\":\"mining.suggest_difficulty\",\"params\":[%.10g]}\n", g_stratum_id, difficulty);
    Serial.print("  Sending  : "); Serial.print(payload);
    return client.print(payload);
}

// parse_extract_id — EXATO do NerdMiner_v2
unsigned long parse_extract_id(const String &line)
{
    DeserializationError error = deserializeJson(g_doc, line);
    if (error) return 0;
    if (!g_doc.containsKey("id")) return 0;
    return (unsigned long)g_doc["id"];
}

// ============================================================================
// SHA256 (nosso — mbedtls, equivalente ao software path do NerdMiner_v2)
// ============================================================================

void doubleSHA256(const uint8_t* data, size_t len, uint8_t* out) {
    uint8_t hash1[32];
    mbedtls_sha256(data, len, hash1, 0);  // 0 = SHA256 (not SHA224)
    mbedtls_sha256(hash1, 32, out, 0);
}

// ============================================================================
// VARIÁVEIS GLOBAIS DO NOSSO FIRMWARE (UI, WiFi, estatísticas)
// ============================================================================

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
unsigned long localShares = 0;

char chars[] = " abcdefghijklmnopqrstuvwxyz0123456789@!#$%&*";
int charIndex = 0;
int scanNetworksCount = 0;
int selectedNetworkIndex = 0;

// --- State machine ---
enum UI_State { STATE_SCANNING, STATE_SELECT_SSID, STATE_INPUT_PASSWORD, STATE_CONNECTING, STATE_MINING };
UI_State currentState = STATE_SCANNING;

// --- Mining state (usa structs do NerdMiner_v2) ---
mining_subscribe mWorker;
mining_job mJob;
miner_data mMiner;
double currentPoolDifficulty = DEFAULT_DIFFICULTY;
bool isSubscribed = false;
bool isAuthorized = false;
bool hasJob = false;
unsigned long lastPoolDataTime = 0;
unsigned long lastSuggestTime = 0;
unsigned long lastJobTime = 0;
bool firstHeaderLog = true;

// Submit tracking (para não confundir resposta de submit com keepalive)
#define MAX_SUBMIT_IDS 32
unsigned long submitIdList[MAX_SUBMIT_IDS];
int submitIdCount = 0;
int sharesSubmitted = 0;

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

// ============================================================================
// BUZZER
// ============================================================================
void buzzerInit() { pinMode(BUZZER, INPUT); }
void beep(int ms = 50) {
    pinMode(BUZZER, OUTPUT);
    for (int i = 0; i < ms * 2; i++) {
        digitalWrite(BUZZER, HIGH); delayMicroseconds(250);
        digitalWrite(BUZZER, LOW); delayMicroseconds(250);
    }
    pinMode(BUZZER, INPUT);
}
void buzzerSuccess() { beep(300); delay(150); beep(300); delay(150); beep(300); }

// ============================================================================
// TELA OLED
// ============================================================================

void drawSplash() {
    display.clearDisplay();
    display.setTextSize(3); display.setTextColor(WHITE);
    display.setCursor(15, 15); display.print("MINER");
    display.display(); delay(2000);
}

void drawUI() {
    display.clearDisplay();
    int batRaw = analogRead(BAT_ADC);
    int batPct = map(batRaw, 0, 4095, 0, 100);
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(85, 0); display.print("B:"); display.print(batPct); display.print("%");

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
        display.setCursor(0, 30); display.print("> "); display.print(chars[charIndex]);
        display.setCursor(0, 45); display.print("SEL:Add BCK:Del");
        display.setCursor(0, 55); display.print("UP+DOWN: Conectar");
    }
    else if (currentState == STATE_CONNECTING) {
        display.setCursor(0, 20); display.print("Conectando...");
    }
    else if (currentState == STATE_MINING) {
        display.setCursor(0, 0); display.print("BTC MINER v2.2");
        display.drawLine(0, 10, 128, 10, WHITE);
        display.setCursor(0, 15); display.print(poolStatus.substring(0, 21));
        display.setCursor(0, 25); display.print("H/s:");
        if (hashrate >= 1000.0) { display.print(hashrate / 1000.0, 1); display.print("k"); }
        else display.print((unsigned long)hashrate);
        display.setCursor(64, 25); display.print("Loc:"); display.print(localShares);
        display.setCursor(0, 35); display.print("Hashes:"); display.print(hashesTotal / 1000); display.print("k");
        display.setCursor(0, 45);
        display.print("Ok:"); display.print(acceptedShares);
        display.print(" Rj:"); display.print(rejectedShares);
        display.print(" S:"); display.print(sharesSubmitted);
        display.setCursor(0, 55);
        display.print("pD:");
        if (currentPoolDifficulty < 0.01) display.print(currentPoolDifficulty, 4);
        else display.print(currentPoolDifficulty, 1);
        display.print(" b:");
        if (bestDiff < 0.01) display.print(bestDiff, 4);
        else display.print(bestDiff, 1);
    }
    display.display();
}

// ============================================================================
// CONEXÃO STRATUM — usando funções EXATAS do NerdMiner_v2
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
    lastSuggestTime = 0;
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

    // STEP 1: mining.subscribe (função EXATA do NerdMiner_v2)
    mWorker = init_mining_subscribe();
    if (!tx_mining_subscribe(client, mWorker)) {
        Serial.println("[STRATUM] Subscribe FAILED");
        poolStatus = "ERRO: Subscribe";
        client.stop();
        return;
    }
    isSubscribed = true;

    // STEP 2: mining.authorize (função EXATA do NerdMiner_v2)
    strncpy(mWorker.wName, WORKER_ID, sizeof(mWorker.wName) - 1);
    strncpy(mWorker.wPass, WORKER_PASS, sizeof(mWorker.wPass) - 1);
    tx_mining_auth(client, mWorker.wName, mWorker.wPass);

    // STEP 3: mining.suggest_difficulty (função EXATA do NerdMiner_v2)
    tx_suggest_difficulty(client, DEFAULT_DIFFICULTY);

    // Le respostas pendentes
    delay(300);
    while (client.available()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() < 2) continue;

        stratum_method method = parse_mining_method(line);
        if (method == STRATUM_SUCCESS) {
            // authorize response
            isAuthorized = true;
            poolStatus = "Autorizado!";
            Serial.println("[STRATUM] Authorized OK!");
        }
        else if (method == MINING_SET_DIFFICULTY) {
            parse_mining_set_difficulty(line, currentPoolDifficulty);
        }
        else if (method == MINING_NOTIFY) {
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
// MINING LOOP — usa mMiner.bytearray_blockheader EXATO do NerdMiner_v2
// ============================================================================

void miningLoop() {
    // 1. Processa mensagens pendentes da pool
    while (client.available()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() < 2) continue;
        lastPoolDataTime = millis();

        stratum_method method = parse_mining_method(line);

        if (method == STRATUM_SUCCESS) {
            // Resposta a algum submit
            unsigned long resp_id = parse_extract_id(line);
            if (isRealSubmitId(resp_id)) {
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
            }
        }
        else if (method == MINING_NOTIFY) {
            if (parse_mining_notify(line, mJob)) {
                hasJob = true;
                lastJobTime = millis();
                lastPoolDataTime = millis();

                // RECALCULA mining data — função EXATA do NerdMiner_v2
                Serial.println("[JOB] New mining.notify received, recalculating...");
                mMiner = calculateMiningData(mWorker, mJob);
                firstHeaderLog = true;

                Serial.printf("[JOB] job=%s branches=%d nbits=%s ntime=%s\n",
                    mJob.job_id.c_str(),
                    mJob.merkle_branch.size(),
                    mJob.nbits.c_str(),
                    mJob.ntime.c_str());
            } else {
                Serial.println("[JOB] Parse error, reconnecting...");
                client.stop();
                isSubscribed = false;
                isAuthorized = false;
                hasJob = false;
                delay(2000);
                connectToStratum();
                return;
            }
        }
        else if (method == MINING_SET_DIFFICULTY) {
            parse_mining_set_difficulty(line, currentPoolDifficulty);
        }
        else if (method == STRATUM_PARSE_ERROR) {
            unsigned long resp_id = parse_extract_id(line);
            if (isRealSubmitId(resp_id)) {
                rejectedShares++;
                Serial.printf("[POOL] Parse error for submit id=%lu\n", resp_id);
            }
        }
    }

    unsigned long now = millis();

    // 2. Verifica conexão
    if (!client.connected()) {
        Serial.println("[STRATUM] Connection lost! Reconnecting...");
        poolStatus = "Reconectando...";
        client.stop();
        isSubscribed = false;
        isAuthorized = false;
        hasJob = false;
        delay(3000);
        connectToStratum();
        return;
    }

    // 3. Inatividade do pool
    if (now - lastPoolDataTime > POOLINACTIVITY_TIME_ms) {
        Serial.println("[STRATUM] Pool inactivity timeout. Reconnecting...");
        poolStatus = "Timeout Pool...";
        client.stop();
        isSubscribed = false;
        isAuthorized = false;
        hasJob = false;
        delay(2000);
        connectToStratum();
        return;
    }

    // 4. Keep-alive (igual NerdMiner_v2: suggest_difficulty)
    if (now - lastPoolDataTime > KEEPALIVE_TIME_ms) {
        Serial.println("  Sending  : KeepAlive suggest_difficulty");
        tx_suggest_difficulty(client, currentPoolDifficulty);
        lastPoolDataTime = now;
    }

    // 5. Só minera se tem job
    if (!hasJob || !isSubscribed) {
        if (!hasJob && isSubscribed) poolStatus = "Aguardando job...";
        else if (!isSubscribed) poolStatus = "Subscribing...";
        return;
    }

    // 6. MINERAÇÃO — usa mMiner.bytearray_blockheader gerado por calculateMiningData
    //    que é a função EXATA do NerdMiner_v2
    if (firstHeaderLog) {
        firstHeaderLog = false;
        Serial.print("[DEBUG] Full header (80 bytes): ");
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
        // Escreve nonce no header (offset 76, 4 bytes, little-endian nativo)
        memcpy(&mMiner.bytearray_blockheader[76], &nonce, 4);

        // Double SHA256 do header de 80 bytes
        uint8_t hashBe[32];
        doubleSHA256(mMiner.bytearray_blockheader, 80, hashBe);
        localH++;

        // Calcula dificuldade — EXATO do NerdMiner_v2:
        // diff_from_target recebe hash em BIG-ENDIAN (ordem natural do SHA256)
        double hashDiff = diff_from_target(hashBe);

        if (hashDiff > bestDiff) {
            bestDiff = hashDiff;
        }

        // Conta shares locais para mostrar atividade
        if (hashDiff >= 0.00001) {
            localShares++;
            if (localShares <= 3 || localShares % 50 == 0) {
                Serial.printf("[LOCAL] diff=%.6f nonce=%08x (total:%lu best:%.6f)\n",
                    hashDiff, nonce, localShares, bestDiff);
            }
        }

        // Submete se atende dificuldade da pool
        if (hashDiff >= currentPoolDifficulty) {
            Serial.printf("[FOUND] diff=%.6f nonce=%08x job=%s\n",
                hashDiff, nonce, mJob.job_id.c_str());

            // Dump hash para debug
            Serial.print("[FOUND] Hash BE: ");
            for (int i = 0; i < 32; i++) {
                if (hashBe[i] < 0x10) Serial.print("0");
                Serial.print(hashBe[i], HEX);
            }
            Serial.println();

            // Submit usando função EXATA do NerdMiner_v2
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
    hashesInWindow += localH;

    // 7. Hashrate (janela de 1 segundo)
    unsigned long now2 = millis();
    if (now2 - lastHashTime >= 1000) {
        double elapsed = (double)(now2 - lastHashTime) / 1000.0;
        hashrate = (double)hashesInWindow / elapsed;
        hashesInWindow = 0;
        lastHashTime = now2;
        static unsigned long lastHRLog = 0;
        if (now2 - lastHRLog >= 5000) {
            Serial.printf("[STATS] H/s=%.0f total=%luk local=%lu best=%.6f poolDiff=%.6f Ok=%d Rj=%d\n",
                hashrate, hashesTotal/1000, localShares, bestDiff, currentPoolDifficulty,
                acceptedShares, rejectedShares);
            lastHRLog = now2;
        }
    }

    // 8. Atualiza display
    if (hashrate > 100) {
        poolStatus = "Minerando " + String((unsigned long)hashrate) + " H/s";
    }
}

// ============================================================================
// BOTÕES
// ============================================================================

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
            int att = 0;
            while (WiFi.status() != WL_CONNECTED && att < 20) { delay(500); att++; }
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

// ============================================================================
// SETUP / LOOP
// ============================================================================

void setup() {
    Serial.begin(115200);
    Serial.println("\n\n=== ESP32 BTC Miner v2.2 (Full NerdMiner_v2 Port) ===");

    pinMode(BTN_UP, INPUT_PULLUP); pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_SEL, INPUT_PULLUP); pinMode(BTN_BACK, INPUT_PULLUP);
    pinMode(BAT_ADC, INPUT); buzzerInit();

    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED not found!"); for (;;);
    }
    drawSplash();

    preferences.begin("wifi", false);
    ssid = preferences.getString("ssid", "");
    password = preferences.getString("pass", "");
    preferences.end();

    if (ssid.length() > 0) {
        Serial.printf("[WIFI] Connecting to: %s\n", ssid.c_str());
        WiFi.begin(ssid.c_str(), password.c_str());
        int att = 0;
        while (WiFi.status() != WL_CONNECTED && att < 20) { delay(500); att++; }
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
    static unsigned long lastScreen = 0;
    if (millis() - lastScreen > 250) { drawUI(); lastScreen = millis(); }
}
