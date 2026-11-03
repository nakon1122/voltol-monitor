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
bool busy();
uint32_t baud();
float hz();
uint32_t goodFrames();
uint32_t badFrames();
bool alive();  // live data within the last second
}  // namespace vlink
