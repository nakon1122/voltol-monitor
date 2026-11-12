#include "blesetup.h"
#ifndef VOLTOL_NO_BLE
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <Preferences.h>
#include <esp_bt.h>
#include "net.h"
#include "cloud.h"

namespace blesetup {
static const char *SVC = "648b7040-33c9-4a41-a133-83c009df0001";
static const char *CH_WRITE = "648b7040-33c9-4a41-a133-83c009df0002";
static const char *CH_STATUS = "648b7040-33c9-4a41-a133-83c009df0003";
static const char *CH_PUB = "648b7040-33c9-4a41-a133-83c009df0004";
static const char *CH_SCAN = "648b7040-33c9-4a41-a133-83c009df0005";
static const uint32_t WINDOW_MS = 180000;

static DoneFn doneFn = nullptr;
static bool on = false, physical = false;
static char st[16] = "idle";
static uint8_t grantKey[16];
static NimBLECharacteristic *cStatus = nullptr, *cPub = nullptr, *cScan = nullptr;
static uint8_t pkt[320];
static size_t pktLen = 0;
static volatile bool pktReady = false, scanReq = false, scanning = false;
// ECDH (physical / boot window)
static mbedtls_ecp_group grp;
static mbedtls_mpi priv;
static mbedtls_ecp_point pub;
static bool haveKey = false;
// result handed to loop()
static char rSsid[33], rPass[65], rTok[65];
static volatile bool resultReady = false;
static bool always = true;      // setting: Bluetooth stays on all the time (S3)
static bool needSetup = false;  // module has no cloud token yet
static bool grantOn = false;    // one-time code from the module page is valid
static uint32_t windowEnd = 0;  // 0 = no running window
static uint32_t lastAdvCheck = 0;
static uint8_t pubRaw[65];
static bool screenOpen = false;  // pairing window opened from the module screen
static uint32_t acceptedAt = 0;  // setup received: report Wi-Fi / cloud progress to the phone

static int rng(void *, unsigned char *out, size_t n) { esp_fill_random(out, n); return 0; }
static void setState(const char *s) {
  strlcpy(st, s, sizeof st);
  if (cStatus) cStatus->setValue((uint8_t *)st, strlen(st));
}

class WriteCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    std::string v = c->getValue();
    if (v.empty()) return;
    uint8_t op = (uint8_t)v[0];
    if (op == 0) { pktLen = 0; }
    else if (op == 1) {
      size_t k = v.size() - 1;
      if (pktLen + k <= sizeof pkt) { memcpy(pkt + pktLen, v.data() + 1, k); pktLen += k; }
      else pktLen = sizeof pkt + 1;  // too long: rejected at the end
    } else if (op == 2) { pktReady = true; }
    else if (op == 3) { scanReq = true; }
  }
};
static WriteCb writeCb;

static bool decrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *enc, size_t len, uint8_t *out) {
  if (len < 16) return false;
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
            mbedtls_gcm_auth_decrypt(&g, len - 16, nonce, 12, nullptr, 0, enc + len - 16, 16, enc, out) == 0;
  mbedtls_gcm_free(&g);
  return ok;
}

static void handlePacket() {
  pktReady = false;
  if (pktLen > sizeof pkt) { setState("invalid-data"); return; }
  uint8_t key[16];
  size_t off = 0;
  if (!physical && !grantOn) { setState("invalid-code"); return; }  // locked: needs a code from the module page
  if (physical) {
    if (!(needSetup || screenOpen) || !haveKey || pktLen < 65 + 12 + 16) { setState("invalid-data"); return; }
    mbedtls_ecp_point peer;
    mbedtls_mpi z;
    mbedtls_ecp_point_init(&peer);
    mbedtls_mpi_init(&z);
    uint8_t zb[32];
    bool ok = mbedtls_ecp_point_read_binary(&grp, &peer, pkt, 65) == 0 &&
              mbedtls_ecdh_compute_shared(&grp, &z, &peer, &priv, rng, nullptr) == 0 &&
              mbedtls_mpi_write_binary(&z, zb, 32) == 0;
    mbedtls_ecp_point_free(&peer);
    mbedtls_mpi_free(&z);
    if (!ok) { setState("invalid-data"); return; }
    memcpy(key, zb, 16);  // Web Crypto deriveBits(256) = X coordinate; first 16 bytes = AES-128 key
    off = 65;
  } else {
    memcpy(key, grantKey, 16);
  }
  if (pktLen < off + 12 + 16) { setState("invalid-data"); return; }
  size_t encLen = pktLen - off - 12;
  static uint8_t plain[320];
  if (!decrypt(key, pkt + off, pkt + off + 12, encLen, plain)) { setState(physical ? "invalid-data" : "invalid-code"); return; }
  size_t pl = encLen - 16;
  uint8_t sl = plain[0], pw = plain[1];
  if (pl != (size_t)66 + sl + pw || sl == 0 || sl > 32 || pw > 64) { setState("invalid-data"); return; }
  memcpy(rSsid, plain + 2, sl); rSsid[sl] = 0;
  memcpy(rPass, plain + 2 + sl, pw); rPass[pw] = 0;
  memcpy(rTok, plain + 2 + sl + pw, 64); rTok[64] = 0;
  for (int i = 0; i < 64; i++) if (!isxdigit((unsigned char)rTok[i])) { setState("invalid-data"); return; }
  resultReady = true;
}

