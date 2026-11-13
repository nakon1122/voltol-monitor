// Smart BMS link over Bluetooth (see bmslink.h). The protocol work is in
// bms_engine.h; this file only moves bytes between the radio and the engine.
#include "bmslink.h"
#include <Arduino.h>
#include <string.h>

#ifndef VOTOL_NO_BLE
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "bms_engine.h"
#include "net.h"

namespace bmslink {
// ---- shared between the task and the main loop (under mtx)
struct Shared {
  uint8_t link = bms::L_OFF, brand = 0, savedBrand = 0, savedType = 0;
  char name[24] = "", mac[18] = "";
  int8_t rssi = 0;
  uint32_t sampleAt = 0;
  jk::Sample s;
  bool have = false;
  float pv[bms::MAX_PARAMS] = {0};
  uint64_t pvHave = 0;
  bool settingsDone = false;
  uint32_t setSeq = 0;
  bms::Found found[8];
  int foundN = 0;
  bool reading = false, writing = false;
  uint32_t readAt = 0;
  char msg[96] = "";
  uint8_t msgKind = 0;
  // requests
  bool reqScan = false, reqForget = false, reqRead = false, reqConnect = false, paused = false;
  bms::Found want;
  int wN = 0, wIdx[16];
  float wVal[16];
  char wBy[8] = "";
  bool resultReady = false;
  WriteResult result;
};
static Shared sh;
static SemaphoreHandle_t mtx = nullptr;
struct Guard { Guard() { xSemaphoreTake(mtx, portMAX_DELAY); } ~Guard() { xSemaphoreGive(mtx); } };
static TaskHandle_t taskH = nullptr;
static volatile bool radioOn = false;

// ---- notifications: the NimBLE callback only queues the bytes
struct Chunk { uint16_t n; uint8_t d[256]; };
static QueueHandle_t q = nullptr;
static void onNotify(NimBLERemoteCharacteristic *, uint8_t *p, size_t n, bool) {
  static Chunk c;
  if (n > sizeof c.d) n = sizeof c.d;
  c.n = n;
  memcpy(c.d, p, n);
  xQueueSend(q, &c, 0);
}

static void save() {
  Preferences p;
  p.begin("votol-bms", false);
  p.putString("mac", sh.mac);
  p.putString("name", sh.name);
  p.putUChar("type", sh.savedType);
  p.putUChar("brand", sh.savedBrand);
  p.end();
}
static void radio() {
  if (radioOn) return;
  net::btCoex(true);  // Wi-Fi modem sleep must be on before the radio is shared
  if (!NimBLEDevice::getInitialized()) {
    NimBLEDevice::init("VOTOL Setup");
    NimBLEDevice::setMTU(247);
  }
  radioOn = true;
}
static uint8_t guessBrand(const std::string &name, NimBLEAdvertisedDevice &d) {
  String u = name.c_str();
  u.toUpperCase();
  if (u.startsWith("JK")) return bms::BR_JK;
  if (u.startsWith("ANT")) return bms::BR_ANT;
  if (u.startsWith("JBD") || u.startsWith("XIAOXIANG") || u.startsWith("SP")) return bms::BR_JBD;
  if (u.startsWith("DL-") || u.startsWith("DALY")) return bms::BR_DALY;
  if (d.haveServiceUUID() && d.isAdvertisingService(NimBLEUUID(uint16_t(0xff00)))) return bms::BR_JBD;
  return 0;
}
static bool candidate(const std::string &name, NimBLEAdvertisedDevice &d) {
  String u = name.c_str();
  u.toUpperCase();
  return u.startsWith("JK") || u.startsWith("ANT") || u.startsWith("JBD") || u.startsWith("XIAOXIANG") ||
         u.startsWith("DL-") || u.startsWith("DALY") ||
         (d.haveServiceUUID() && (d.isAdvertisingService(NimBLEUUID(uint16_t(0xffe0))) ||
                                  d.isAdvertisingService(NimBLEUUID(uint16_t(0xff00))) ||
                                  d.isAdvertisingService(NimBLEUUID(uint16_t(0xfff0)))));
}

static void task(void *) {
  NimBLEClient *client = nullptr;
  NimBLERemoteCharacteristic *chMain = nullptr, *chJbd = nullptr, *chDaly = nullptr;
  static bmse::Engine eng;
  uint32_t lastTry = 0, linkedAt = 0, rssiAt = 0, lastSeq = 0;
  bool everOnline = false, wasConnected = false;
  WriteResult job{};
  bool jobOn = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10));
    uint32_t now = millis();
    bool doScan, doForget, doRead, doConnect, paused;
    bms::Found want;
    char mac[18];
    uint8_t type, savedBrand;
    {
      Guard g;
      doScan = sh.reqScan; sh.reqScan = false;
      doForget = sh.reqForget; sh.reqForget = false;
      doRead = sh.reqRead; sh.reqRead = false;
      doConnect = sh.reqConnect; sh.reqConnect = false;
      want = sh.want;
      paused = sh.paused;
      strlcpy(mac, sh.mac, sizeof mac);
      type = sh.savedType; savedBrand = sh.savedBrand;
    }
    auto drop = [&]() {
      if (client && client->isConnected()) client->disconnect();
      chMain = chJbd = chDaly = nullptr;
      xQueueReset(q);
    };
    if (doForget) {
      drop();
      Guard g;
      sh.mac[0] = sh.name[0] = 0; sh.savedBrand = sh.savedType = 0;
      sh.have = false; sh.settingsDone = false; sh.pvHave = 0; sh.link = bms::L_OFF; sh.brand = 0;
      save();
      everOnline = false;
      continue;
    }
    if (doConnect) {
      drop();
      Guard g;
      strlcpy(sh.mac, want.mac, sizeof sh.mac);
      strlcpy(sh.name, want.name, sizeof sh.name);
      sh.savedType = want.type; sh.savedBrand = want.brand;
      sh.have = false; sh.settingsDone = false; sh.pvHave = 0; sh.brand = 0;
      sh.link = bms::L_CONNECT;
      save();
      everOnline = false;
      lastTry = 0;
      continue;
    }
    if (paused) { drop(); vTaskDelay(pdMS_TO_TICKS(300)); continue; }
    if (doScan) {
      radio();
      drop();
      { Guard g; sh.link = bms::L_SCAN; sh.foundN = 0; }
      NimBLEScan *sc = NimBLEDevice::getScan();
      sc->setActiveScan(true);
      sc->clearResults();
      NimBLEScanResults r = sc->start(6, false);
      bms::Found list[8];
      int n = 0;
      for (int i = 0; i < r.getCount(); i++) {
        NimBLEAdvertisedDevice d = r.getDevice(i);
        std::string nm = d.getName();
        if (!candidate(nm, d)) continue;
        bms::Found f;
        strlcpy(f.name, nm.length() ? nm.c_str() : "BMS", sizeof f.name);
        strlcpy(f.mac, d.getAddress().toString().c_str(), sizeof f.mac);
        f.rssi = (int8_t)d.getRSSI();
        f.brand = guessBrand(nm, d);
        f.type = d.getAddressType();
        if (n < 8) list[n++] = f;
        else {  // keep the strongest
          int lo = 0;
          for (int k = 1; k < 8; k++) if (list[k].rssi < list[lo].rssi) lo = k;
          if (f.rssi > list[lo].rssi) list[lo] = f;
        }
      }
      sc->clearResults();
      for (int i = 1; i < n; i++) for (int k = i; k > 0 && list[k].rssi > list[k - 1].rssi; k--) { bms::Found t = list[k]; list[k] = list[k - 1]; list[k - 1] = t; }
      Guard g;
      memcpy(sh.found, list, sizeof list);
      sh.foundN = n;
      if (!sh.mac[0] && n == 1) {  // exactly one BMS around and none chosen yet: take it
        strlcpy(sh.mac, list[0].mac, sizeof sh.mac);
        strlcpy(sh.name, list[0].name, sizeof sh.name);
        sh.savedType = list[0].type; sh.savedBrand = list[0].brand;
        save();
        sh.link = bms::L_CONNECT;
        lastTry = 0;
      } else sh.link = sh.mac[0] ? (everOnline ? bms::L_LOST : bms::L_CONNECT) : bms::L_OFF;
      continue;
    }
    if (!mac[0]) continue;
    radio();
    if (!client) { client = NimBLEDevice::createClient(); client->setConnectTimeout(8); }
    if (!client->isConnected()) {
      if (wasConnected) {  // link dropped: NimBLE freed the remote objects
        wasConnected = false;
        chMain = chJbd = chDaly = nullptr;
        Guard g;
        sh.link = bms::L_LOST;
      }
      if (lastTry && now - lastTry < 10000) continue;
      lastTry = now;
      { Guard g; if (!everOnline) sh.link = bms::L_CONNECT; }
      xQueueReset(q);
      if (!client->connect(NimBLEAddress(std::string(mac), type))) {
        Guard g;
        sh.link = everOnline ? bms::L_LOST : bms::L_CONNECT;
        continue;
      }
      // discover once (refresh), then use the cached objects only
      client->getServices(true);
      if (auto svcs = client->getServices(false))
        for (auto sv : *svcs) {
          bool ffe0 = sv->getUUID() == NimBLEUUID(uint16_t(0xffe0)), ff00 = sv->getUUID() == NimBLEUUID(uint16_t(0xff00)),
               fff0 = sv->getUUID() == NimBLEUUID(uint16_t(0xfff0));
          if (!ffe0 && !ff00 && !fff0) continue;  // never touch other services (firmware update etc.)
          auto cs = sv->getCharacteristics(true);
          if (!cs) continue;
          for (auto cc : *cs) {
            bool ffe1 = cc->getUUID() == NimBLEUUID(uint16_t(0xffe1));
            if (ffe0 && !ffe1 && sv->getCharacteristic(NimBLEUUID(uint16_t(0xffe1)))) continue;
            if (cc->canNotify() || cc->canIndicate()) {
              bool ok = false;
              for (int a = 0; a < 2 && !ok; a++) { ok = cc->subscribe(cc->canNotify(), onNotify, true); if (!ok) delay(200); }
            }
            if (ffe0 && ffe1 && (cc->canWrite() || cc->canWriteNoResponse())) chMain = cc;
            if (ff00 && cc->getUUID() == NimBLEUUID(uint16_t(0xff02))) chJbd = cc;
            if (fff0 && cc->getUUID() == NimBLEUUID(uint16_t(0xfff2))) chDaly = cc;
          }
        }
      if (!chMain && !chJbd && !chDaly) { client->disconnect(); continue; }
      eng.reset(chMain, chJbd, chDaly, savedBrand == bms::BR_ANT, 0);  // the brand is worked out from the answers
      wasConnected = true;
      linkedAt = now;
      lastSeq = 0;
      continue;
    }
    // ---- connected
    Chunk c;
    while (xQueueReceive(q, &c, 0) == pdTRUE) eng.feed(c.d, c.n, millis());
    if (doRead) eng.readSettings();
    {  // write job from the screen or the cloud
      int n = 0, idx[16];
      float val[16];
      char by[8] = "";
      {
        Guard g;
        if (sh.wN && !eng.writing) {
          n = sh.wN; memcpy(idx, sh.wIdx, sizeof idx); memcpy(val, sh.wVal, sizeof val); strlcpy(by, sh.wBy, sizeof by);
          sh.wN = 0;
        }
      }
      if (n) {
        job = WriteResult{};
        strlcpy(job.by, by, sizeof job.by);
        job.n = n;
        for (int k = 0; k < n; k++) { job.idx[k] = idx[k]; job.before[k] = eng.pv[idx[k]]; eng.queueWrite(idx[k], val[k]); }
        eng.startWrites(millis());
        jobOn = true;
        Guard g;
        sh.writing = true;
        snprintf(sh.msg, sizeof sh.msg, "Writing...");
        sh.msgKind = 0;
      }
    }
    bmse::Out o;
    for (int k = 0; k < 4 && eng.next(millis(), o); k++) {
      NimBLERemoteCharacteristic *w = o.ch == bmse::CH_JBD ? (chJbd ? chJbd : chMain) : o.ch == bmse::CH_DALY ? (chDaly ? chDaly : chMain) : chMain;
      if (!w || !o.n) continue;
      bool resp = o.resp == bmse::R_WITH ? w->canWrite() : o.resp == bmse::R_WITHOUT ? !w->canWriteNoResponse() : !w->canWriteNoResponse();
      w->writeValue(o.d, o.n, resp);
    }
    if (jobOn && eng.writeDone) {
      jobOn = false;
      job.ok = eng.writeOk;
      strlcpy(job.msg, eng.writeMsg, sizeof job.msg);
      for (int k = 0; k < job.n; k++) job.after[k] = eng.pv[job.idx[k]];
      Guard g;
      sh.result = job;
      sh.resultReady = true;
      sh.writing = false;
      strlcpy(sh.msg, eng.writeMsg, sizeof sh.msg);
      sh.msgKind = eng.writeOk ? 1 : 2;
    }
    if (now - rssiAt > 5000) { rssiAt = now; int r = client->getRssi(); Guard g; sh.rssi = (int8_t)r; }
    // nothing for 15 s: drop the link and reconnect
    if (now - linkedAt > 15000 && (!eng.have || now - eng.sampleAt > 15000)) {
      client->disconnect();
      lastTry = now - 7000;
      continue;
    }
    Guard g;
    if (eng.brand && !sh.savedBrand) { sh.savedBrand = eng.brand; save(); }
    sh.brand = eng.brand;
    if (eng.have) { sh.s = eng.s; sh.have = true; sh.sampleAt = eng.sampleAt; }
    if (eng.have && now - eng.sampleAt < 6000) { sh.link = bms::L_ONLINE; everOnline = true; }
    if (eng.settingsSeq != lastSeq || eng.settingsDone != sh.settingsDone) {
      lastSeq = eng.settingsSeq;
      memcpy(sh.pv, eng.pv, sizeof sh.pv);
      sh.pvHave = eng.pvHave;
      sh.settingsDone = eng.settingsDone;
      sh.setSeq++;
      if (eng.settingsDone) sh.reading = false;
    }
    if (sh.reading && now - sh.readAt > 12000) sh.reading = false;
  }
}

