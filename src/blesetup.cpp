#include "blesetup.h"
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include "net.h"

namespace blesetup {
static const char *SVC = "648b7040-33c9-4a41-a133-83c009df0001";
static const char *CH_WRITE = "648b7040-33c9-4a41-a133-83c009df0002";
static const char *CH_STATUS = "648b7040-33c9-4a41-a133-83c009df0003";
static const char *CH_PUB = "648b7040-33c9-4a41-a133-83c009df0004";
static const char *CH_SCAN = "648b7040-33c9-4a41-a133-83c009df0005";
static const uint32_t WINDOW_MS = 180000;

static DoneFn doneFn = nullptr;
static bool on = false, physical = false;
static uint32_t until = 0;
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
  if (physical) {
    if (!haveKey || pktLen < 65 + 12 + 16) { setState("invalid-data"); return; }
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

static void start() {
  if (on) return;
  NimBLEDevice::init("VOLTOL Setup");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEServer *srv = NimBLEDevice::createServer();
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
}
static void stop() {
  if (!on) return;
  NimBLEDevice::deinit(true);
  cStatus = cPub = cScan = nullptr;
  on = false;
  if (haveKey) { mbedtls_ecp_group_free(&grp); mbedtls_mpi_free(&priv); mbedtls_ecp_point_free(&pub); haveKey = false; }
  strlcpy(st, "idle", sizeof st);
}

void begin(bool bootWindow, DoneFn done) {
  doneFn = done;
  if (!bootWindow) return;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&priv);
  mbedtls_ecp_point_init(&pub);
  uint8_t q[65];
  size_t ql = 0;
  haveKey = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_ecp_gen_keypair(&grp, &priv, &pub, rng, nullptr) == 0 &&
            mbedtls_ecp_point_write_binary(&grp, &pub, MBEDTLS_ECP_PF_UNCOMPRESSED, &ql, q, sizeof q) == 0 && ql == 65;
  if (!haveKey) return;
  start();
  cPub->setValue(q, 65);
  physical = true;
  until = millis() + WINDOW_MS;
  setState("boot-ready");
}

String startGrant() {
  start();
  esp_fill_random(grantKey, sizeof grantKey);
  physical = false;
  until = millis() + WINDOW_MS;
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
    setState("accepted");
    until = millis() + 15000;  // let the web page read "accepted", then switch Bluetooth off
  }
  if (scanReq) {
    scanReq = false;
    net::scanStart();
    scanning = true;
    uint8_t busy = 119;
    cScan->setValue(&busy, 1);
  }
  if (scanning && !net::scanBusy()) { scanning = false; fillScan(); }
  if ((int32_t)(millis() - until) > 0) {
    if (strcmp(st, "accepted") && on) setState("expired");
    stop();
  }
}
bool active() { return on; }
const char *state() { return st; }
int secondsLeft() { return on ? (int)((int32_t)(until - millis()) / 1000) : 0; }
}  // namespace blesetup
