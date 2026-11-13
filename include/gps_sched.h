// GPS serial housekeeping, called every 2 s (pure logic, unit-tested on the PC in test/swap_test.cpp):
// nothing heard for 4 s -> try the next baud rate; no fast u-blox output -> re-send the u-blox setup
// (115200 baud, NAV-PVT at the chosen rate). While the setup cannot reach the GPS (VOTOL wires swapped)
// it only hunts the baud rate and keeps whatever the receiver sends (factory: NMEA once per second).
#pragma once
#include <stdint.h>

struct GpsSched {
  static constexpr uint32_t BAUDS[3] = {115200, 9600, 38400};
  int baudIx = 0;
  uint32_t baudAt = 0, cfgAt = 0;
  // returns true when the baud rate changed (use BAUDS[baudIx]); cfg = send the u-blox setup now
  bool tick(uint32_t now, uint32_t last, bool ubx, float hz, float wantHz, bool blocked, bool &cfg) {
    bool changed = false;
    cfg = false;
    if (blocked && last == 0 && baudIx == 0) {  // setup impossible and nothing heard yet: factory rate first
      baudIx = 1;
      baudAt = now;
      return true;
    }
    if (now - last > 4000 && now - baudAt > 4000) {
      baudIx = (baudIx + 1) % 3;
      baudAt = now;
      changed = true;
    }
    if (!blocked && now - cfgAt > 15000 && (!ubx || now - last > 4000 || hz < wantHz)) {
      cfgAt = now;
      cfg = true;
      if (baudIx != 0) changed = true;
      baudIx = 0;
    }
    return changed;
  }
};
