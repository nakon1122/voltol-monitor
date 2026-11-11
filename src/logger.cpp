// CSV logging to the microSD card when present, otherwise to internal flash
// (LittleFS). Files (all open directly in Excel / Google Sheets):
//   /log/T0042_detail.csv  every live frame (or the chosen rate)
//   /log/T0042_sec.csv     one row per second: avg / min / max
//   /faults.csv            fault set/clear events + 5 s of data before each fault
//   /trips.csv             one summary row per trip
//   /drag2.csv             every drag run (201 / 402 / 1000 m; also runs stopped early)
//   /drag.csv              402 m runs of v1.7.x and older (kept, read when drag2 is empty)
// Rows are buffered in RAM and written every 2 s from the main loop, so the
// controller link never waits for the flash. When space runs low the oldest
// trip files are deleted first.
#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include "config.h"
#include "logger.h"

namespace logger {
static fs::FS *F = &LittleFS;
static bool onSd = false, ok = false;
#if HAS_SD
static SPIClass sdSpi(VSPI);
#endif
static SemaphoreHandle_t mux = nullptr;
static String bufDetail, bufSec, bufFault;
static char pathDetail[32], pathSec[32];
static uint16_t trip = 0;

bool begin() {
  if (!mux) mux = xSemaphoreCreateMutex();
#if HAS_SD
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, sdSpi, 20000000) && SD.cardType() != CARD_NONE) {
    F = &SD; onSd = true;
  } else
#endif
  {
    LittleFS.begin(true);
    F = &LittleFS; onSd = false;
  }
  ok = true;
  if (!F->exists("/log")) F->mkdir("/log");
  if (!F->exists("/bk")) F->mkdir("/bk");
  return onSd;
}
bool sd() { return onSd; }
fs::FS &fs() { return *F; }
uint32_t totalKB() { return onSd ? SD.totalBytes() / 1024 : LittleFS.totalBytes() / 1024; }
uint32_t freeKB() {
  return onSd ? (SD.totalBytes() - SD.usedBytes()) / 1024 : (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024;
}

static void append(const char *path, const String &s, const char *headerIfNew) {
  bool isNew = !F->exists(path);
  File f = F->open(path, FILE_APPEND);
  if (!f) return;
  if (isNew && headerIfNew) f.print(headerIfNew);
  f.print(s);
  f.close();
}
// Delete the oldest trip file while free space is short.
static void makeRoom() {
  uint32_t need = onSd ? 10240 : 48;
  for (int guard = 0; guard < 20 && freeKB() < need; guard++) {
    File d = F->open("/log");
    String oldest;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
      String n = String("/log/") + e.name();
      if (n.indexOf(pathDetail + 5) < 0 && n.indexOf(pathSec + 5) < 0 && (oldest == "" || n < oldest)) oldest = n;
    }
    if (oldest == "") break;
    F->remove(oldest);
  }
}

static const char *H_DETAIL = "time,km_h,rpm,amp,volt,watt,ctrl_c,motor_c,gear,status,fault_hex,fault,dist_m\n";
static const char *H_SEC = "time,km_h_avg,km_h_max,rpm_avg,rpm_max,amp_avg,amp_min,amp_max,volt_avg,volt_min,ctrl_c_max,motor_c_max,fault_hex,wh_trip,dist_m\n";
static const char *H_FAULT = "time,trip,event,fault_hex,fault,km_h,rpm,amp,volt,ctrl_c,motor_c\n";

