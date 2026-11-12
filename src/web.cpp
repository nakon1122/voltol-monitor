// Wi-Fi hotspot "VOTOL-xxxx" (password in config.h) with a small web page:
//   live values, download/delete of every log CSV and parameter backup,
//   clock sync from the phone, and firmware update (.bin upload).
// Realtime: port 81 /live is a Server-Sent-Events stream. Every new controller
// frame is pushed to the open pages at once (no request per update), from its
// own task so it never waits for the screen or the port-80 page server.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include "config.h"
#include "logger.h"
#include "web.h"
#include "ota.h"
#include "net.h"

namespace web {
static WebServer srv(80);
static bool on = false;
static ApiFn apiFn;
static TimeFn setTime;
static char ssidBuf[24];
static bool srvStarted = false;
static String uploadErr;

// ---- realtime stream (SSE, port 81) ----
static WiFiServer sse(81);
static const int SSE_MAX = 4;
static WiFiClient sseCl[SSE_MAX];
static LiveFn liveFn;
static FrameFn frameFn;
static volatile int sseCount = 0;
static void sseTask(void *) {
  uint32_t lastSend = 0, lastFrames = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5));
    if (!srvStarted) continue;
    WiFiClient c = sse.available();
    if (c) {
      // read the request head (GET /live ...) then answer with the stream headers
      uint32_t t0 = millis(); String head;
      while (c.connected() && millis() - t0 < 300 && head.indexOf("\r\n\r\n") < 0) { while (c.available()) head += (char)c.read(); delay(1); }
      int slot = -1;
      for (int i = 0; i < SSE_MAX; i++) if (!sseCl[i] || !sseCl[i].connected()) { slot = i; break; }
      if (slot < 0) { c.stop(); }
      else {
        c.setNoDelay(true);
        c.print("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                "Access-Control-Allow-Origin: *\r\nConnection: keep-alive\r\n\r\nretry: 1000\n\n");
        sseCl[slot] = c;
      }
    }
    int n = 0;
    for (int i = 0; i < SSE_MAX; i++) if (sseCl[i] && sseCl[i].connected()) n++;
    sseCount = n;
    if (!n || !liveFn) continue;
    uint32_t now = millis(), fr = frameFn ? frameFn() : 0;
    // a new controller frame (at most every 30 ms), else 10 Hz for GPS / drag timer
    if (!((fr != lastFrames && now - lastSend >= 30) || now - lastSend >= 100)) continue;
    lastFrames = fr; lastSend = now;
    String msg = "data: " + liveFn() + "\n\n";
    for (int i = 0; i < SSE_MAX; i++) {
      if (!sseCl[i] || !sseCl[i].connected()) continue;
      if (sseCl[i].write((const uint8_t *)msg.c_str(), msg.length()) != msg.length()) sseCl[i].stop();
    }
  }
}
void setLive(LiveFn live, FrameFn frames) { liveFn = live; frameFn = frames; }
int liveClients() { return sseCount; }

#include "web_page.h"  // gzip of assets/web/index.html (tools/embed_web.py)

static void listDir(fs::FS &F, const char *dir, String &out) {
  File d = F.open(dir);
  if (!d) return;
  for (File e = d.openNextFile(); e; e = d.openNextFile()) {
    String n = String(dir) + (strcmp(dir, "/") ? "/" : "") + e.name();
    if (e.isDirectory()) { listDir(F, n.c_str(), out); continue; }
    if (out.length() > 1) out += ",";
    out += "{\"n\":\"" + n + "\",\"s\":" + String((unsigned)e.size()) + "}";
  }
}

static void sendPage() {
  srv.sendHeader("Content-Encoding", "gzip");
  srv.sendHeader("Cache-Control", "no-cache");
  srv.send_P(200, "text/html", (const char *)WEB_PAGE_GZ, sizeof WEB_PAGE_GZ);
}
static void apiRoute() {
  int code = 200;
  String out = apiFn(srv.uri(), [](const char *k) { return srv.arg(k); }, code);
  srv.send(code, "application/json", out);
}
void begin(ApiFn api, TimeFn time) {
  apiFn = api;
  setTime = time;
  srv.on("/", sendPage);
  srv.onNotFound([] {
    if (srv.uri().startsWith("/api/")) apiRoute();
    else sendPage();
  });
  srv.on("/api/time", [] {
    if (setTime) setTime(strtoull(srv.arg("t").c_str(), nullptr, 10), srv.arg("tz").toInt());
    srv.send(200, "text/plain", "ok");
  });
  srv.on("/api/files", [] {
    String out = "[";
    listDir(logger::fs(), "/", out);
    srv.send(200, "application/json", out + "]");
  });
  srv.on("/dl", [] {
    String n = srv.arg("f");
    File f = logger::fs().open(n, FILE_READ);
    if (!f || n.indexOf("..") >= 0) { srv.send(404, "text/plain", "not found"); return; }
    String base = n.substring(n.lastIndexOf('/') + 1);
    srv.sendHeader("Content-Disposition", "attachment; filename=\"" + base + "\"");
    srv.streamFile(f, base.endsWith(".csv") ? "text/csv" : "application/octet-stream");
    f.close();
  });
  srv.on("/del", [] {
    String n = srv.arg("f");
    bool ok = n.indexOf("..") < 0 && logger::fs().remove(n);
    srv.send(ok ? 200 : 404, "text/plain", ok ? "deleted" : "not found");
  });
  srv.on(
      "/update", HTTP_POST,
      [] {
        const char *st = "<meta name=viewport content='width=device-width'><body style='background:#05070A;color:#E8EDF2;font-family:sans-serif;padding:20px'>";
        if (uploadErr.length()) {
          srv.send(200, "text/html", String(st) + "<h2 style='color:#FF3B30'>Update refused</h2><p>" + uploadErr + "</p><a href='/' style='color:#4FC3FF'>Back</a>");
          return;
        }
        bool ok = !Update.hasError();
        srv.send(200, "text/html", String(st) + (ok ? "<h2>Update OK - restarting</h2><p>Reconnect in 20 s.</p>" : "<h2 style='color:#FF3B30'>Update FAILED</h2><a href='/' style='color:#4FC3FF'>Back</a>"));
        if (ok) { ota::markPending(); delay(800); ESP.restart(); }
      },
      [] {
        HTTPUpload &u = srv.upload();
        if (u.status == UPLOAD_FILE_START) {
          const char *why = ota::uploadBlocked();
          uploadErr = why ? why : "";
          if (!why) Update.begin(UPDATE_SIZE_UNKNOWN);
        } else if (uploadErr.length()) return;
        else if (u.status == UPLOAD_FILE_WRITE) Update.write(u.buf, u.currentSize);
        else if (u.status == UPLOAD_FILE_END) Update.end(true);
        else if (u.status == UPLOAD_FILE_ABORTED) Update.abort();
      });
}
// Hotspot on/off. The internet Wi-Fi (net.cpp) runs at the same time; the
// web page is then also reachable from the home network.
void enable(bool en) {
  if (en == on && srvStarted) return;
  on = en;
  net::hotspot(en);  // net.cpp keeps hotspot + internet Wi-Fi working together
  strlcpy(ssidBuf, net::apSsid(), sizeof ssidBuf);
  if (!srvStarted) {
    srv.begin();
    sse.begin();
    xTaskCreatePinnedToCore(sseTask, "sse", 6144, nullptr, 2, nullptr, 1);
    srvStarted = true;
  }
}
void loop() { if (srvStarted) srv.handleClient(); }
const char *ssid() { return on ? ssidBuf : "off"; }
}  // namespace web
