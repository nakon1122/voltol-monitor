#pragma once
#include <stdint.h>

namespace ota {
enum State : uint8_t { S_IDLE, S_CHECKING, S_DOWNLOADING, S_RESTARTING, S_ERROR };
struct Status {
  bool connected = false;   // joined the internet Wi-Fi
  char ip[16] = "";
  bool available = false;   // newer firmware on GitHub
  char latest[16] = "";
  uint8_t state = S_IDLE;
  int progress = 0;         // download %
  char msg[64] = "";        // English, for the web page
  const char *fmt = "";     // same message as format + argument, translated on screen
  char arg[24] = "";
  bool rolledBack = false;  // last update failed to start, previous firmware restored
};
// Returns a reason why updating is not allowed right now, or nullptr.
typedef const char *(*GuardFn)();

void bootCheck();    // first thing in setup(): boot-loop rollback
void markPending();  // call after any successful update, before restarting
void begin(const char *boardKey, GuardFn guard);  // "cyd35" or "s3"
void loop();
bool staConfigured();
const char *staSsid();
void setWifi(const char *ssid, const char *pass);
void check();
void install();
const Status &status();
const char *uploadBlocked();
}  // namespace ota