static void fillScan() {
  static uint8_t buf[512];
  size_t pos = 1;
  int c = 0, n = net::scanCount();
  for (int i = 0; i < n && c < 12; i++) {
    String s; int r; bool lock;
    if (!net::scanItem(i, s, r, lock)) continue;
    size_t len = s.length() > 32 ? 32 : s.length();
    if (pos + 3 + len > sizeof buf) break;
    buf[pos++] = len; buf[pos++] = lock ? 1 : 0; buf[pos++] = (uint8_t)(int8_t)r;
    memcpy(buf + pos, s.c_str(), len); pos += len;
    c++;
  }
  buf[0] = c;
  cScan->setValue(buf, pos);
}


static void setPowerMax() {
  // strongest radio the chip allows: range/stability over current draw
#if CONFIG_IDF_TARGET_ESP32S3
  const esp_power_level_t lvl = ESP_PWR_LVL_P21;
#else
  const esp_power_level_t lvl = ESP_PWR_LVL_P9;
#endif
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, lvl);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, lvl);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, lvl);
  for (int h = 0; h < 3; h++) esp_ble_tx_power_set((esp_ble_power_type_t)(ESP_BLE_PWR_TYPE_CONN_HDL0 + h), lvl);
}

static void start() {
  if (on) return;
  net::btCoex(true);  // modem sleep on first, or the radio driver aborts
  NimBLEDevice::init("VOLTOL Setup");
  setPowerMax();
  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->advertiseOnDisconnect(true);
  NimBLEService *svc = srv->createService(SVC);
  NimBLECharacteristic *w = svc->createCharacteristic(CH_WRITE, NIMBLE_PROPERTY::WRITE);
  w->setCallbacks(&writeCb);
  cStatus = svc->createCharacteristic(CH_STATUS, NIMBLE_PROPERTY::READ);
  cPub = svc->createCharacteristic(CH_PUB, NIMBLE_PROPERTY::READ);
  cScan = svc->createCharacteristic(CH_SCAN, NIMBLE_PROPERTY::READ);
  uint8_t zero = 0;
  cScan->setValue(&zero, 1);
  svc->start();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC);
  adv->setScanResponse(true);
  adv->start();
  on = true;
  if (haveKey) cPub->setValue(pubRaw, 65);
  setState(st);
}
static void freeKey() {
  if (haveKey) { mbedtls_ecp_group_free(&grp); mbedtls_mpi_free(&priv); mbedtls_ecp_point_free(&pub); haveKey = false; }
}
static void stop() {
  if (!on) return;
  NimBLEDevice::deinit(true);
  cStatus = cPub = cScan = nullptr;
  on = false;
  net::btCoex(false);
}
static bool makeKey() {
  if (haveKey) return true;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&priv);
  mbedtls_ecp_point_init(&pub);
  size_t ql = 0;
  haveKey = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_ecp_gen_keypair(&grp, &priv, &pub, rng, nullptr) == 0 &&
            mbedtls_ecp_point_write_binary(&grp, &pub, MBEDTLS_ECP_PF_UNCOMPRESSED, &ql, pubRaw, sizeof pubRaw) == 0 && ql == 65;
  if (!haveKey) { mbedtls_ecp_group_free(&grp); mbedtls_mpi_free(&priv); mbedtls_ecp_point_free(&pub); }
  return haveKey;
}
// resting state when no window is running
static void rest() {
  grantOn = false;
  screenOpen = false;
  acceptedAt = 0;
  windowEnd = 0;
  if (needSetup && makeKey()) {
    physical = true;
    if (!on && always) start();
    if (cPub) cPub->setValue(pubRaw, 65);
    setState("boot-ready");
    if (!always) windowEnd = millis() + WINDOW_MS;  // setting off: old 3-minute window
  } else {
    physical = false;
    freeKey();
    if (always) { if (!on) start(); setState("locked"); }
    else { stop(); strlcpy(st, "idle", sizeof st); }
  }
}