void startTrip(uint16_t id) {
  flush();
  trip = id;
  snprintf(pathDetail, sizeof pathDetail, "/log/T%04u_detail.csv", id);
  snprintf(pathSec, sizeof pathSec, "/log/T%04u_sec.csv", id);
}
static void add(String &b, const char *line) {
  if (!mux) return;
  xSemaphoreTake(mux, portMAX_DELAY);
  if (b.length() < 24000) b += line;
  xSemaphoreGive(mux);
}
void detail(const char *t, const Row &r) {
  char f[64], l[200];
  votol::faultText(r.d.faults, f, sizeof f);
  for (char *p = f; *p; p++) if (*p == ',') *p = ';';
  snprintf(l, sizeof l, "%s,%.1f,%d,%.1f,%.1f,%.0f,%d,%d,%c,%s,%08X,%s,%.1f\n", t, r.kmh, r.d.rpm, r.d.amp, r.d.volt,
           r.d.amp * r.d.volt, r.d.tCtrl, r.d.tMotor, "LMHS"[r.d.gear()],
           votol::STATUS_NAMES[r.d.status < 8 ? r.d.status : 0], (unsigned)r.d.faults, f, r.distM);
  add(bufDetail, l);
}
void second(const char *t, const ride::SecAgg &a, double whTrip, double distM) {
  char l[220];
  snprintf(l, sizeof l, "%s,%.1f,%.1f,%.0f,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,%08X,%.2f,%.0f\n", t, a.avgKmh(), a.maxKmh,
           a.avgRpm(), a.maxRpm, a.avgA(), a.minA, a.maxA, a.avgV(), a.minV, a.maxTc, a.maxTm, (unsigned)a.faults,
           whTrip, distM);
  add(bufSec, l);
}
void fault(const char *t, const char *event, const Row &r) {
  char f[64], l[220];
  votol::faultText(r.d.faults, f, sizeof f);
  for (char *p = f; *p; p++) if (*p == ',') *p = ';';
  snprintf(l, sizeof l, "%s,%u,%s,%08X,%s,%.1f,%d,%.1f,%.1f,%d,%d\n", t, trip, event, (unsigned)r.d.faults, f, r.kmh,
           r.d.rpm, r.d.amp, r.d.volt, r.d.tCtrl, r.d.tMotor);
  add(bufFault, l);
}
void flush() {
  if (!ok || !mux) return;
  String d, s, f;
  xSemaphoreTake(mux, portMAX_DELAY);
  d = bufDetail; s = bufSec; f = bufFault;
  bufDetail = ""; bufSec = ""; bufFault = "";
  xSemaphoreGive(mux);
  if (!d.length() && !s.length() && !f.length()) return;
  makeRoom();
  if (trip && d.length()) append(pathDetail, d, H_DETAIL);
  if (trip && s.length()) append(pathSec, s, H_SEC);
  if (f.length()) append("/faults.csv", f, H_FAULT);
}
void tripRow(const TripRow &t) {
  char l[200];
  snprintf(l, sizeof l, "%u,%s,%.2f,%.1f,%.1f,%.1f,%.0f,%.1f,%u\n", t.id, t.when, t.km, t.maxKmh, t.avgKmh, t.maxA, t.wh,
           t.whKm, (unsigned)t.secs);
  append("/trips.csv", l, "trip,start,km,max_km_h,avg_km_h,max_amp,wh,wh_per_km,seconds\n");
}
static const int DRAG_M[6] = {0, 0, 0, 201, 402, 1000};
void dragRow(const DragRow &r) {
  char l[240];
  int k = snprintf(l, sizeof l, "%s,%d,%s", r.when, DRAG_M[r.n <= 5 ? r.n : 4], r.done ? "finished" : "stopped");
  for (int i = 0; i < 5; i++) k += snprintf(l + k, sizeof l - k, r.t[i] > 0 ? ",%.3f" : ",", r.t[i]);
  for (int i = 0; i < 5; i++) k += snprintf(l + k, sizeof l - k, r.trap[i] > 0 ? ",%.1f" : ",", r.trap[i]);
  snprintf(l + k, sizeof l - k, ",%.0f\n", r.peakA);
  append("/drag2.csv", l, "time,distance_m,result,t_60ft,t_100m,t_201m,t_402m,t_1000m,trap_60ft,trap_100m,trap_201m,trap_402m,trap_1000m,peak_amp\n");
}

