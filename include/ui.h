// Screens of the CYD 3.5" (480x320, landscape). The firmware fills UiState,
// the UI draws it and reports what the rider asked for in UiActions.
#pragma once
#include <stdint.h>
#include "gfx.h"
#include "ride.h"
#include "votol_proto.h"

struct Settings {
  uint8_t dash = 0;         // 0 A Sport Arc, 1 B Digital Race, 2 C Twin Gauge
  uint16_t tyreW = 130, tyreA = 70, tyreR = 17;
  int8_t wheelCorr = 0;     // circumference correction, 0.1 % steps (-100..100)
  uint16_t ratioX100 = 100; // motor turns per wheel turn x100 (hub motor = 100)
  uint8_t cells = 20;       // battery series cells
  uint16_t cellEmptyMv = 3000, cellFullMv = 4150;
  uint8_t speedSrc = 2;     // 0 RPM, 1 GPS, 2 AUTO (GPS when it has a fix)
  uint8_t logRate = 0;      // index into LOG_RATES
  uint16_t maxKmh = 120, maxAmp = 200;
  uint8_t backlight = 4;    // 1..5
  int8_t tzHours = 7;       // local time = UTC + this (GPS time)
  uint8_t rollout = 0;      // drag: 1 = clock starts after the first foot (drag-strip style)
  uint8_t gpsRate = 0;      // 0 = 10 Hz, 1 = 25 Hz (u-blox M10)
  uint8_t maxG10 = 16;      // max traction for the drag timer, 0.1 g (wheel-spin limit)
  uint8_t lang = 0;         // 0 Thai, 1 English
  uint8_t advanced = 0;     // show risky parameters (hall angle, pole pairs ...)
  uint8_t wifi = 1;
  uint8_t dragTarget = 1;   // drag distance: 0 = 201 m, 1 = 402 m, 2 = 1000 m
  float circMm() const {
    return ride::tyreCircMm(tyreW, tyreA, tyreR) * (1000 + wheelCorr) / 1000.0f;
  }
};
static const uint8_t LOG_RATES[5] = {0, 10, 5, 2, 1};  // 0 = every frame

struct TripRow { uint16_t id; char when[16]; float km, maxKmh, avgKmh, maxA, wh, whKm; uint32_t secs; };
struct FaultRow { char when[16]; bool set; uint32_t code; float kmh, amp, volt; int rpm; };
// one drag run: n = splits of the chosen distance (3 = 201 m, 4 = 402 m, 5 = 1000 m), done = reached it
struct DragRow { char when[16]; uint8_t n = 4; bool done = true; float t[5] = {0}, trap[5] = {0}; float peakA = 0; uint32_t rid = 0; };
struct BackupRow { char when[16]; uint32_t sum; uint8_t id; };
struct WifiNet { char ssid[33]; int8_t rssi; bool lock, saved; };

// Launch / street profiles: 3 throttle settings that cut wheel spin.
static const char *const PROF_FIELDS[3] = {"Rate of rise", "Start torque", "Soft-start grade"};
struct LaunchProf {
  uint8_t have[2] = {0, 0};  // 0 street, 1 launch
  int32_t v[2][3] = {{0, 0, 0}, {0, 0, 0}};
};

struct UiState {
  // controller link
  bool linked = false;
  float hz = 0;
  uint32_t baud = 9600, frames = 0, errors = 0;
  votol::Live live;
  // derived
  float kmh = 0, kw = 0;
  int soc = 0;
  uint8_t src = 0;  // speed source in use: 0 RPM, 1 GPS
  bool speedOk = false;  // speed available (controller or GPS)
  bool gpsSeen = false, gpsFix = false, gpsUbx = false;
  int gpsSats = 0;
  float gpsHz = 0, gpsKmh = 0, gpsSAcc = 99;  // speed accuracy, km/h
  bool dragWaitGps = false;  // armed for GPS but signal not good enough yet
  bool slipOk = false;       // controller + GPS both live: wheel slip known
  float slipPct = 0;
  ride::Trip trip;
  double odoKm = 0;
  char clock[8] = "";      // "18:42" when time is known
  // drag
  ride::DragTimer *drag = nullptr;
  double now = 0;          // seconds, same clock as the drag timer
  bool haveBest = false;
  ride::DragResult best;
  char bestWhen[16] = "";
  DragRow runs[10];         // newest drag runs (screen); the web page reads up to 50
  int runN = 0;
  // logs
  bool sd = false;
  uint32_t fsFreeKB = 0, fsTotalKB = 0;
  TripRow trips[8];
  int tripN = 0;
  FaultRow faults[10];
  int faultN = 0;
  float gKmh[240], gAmp[240];  // last minutes, 1 point per second
  int gN = 0;
  // parameters
  votol::ParamImage params;  // last read from the controller
  char paramsWhen[16] = "";
  uint8_t edit[votol::PARAM_BYTES];  // working copy being edited
  bool busy = false;         // read/write in progress
  char msg[64] = "";         // last read/write result
  uint8_t msgKind = 0;       // 0 info, 1 ok, 2 error
  BackupRow backups[10];
  int backupN = 0;
  LaunchProf prof;
  // firmware update
  bool netOn = false;          // internet Wi-Fi configured
  bool netConnected = false;
  char netSsid[33] = "", netIp[16] = "";
  bool updAvailable = false, rolledBack = false;
  char updLatest[16] = "", updMsg[64] = "";
  const char *updFmt = "";     // updMsg as an English format, translated on screen
  char updArg[24] = "";
  // internet Wi-Fi picker on the screen
  WifiNet wifiNets[10];
  int wifiN = 0;
  bool wifiScanning = false;
  uint8_t updState = 0;        // ota::State
  int updProgress = 0;
  // info
  const char *fw = "";
  char ssid[24] = "", ip[24] = "";
  uint32_t heapKB = 0;
};

struct UiActions {
  bool readParams = false, writeParams = false, clearFaults = false;
  int restoreBackup = -1;     // backup id to load into edit[]
  bool armDrag = false, cancelDrag = false, settingsChanged = false;
  bool touchCal = false, newTrip = false;
  bool checkUpdate = false, installUpdate = false, profChanged = false;
  bool wifiScan = false, wifiJoin = false, wifiForget = false;
  char wifiSsid[33] = "", wifiPass[65] = "";
};

enum Page : uint8_t {
  PG_BOOT, PG_DASH, PG_DRAG, PG_DRAG_HIST, PG_LOG, PG_GRAPH, PG_FAULTS,
  PG_TUNE, PG_EDIT, PG_CONFIRM, PG_BACKUPS, PG_SETUP, PG_WHEEL, PG_INFO, PG_LAUNCH, PG_WIFI, PG_KEYB, PG_COUNT
};

namespace ui {
extern Page page;
extern bool menuOpen;
extern int editGroup;
// Full redraw of the current page.
void draw(Gfx &g, const UiState &s, const Settings &cfg);
// Refresh only the live values (called ~10x per second).
void update(Gfx &g, const UiState &s, const Settings &cfg);
// Handle a tap. Returns true when the page must be fully redrawn.
bool touch(int x, int y, UiState &s, Settings &cfg, UiActions &act);
// Called every loop: closes the dashboard menu after a few seconds etc.
bool tick(uint32_t ms);
void go(Page p);
// on-screen keyboard state (used by the PC preview renderer)
void kbPreset(const char *ssid, const char *pass, int mode, bool show);
}  // namespace ui