void begin() {
  if (mtx) return;
  mtx = xSemaphoreCreateMutex();
  q = xQueueCreate(16, sizeof(Chunk));
  Preferences p;
  p.begin("votol-bms", true);
  strlcpy(sh.mac, p.getString("mac", "").c_str(), sizeof sh.mac);
  strlcpy(sh.name, p.getString("name", "").c_str(), sizeof sh.name);
  sh.savedType = p.getUChar("type", 0);
  sh.savedBrand = p.getUChar("brand", 0);
  p.end();
  if (sh.mac[0]) sh.link = bms::L_CONNECT;
  xTaskCreatePinnedToCore(task, "bms", 8192, nullptr, 1, &taskH, 0);
}
bool enabled() { Guard g; return sh.mac[0] || sh.link == bms::L_SCAN; }
void scan() { Guard g; sh.reqScan = true; sh.link = bms::L_SCAN; }
void connect(const bms::Found &f) { Guard g; sh.want = f; sh.reqConnect = true; sh.link = bms::L_CONNECT; strlcpy(sh.name, f.name, sizeof sh.name); }
void forget() { Guard g; sh.reqForget = true; }
void readSettings() { Guard g; sh.reqRead = true; sh.reading = true; sh.readAt = millis(); }
const char *write(const int *idx, const float *val, int n, const char *by) {
  Guard g;
  if (sh.link != bms::L_ONLINE) return "BMS not connected";
  if (!sh.settingsDone) return "read the BMS settings first";
  if (sh.writing || sh.wN) return "busy";
  if (n < 1 || n > 16) return "nothing to write";
  float cur[bms::MAX_PARAMS];
  memcpy(cur, sh.pv, sizeof cur);
  for (int k = 0; k < n; k++) if (idx[k] >= 0 && idx[k] < bms::MAX_PARAMS) cur[idx[k]] = val[k];
  for (int k = 0; k < n; k++) {
    const char *why = bms::check(sh.brand, cur, idx[k], val[k]);
    if (why) return why;
  }
  sh.wN = n;
  memcpy(sh.wIdx, idx, sizeof(int) * n);
  memcpy(sh.wVal, val, sizeof(float) * n);
  strlcpy(sh.wBy, by, sizeof sh.wBy);
  return nullptr;
}
bool poll(BmsUi &ui) {
  static uint32_t seenSeq = 0;
  Guard g;
  uint32_t now = millis();
  ui.link = sh.link;
  ui.brand = sh.brand ? sh.brand : sh.savedBrand;
  strlcpy(ui.name, sh.name, sizeof ui.name);
  strlcpy(ui.mac, sh.mac, sizeof ui.mac);
  ui.rssi = sh.rssi;
  ui.age = sh.sampleAt ? (now - sh.sampleAt) / 1000.0f : 0;
  ui.have = sh.have;
  memcpy(ui.found, sh.found, sizeof ui.found);
  ui.foundN = sh.foundN;
  if (ui.sel >= ui.foundN) ui.sel = -1;
  if (sh.have) {
    const jk::Sample &s = sh.s;
    bms::Live &L = ui.live;
    L.volt = s.voltage; L.amp = s.current; L.soc = s.soc; L.remainAh = s.capacity; L.fullAh = s.full; L.cycles = s.cycles;
    L.nTemp = 2; L.temp[0] = s.temp1; L.temp[1] = s.temp2; L.mos = s.mos;
    L.nCells = s.count > 32 ? 32 : s.count;
    for (int i = 0; i < 32; i++) L.cell[i] = i < L.nCells ? s.cells[i] : 0;
    L.chg = s.charge; L.dsg = s.discharge;
    L.bal = (ui.brand == bms::BR_JK || ui.brand == bms::BR_DALY) && (sh.pvHave >> 2 & 1) && sh.pv[2] > 0.5f;
    L.balMask = 0;
    L.alarms = bms::alarms(L, ui.brand, sh.pv, sh.pvHave);
  }
  ui.busy = sh.writing || sh.reading;
  if (sh.msg[0]) { strlcpy(ui.msg, sh.msg, sizeof ui.msg); ui.msgKind = sh.msgKind; }
  bool changed = false;
  if (sh.setSeq != seenSeq) {
    seenSeq = sh.setSeq;
    bool editing = ui.pHave && ui.changed() && !sh.resultReady;
    memcpy(ui.pv, sh.pv, sizeof ui.pv);
    if (!editing) memcpy(ui.pe, sh.pv, sizeof ui.pe);
    ui.pHave = sh.settingsDone;
    changed = true;
  }
  return changed;
}
bool takeWrite(WriteResult &r) {
  Guard g;
  if (!sh.resultReady) return false;
  r = sh.result;
  sh.resultReady = false;
  return true;
}
bool busy() { return radioOn; }
void pause(bool on) { Guard g; sh.paused = on; }
}  // namespace bmslink

#else  // ---------------------------------------------------- no Bluetooth
namespace bmslink {
void begin() {}
bool enabled() { return false; }
void scan() {}
void connect(const bms::Found &) {}
void forget() {}
void readSettings() {}
const char *write(const int *, const float *, int, const char *) { return "this board has no Bluetooth"; }
bool poll(BmsUi &ui) { ui.link = bms::L_NOBLE; ui.have = false; return false; }
bool takeWrite(WriteResult &) { return false; }
bool busy() { return false; }
void pause(bool) {}
}  // namespace bmslink
#endif
