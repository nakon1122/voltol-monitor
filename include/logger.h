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
int loadDrag(DragRow *rows, int max);
int loadFaults(FaultRow *rows, int max);
int loadBackups(BackupRow *rows, int max);
int saveBackup(const uint8_t *img, uint32_t sum, const char *when, int lastId);
bool loadBackup(int id, uint8_t *img);
}  // namespace logger
