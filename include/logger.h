#pragma once
#include <FS.h>
#include "ride.h"
#include "ui.h"
#include "votol_proto.h"

namespace logger {
struct Row { votol::Live d; float kmh; double distM; };
bool begin();  // true when a microSD card is used
bool sd();
fs::FS &fs();
uint32_t freeKB();
uint32_t totalKB();
void startTrip(uint16_t id);
void detail(const char *time, const Row &r);
void second(const char *time, const ride::SecAgg &a, double whTrip, double distM);
void fault(const char *time, const char *event, const Row &r);  // event: SET / CLEAR / PRE
void flush();
void tripRow(const TripRow &t);
void dragRow(const DragRow &r);
int loadTrips(TripRow *rows, int max);
#if !defined(VOLTOL_HEADLESS) && !defined(VOLTOL_S340) && !defined(VOLTOL_P4C5)
static const int DRAG_ROWS_MAX = 100;   // CYD: small RAM
#else
static const int DRAG_ROWS_MAX = 300;
#endif
// Trace of a drag run (every 0.05 s) for the graph on the module web page: /tr/<run id>.bin,
// 20-byte records: u32 ms since boot, f32 metres, u16 km/h*10, i16 A*10, u16 rpm, u16 V*10, u8 raw[3], pad
struct TraceSample { uint32_t ms; float d; uint16_t kmh10; int16_t a10; uint16_t rpm; uint16_t v10; uint8_t raw[3]; uint8_t pad; };
void traceStart();
void traceAdd(const TraceSample &s);
void traceSave(uint32_t runId);   // keeps the newest runs only (space on the board)
int loadDrag(DragRow *rows, int max);   // newest first, up to DRAG_ROWS_MAX
int loadFaults(FaultRow *rows, int max);
int loadBackups(BackupRow *rows, int max);
int saveBackup(const uint8_t *img, uint32_t sum, const char *when, int lastId);
bool loadBackup(int id, uint8_t *img);
}  // namespace logger
