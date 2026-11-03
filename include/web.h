#pragma once
#include <Arduino.h>
namespace web {
typedef String (*LiveJsonFn)();
typedef void (*TimeFn)(uint64_t epochMs, int tzMinutes);
void begin(LiveJsonFn live, TimeFn time);
void enable(bool on);
void loop();
const char *ssid();
}  // namespace web
