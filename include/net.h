// Wi-Fi for VOLTOL Monitor: the board's own hotspot "VOLTOL-xxxx" (always on)
// plus up to 8 saved home / phone-hotspot networks for the internet.
//
// Hotspot stays usable while the internet Wi-Fi connects (fix for v1.6.x):
//  * modem sleep is off (AP + STA with power save drops hotspot clients)
//  * no automatic reconnect storms: the board tries one saved network at a
//    time, every 30 s, and NEVER scans/reconnects while a phone is connected
//    to the hotspot (a scan moves the radio off the hotspot channel)
//  * when the internet Wi-Fi is on another channel the hotspot follows it once,
//    right after connecting (ESP32 has one radio), then stays there.
#pragma once
#include <Arduino.h>

namespace net {
static const int MAX_NETS = 8;
struct Saved { char ssid[33]; char pass[65]; };

void begin(bool hotspot);         // loads saved networks, starts hotspot + STA
void loop();                      // call often from loop()
void hotspot(bool on);            // the S3 board keeps it on
bool hotspotOn();
uint32_t dropCount();  // internet Wi-Fi losses since boot
const char *apSsid();
int apClients();

int count();
const Saved &saved(int i);
bool add(const char *ssid, const char *pass);  // save (or update) and connect to it now
bool remove(const char *ssid);
void reconnectNow();

bool online();                    // joined an internet Wi-Fi
const char *ssid();               // network in use ("" when offline)
const char *ip();
int rssi();
int channel();

// Background scan for the web page / Bluetooth setup (does not drop the hotspot
// for long: one pass, only on request).
// call with true BEFORE starting Bluetooth and false after stopping it
void btCoex(bool on);
void scanStart();
bool scanBusy();
String scanJson();                // [{"s":"name","r":-60,"l":1},...]
int scanCount();
bool scanItem(int i, String &ssid, int &rssi, bool &lock);

String listJson();                // [{"s":"Home","on":true},...] (no passwords)
}  // namespace net