// Last rows of a CSV, newest first (small files: trips, drag, faults).
static int tail(const char *path, String *out, int max, size_t win = 6000) {
  File f = F->open(path, FILE_READ);
  if (!f) return 0;
  size_t sz = f.size();
  if (sz > win) f.seek(sz - win);
  String all = f.readString();
  f.close();
  int n = 0, end = all.length();
  while (n < max && end > 0) {
    int st = all.lastIndexOf('\n', end - 2);
    String ln = all.substring(st + 1, end);
    ln.trim();
    end = st + 1;
    if (st < 0) end = 0;
    // skip the header line only: rows may start with a letter ("boot+12s" before the clock is known)
    if (ln.length() && !ln.startsWith("time,") && !ln.startsWith("trip,") && !ln.startsWith("id,")) out[n++] = ln;
    if (st < 0) break;
  }
  return n;
}
static int fields(const String &s, String *f, int max) {
  int n = 0, st = 0;
  while (n < max) {
    int c = s.indexOf(',', st);
    f[n++] = c < 0 ? s.substring(st) : s.substring(st, c);
    if (c < 0) break;
    st = c + 1;
  }
  return n;
}
int loadTrips(TripRow *rows, int max) {
  String l[8], f[9];
  int n = tail("/trips.csv", l, max < 8 ? max : 8);
  for (int i = 0; i < n; i++) {
    fields(l[i], f, 9);
    TripRow &t = rows[i];
    t.id = f[0].toInt(); strlcpy(t.when, f[1].c_str(), sizeof t.when);
    t.km = f[2].toFloat(); t.maxKmh = f[3].toFloat(); t.avgKmh = f[4].toFloat(); t.maxA = f[5].toFloat();
    t.wh = f[6].toFloat(); t.whKm = f[7].toFloat(); t.secs = f[8].toInt();
  }
  return n;
}
int loadDrag(DragRow *rows, int max) {
  static String l[DRAG_ROWS_MAX];
  String f[14];
  if (max > DRAG_ROWS_MAX) max = DRAG_ROWS_MAX;
  int n = tail("/drag2.csv", l, max, 9000);
  for (int i = 0; i < n; i++) {
    fields(l[i], f, 14);
    DragRow &r = rows[i];
    r = DragRow();
    strlcpy(r.when, f[0].c_str(), sizeof r.when);
    int m = f[1].toInt();
    r.n = m >= 1000 ? 5 : m >= 402 ? 4 : 3;
    r.done = f[2] == "finished";
    for (int k = 0; k < 5; k++) { r.t[k] = f[3 + k].toFloat(); r.trap[k] = f[8 + k].toFloat(); }
    r.peakA = f[13].toFloat();
  }
  if (n < max) {  // older 402 m runs (v1.7.x file)
    String o[DRAG_ROWS_MAX];
    int m = tail("/drag.csv", o, max - n, 6000);
    for (int i = 0; i < m; i++) {
      fields(o[i], f, 10);
      DragRow &r = rows[n + i];
      r = DragRow();
      strlcpy(r.when, f[0].c_str(), sizeof r.when);
      for (int k = 0; k < 4; k++) { r.t[k] = f[1 + k].toFloat(); r.trap[k] = f[5 + k].toFloat(); }
      r.peakA = f[9].toFloat();
    }
    n += m;
  }
  return n;
}
int loadFaults(FaultRow *rows, int max) {
  String l[24], f[11];
  int n = tail("/faults.csv", l, 24), k = 0;
  for (int i = 0; i < n && k < max; i++) {
    fields(l[i], f, 11);
    if (f[2] == "PRE") continue;
    FaultRow &r = rows[k++];
    String t = f[0];
    int sp = t.indexOf(' ');
    strlcpy(r.when, (sp > 0 ? t.substring(sp + 1, sp + 9) : t).c_str(), sizeof r.when);
    r.set = f[2] == "SET";
    r.code = strtoul(f[3].c_str(), nullptr, 16);
    r.kmh = f[5].toFloat(); r.rpm = f[6].toInt(); r.amp = f[7].toFloat(); r.volt = f[8].toFloat();
  }
  return k;
}

// ---- parameter backups: /bk/NNN.bin (119 bytes) + /bk/index.csv ----
int loadBackups(BackupRow *rows, int max) {
  String l[10], f[3];
  int n = tail("/bk/index.csv", l, max < 10 ? max : 10);
  for (int i = 0; i < n; i++) {
    fields(l[i], f, 3);
    rows[i].id = f[0].toInt();
    strlcpy(rows[i].when, f[1].c_str(), sizeof rows[i].when);
    rows[i].sum = strtoul(f[2].c_str(), nullptr, 16);
  }
  return n;
}
int saveBackup(const uint8_t *img, uint32_t sum, const char *when, int lastId) {
  int id = lastId + 1;
  char p[24];
  snprintf(p, sizeof p, "/bk/%03d.bin", id);
  File f = F->open(p, FILE_WRITE);
  if (!f) return lastId;
  f.write(img, votol::PARAM_BYTES);
  f.close();
  char l[64];
  snprintf(l, sizeof l, "%d,%s,%05X\n", id, when, (unsigned)sum);
  append("/bk/index.csv", l, "id,time,check\n");
  return id;
}
bool loadBackup(int id, uint8_t *img) {
  char p[24];
  snprintf(p, sizeof p, "/bk/%03d.bin", id);
  File f = F->open(p, FILE_READ);
  if (!f) return false;
  bool ok = f.read(img, votol::PARAM_BYTES) == votol::PARAM_BYTES;
  f.close();
  return ok;
}
}  // namespace logger
