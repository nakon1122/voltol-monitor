#include "net.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "config.h"

namespace net {
static Saved nets[MAX_NETS];
static int n = 0;
static bool apOn = false;
static char apName[24] = "";
static int tryIdx = -1;            // network being tried (-1 none)
static uint32_t lastTry = 0, tryStart = 0;
static bool wasOnline = false;
static char curSsid[33] = "", curIp[16] = "";
static int curRssi = 0, curCh = 0;
static const uint32_t RETRY_MS = 30000, TRY_MS = 15000;

static void persist() {
  Preferences p;
  p.begin("voltol-net", false);
  p.putUChar("count", n);
  for (int i = 0; i < MAX_NETS; i++) {
    char ks[6], kp[6];
    snprintf(ks, sizeof ks, "s%d", i);
    snprintf(kp, sizeof kp, "p%d", i);
    if (i < n) { p.putString(ks, nets[i].ssid); p.putString(kp, nets[i].pass); }
    else { p.remove(ks); p.remove(kp); }
  }
  p.remove("ssid");  // v1.3 single-network keys, migrated
  p.remove("pass");
  p.end();
}
static void load() {
  Preferences p;
  p.begin("voltol-net", true);
  int c = p.getUChar("count", 0);
  if (c > MAX_NETS) c = MAX_NETS;
  n = 0;
  for (int i = 0; i < c; i++) {
    char ks[6], kp[6];
    snprintf(ks, sizeof ks, "s%d", i);
    snprintf(kp, sizeof kp, "p%d", i);
    String s = p.getString(ks, ""), pw = p.getString(kp, "");
    if (!s.length()) continue;
    strlcpy(nets[n].ssid, s.c_str(), sizeof nets[n].ssid);
    strlcpy(nets[n].pass, pw.c_str(), sizeof nets[n].pass);
    n++;
  }
  String old = p.getString("ssid", "");  // v1.3.0 / v1.6.x single network
  String oldp = p.getString("pass", "");
  p.end();
  if (old.length()) {
    bool have = false;
    for (int i = 0; i < n; i++) have |= old == nets[i].ssid;
    if (!have && n < MAX_NETS) {
      for (int i = n; i > 0; i--) nets[i] = nets[i - 1];
      strlcpy(nets[0].ssid, old.c_str(), sizeof nets[0].ssid);
      strlcpy(nets[0].pass, oldp.c_str(), sizeof nets[0].pass);
      n++;
    }
    persist();
  }
}

static void applyMode() {
  wifi_mode_t m = apOn ? (n ? WIFI_AP_STA : WIFI_AP) : (n ? WIFI_STA : WIFI_OFF);
  if (WiFi.getMode() != m) WiFi.mode(m);
  if (m != WIFI_OFF) WiFi.setSleep(false);  // power save drops hotspot clients in AP+STA
}

void hotspot(bool on) {
  if (on == apOn && (!on || WiFi.getMode() & WIFI_AP)) return;
  apOn = on;
  applyMode();
  if (on) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(apName, sizeof apName, "VOLTOL-%02X%02X", mac[4], mac[5]);
    // channel 1 at first; once the internet Wi-Fi joins, the radio (and the hotspot) follow its channel
    WiFi.softAP(apName, AP_PASSWORD, 1, 0, 4);
  } else {
    WiFi.softAPdisconnect(true);
  }
}

static void startTry(int i) {
  if (i < 0 || i >= n) return;
  tryIdx = i;
  tryStart = lastTry = millis();
  applyMode();
  WiFi.disconnect(false, false);
  WiFi.begin(nets[i].ssid, nets[i].pass);
}

void begin(bool hs) {
  WiFi.persistent(false);       // we keep the networks ourselves
  WiFi.setAutoReconnect(false); // no background reconnect storms
  load();
  hotspot(hs);
  applyMode();
  if (n) startTry(0);
}

