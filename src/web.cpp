// Wi-Fi hotspot "VOLTOL-xxxx" (password in config.h) with a small web page:
//   live values, download/delete of every log CSV and parameter backup,
//   clock sync from the phone, and firmware update (.bin upload).
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include "config.h"
#include "logger.h"
#include "web.h"

namespace web {
static WebServer srv(80);
static bool on = false;
static ApiFn apiFn;
static TimeFn setTime;
static char ssidBuf[24];

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
        bool ok = !Update.hasError();
        srv.send(200, "text/html", ok ? "<h2>Update OK - restarting</h2>" : "<h2>Update FAILED</h2>");
        if (ok) { delay(800); ESP.restart(); }
      },
      [] {
        HTTPUpload &u = srv.upload();
        if (u.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
        else if (u.status == UPLOAD_FILE_WRITE) Update.write(u.buf, u.currentSize);
        else if (u.status == UPLOAD_FILE_END) Update.end(true);
      });
}
void enable(bool en) {
  if (en == on) return;
  on = en;
  if (en) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(ssidBuf, sizeof ssidBuf, "VOLTOL-%02X%02X", mac[4], mac[5]);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssidBuf, AP_PASSWORD);
    srv.begin();
  } else {
    srv.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}
void loop() { if (on) srv.handleClient(); }
const char *ssid() { return on ? ssidBuf : "off"; }
}  // namespace web
