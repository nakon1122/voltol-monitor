#pragma once
#include <stdint.h>
#include "gfx.h"
void displayBegin();
Gfx &displayGfx();
void displayBacklight(uint8_t level);  // 1..5
bool displayTouch(int &x, int &y);
void displayCalibrate(bool force);     // loads saved calibration or runs it
