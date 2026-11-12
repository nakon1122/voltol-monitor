// VOLTOL Cloud (voltol.battlab.net): live telemetry to the BATTLAB Supabase
// project and commands back from the web page (Wi-Fi networks, update, drag).
// Runs in its own task on core 0, so a slow internet never blocks the web
// page, the screen or the controller link.
#pragma once
#include <Arduino.h>

namespace cloud {
// Fill `out` (JSON object text for "payload") - called from the cloud task.
typedef void (*PayloadFn)(String &out, int &rpm, bool &moving);
// Execute one command from the web page; return a short result text, ok=false on refusal.
typedef String (*CmdFn)(const String &cmd, const String &argsJson, bool &ok);

void begin(PayloadFn payload, CmdFn cmd);
// Called after an upload was accepted (the samples in it can be dropped); return true when
// samples are still waiting, so the next upload goes out right away (catch-up after a drop-out).
typedef bool (*AckFn)();
void setAck(AckFn ack);
bool setToken(const char *hex64);   // from Bluetooth setup / USB serial / module web page
bool configured();
void forget();
bool online();                      // last upload accepted (< 10 s ago)
int lastHttp();
int32_t ageMs();                    // since last accepted upload, -1 never
void serialLoop();                  // "CLOUD_TOKEN <64 hex>" over USB serial
String statusJson();
}  // namespace cloud
