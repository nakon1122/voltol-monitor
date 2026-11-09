#include "cloud.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "cloud_ca_bundle.h"
#include "config.h"
#include "net.h"

namespace cloud {
static const char *BASE = "https://zpsxcdzkayqhzbdcdoed.supabase.co/functions/v1/voltol-device/";
static char token[65] = "";
static volatile int httpCode = 0;
static volatile uint32_t okAt = 0;
static PayloadFn payloadFn = nullptr;
static CmdFn cmdFn = nullptr;
static SemaphoreHandle_t mtx;

static bool validHex(const char *t) {
  if (!t || strlen(t) != 64) return false;
  for (int i = 0; i < 64; i++) if (!isxdigit((unsigned char)t[i])) return false;
  return true;
}
bool setToken(const char *t) {
  if (!validHex(t)) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = 0; i < 64; i++) token[i] = tolower((unsigned char)t[i]);
  token[64] = 0;
  xSemaphoreGive(mtx);
  Preferences p;
  p.begin("voltol-cloud", false);  // same place as v1.6.x: existing pairings keep working
  p.putString("token", token);
  p.end();
  okAt = 0;
  return true;
}
void forget() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  token[0] = 0;
  xSemaphoreGive(mtx);
  Preferences p;
  p.begin("voltol-cloud", false);
  p.remove("token");
  p.end();
}
bool configured() { return token[0] != 0; }
bool online() { return okAt && millis() - okAt < 10000; }
int lastHttp() { return httpCode; }
int32_t ageMs() { return okAt ? (int32_t)(millis() - okAt) : -1; }

static bool clockOk() { return time(nullptr) > 1735689600; }  // after 2025-01-01: certificates can be checked

// One HTTPS POST. Keeps the TLS session open between calls (keep-alive) so a
// telemetry upload costs ~30 ms instead of a new handshake every second.
static WiFiClientSecure *tls = nullptr;
static HTTPClient http;
static int post(const char *action, const String &body, String *reply) {
  if (!tls) {
    tls = new WiFiClientSecure();
    if (clockOk()) tls->setCACertBundle(CLOUD_CA_BUNDLE);
    else tls->setInsecure();  // only until the clock is set (first seconds after boot)
    tls->setTimeout(6);
  }
  http.setReuse(true);
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  if (!http.begin(*tls, String(BASE) + action)) return -1;
  char auth[72];
  xSemaphoreTake(mtx, portMAX_DELAY);
  snprintf(auth, sizeof auth, "Bearer %s", token);
  xSemaphoreGive(mtx);
  http.addHeader("Authorization", auth);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (reply && code > 0) *reply = http.getString();
  if (code <= 0) { http.end(); delete tls; tls = nullptr; }  // broken connection: next call starts fresh
  return code;
}

static void runCommands(const String &reply) {
  if (reply == "A" || reply == "C") {  // drag arm / cancel (same as v1.6.x)
    bool ok = true;
    if (cmdFn) cmdFn(reply == "A" ? "drag_arm" : "drag_cancel", "{}", ok);
    return;
  }
  if (!reply.length() || reply[0] != '{' || !cmdFn) return;
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, reply)) return;
  JsonArray cmds = doc["cmds"].as<JsonArray>();
  if (cmds.isNull()) return;
  for (JsonObject c : cmds) {
    String id = c["id"] | "";
    String name = c["cmd"] | "";
    String args;
    serializeJson(c["args"], args);
    bool ok = true;
    String msg = cmdFn(name, args, ok);
    if (!id.length()) continue;
    DynamicJsonDocument r(512);
    r["id"] = id;
    r["ok"] = ok;
    r["msg"] = msg.substring(0, 160);
    String body;
    serializeJson(r, body);
    post("cmd-result", body, nullptr);
  }
}

static void task(void *) {
  bool ntp = false;
  uint32_t last = 0;
  bool hadClock = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!net::online()) { if (tls) { http.end(); delete tls; tls = nullptr; } continue; }
    if (!ntp) { configTime(0, 0, "time.google.com", "time.cloudflare.com", "pool.ntp.org"); ntp = true; }
    if (!hadClock && clockOk()) { hadClock = true; if (tls) { http.end(); delete tls; tls = nullptr; } }  // reconnect with certificate checks
    if (!configured() || !payloadFn) continue;
    if (millis() - last < 500) continue;
    String payload;
    int rpm = 0;
    bool moving = false;
    payloadFn(payload, rpm, moving);
    if (!moving && millis() - last < 1000) continue;  // 2 per second while riding, 1 per second parked
    last = millis();
    String body;
    body.reserve(payload.length() + 48);
    body = "{\"type\":\"telemetry\",\"rpm\":" + String(rpm) + ",\"payload\":" + payload + "}";
    String reply;
    int code = post("ingest", body, &reply);
    httpCode = code;
    if (code == 200 || code == 201) { okAt = millis(); runCommands(reply); }
    else if (code == 401) vTaskDelay(pdMS_TO_TICKS(10000));  // token removed on the web: slow down
    else vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void begin(PayloadFn p, CmdFn c) {
  mtx = xSemaphoreCreateMutex();
  payloadFn = p;
  cmdFn = c;
  Preferences pr;
  pr.begin("voltol-cloud", true);
  String t = pr.getString("token", "");
  pr.end();
  if (validHex(t.c_str())) strlcpy(token, t.c_str(), sizeof token);
  xTaskCreatePinnedToCore(task, "cloud", 12288, nullptr, 1, nullptr, 0);
}

void serialLoop() {
  static char line[84];
  static size_t len = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      line[len] = 0;
      if (len == 76 && !strncmp(line, "CLOUD_TOKEN ", 12)) Serial.println(setToken(line + 12) ? "CLOUD_TOKEN OK" : "CLOUD_TOKEN BAD");
      len = 0;
    } else if (len < sizeof line - 1) line[len++] = ch;
  }
}
String statusJson() {
  String o = "{\"configured\":";
  o += configured() ? "true" : "false";
  o += ",\"online\":";
  o += online() ? "true" : "false";
  o += ",\"http\":" + String(httpCode) + ",\"age\":" + String(ageMs()) + "}";
  return o;
}
}  // namespace cloud
