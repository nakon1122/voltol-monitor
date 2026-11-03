// VOLTOL Monitor - dashboard, logger, 402 m timer and parameter tuner for
// VOTOL EM-series controllers on the ESP32-3248S035 (CYD 3.5").
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <sys/time.h>
#include "config.h"
#include "display.h"
#include "gps.h"
#include "link.h"
#include "logger.h"
#include "ride.h"
#include "ui.h"
#include "web.h"

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
static const uint8_t CFG_VER = 2;
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
static gps::Nmea nmea;
static uint32_t gpsLastRmc = 0, gpsRmcCount = 0, gpsHzAt = 0, gpsBaudAt = 0;
static int gpsBaudIx = 0;
static const uint32_t GPS_BAUDS[3] = {9600, 115200, 38400};
static bool gpsUsable() { return S.gpsFix && millis() - gpsLastRmc < 1500; }
static bool useGps() { return cfg.speedSrc != 0 && gpsUsable(); }

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
  S.kw = d.volt * d.amp / 1000;
  S.soc = ride::socFromVolt(d.volt, cfg.cells, cfg.cellEmptyMv / 1000.0f, cfg.cellFullMv / 1000.0f);
  S.trip.add(t, S.kmh, d.amp, d.volt);
  S.odoKm = odoBase + S.trip.distM / 1000;
  if (!g) drag.feed(t, rpmKmh, d.amp, d.volt);
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
static String liveJson() {
  LOCK();
  char f[80], b[400];
  votol::faultText(S.live.faults, f, sizeof f);
  snprintf(b, sizeof b,
           "{\"linked\":%s,\"kmh\":%.1f,\"rpm\":%d,\"volt\":%.1f,\"amp\":%.1f,\"kw\":%.2f,\"soc\":%d,\"tc\":%d,\"tm\":%d,"
           "\"trip\":%.2f,\"status\":\"%s\",\"fault\":\"%s\",\"hz\":%.1f}",
           S.linked ? "true" : "false", S.kmh, S.live.rpm, S.live.volt, S.live.amp, S.kw, S.soc, S.live.tCtrl,
           S.live.tMotor, S.trip.distM / 1000, votol::STATUS_NAMES[S.live.status < 8 ? S.live.status : 0], f, S.hz);
  UNLOCK();
  return String(b);
}

// ------------------------------------------------------------------ setup ----
void setup() {
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
    p.end();
  }
  Serial1.begin(GPS_BAUDS[0], SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  web::begin(liveJson, setClock);
  web::enable(cfg.wifi);
  S.busy = true;
  vlink::begin(onLive, onParams, onWrite);
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
  if (a.armDrag) { drag.arm(); dragSaved = -1; }
  if (a.cancelDrag) drag.cancel();
  if (a.settingsChanged) { cfgDirtyAt = millis(); displayBacklight(cfg.backlight); web::enable(cfg.wifi); }
  if (a.newTrip) newTrip(false);
}

void loop() {
  uint32_t now = millis();
  web::loop();
  // GPS
  while (Serial1.available()) {
    if (nmea.push((char)Serial1.read())) {
      LOCK();
      gpsLastRmc = millis();
      gpsRmcCount++;
      S.gpsSeen = true;
      S.gpsFix = nmea.fix.valid;
      S.gpsSats = nmea.fix.sats;
      S.gpsKmh = nmea.fix.kmh;
      if (useGps()) drag.feed(gpsLastRmc / 1000.0, S.gpsKmh, S.live.amp, S.live.volt);
      if (!timeKnown && nmea.fix.valid && nmea.fix.dateDdmmyy) {
        struct tm tm = {};
        uint32_t d = nmea.fix.dateDdmmyy, h = nmea.fix.utcHhmmss;
        tm.tm_mday = d / 10000; tm.tm_mon = (d / 100) % 100 - 1; tm.tm_year = 100 + d % 100;
        tm.tm_hour = h / 10000; tm.tm_min = (h / 100) % 100; tm.tm_sec = h % 100;
        setenv("TZ", "UTC0", 1); tzset();
        setClock((uint64_t)mktime(&tm) * 1000, cfg.tzHours * 60);
      }
      UNLOCK();
    }
  }
  if (now - gpsHzAt >= 2000) {
    S.gpsHz = gpsRmcCount * 1000.0f / (now - gpsHzAt);
    gpsRmcCount = 0; gpsHzAt = now;
    if (!S.gpsSeen && now - gpsBaudAt > 4000) {  // look for the GPS at other baud rates
      gpsBaudIx = (gpsBaudIx + 1) % 3;
      Serial1.updateBaudRate(GPS_BAUDS[gpsBaudIx]);
      gpsBaudAt = now;
    }
    if (now - gpsLastRmc > 3000) S.gpsFix = false;
  }
  // link status
  S.linked = vlink::alive();
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
    if (!S.haveBest || drag.r.s[3].t < S.best.s[3].t) {
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
