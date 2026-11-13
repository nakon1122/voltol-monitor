// Smart BMS over Bluetooth: finds the BMS, keeps the link, polls it and reads /
// writes its settings (protocol work in bms_engine.h). Runs in its own task;
// the main loop copies the state into the screen with poll().
// Boards built with VOTOL_NO_BLE get a stub that reports "no Bluetooth".
#pragma once
#include <stdint.h>
#include "bms.h"

namespace bmslink {
struct Sample { uint32_t ms; float v, a, soc, dmv; };  // one log point
void begin();                      // reconnects to the saved BMS (Bluetooth starts only when one is saved)
bool enabled();                    // a BMS is saved or being searched for
void scan();                       // look for BMS nearby (starts Bluetooth)
void connect(const bms::Found &f);
void forget();
void readSettings();
// Write settings (unified index, value). Checked against the current values first.
// Returns nullptr when queued, else the reason. by = "screen" or "cloud".
const char *write(const int *idx, const float *val, int n, const char *by);
// Copy the current state into the screen state (keeps the screen's edit copy).
// Returns true when the settings changed (cloud snapshot due).
bool poll(BmsUi &ui);
// A finished write: true once per job, with what the cloud history needs.
struct WriteResult { bool ok; char by[8]; char msg[96]; int n; int idx[16]; float before[16], after[16]; };
bool takeWrite(WriteResult &r);
bool busy();                       // Bluetooth radio in use (pause during firmware download)
void pause(bool on);
}  // namespace bmslink