void loop() {
  uint32_t now = millis();
  bool on = WiFi.status() == WL_CONNECTED;
  if (on) {
    if (!wasOnline) {
      strlcpy(curSsid, WiFi.SSID().c_str(), sizeof curSsid);
      strlcpy(curIp, WiFi.localIP().toString().c_str(), sizeof curIp);
      // remember the network that worked: tried first next time
      for (int i = 1; i < n; i++) if (!strcmp(nets[i].ssid, curSsid)) {
        Saved s = nets[i];
        for (int k = i; k > 0; k--) nets[k] = nets[k - 1];
        nets[0] = s;
        persist();
        break;
      }
    }
    static uint32_t lastR = 0;
    if (now - lastR > 2000) { lastR = now; curRssi = WiFi.RSSI(); curCh = WiFi.channel(); }
    tryIdx = -1;
  } else {
    if (wasOnline) { lastTry = now; curSsid[0] = 0; curIp[0] = 0; curRssi = 0; }
    if (tryIdx >= 0 && now - tryStart > TRY_MS) {  // this one did not work: stop trying, rest
      WiFi.disconnect(false, false);
      tryIdx = -1;
      lastTry = now;
    }
    // next attempt only when nobody is using the hotspot (connecting scans all
    // channels and would cut the phone that is on the module's web page);
    // with a phone connected, one quiet try every 5 minutes
    if (n && tryIdx < 0 && now - lastTry > RETRY_MS && (apClients() == 0 || now - lastTry > 300000)) {
      static int next = 0;
      startTry(next % n);
      next++;
    }
  }
  wasOnline = on;
}

bool hotspotOn() { return apOn; }
const char *apSsid() { return apOn ? apName : "off"; }
int apClients() { return apOn ? WiFi.softAPgetStationNum() : 0; }
int count() { return n; }
const Saved &saved(int i) { return nets[i]; }

bool add(const char *s, const char *pw) {
  if (!s || !s[0] || strlen(s) > 32 || (pw && strlen(pw) > 64)) return false;
  int found = -1;
  for (int i = 0; i < n; i++) if (!strcmp(nets[i].ssid, s)) found = i;
  if (found < 0) { if (n < MAX_NETS) n++; found = n - 1; }   // full: the oldest (last) is replaced
  for (int k = found; k > 0; k--) nets[k] = nets[k - 1];
  strlcpy(nets[0].ssid, s, sizeof nets[0].ssid);
  strlcpy(nets[0].pass, pw ? pw : "", sizeof nets[0].pass);
  persist();
  startTry(0);
  return true;
}
bool remove(const char *s) {
  int found = -1;
  for (int i = 0; i < n; i++) if (!strcmp(nets[i].ssid, s)) found = i;
  if (found < 0) return false;
  for (int k = found; k < n - 1; k++) nets[k] = nets[k + 1];
  n--;
  persist();
  if (!strcmp(curSsid, s)) { WiFi.disconnect(false, false); wasOnline = false; curSsid[0] = 0; }
  applyMode();
  return true;
}
void reconnectNow() { if (n) startTry(0); }

bool online() { return WiFi.status() == WL_CONNECTED; }
const char *ssid() { return online() ? curSsid : ""; }
const char *ip() { return online() ? curIp : ""; }
int rssi() { return online() ? curRssi : 0; }
int channel() { return curCh; }

// ---------------------------------------------------------------- scan ----
static int scanN = -1;  // -1 never, -2 busy
void scanStart() {
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
  WiFi.scanDelete();
  WiFi.scanNetworks(true, false, false, 120);  // async, ~1.6 s, 120 ms per channel
  scanN = -2;
}
bool scanBusy() {
  int r = WiFi.scanComplete();
  if (r >= 0) scanN = r;
  return r == WIFI_SCAN_RUNNING;
}
int scanCount() { scanBusy(); return scanN < 0 ? 0 : scanN; }
bool scanItem(int i, String &s, int &r, bool &lock) {
  if (i < 0 || i >= scanCount()) return false;
  s = WiFi.SSID(i);
  r = WiFi.RSSI(i);
  lock = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  return s.length() > 0;
}
static void jsonStr(String &o, const char *s) {
  o += '"';
  for (const char *p = s; *p; p++) {
    if (*p == '"' || *p == '\\') o += '\\';
    if ((uint8_t)*p >= 0x20) o += *p;
  }
  o += '"';
}
String scanJson() {
  String o = "[";
  int c = scanCount();
  for (int i = 0, k = 0; i < c && k < 20; i++) {
    String s; int r; bool l;
    if (!scanItem(i, s, r, l)) continue;
    if (k++) o += ",";
    o += "{\"s\":"; jsonStr(o, s.c_str()); o += ",\"r\":" + String(r) + ",\"l\":" + String(l ? 1 : 0) + "}";
  }
  return o + "]";
}
String listJson() {
  String o = "[";
  for (int i = 0; i < n; i++) {
    if (i) o += ",";
    o += "{\"s\":"; jsonStr(o, nets[i].ssid);
    o += ",\"on\":" + String(online() && !strcmp(curSsid, nets[i].ssid) ? "true" : "false") + "}";
  }
  return o + "]";
}
}  // namespace net
