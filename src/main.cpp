// VOLTOL Monitor - dashboard, logger, 402 m timer and parameter tuner for
// VOTOL EM-series controllers on the ESP32-3248S035 (CYD 3.5").
#include <Arduino.h>
#include <ArduinoJson.h>  // before i18n.h: its T() macro clashes with template names
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <sys/time.h>
#include <cmath>
#include <functional>
#include "config.h"
#include "display.h"
#include "gps.h"
#include "link.h"
#include "logger.h"
#include "ota.h"
#include "ride.h"
#include "ui.h"
#include "web.h"
#include "net.h"
#include "cloud.h"
#include "blesetup.h"

static UiState S;
static Settings cfg;
static ride::DragTimer drag;
static SemaphoreHandle_t lock;
static volatile bool needFull = true;
#define LOCK() xSemaphoreTake(lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(lock)

// ------------------------------------------------------------------ time ----
static bool timeKnown = false;
static int64_t epochAtBootMs = 0;  // local time (tz applied) at millis() == 0
static void setClock(uint64_t epochMs, int tzMin) {
  epochAtBootMs = (int64_t)epochMs + (int64_t)tzMin * 60000 - (int64_t)millis();
  timeKnown = true;
}
// "2026-11-03 18:42:01.250" or "+123.250" (seconds since power-on)
static void stamp(char *out, int n, uint32_t ms) {
  if (!timeKnown) { snprintf(out, n, "+%lu.%03lu", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000)); return; }
  int64_t t = epochAtBootMs + ms;
  time_t sec = (time_t)(t / 1000);
  struct tm tm;
  gmtime_r(&sec, &tm);
  snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
           tm.tm_min, tm.tm_sec, (int)(t % 1000));
}
static void shortWhen(char *out, int n) {  // "03 Nov 18:42"
  if (!timeKnown) { snprintf(out, n, "boot+%lus", (unsigned long)(millis() / 1000)); return; }
  static const char *M[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  time_t sec = (time_t)((epochAtBootMs + millis()) / 1000);
  struct tm tm;
  gmtime_r(&sec, &tm);
  snprintf(out, n, "%02d %s %02d:%02d", tm.tm_mday, M[tm.tm_mon], tm.tm_hour, tm.tm_min);
}

// -------------------------------------------------------------- settings ----
static const uint8_t CFG_VER = 5;
static void loadSettings() {
  Preferences p;
  p.begin("voltol", true);
  if (p.getUChar("cfgv", 0) == CFG_VER) p.getBytes("cfg", &cfg, sizeof cfg);
  p.end();
}
static uint32_t cfgDirtyAt = 0;
static void saveSettings() {
  Preferences p;
  p.begin("voltol", false);
  p.putUChar("cfgv", CFG_VER);
  p.putBytes("cfg", &cfg, sizeof cfg);
  p.end();
}

// ------------------------------------------------------------------ trip ----
struct TripSnap {
  uint16_t id;
  char when[16];
  double distM, wh, ah, movingS, odoKm;
  float maxKmh, maxA;
  uint32_t secs;
};
static uint16_t tripId = 0;
static char tripWhen[16];
static double odoBase = 0;
static uint32_t tripStartMs = 0;
static TripRow snapRow(const TripSnap &t) {
  TripRow r;
  r.id = t.id; strlcpy(r.when, t.when, sizeof r.when);
  r.km = t.distM / 1000; r.maxKmh = t.maxKmh; r.avgKmh = t.movingS > 1 ? t.distM / t.movingS * 3.6 : 0;
  r.maxA = t.maxA; r.wh = t.wh; r.whKm = t.distM > 50 ? t.wh / (t.distM / 1000) : 0; r.secs = t.secs;
  return r;
}
static void saveTrip() {
  TripSnap t;
  t.id = tripId; strlcpy(t.when, tripWhen, sizeof t.when);
  t.distM = S.trip.distM; t.wh = S.trip.wh; t.ah = S.trip.ah; t.movingS = S.trip.movingS;
  t.maxKmh = S.trip.maxKmh; t.maxA = S.trip.maxA; t.secs = (millis() - tripStartMs) / 1000;
  t.odoKm = odoBase + S.trip.distM / 1000;
  Preferences p;
  p.begin("voltol", false);
  p.putBytes("trip", &t, sizeof t);
  p.end();
}
// Close the trip stored in flash (previous power-on or "NEW TRIP") and start a new one.
static void newTrip(bool fromBoot) {
  Preferences p;
  p.begin("voltol", false);
  TripSnap t;
  if (fromBoot && p.getBytes("trip", &t, sizeof t) == sizeof t) {
    odoBase = t.odoKm;
    if (t.distM > 50) logger::tripRow(snapRow(t));
  } else if (!fromBoot) {
    p.end();
    saveTrip();
    p.begin("voltol", false);
    if (p.getBytes("trip", &t, sizeof t) == sizeof t && t.distM > 50) logger::tripRow(snapRow(t));
    odoBase += S.trip.distM / 1000;
  }
  tripId = p.getUShort("tripid", 0) + 1;
  p.putUShort("tripid", tripId);
  p.end();
  S.trip = ride::Trip();
  tripStartMs = millis();
  shortWhen(tripWhen, sizeof tripWhen);
  logger::startTrip(tripId);
  saveTrip();
  S.tripN = logger::loadTrips(S.trips, 8);
}

// ------------------------------------------------------------------- GPS ----
// u-blox NAV-PVT preferred (speed accuracy + satellite time of each fix);
// any NMEA receiver works as a fallback.
static gps::Nmea nmea;
static gps::Ubx ubx;
static uint32_t gpsLast = 0, gpsCount = 0, gpsHzAt = 0, gpsBaudAt = 0, gpsCfgAt = 0;
static int gpsBaudIx = 0;
static const uint32_t GPS_BAUDS[3] = {115200, 9600, 38400};
static double gpsOffset = 1e18;  // millis-clock seconds minus GPS seconds (smallest seen)
static uint32_t lastItow = 0;
static bool gpsUsable() { return S.gpsFix && millis() - gpsLast < 1500; }
static bool useGps() { return cfg.speedSrc != 0 && gpsUsable(); }
// Good enough to time a drag run from GPS.
static bool gpsDragOk() { return gpsUsable() && S.gpsSats >= 6 && (!S.gpsUbx || S.gpsSAcc <= 1.0f); }
static bool dragOnGps() { return cfg.speedSrc == 1 || (cfg.speedSrc == 2 && (gpsUsable() || !vlink::alive())); }
// Hybrid launch: first two moving RPM samples, extrapolated back to 0 km/h.
static double hyT0 = -1, hyT1 = -1, hyV1 = 0, hyStill = -1;
static void armDrag() {
  double g = cfg.maxG10 / 10.0;
  drag.cfg = dragOnGps() ? ride::DragCfg::gps(cfg.rollout, g) : ride::DragCfg::rpm(cfg.rollout, g);
  drag.arm();
  S.dragWaitGps = drag.cfg.fitN > 2 && !gpsDragOk();
  hyT0 = hyT1 = hyStill = -1;
}
static void hybridRpm(double t, float kmh) {
  using ride::DragTimer;
  if (drag.cfg.fitN <= 2 || (drag.state != DragTimer::READY && drag.state != DragTimer::RUN)) return;
  if (kmh <= 0.05f) { if (hyT0 < 0) hyStill = t; return; }
  if (hyT0 < 0) { hyT0 = t; hyV1 = kmh; return; }
  if (hyT1 < 0) {
    hyT1 = t;
    double est = kmh > hyV1 ? hyT0 - hyV1 * (t - hyT0) / (kmh - hyV1) : hyT0;
    if (hyStill >= 0 && est < hyStill) est = hyStill;
    if (est > hyT0) est = hyT0;
    drag.launchHint(est);
  }
}
// One GPS speed sample (t = seconds on the millis clock).
static void gpsSample(double t, float kmh) {
  gpsLast = millis();
  gpsCount++;
  S.gpsSeen = true;
  S.gpsKmh = kmh < 0 ? 0 : kmh;
  bool onGps = drag.cfg.fitN > 2;  // armed in GPS mode
  S.dragWaitGps = onGps && drag.state == ride::DragTimer::ARMED && !gpsDragOk();
  if (onGps && (gpsDragOk() || drag.state == ride::DragTimer::RUN)) drag.feed(t, S.gpsKmh, S.live.amp, S.live.volt);
  // no controller: the GPS drives the dashboard and the trip
  if (useGps() || !vlink::alive()) {
    S.src = useGps();
    if (!vlink::alive() && S.gpsFix) {
      S.kmh = S.gpsKmh;
      S.speedOk = true;
      S.trip.add(t, S.kmh, 0, 0);
      S.odoKm = odoBase + S.trip.distM / 1000;
    }
  }
}
static void gpsClock(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t se) {
  if (timeKnown || y < 2024) return;
  struct tm tm = {};
  tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
  setenv("TZ", "UTC0", 1); tzset();
  setClock((uint64_t)mktime(&tm) * 1000, cfg.tzHours * 60);
}
static void gpsPoll() {
  while (Serial1.available()) {
    uint8_t b = (uint8_t)Serial1.read();
    if (ubx.push(b)) {
      const gps::Pvt &p = ubx.pvt;
      LOCK();
      S.gpsUbx = true;
      S.gpsFix = p.fixOk;
      S.gpsSats = p.numSV;
      S.gpsSAcc = p.sAccKmh;
      // GPS time of the fix mapped onto the millis clock: exact spacing
      // between samples, independent of serial / loop delays.
      double tg = p.iTOW / 1000.0, tm = millis() / 1000.0;
      if (p.iTOW < lastItow || tm - tg < gpsOffset - 5) gpsOffset = tm - tg;  // new week / first fix
      if (tm - tg < gpsOffset) gpsOffset = tm - tg;
      lastItow = p.iTOW;
      gpsSample(tg + gpsOffset, p.kmh);
      if (p.timeValid) gpsClock(p.year, p.mon, p.day, p.hour, p.min, p.sec);
      UNLOCK();
    }
    if (nmea.push((char)b) && !S.gpsUbx) {
      LOCK();
      S.gpsFix = nmea.fix.valid;
      S.gpsSats = nmea.fix.sats;
      S.gpsSAcc = 99;
      gpsSample(millis() / 1000.0, nmea.fix.kmh);
      uint32_t d = nmea.fix.dateDdmmyy, h = nmea.fix.utcHhmmss;
      if (nmea.fix.valid && d) gpsClock(2000 + d % 100, (d / 100) % 100, d / 10000, h / 10000, (h / 100) % 100, h % 100);
      UNLOCK();
    }
  }
  uint32_t now = millis();
  if (now - gpsHzAt >= 2000) {
    S.gpsHz = gpsCount * 1000.0f / (now - gpsHzAt);
    gpsCount = 0; gpsHzAt = now;
    if (now - gpsLast > 3000) { S.gpsFix = false; S.dragWaitGps = drag.cfg.fitN > 2 && drag.state == ride::DragTimer::ARMED; }
    // nothing heard: try the other baud rates, and re-send the u-blox setup
    if (now - gpsLast > 4000 && now - gpsBaudAt > 4000) {
      gpsBaudIx = (gpsBaudIx + 1) % 3;
      Serial1.updateBaudRate(GPS_BAUDS[gpsBaudIx]);
      gpsBaudAt = now;
    }
    if (now - gpsCfgAt > 15000 && (!S.gpsUbx || now - gpsLast > 4000 || S.gpsHz < (cfg.gpsRate ? 15 : 7))) {
      gpsCfgAt = now;
      vlink::requestGpsConfig(cfg.gpsRate ? 25 : 10);
      gpsBaudIx = 0;
      Serial1.updateBaudRate(GPS_BAUDS[0]);
    }
  }
}

// ----------------------------------------------------------- live frames ----
static ride::SecAgg sec;
static uint32_t secAt = 0, lastDetailMs = 0;
static logger::Row pre[50];
static int preN = 0, preHead = 0;
static uint32_t lastFaults = 0;
static int dragSaved = -1;  // 1 = current DONE result already saved

static void pushFaultRow(bool set, uint32_t code, const logger::Row &r) {
  memmove(&S.faults[1], &S.faults[0], sizeof(FaultRow) * 9);
  FaultRow &f = S.faults[0];
  char t[32];
  stamp(t, sizeof t, millis());
  const char *sp = strchr(t, ' ');
  strlcpy(f.when, sp ? sp + 1 : t, 9);
  f.set = set; f.code = code; f.kmh = r.kmh; f.amp = r.d.amp; f.volt = r.d.volt; f.rpm = r.d.rpm;
  if (S.faultN < 10) S.faultN++;
}

static void onLive(const votol::Live &d, uint32_t ms) {
  LOCK();
  S.live = d;
  double t = ms / 1000.0;
  float rpmKmh = ride::rpmToKmh(abs(d.rpm), cfg.circMm(), cfg.ratioX100 / 100.0f);
  bool g = useGps();
  S.src = g;
  S.kmh = g ? S.gpsKmh : rpmKmh;
  S.speedOk = true;
  S.kw = d.volt * d.amp / 1000;
  S.soc = ride::socFromVolt(d.volt, cfg.cells, cfg.cellEmptyMv / 1000.0f, cfg.cellFullMv / 1000.0f);
  S.trip.add(t, S.kmh, d.amp, d.volt);
  S.odoKm = odoBase + S.trip.distM / 1000;
  if (drag.cfg.fitN <= 2) drag.feed(t, rpmKmh, d.amp, d.volt);  // armed in RPM mode
  else hybridRpm(t, rpmKmh);                                      // GPS run: launch moment from RPM
  // wheel slip: wheel speed (RPM) vs ground speed (GPS)
  S.slipOk = gpsUsable() && S.gpsKmh > 10;
  if (S.slipOk) {
    S.slipPct = (rpmKmh - S.gpsKmh) / S.gpsKmh * 100;
    drag.slip(S.slipPct);
  }
  logger::Row row = {d, S.kmh, S.trip.distM};
  char ts[32];
  stamp(ts, sizeof ts, ms);
  // detail log at the chosen rate (0 = every frame)
  uint8_t rate = LOG_RATES[cfg.logRate < 5 ? cfg.logRate : 0];
  if (!rate || ms - lastDetailMs >= 1000u / rate) { logger::detail(ts, row); lastDetailMs = ms; }
  // 1-second summary + graph
  sec.add(S.kmh, d.rpm, d.amp, d.volt, d.tCtrl, d.tMotor, d.faults);
  if (ms - secAt >= 1000) {
    logger::second(ts, sec, S.trip.wh, S.trip.distM);
    if (S.gN == 240) { memmove(S.gKmh, S.gKmh + 1, 239 * 4); memmove(S.gAmp, S.gAmp + 1, 239 * 4); S.gN--; }
    S.gKmh[S.gN] = sec.avgKmh(); S.gAmp[S.gN] = sec.avgA(); S.gN++;
    sec = ride::SecAgg();
    secAt = ms;
  }
  // faults: log the change plus the last 5 s before it
  if (d.faults != lastFaults) {
    if (d.faults & ~lastFaults) {
      char pt[32];
      for (int i = 0; i < preN; i++) {
        const logger::Row &r = pre[(preHead + 50 - preN + i) % 50];
        snprintf(pt, sizeof pt, "%s", ts);
        logger::fault(pt, "PRE", r);
      }
      logger::fault(ts, "SET", row);
      pushFaultRow(true, d.faults, row);
    } else {
      logger::fault(ts, "CLEAR", row);
      pushFaultRow(false, lastFaults, row);
    }
    lastFaults = d.faults;
  }
  pre[preHead] = row;
  preHead = (preHead + 1) % 50;
  if (preN < 50) preN++;
  UNLOCK();
}

// ------------------------------------------------------------ parameters ----
static void onParams(const votol::ParamImage &p, bool ok) {
  LOCK();
  S.busy = false;
  if (ok) {
    S.params = p;
    memcpy(S.edit, p.b, votol::PARAM_BYTES);
    shortWhen(S.paramsWhen, sizeof S.paramsWhen);
    uint32_t sum = p.checkSum();
    if (!S.backupN || S.backups[0].sum != sum) {
      char w[16]; shortWhen(w, sizeof w);
      logger::saveBackup(p.b, sum, w, S.backupN ? S.backups[0].id : 0);
      S.backupN = logger::loadBackups(S.backups, 10);
    }
    snprintf(S.msg, sizeof S.msg, "Read OK");
    S.msgKind = 1;
  } else {
    snprintf(S.msg, sizeof S.msg, "No answer from controller");
    S.msgKind = 2;
  }
  needFull = true;
  UNLOCK();
}
static void onWrite(bool ok, const char *msg) {
  LOCK();
  S.busy = false;
  strlcpy(S.msg, msg, sizeof S.msg);
  S.msgKind = ok ? 1 : 2;
  if (ok) ui::go(PG_TUNE);
  needFull = true;
  UNLOCK();
}

// ------------------------------------------------------------------- web ----
// JSON API used by the web dashboard (src/web.cpp, assets/web/index.html).
struct J {
  String s;
  bool first = true;
  J &k(const char *key) { s += first ? "" : ","; first = false; s += "\""; s += key; s += "\":"; return *this; }
  J &n(const char *key, double v, int dec = 1) { k(key); char b[24]; snprintf(b, 24, "%.*f", dec, std::isfinite(v) ? v : 0.0); s += b; return *this; }
  J &i(const char *key, long v) { k(key); s += String(v); return *this; }
  J &b(const char *key, bool v) { k(key); s += v ? "true" : "false"; return *this; }
  J &t(const char *key, const char *v) {
    k(key); s += "\"";
    for (const char *p = v; *p; p++) { if (*p == '"' || *p == '\\') s += '\\'; s += *p; }
    s += "\""; return *this;
  }
  J &raw(const char *key, const String &v) { k(key); s += v; return *this; }
  String obj() { return "{" + s + "}"; }
};
static String splitsJson(const ride::DragResult &r) {
  String o = "[";
  for (int i = 0; i < ride::SPLITS; i++) {
    J j;
    j.b("done", r.s[i].done).n("t", r.s[i].t, 3).n("v", r.s[i].trapKmh, 1).n("a", r.s[i].peakA, 0);
    o += (i ? "," : "") + j.obj();
  }
  return o + "]";
}
static const char *const DRAG_STATE[6] = {"IDLE", "ARMED", "READY", "RUN", "DONE", "ABORTED"};
static String liveJsonLocked() {
  char f[96];
  votol::faultText(S.live.faults, f, sizeof f);
  J g; g.b("seen", S.gpsSeen).b("fix", S.gpsFix).b("ubx", S.gpsUbx).i("sats", S.gpsSats).n("hz", S.gpsHz).n("acc", S.gpsSAcc, 2);
  J tr; tr.n("km", S.trip.distM / 1000, 2).n("max", S.trip.maxKmh).n("avg", S.trip.avgKmh()).n("maxA", S.trip.maxA)
         .n("wh", S.trip.wh, 0).n("whkm", S.trip.whPerKm());
  J d; d.t("state", DRAG_STATE[drag.state]).n("el", drag.elapsed(millis() / 1000.0), 2).n("dist", drag.distance())
        .n("spd", drag.speedKmh()).b("gps", drag.cfg.fitN > 2).b("wait", S.dragWaitGps).b("roll", cfg.rollout)
        .raw("sp", splitsJson(drag.r)).n("peakA", drag.r.peakA, 0).n("peakKw", drag.r.peakKw).b("spin", drag.r.wheelspin)
        .b("spinCorr", drag.r.spinCorrected).n("maxSlip", drag.r.maxSlip, 0).b("slipOk", S.slipOk).n("slip", S.slipPct, 0)
        .b("hybrid", drag.cfg.fitN > 2 && S.linked)
        .b("haveBest", S.haveBest).raw("best", splitsJson(S.best)).t("bestWhen", S.bestWhen);
  char gs[2] = {S.linked ? "LMHS"[S.live.gear()] : '-', 0};
  J j;
  j.b("linked", S.linked).b("speedOk", S.speedOk).n("kmh", S.kmh).i("rpm", S.live.rpm).n("volt", S.live.volt)
   .n("amp", S.live.amp).n("kw", S.kw, 2).i("soc", S.soc).i("tc", S.live.tCtrl).i("tm", S.live.tMotor)
   .t("gear", gs)
   .t("status", votol::STATUS_NAMES[S.live.status < 8 ? S.live.status : 0]).i("faults", S.live.faults).t("fault", f)
   .n("hz", S.hz).i("baud", S.baud).i("src", S.src).raw("gps", g.obj()).raw("trip", tr.obj()).n("odo", S.odoKm, 1)
   .t("fw", S.fw).b("sd", S.sd).i("freeKB", S.fsFreeKB).i("maxKmh", cfg.maxKmh).i("maxAmp", cfg.maxAmp)
   .i("dash", cfg.dash).raw("drag", d.obj());
  return j.obj();
}
static String listsJson() {
  String t = "[", f = "[", r = "[", b = "[";
  for (int i = 0; i < S.tripN; i++) {
    J j; const TripRow &x = S.trips[i];
    j.i("id", x.id).t("when", x.when).n("km", x.km, 2).n("max", x.maxKmh).n("maxA", x.maxA).n("whkm", x.whKm);
    t += (i ? "," : "") + j.obj();
  }
  for (int i = 0; i < S.faultN; i++) {
    J j; const FaultRow &x = S.faults[i]; char n[80]; votol::faultText(x.code, n, sizeof n);
    j.t("when", x.when).b("set", x.set).t("name", n).n("kmh", x.kmh).n("amp", x.amp).n("volt", x.volt);
    f += (i ? "," : "") + j.obj();
  }
  for (int i = 0; i < S.runN; i++) {
    J j; const DragRow &x = S.runs[i];
    String ts = "[", vs = "[";
    for (int k = 0; k < 4; k++) { ts += String(k ? "," : "") + String(x.t[k], 3); vs += String(k ? "," : "") + String(x.trap[k], 1); }
    j.t("when", x.when).raw("t", ts + "]").raw("v", vs + "]").n("peakA", x.peakA, 0);
    r += (i ? "," : "") + j.obj();
  }
  for (int i = 0; i < S.backupN; i++) {
    J j; j.i("id", S.backups[i].id).t("when", S.backups[i].when).i("sum", S.backups[i].sum);
    b += (i ? "," : "") + j.obj();
  }
  String g = "{\"kmh\":[", a = "\"amp\":[";
  for (int i = 0; i < S.gN; i++) { g += String(i ? "," : "") + String(S.gKmh[i], 1); a += String(i ? "," : "") + String(S.gAmp[i], 1); }
  J j; j.raw("trips", t + "]").raw("faults", f + "]").raw("runs", r + "]").raw("backups", b + "]")
       .raw("graph", g + "]," + a + "]}");
  return j.obj();
}
static String paramsJson() {
  String fl = "[";
  bool have = S.params.complete();
  for (int i = 0; i < votol::PARAM_COUNT && have; i++) {
    const votol::ParamDef &d = votol::PARAMS[i];
    J j;
    j.i("i", i).t("g", d.group).t("n", d.name).t("u", d.unit).i("d", d.dec)
     .n("v", votol::shown(S.edit, d), d.dec).n("o", votol::shown(S.params.b, d), d.dec)
     .i("r", votol::getRaw(S.edit, d)).i("ro", votol::getRaw(S.params.b, d)).i("st", d.step).i("mn", d.min).i("mx", d.max).b("adv", d.adv);
    fl += (i ? "," : "") + j.obj();
  }
  J j;
  j.b("have", have).b("busy", S.busy).t("when", S.paramsWhen).i("sum", have ? S.params.checkSum() : 0)
   .t("msg", S.msg).i("kind", S.msgKind).b("stopped", S.linked && S.live.rpm == 0).b("linked", S.linked)
   .i("lastBackup", S.backupN ? S.backups[0].id : 0).raw("fields", fl + "]");
  return j.obj();
}
// Settings exposed to the web page: key, pointer, min, max.
struct SetDef { const char *k; int lo, hi; };
static const SetDef SETS[] = {{"dash", 0, 2}, {"speedSrc", 0, 2}, {"logRate", 0, 4}, {"cells", 6, 40},
  {"cellEmptyMv", 2000, 4000}, {"cellFullMv", 3000, 4400}, {"maxKmh", 40, 400}, {"maxAmp", 50, 1500},
  {"backlight", 1, 5}, {"wifi", 0, 1}, {"tzHours", -12, 14}, {"rollout", 0, 1}, {"gpsRate", 0, 1},
  {"tyreW", 50, 300}, {"tyreA", 30, 100}, {"tyreR", 8, 21}, {"wheelCorr", -100, 100}, {"ratioX100", 50, 2000}, {"maxG10", 5, 25}, {"advanced", 0, 1}};
static int setGet(int i) {
  switch (i) {
  case 0: return cfg.dash; case 1: return cfg.speedSrc; case 2: return cfg.logRate; case 3: return cfg.cells;
  case 4: return cfg.cellEmptyMv; case 5: return cfg.cellFullMv; case 6: return cfg.maxKmh; case 7: return cfg.maxAmp;
  case 8: return cfg.backlight; case 9: return cfg.wifi; case 10: return cfg.tzHours; case 11: return cfg.rollout;
  case 12: return cfg.gpsRate; case 13: return cfg.tyreW; case 14: return cfg.tyreA; case 15: return cfg.tyreR;
  case 16: return cfg.wheelCorr; case 17: return cfg.ratioX100; case 18: return cfg.maxG10; default: return cfg.advanced;
  }
}
static void setPut(int i, int v) {
  v = v < SETS[i].lo ? SETS[i].lo : v > SETS[i].hi ? SETS[i].hi : v;
  switch (i) {
  case 0: cfg.dash = v; break; case 1: cfg.speedSrc = v; break; case 2: cfg.logRate = v; break; case 3: cfg.cells = v; break;
  case 4: cfg.cellEmptyMv = v; break; case 5: cfg.cellFullMv = v; break; case 6: cfg.maxKmh = v; break;
  case 7: cfg.maxAmp = v; break; case 8: cfg.backlight = v; break;
#ifdef VOLTOL_HEADLESS
  case 9: cfg.wifi = 1; break;  // the web page is the only screen
#else
  case 9: cfg.wifi = v; break;
#endif
  case 10: cfg.tzHours = v; break; case 11: cfg.rollout = v; break;
  case 12: cfg.gpsRate = v; vlink::requestGpsConfig(v ? 25 : 10); break; case 13: cfg.tyreW = v; break;
  case 14: cfg.tyreA = v; break; case 15: cfg.tyreR = v; break; case 16: cfg.wheelCorr = v; break;
  case 17: cfg.ratioX100 = v; break;
  case 18: cfg.maxG10 = v; break;
  default: cfg.advanced = v; break;
  }
}
// ---- launch profiles (Street / Launch) ----
static void saveProf() {
  Preferences p;
  p.begin("voltol", false);
  p.putBytes("prof", &S.prof, sizeof S.prof);
  p.end();
}
static int profParam(int f) {
  for (int i = 0; i < votol::PARAM_COUNT; i++) if (!strcmp(votol::PARAMS[i].name, PROF_FIELDS[f])) return i;
  return 0;
}
static String profJson() {
  String o = "[";
  for (int q = 0; q < 2; q++) {
    J j; j.b("have", S.prof.have[q]);
    String v = "[";
    for (int f = 0; f < 3; f++) v += String(f ? "," : "") + String((long)S.prof.v[q][f]);
    j.raw("v", v + "]");
    o += (q ? "," : "") + j.obj();
  }
  String n = "[", now = "[";
  for (int f = 0; f < 3; f++) {
    n += String(f ? "," : "") + "\"" + PROF_FIELDS[f] + "\"";
    now += String(f ? "," : "") + String((long)(S.params.complete() ? votol::getRaw(S.params.b, votol::PARAMS[profParam(f)]) : 0));
  }
  J j; j.raw("p", o + "]").raw("names", n + "]").raw("now", now + "]").b("have", S.params.complete());
  return j.obj();
}
static String otaJson() {
  const ota::Status &o = ota::status();
  J j;
  j.t("version", VOLTOL_FW).b("netOn", ota::staConfigured()).t("ssid", ota::staSsid()).b("connected", o.connected)
   .t("ip", o.ip).b("available", o.available).t("latest", o.latest).i("state", o.state).i("progress", o.progress)
   .t("msg", o.msg).b("rolledBack", o.rolledBack);
  return j.obj();
}
static String settingsJson() {
  J j;
  for (int i = 0; i < (int)(sizeof SETS / sizeof SETS[0]); i++) j.i(SETS[i].k, setGet(i));
  j.n("circMm", cfg.circMm(), 0).n("kmhAt1000", ride::rpmToKmh(1000, cfg.circMm(), cfg.ratioX100 / 100.0f));
#ifdef VOLTOL_HEADLESS
  j.b("headless", true);
#else
  j.b("headless", false);
#endif
  return j.obj();
}
static void doActions(UiActions &a);
static const char *updateGuard();
// ------------------------------------------------------------------ cloud ----
static volatile uint32_t restartAt = 0;
static uint32_t dragRunId = 0;
static int lastDragState = -1;
static void jstr(String &o, const char *s) {
  o += '"';
  for (const char *p = s; *p; p++) { if (*p == '"' || *p == '\\') o += '\\'; if ((uint8_t)*p >= 0x20) o += *p; }
  o += '"';
}
// 0.2 s samples for the cloud page: [uptime_ms, rpm, km/h, V, A], sent in batches
// with every upload (2 per second while riding) so the dashboard moves smoothly.
static uint32_t cloudScanAt = 0;  // Wi-Fi scan asked from the cloud page: results go up for 2 minutes
static const int FR_N = 10;
static float frBuf[FR_N][5];
static uint32_t frT[FR_N];  // uptime kept as integer (a float loses ms after 4.6 h)
static int frHead = 0, frCount = 0;
static uint32_t frLast = 0;
static void frameSample(uint32_t now) {  // under LOCK
  if (now - frLast < 200) return;
  frLast = now;
  bool ctl = S.linked;
  float *f = frBuf[frHead];
  frT[frHead] = now; f[0] = 0; f[1] = ctl ? S.live.rpm : 0; f[2] = S.speedOk ? S.kmh : 0; f[3] = ctl ? S.live.volt : 0; f[4] = ctl ? S.live.amp : 0;
  frHead = (frHead + 1) % FR_N;
  if (frCount < FR_N) frCount++;
}
// Same fields as v1.6.x (the web dashboard reads them) + Wi-Fi list for remote control.
static void cloudPayload(String &o, int &rpm, bool &moving) {
  LOCK();
  bool ctl = S.linked;
  rpm = ctl ? S.live.rpm : 0;
  if (drag.state != lastDragState) { if (drag.state == ride::DragTimer::RUN) dragRunId = millis(); lastDragState = drag.state; }
  moving = rpm != 0 || drag.state == ride::DragTimer::RUN;
  char b[640];
  snprintf(b, sizeof b,
           "{\"firmware\":\"" VOLTOL_FW "\",\"board\":\"%s\",\"controller_online\":%s,\"wifi_online\":true,\"wifi_rssi\":%d,"
           "\"uart_baud\":%lu,\"uart_hz\":%.1f,\"valid_frames\":%lu,\"bad_frames\":%lu,"
           "\"speed_kmh\":%.1f,\"voltage\":%.1f,\"current\":%.1f,\"power_kw\":%.2f,\"soc\":%d,"
           "\"controller_temp_c\":%d,\"motor_temp_c\":%d,\"fault_mask\":%lu,\"gear\":\"%c\","
           "\"gps\":{\"fix\":%s,\"sats\":%d},\"hotspot_clients\":%d,\"uptime_s\":%lu,",
#ifdef VOLTOL_HEADLESS
           "s3",
#else
           "cyd35",
#endif
           ctl ? "true" : "false", net::rssi(), (unsigned long)S.baud, S.hz, (unsigned long)S.frames, (unsigned long)S.errors,
           S.speedOk ? S.kmh : 0.0f, ctl ? S.live.volt : 0.0f, ctl ? S.live.amp : 0.0f, ctl ? S.kw : 0.0f, S.soc,
           ctl ? S.live.tCtrl : 0, ctl ? S.live.tMotor : 0, (unsigned long)(ctl ? S.live.faults : 0),
           ctl ? "LMHS"[S.live.gear()] : '-', S.gpsFix ? "true" : "false", S.gpsSats, net::apClients(),
           (unsigned long)(millis() / 1000));
  o = b;
  o += "\"wifi_ssid\":"; jstr(o, net::ssid());
  o += ",\"wifi_saved\":[";
  for (int i = 0; i < net::count(); i++) { if (i) o += ","; jstr(o, net::saved(i).ssid); }
  o += "],\"hotspot\":"; jstr(o, net::apSsid());
  o += ",\"ip\":"; jstr(o, net::ip());
  char d[200];
  snprintf(d, sizeof d, ",\"drag\":{\"state\":\"%s\",\"elapsed_s\":%.3f,\"distance_m\":%.1f,\"run_id\":\"%lu\",\"splits\":",
           DRAG_STATE[drag.state], drag.elapsed(millis() / 1000.0), drag.distance(), (unsigned long)dragRunId);
  o += d;
  o += "[";
  for (int i = 0; i < ride::SPLITS; i++) {
    const ride::Split &x = drag.r.s[i];
    char sp[96];
    snprintf(sp, sizeof sp, "%s{\"done\":%s,\"t\":%.3f,\"v\":%.1f,\"a\":%.0f}", i ? "," : "", x.done ? "true" : "false", x.t, x.trapKmh, x.peakA);
    o += sp;
  }
  o += "]}";
  o += ",\"frames\":[";
  for (int i = 0; i < frCount; i++) {
    int k = (frHead - frCount + i + FR_N) % FR_N;
    const float *f = frBuf[k];
    char fr[72];
    snprintf(fr, sizeof fr, "%s[%lu,%d,%.1f,%.1f,%.1f]", i ? "," : "", (unsigned long)frT[k], (int)f[1], f[2], f[3], f[4]);
    o += fr;
  }
  frCount = 0;
  o += "]";
  if (cloudScanAt && millis() - cloudScanAt < 120000) {
    bool busy = net::scanBusy();
    o += String(",\"wifi_scan_busy\":") + (busy ? "true" : "false");
    if (!busy) o += ",\"wifi_scan\":" + net::scanJson();
  }
  o += "}";
  UNLOCK();
}
static String cloudCmd(const String &cmd, const String &args, bool &ok) {
  DynamicJsonDocument a(512);
  deserializeJson(a, args);
  String out;
  LOCK();
  UiActions act;
  if (cmd == "wifi_add") {
    const char *ss = a["ssid"] | "";
    const char *pw = a["pass"] | "";
    ok = net::add(ss, pw);
    out = ok ? String("saved ") + ss : "bad network name or password";
  } else if (cmd == "wifi_remove") {
    const char *ss = a["ssid"] | "";
    ok = net::remove(ss);
    out = ok ? String("removed ") + ss : "not saved on the module";
  } else if (cmd == "wifi_scan") {
    if (!net::scanBusy()) net::scanStart();
    cloudScanAt = millis();
    out = "scanning";
  } else if (cmd == "ota_check") { ota::check(); out = "checking"; }
  else if (cmd == "ota_install") {
    const char *why = updateGuard();
    if (why) { ok = false; out = why; } else { ota::install(); out = "installing"; }
  } else if (cmd == "restart") {
    const char *why = updateGuard();
    if (why) { ok = false; out = why; } else { restartAt = millis() + 1500; out = "restarting"; }
  } else if (cmd == "drag_arm") { act.armDrag = true; doActions(act); out = "armed"; }
  else if (cmd == "drag_cancel") { act.cancelDrag = true; doActions(act); out = "cancelled"; }
  else { ok = false; out = "unknown command"; }
  UNLOCK();
  return out;
}
static String wifiJson() {
  String o = "{\"saved\":" + net::listJson() + ",\"online\":" + String(net::online() ? "true" : "false");
  o += ",\"ssid\":"; jstr(o, net::ssid());
  o += ",\"ip\":"; jstr(o, net::ip());
  o += ",\"rssi\":" + String(net::rssi()) + ",\"channel\":" + String(net::channel());
  o += ",\"hotspot\":"; jstr(o, net::apSsid());
  o += ",\"clients\":" + String(net::apClients()) + ",\"scanBusy\":" + String(net::scanBusy() ? "true" : "false");
  o += ",\"scan\":" + net::scanJson() + "}";
  return o;
}
static String cloudJson() {
  String o = "{\"cloud\":" + cloud::statusJson() + ",\"ble\":";
  jstr(o, blesetup::state());
  o += ",\"bleLeft\":" + String(blesetup::secondsLeft());
  o += String(",\"bleOn\":") + (blesetup::enabled() ? "true" : "false") + "}";
  return o;
}
static void onBleSetup(const char *ssid, const char *pass, const char *token) {
  net::add(ssid, pass);
  cloud::setToken(token);
}
static String api(const String &path, const std::function<String(const char *)> &arg, int &code) {
  code = 200;
  LOCK();
  String out;
  UiActions a;
  if (path == "/api/live") out = liveJsonLocked();
  else if (path == "/api/lists") out = listsJson();
  else if (path == "/api/params") out = paramsJson();
  else if (path == "/api/settings") out = settingsJson();
  else if (path == "/api/drag") {
    String c = arg("cmd");
    if (c == "arm") a.armDrag = true; else a.cancelDrag = true;
    doActions(a);
    out = liveJsonLocked();
  } else if (path == "/api/params/read") { if (!S.busy) a.readParams = true; doActions(a); out = paramsJson(); }
  else if (path == "/api/params/set") {
    int i = arg("i").toInt();
    if (S.params.complete() && i >= 0 && i < votol::PARAM_COUNT) {
      const votol::ParamDef &d = votol::PARAMS[i];
      votol::setRaw(S.edit, d, arg("raw").toInt());
    }
    out = paramsJson();
  } else if (path == "/api/params/undo") { memcpy(S.edit, S.params.b, votol::PARAM_BYTES); out = paramsJson(); }
  else if (path == "/api/params/restore") { a.restoreBackup = arg("id").toInt(); doActions(a); out = paramsJson(); }
  else if (path == "/api/params/write") {
    if (arg("confirm") != "yes") { code = 400; out = "{\"error\":\"confirm=yes required\"}"; }
    else if (!S.linked || S.live.rpm != 0) { code = 409; out = "{\"error\":\"Stop the bike first (0 RPM)\"}"; }
    else if (S.busy) { code = 409; out = "{\"error\":\"busy\"}"; }
    else { a.writeParams = true; doActions(a); out = paramsJson(); }
  } else if (path == "/api/set") {
    String k = arg("k");
    for (int i = 0; i < (int)(sizeof SETS / sizeof SETS[0]); i++)
      if (k == SETS[i].k) { setPut(i, arg("v").toInt()); a.settingsChanged = true; }
    doActions(a);
    needFull = true;
    out = settingsJson();
  } else if (path == "/api/clearfaults") { a.clearFaults = true; doActions(a); out = "{}"; }
  else if (path == "/api/newtrip") { a.newTrip = true; doActions(a); out = "{}"; }
  else if (path == "/api/ota") out = otaJson();
  else if (path == "/api/ota/check") { ota::check(); out = otaJson(); }
  else if (path == "/api/ota/install") {
    const char *why = ota::uploadBlocked();
    if (why) { code = 409; J j; j.t("error", why); out = j.obj(); } else { ota::install(); out = otaJson(); }
  } else if (path == "/api/wifi") {
    String ss = arg("ssid");
    if (ss.length()) net::add(ss.c_str(), arg("pass").c_str());
    out = wifiJson();
  } else if (path == "/api/wifi/list") out = wifiJson();
  else if (path == "/api/wifi/remove") { net::remove(arg("ssid").c_str()); out = wifiJson(); }
  else if (path == "/api/wifi/scan") { if (!net::scanBusy()) net::scanStart(); out = wifiJson(); }
  else if (path == "/api/pair/start") {
    String code = blesetup::startGrant();
    out = "{\"code\":\"" + code + "\",\"url\":\"https://voltol.battlab.net/#pair=" + code + "\",\"left\":180}";
  } else if (path == "/api/cloud") out = cloudJson();
  else if (path == "/api/ble") { blesetup::setEnabled(arg("on") == "1"); out = cloudJson(); }
  else if (path == "/api/cloud/token") {
    bool ok = cloud::setToken(arg("t").c_str());
    if (!ok) code = 400;
    out = cloudJson();
  } else if (path == "/api/cloud/forget") { cloud::forget(); out = cloudJson(); }
  else if (path == "/api/profile") out = profJson();
  else if (path == "/api/profile/set") {
    int q = arg("p").toInt() & 1, f = arg("f").toInt();
    if (f >= 0 && f < 3) {
      const votol::ParamDef &d = votol::PARAMS[profParam(f)];
      int32_t v = arg("raw").toInt();
      S.prof.v[q][f] = v < d.min ? d.min : v > d.max ? d.max : v;
      if (!S.prof.have[q] && S.params.complete())
        for (int k = 0; k < 3; k++) if (k != f) S.prof.v[q][k] = votol::getRaw(S.params.b, votol::PARAMS[profParam(k)]);
      S.prof.have[q] = 1;
      saveProf();
    }
    out = profJson();
  } else if (path == "/api/profile/capture") {
    int q = arg("p").toInt() & 1;
    if (S.params.complete()) {
      for (int k = 0; k < 3; k++) S.prof.v[q][k] = votol::getRaw(S.params.b, votol::PARAMS[profParam(k)]);
      S.prof.have[q] = 1; saveProf();
    }
    out = profJson();
  } else if (path == "/api/profile/apply") {
    int q = arg("p").toInt() & 1;
    if (S.prof.have[q] && S.params.complete())
      for (int k = 0; k < 3; k++) votol::setRaw(S.edit, votol::PARAMS[profParam(k)], S.prof.v[q][k]);
    out = paramsJson();
  }
  else { code = 404; out = "{\"error\":\"unknown\"}"; }
  UNLOCK();
  return out;
}

// ------------------------------------------------------------------ setup ----
// Updating is refused while riding or timing a run.
static const char *updateGuard() {
  if (vlink::alive() && S.live.rpm != 0) return "Motor turning: stop the bike (0 RPM) to update";
  if (drag.state == ride::DragTimer::RUN) return "Drag run in progress";
  return nullptr;
}

void setup() {
  ota::bootCheck();  // boot-loop protection after an update
  Serial.begin(115200);
  lock = xSemaphoreCreateMutex();
  loadSettings();
  displayBegin();
  displayBacklight(cfg.backlight);
  S.fw = "VOLTOL Monitor " VOLTOL_FW;
  S.drag = &drag;
  ui::go(PG_BOOT);
  ui::draw(displayGfx(), S, cfg);
  displayCalibrate(false);
  S.sd = logger::begin();
  newTrip(true);
  S.runN = logger::loadDrag(S.runs, 10);
  S.faultN = logger::loadFaults(S.faults, 10);
  S.backupN = logger::loadBackups(S.backups, 10);
  {
    Preferences p;
    p.begin("voltol", true);
    if (p.getBytes("best", &S.best, sizeof S.best) == sizeof S.best && S.best.finished()) {
      S.haveBest = true;
      p.getString("bestw", S.bestWhen, sizeof S.bestWhen);
    }
    p.getBytes("prof", &S.prof, sizeof S.prof);
    p.end();
  }
  Serial1.setRxBufferSize(2048);
  Serial1.begin(GPS_BAUDS[0], SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  web::begin(api, setClock);
#ifdef VOLTOL_HEADLESS
  cfg.wifi = 1;
#endif
  net::begin(cfg.wifi);
#ifdef VOLTOL_HEADLESS
  ota::begin("s3", updateGuard);
#else
  ota::begin("cyd35", updateGuard);
#endif
  web::enable(cfg.wifi);
  cloud::begin(cloudPayload, cloudCmd);
  blesetup::begin(!cloud::configured(), onBleSetup);  // Bluetooth on all the time (setting) / 3-minute window for a new module
  S.busy = true;
  vlink::begin(onLive, onParams, onWrite);
  vlink::requestGpsConfig(cfg.gpsRate ? 25 : 10);
  gpsCfgAt = millis();
  delay(1200);
  ui::go(PG_DASH);
  needFull = true;
}

// ------------------------------------------------------------------- loop ----
static uint32_t lastUpd = 0, lastFlush = 0, lastSnap = 0, lastTouch = 0;
static bool touchDown = false;

static void doActions(UiActions &a) {
  if (a.readParams) { S.busy = true; S.msg[0] = 0; vlink::requestRead(); }
  if (a.writeParams) { S.busy = true; snprintf(S.msg, sizeof S.msg, "Writing..."); S.msgKind = 0; vlink::requestWrite(S.edit); }
  if (a.clearFaults) vlink::requestClearFaults();
  if (a.restoreBackup >= 0) {
    if (logger::loadBackup(a.restoreBackup, S.edit)) snprintf(S.msg, sizeof S.msg, "Backup #%d loaded", a.restoreBackup);
    else snprintf(S.msg, sizeof S.msg, "Backup #%d not found", a.restoreBackup);
    S.msgKind = 0;
  }
  if (a.armDrag) { armDrag(); dragSaved = -1; }
  if (a.cancelDrag) { drag.cancel(); S.dragWaitGps = false; }
  if (a.settingsChanged) {
#ifdef VOLTOL_HEADLESS
    cfg.wifi = 1;
#endif
    cfgDirtyAt = millis(); displayBacklight(cfg.backlight); web::enable(cfg.wifi);
  }
  if (a.newTrip) newTrip(false);
  if (a.checkUpdate) ota::check();
  if (a.installUpdate) {
    const char *why = updateGuard();
    if (why) { strlcpy(S.updMsg, why, sizeof S.updMsg); S.updFmt = why; S.updArg[0] = 0; } else ota::install();
  }
  if (a.profChanged) saveProf();
}

void loop() {
  uint32_t now = millis();
  web::loop();
  LOCK();
  net::loop();
  blesetup::loop();
  frameSample(now);
  UNLOCK();
  cloud::serialLoop();
  if (restartAt && (int32_t)(now - restartAt) > 0) ESP.restart();
  ota::loop();
  {
    const ota::Status &o = ota::status();
    S.netOn = ota::staConfigured();
    strlcpy(S.netSsid, ota::staSsid(), sizeof S.netSsid);
    S.netConnected = o.connected;
    strlcpy(S.netIp, o.ip, sizeof S.netIp);
    S.updAvailable = o.available;
    strlcpy(S.updLatest, o.latest, sizeof S.updLatest);
    if (o.msg[0]) { strlcpy(S.updMsg, o.msg, sizeof S.updMsg); S.updFmt = o.fmt; strlcpy(S.updArg, o.arg, sizeof S.updArg); }
    S.updState = o.state;
    S.updProgress = o.progress;
    S.rolledBack = o.rolledBack;
  }
  gpsPoll();
  // link status
  S.linked = vlink::alive();
  if (!S.linked && !gpsUsable()) S.speedOk = false;
  S.hz = vlink::hz();
  S.baud = vlink::baud();
  S.frames = vlink::goodFrames();
  S.errors = vlink::badFrames();
  S.sd = logger::sd();
  S.heapKB = ESP.getFreeHeap() / 1024;
  strlcpy(S.ssid, web::ssid(), sizeof S.ssid);
  strlcpy(S.ip, cfg.wifi ? "http://192.168.4.1" : "-", sizeof S.ip);
  S.now = now / 1000.0;
  // finished 402 m run: save it. The best time shown on screen is replaced
  // only after the rider leaves the result screen, so it can show "NEW BEST".
  static bool pendingBest = false;
  static ride::DragResult pending;
  static char pendingWhen[16];
  if (drag.state == ride::DragTimer::DONE && dragSaved != 1) {
    dragSaved = 1;
    DragRow r;
    shortWhen(r.when, sizeof r.when);
    for (int i = 0; i < 4; i++) { r.t[i] = drag.r.s[i].t; r.trap[i] = drag.r.s[i].trapKmh; }
    r.peakA = drag.r.peakA;
    LOCK();
    logger::dragRow(r);
    S.runN = logger::loadDrag(S.runs, 10);
    UNLOCK();
    if (!drag.r.spinCorrected && (!S.haveBest || drag.r.s[3].t < S.best.s[3].t)) {
      pending = drag.r; strlcpy(pendingWhen, r.when, sizeof pendingWhen); pendingBest = true;
      Preferences p;
      p.begin("voltol", false);
      p.putBytes("best", &pending, sizeof pending);
      p.putString("bestw", pendingWhen);
      p.end();
    }
  }
  if (drag.state != ride::DragTimer::DONE) {
    dragSaved = -1;
    if (pendingBest) {
      LOCK();
      S.best = pending; strlcpy(S.bestWhen, pendingWhen, sizeof S.bestWhen); S.haveBest = true;
      UNLOCK();
      pendingBest = false;
    }
  }
  // touch
  int tx, ty;
  bool down = displayTouch(tx, ty);
  if (down && !touchDown && now - lastTouch > 180) {
    lastTouch = now;
    UiActions a;
    LOCK();
    bool redraw = ui::touch(tx, ty, S, cfg, a);
    doActions(a);
    UNLOCK();
    if (a.touchCal) { displayCalibrate(true); redraw = true; }
    if (redraw) needFull = true;
  }
  touchDown = down;
  if (ui::tick(now)) needFull = true;
  // draw
  if (needFull) {
    needFull = false;
    LOCK();
    ui::draw(displayGfx(), S, cfg);
    UNLOCK();
    lastUpd = now;
  } else if (now - lastUpd >= 100) {
    lastUpd = now;
    LOCK();
    ui::update(displayGfx(), S, cfg);
    UNLOCK();
  }
  // housekeeping
  if (now - lastFlush >= 2000) {
    lastFlush = now;
    logger::flush();
    S.fsFreeKB = logger::freeKB();
    S.fsTotalKB = logger::totalKB();
  }
  if (now - lastSnap >= 30000) { lastSnap = now; LOCK(); saveTrip(); UNLOCK(); }
  if (cfgDirtyAt && now - cfgDirtyAt > 2000) { cfgDirtyAt = 0; saveSettings(); }
  delay(5);
}