static bool loadSetting() {
  Preferences p;
  p.begin("voltol-ble", true);
  bool v = p.getBool("on", true);
  p.end();
  return v;
}

void begin(bool unconfigured, DoneFn done) {
  doneFn = done;
  always = loadSetting();
  needSetup = unconfigured;
  if (!needSetup && !always) return;  // nothing to do: stays off
  rest();
}

void setEnabled(bool v) {
  Preferences p;
  p.begin("voltol-ble", false);
  p.putBool("on", v);
  p.end();
  always = v;
  if (!windowEnd || v) rest();  // a running pairing window is left to finish
}
bool enabled() { return always; }

String startGrant() {
  esp_fill_random(grantKey, sizeof grantKey);
  physical = false;
  grantOn = true;
  if (!on) start();
  windowEnd = millis() + WINDOW_MS;
  setState("ready");
  char hex[33];
  for (int i = 0; i < 16; i++) sprintf(hex + i * 2, "%02x", grantKey[i]);
  return String(hex);
}

void loop() {
  if (!on) return;
  if (pktReady) handlePacket();
  if (resultReady) {
    resultReady = false;
    if (doneFn) doneFn(rSsid, rPass, rTok);
    memset(rPass, 0, sizeof rPass);
    memset(grantKey, 0, sizeof grantKey);
    needSetup = false;
    grantOn = false;
    physical = false;
    screenOpen = false;
    setState("accepted");
    acceptedAt = millis();
    windowEnd = millis() + 120000;  // stay connectable: the phone page follows Wi-Fi + cloud progress
  }
  if (acceptedAt) {  // accepted -> joining -> online -> cloud-ok (or wifi-fail / cloud-fail)
    uint32_t t = millis() - acceptedAt;
    const char *ns = !net::online() ? (t > 45000 ? "wifi-fail" : "joining")
                     : cloud::online() ? "cloud-ok" : (t > 90000 ? "cloud-fail" : "online");
    if (t > 1500 && strcmp(st, ns)) {
      setState(ns);
      if (!strcmp(ns, "wifi-fail") && makeKey()) {  // let the phone send a corrected password right away
        physical = true;
        screenOpen = true;
        if (cPub) cPub->setValue(pubRaw, 65);
        windowEnd = millis() + WINDOW_MS;
      }
      if (!strcmp(ns, "cloud-ok") && (int32_t)(windowEnd - millis()) > 20000) windowEnd = millis() + 20000;
    }
  }
  if (scanReq) {
    scanReq = false;
    net::scanStart();
    scanning = true;
    uint8_t busy = 119;
    cScan->setValue(&busy, 1);
  }
  if (scanning && !net::scanBusy()) { scanning = false; fillScan(); }
  if (windowEnd && (int32_t)(millis() - windowEnd) > 0) {
    bool wasGrant = grantOn;
    if (!always && !wasGrant && !acceptedAt) { setState("expired"); stop(); windowEnd = 0; freeKey(); return; }
    rest();
  }
  // keep advertising: restart it if a phone dropped without the stack doing so
  if (on && millis() - lastAdvCheck > 3000) {
    lastAdvCheck = millis();
    NimBLEServer *srv = NimBLEDevice::getServer();
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (srv && adv && srv->getConnectedCount() == 0 && !adv->isAdvertising()) adv->start();
  }
}
bool active() { return on; }
const char *state() { return st; }
int secondsLeft() { return on && windowEnd && !acceptedAt ? (int)((int32_t)(windowEnd - millis()) / 1000) : 0; }
bool phone() {
  NimBLEServer *srv = on ? NimBLEDevice::getServer() : nullptr;
  return srv && srv->getConnectedCount() > 0;
}
void startScreen() {
  if (!makeKey()) return;
  physical = true;
  screenOpen = true;
  grantOn = false;
  acceptedAt = 0;
  if (!on) start();
  if (cPub) cPub->setValue(pubRaw, 65);
  windowEnd = millis() + WINDOW_MS;
  setState("boot-ready");
}
}  // namespace blesetup
#else
// CYD 3.5: no room for Bluetooth next to the screen fonts. Pair with the
// module web page (cloud token) or USB serial instead.
namespace blesetup {
void begin(bool, DoneFn) {}
void setEnabled(bool) {}
bool enabled() { return false; }
String startGrant() { return ""; }
void loop() {}
bool active() { return false; }
const char *state() { return "unsupported"; }
int secondsLeft() { return 0; }
bool phone() { return false; }
void startScreen() {}
}  // namespace blesetup
#endif
