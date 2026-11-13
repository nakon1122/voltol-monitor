// Automatic TX/RX swap for the VOTOL wires (pure logic, unit-tested on the PC in test/swap_test.cpp).
// The controller only answers our requests. When nothing valid comes back for SWAP_AFTER_MS, the two
// pins are exchanged and tried again. An orientation is "confirmed" (and saved) once a valid frame
// arrives with it, so a board that was wired the other way starts right away after the next boot.
#pragma once
#include <stdint.h>

struct UartSwap {
  static const uint32_t SWAP_AFTER_MS = 3000;
  bool swapped = false;    // current pin orientation (false = as printed in the wiring guide)
  bool confirmed = false;  // a valid frame was received with the current orientation
  bool saved = false;      // orientation remembered from an earlier run
  uint32_t lastGood = 0, lastSwitch = 0;
  enum Act { NONE, SWITCH, SAVE };

  void begin(bool sav, uint32_t now) { saved = sav; swapped = sav; confirmed = false; lastGood = now; lastSwitch = now; }
  // Call after every poll. gotFrame = at least one valid controller frame since the last call.
  Act update(uint32_t now, bool gotFrame) {
    if (gotFrame) {
      lastGood = now;
      if (!confirmed) { confirmed = true; saved = swapped; return SAVE; }
      return NONE;
    }
    if (now - lastGood > SWAP_AFTER_MS && now - lastSwitch > SWAP_AFTER_MS) {
      swapped = !swapped;
      confirmed = false;
      lastSwitch = now;
      return SWITCH;
    }
    return NONE;
  }
  // The GPS RX hangs on the "board TX" wire of the guide. While the VOTOL wires are confirmed swapped,
  // that wire is our RX (the controller drives it), so the GPS cannot be set up: it stays at its
  // factory output (NMEA, 1 per second). While not confirmed, the remembered orientation decides
  // (a board known to be wired the other way never drives the controller's TX wire).
  bool gpsSetupPossible() const { return confirmed ? !swapped : !saved; }
};
