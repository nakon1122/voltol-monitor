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
  uint8_t wifi = 1;
  float circMm() const {
    return ride::tyreCircMm(tyreW, tyreA, tyreR) * (1000 + wheelCorr) / 1000.0f;
  }
};
static const uint8_t LOG_RATES[5] = {0, 10, 5, 2, 1};  // 0 = every frame

struct TripRow { uint16_t id; char when[16]; float km, maxKmh, avgKmh, maxA, wh, whKm; uint32_t secs; };
struct FaultRow { char when[16]; bool set; uint32_t code; float kmh, amp, volt; int rpm; };
struct DragRow { char when[16]; float t[4], trap[4]; float peakA; };
struct BackupRow { char when[16]; uint32_t sum; uint8_t id; };

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
  bool gpsSeen = false, gpsFix = false;
  int gpsSats = 0;
  float gpsHz = 0, gpsKmh = 0;
  ride::Trip trip;
  double odoKm = 0;
  char clock[8] = "";      // "18:42" when time is known
  // drag
  ride::DragTimer *drag = nullptr;
  double now = 0;          // seconds, same clock as the drag timer
  bool haveBest = false;
  ride::DragResult best;
  char bestWhen[16] = "";
  DragRow runs[10];
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
};

enum Page : uint8_t {
  PG_BOOT, PG_DASH, PG_DRAG, PG_DRAG_HIST, PG_LOG, PG_GRAPH, PG_FAULTS,
  PG_TUNE, PG_EDIT, PG_CONFIRM, PG_BACKUPS, PG_SETUP, PG_WHEEL, PG_INFO, PG_COUNT
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
}  // namespace ui
