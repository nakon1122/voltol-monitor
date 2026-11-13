#pragma once
#include <stdint.h>
#include "gfx.h"
void displayBegin();
Gfx &displayGfx();
void displayBacklight(uint8_t level);  // 1..5
bool displayTouch(int &x, int &y);
void displayCalibrate(bool force);
void displayFlush(bool full);          // P4: copy what changed to the MIPI panel (no-op elsewhere)
