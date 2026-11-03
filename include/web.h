#pragma once
#include <Arduino.h>
#include <functional>
namespace web {
typedef String (*ApiFn)(const String &path, const std::function<String(const char *)> &arg, int &code);
typedef void (*TimeFn)(uint64_t epochMs, int tzMinutes);
void begin(ApiFn api, TimeFn time);
void enable(bool on);
void loop();
const char *ssid();
}  // namespace web
