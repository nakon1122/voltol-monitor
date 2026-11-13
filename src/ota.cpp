// Firmware updates.
//  * Boot-loop protection: after an update the new firmware must run for 60 s.
//    If it restarts 3 times before that, the previous firmware is booted again.
//  * Internet update: the board joins a Wi-Fi network (home router or phone
//    hotspot), reads version.json from GitHub (branch firmware-build, written
//    by the build), downloads the matching *_APP_UPDATE.bin, checks its SHA-256
//    and only then switches to it. Never while the motor turns (guard).
#include <Arduino.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include "config.h"
#include "cloud_ca_bundle.h"
#include "net.h"
#include "ota.h"

// Keep the new image "pending" until markGood() (bootloader rollback, if built in).
extern "C" bool verifyRollbackLater() { return true; }

namespace ota {
static const char *HOST = "raw.githubusercontent.com";
static const char *PATH = "/nakon1122/votol-monitor/firmware-build/firmware/";
static Status st;
static GuardFn guard = nullptr;
static const char *board = "cyd35";
static volatile int request = 0;  // 1 check, 2 install
static String wantSha, wantFile;
static uint32_t wantSize = 0, lastCheck = 0, upSince = 0;
static void say(const char *fmt, const char *arg = "") {
  st.fmt = fmt;
  strlcpy(st.arg, arg, sizeof st.arg);
  snprintf(st.msg, sizeof st.msg, fmt, arg);
}

// ------------------------------------------------------------ rollback ----
void bootCheck() {
  Preferences p;
  p.begin("votol-ota", false);
  st.rolledBack = p.getBool("rolled", false);
  if (p.getBool("pend", false)) {
    uint8_t n = p.getUChar("boots", 0) + 1;
    p.putUChar("boots", n);
    if (n >= 3) {
      const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
      p.putBool("pend", false);
      p.putUChar("boots", 0);
      p.putBool("rolled", true);
      p.end();
      if (other && esp_ota_set_boot_partition(other) == ESP_OK) ESP.restart();
      return;
    }
  }
  p.end();
}
void markPending() {
  Preferences p;
  p.begin("votol-ota", false);
  p.putBool("pend", true);
  p.putUChar("boots", 0);
  p.putBool("rolled", false);
  p.end();
}
static void markGood() {
  Preferences p;
  p.begin("votol-ota", false);
  if (p.getBool("pend", false)) { p.putBool("pend", false); p.putUChar("boots", 0); }
  p.end();
  esp_ota_mark_app_valid_cancel_rollback();
}

// ---------------------------------------------------------------- wifi ----
// Networks live in net.cpp (several saved networks, hotspot-friendly).
bool staConfigured() { return net::count() > 0; }
const char *staSsid() { return net::online() ? net::ssid() : (net::count() ? net::saved(0).ssid : ""); }
void setWifi(const char *s, const char *pw) { if (s && s[0]) net::add(s, pw); st.connected = false; }

static int cmpVer(const char *a, const char *b) {
  int x[3] = {0}, y[3] = {0};
  sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
  sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
  for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}
static String field(const String &js, int from, const char *key) {
  String k = String("\"") + key + "\"";
  int i = js.indexOf(k, from);
  if (i < 0) return "";
  i = js.indexOf(':', i) + 1;
  while (i < (int)js.length() && (js[i] == ' ' || js[i] == '"')) i++;
  int e = i;
  while (e < (int)js.length() && js[e] != '"' && js[e] != ',' && js[e] != '}') e++;
  return js.substring(i, e);
}
// Minimal HTTPS GET; leaves the client positioned at the body. Returns content length.
static long get(WiFiClientSecure &c, const String &path) {
  c.setCACertBundle(CLOUD_CA_BUNDLE
#if ESP_ARDUINO_VERSION_MAJOR >= 3
                    , sizeof(CLOUD_CA_BUNDLE)
#endif
  );
  c.setTimeout(15);
  if (!c.connect(HOST, 443)) return -1;
  c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: votol-monitor\r\nConnection: close\r\n\r\n", path.c_str(), HOST);
  String line = c.readStringUntil('\n');
  if (line.indexOf(" 200") < 0) return -2;
  long len = -3;
  for (;;) {
    line = c.readStringUntil('\n');
    line.trim();
    if (!line.length()) break;
    if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) len = line.substring(15).toInt();
  }
  return len;
}
static void doCheck() {
  st.state = S_CHECKING;
  vTaskDelay(pdMS_TO_TICKS(1500));  // let the cloud upload close its connection first (memory)
  WiFiClientSecure c;
  long n = get(c, String(PATH) + "version.json");
  if (n < 0) {
    st.state = S_ERROR;
    say(n == -1 ? "Cannot reach GitHub" : "version.json not found");
    return;
  }
  String js = c.readString();
  c.stop();
  String ver = field(js, 0, "version");
  int b = js.indexOf(String("\"") + board + "\"");
  if (!ver.length() || b < 0) { st.state = S_ERROR; say("Bad version.json"); return; }
  wantFile = field(js, b, "file");
  wantSha = field(js, b, "sha256");
  wantSize = field(js, b, "size").toInt();
  strlcpy(st.latest, ver.c_str(), sizeof st.latest);
  st.available = cmpVer(VOTOL_FW, st.latest) < 0;
  st.state = S_IDLE;
  say(st.available ? "Version %s available" : "Up to date (%s)", st.latest);
}
static void doInstall() {
#ifdef VOTOL_NO_OTA  // CYD since 1.9.0: one app slot (Bluetooth BMS), install over USB
  st.state = S_ERROR;
  say("Update with USB: votol.battlab.net");
  return;
#endif
  if (!st.available) doCheck();
  if (!st.available) return;
  const char *why = guard ? guard() : nullptr;
  if (why) { st.state = S_ERROR; say(why); return; }
  st.state = S_DOWNLOADING;
  st.progress = 0;
  WiFiClientSecure c;
  long n = get(c, String(PATH) + wantFile);
  if (n <= 0 || (wantSize && (uint32_t)n != wantSize)) {
    char a[16]; snprintf(a, sizeof a, "%ld", n);
    st.state = S_ERROR; say("Download failed (%s)", a); c.stop(); return;
  }
  if (!Update.begin(n)) {
    // a bigger firmware than this board's partition table allows (older layout)
    st.state = S_ERROR; say("Too big for this board: install once with USB (FLASH .bat)"); c.stop(); return;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  static uint8_t buf[2048];
  long done = 0;
  uint32_t lastData = millis();
  while (done < n && millis() - lastData < 20000) {
    int k = c.read(buf, sizeof buf);
    if (k <= 0) { delay(5); continue; }
    lastData = millis();
    mbedtls_sha256_update(&sha, buf, k);
    if (Update.write(buf, k) != (size_t)k) break;
    done += k;
    st.progress = done * 100 / n;
  }
  c.stop();
  uint8_t h[32];
  mbedtls_sha256_finish(&sha, h);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", h[i]);
  if (done != n || wantSha != hex) {
    Update.abort();
    st.state = S_ERROR;
    say(done != n ? "Download interrupted" : "Checksum mismatch - not installed");
    return;
  }
  if (!Update.end(true)) { st.state = S_ERROR; say("Install failed"); return; }
  markPending();
  st.state = S_RESTARTING;
  say("Installed %s - restarting", st.latest);
  delay(1500);
  ESP.restart();
}
static void task(void *) {
  for (;;) {
    if (request) {
      int r = request;
      request = 0;
      if (!st.connected) { st.state = S_ERROR; say("No internet Wi-Fi connection"); }
      else if (r == 1) doCheck();
      else doInstall();
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void begin(const char *boardKey, GuardFn g) {
  board = boardKey;
  guard = g;
  upSince = millis();
  xTaskCreatePinnedToCore(task, "ota", 10240, nullptr, 1, nullptr, 1);
}
void loop() {
  static bool good = false;
  if (!good && millis() - upSince > 60000) { good = true; markGood(); }
  bool c = net::online();
  if (c && !st.connected) {
    strlcpy(st.ip, net::ip(), sizeof st.ip);
    if (!lastCheck || millis() - lastCheck > 6UL * 3600 * 1000) { lastCheck = millis(); request = 1; }
  }
  st.connected = c;
  if (!c) st.ip[0] = 0;
}
void check() { request = 1; }
void install() { request = 2; }
const Status &status() { return st; }
const char *uploadBlocked() {
#ifdef VOTOL_NO_OTA
  return "Update with USB: votol.battlab.net";
#endif
  return guard ? guard() : nullptr;
}
}  // namespace ota
