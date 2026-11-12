// "VOLTOL Setup" Bluetooth pairing with voltol.battlab.net (same protocol as v1.6.x):
// the web page sends the home Wi-Fi + the module's cloud token, encrypted.
//  * boot-ready: a module without a cloud token accepts setup for 3 minutes after
//    power-on (key agreed with ECDH P-256, you are next to the bike).
//  * ready: "PAIR" on the module's web page makes a one-time code (link
//    voltol.battlab.net/#pair=CODE), valid 3 minutes.
//  * locked: (1.7.3) Bluetooth stays on all the time (setting, default on) so the
//    web page can connect at once; nothing can be changed without a code.
// With the setting off, Bluetooth only runs during the windows above.
#pragma once
#include <Arduino.h>

namespace blesetup {
// Called when a valid setup arrived (already decrypted and checked).
typedef void (*DoneFn)(const char *ssid, const char *pass, const char *token);
void begin(bool bootWindow, DoneFn done);
void setEnabled(bool on);   // saved; on = always on
bool enabled();
String startGrant();   // returns the 32-hex one-time code
void loop();
bool active();
const char *state();   // idle / boot-ready / ready / accepted / ...
int secondsLeft();
bool phone();          // a phone is connected over Bluetooth right now
// Open a 3-minute pairing window from the module's own screen (someone is at the bike, like the
// first-boot window): the phone page sends Wi-Fi + cloud code without a code from the module page.
void startScreen();
}  // namespace blesetup
