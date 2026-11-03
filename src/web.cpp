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
static LiveJsonFn liveJson;
static TimeFn setTime;
static char ssidBuf[24];

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>VOLTOL Monitor</title>
<style>body{margin:0;font-family:system-ui,sans-serif;background:#05070A;color:#E8EDF2}
header{background:#0B1016;padding:12px 16px;font-weight:800;letter-spacing:1px}header b{color:#FFC400}
main{padding:12px 16px;max-width:720px}.g{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.c{background:#0F161E;border-radius:10px;padding:10px}.l{font-size:11px;color:#6E7C89}.v{font-size:22px;font-weight:800}
a{color:#4FC3FF}table{width:100%;border-collapse:collapse;font-size:14px}td{padding:6px 4px;border-bottom:1px solid #1A232D}
button{background:#1A222B;color:#E8EDF2;border:0;border-radius:8px;padding:8px 12px}h3{margin:18px 0 8px}</style></head>
<body><header><b>&#9889;</b> VOLTOL Monitor</header><main>
<div class="g" id="live"></div><p class="l" id="st"></p>
<h3>Log files</h3><table id="files"></table>
<h3>Firmware update</h3><form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="fw" accept=".bin"> <button>Upload</button></form>
<p class="l">Use the *_APP_UPDATE.bin file. Settings and logs are kept.</p></main>
<script>
fetch('/api/time?t='+Date.now()+'&tz='+(-new Date().getTimezoneOffset()));
const L=[['km/h','kmh'],['RPM','rpm'],['Volt','volt'],['Amp','amp'],['kW','kw'],['SOC %','soc'],['Ctrl C','tc'],['Motor C','tm'],['Trip km','trip']];
async function live(){try{const j=await (await fetch('/api/live')).json();
document.getElementById('live').innerHTML=L.map(x=>`<div class="c"><div class="l">${x[0]}</div><div class="v">${j[x[1]]}</div></div>`).join('');
document.getElementById('st').textContent=(j.linked?'Controller connected':'No controller data')+' · '+j.status+' · fault: '+j.fault+' · '+j.hz+' Hz';}catch(e){}}
async function files(){const j=await (await fetch('/api/files')).json();
document.getElementById('files').innerHTML=j.map(f=>`<tr><td><a href="/dl?f=${encodeURIComponent(f.n)}">${f.n}</a></td><td>${(f.s/1024).toFixed(1)} KB</td><td><button onclick="del('${f.n}')">delete</button></td></tr>`).join('');}
async function del(n){if(confirm('Delete '+n+'?')){await fetch('/del?f='+encodeURIComponent(n));files();}}
live();files();setInterval(live,700);
</script></body></html>)HTML";

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

void begin(LiveJsonFn live, TimeFn time) {
  liveJson = live;
  setTime = time;
  srv.on("/", [] { srv.send_P(200, "text/html", PAGE); });
  srv.on("/api/live", [] { srv.send(200, "application/json", liveJson()); });
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
