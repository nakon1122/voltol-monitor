#pragma once
#include <Arduino.h>
#include <functional>
namespace web {
typedef String (*ApiFn)(const String &path, const std::function<String(const char *)> &arg, int &code);
typedef void (*TimeFn)(uint64_t epochMs, int tzMinutes);
void begin(ApiFn api, TimeFn time);
typedef String (*LiveFn)();     // live JSON (takes the shared lock itself)
typedef uint32_t (*FrameFn)();  // controller frame counter: a change = new data
void setLive(LiveFn live, FrameFn frames);
int liveClients();
void enable(bool on);
void loop();
const char *ssid();
}  // namespace web
