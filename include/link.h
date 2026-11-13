// UART link to the VOTOL controller (runs in its own FreeRTOS task).
#pragma once
#include <stdint.h>
#include "votol_proto.h"

namespace vlink {
// Called from the link task for every live frame (ms = millis() when received).
typedef void (*LiveFn)(const votol::Live &d, uint32_t ms);
// Called when a full parameter read finished (ok=false: timeout).
typedef void (*ParamsFn)(const votol::ParamImage &p, bool ok);
// Called when a write finished: ok = acknowledged AND read back identical.
typedef void (*WriteFn)(bool ok, const char *msg);

void begin(LiveFn live, ParamsFn params, WriteFn write);
void requestRead();
// Writes all 7 pages of img. Refused (WriteFn called with ok=false) when the
// motor turns or the parameters were never read in this session.
void requestWrite(const uint8_t *img);
void requestClearFaults();
// Configure a u-blox GPS (115200 baud, hz updates, NAV-PVT) via the shared TX pin.
void requestGpsConfig(int hz);
bool busy();
uint32_t baud();
float hz();
uint32_t goodFrames();
uint32_t badFrames();
bool alive();  // live data within the last second
bool swapped();          // the VOTOL TX/RX wires are the other way round (fixed automatically)
bool gpsSetupBlocked();  // ... and therefore the GPS cannot be set up (factory output: NMEA, 1 per second)
}  // namespace vlink
