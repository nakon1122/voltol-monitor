// Optional GPS module (any NMEA receiver, e.g. u-blox M8/M10 at 10-25 Hz).
// Plug its TX into the GPS RX pin (see config.h); the firmware detects it by
// the "$G..RMC" sentences and offers it as a speed source for the dashboard
// and the drag timer. Pure C++ (unit-tested: test/ride_test.cpp).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace gps {

struct Fix {
  bool valid = false;   // RMC status 'A'
  float kmh = 0;
  int sats = 0;
  float hdop = 99;
  uint32_t utcHhmmss = 0, dateDdmmyy = 0;
  uint32_t sentences = 0;
};

class Nmea {
public:
  Fix fix;
  // Returns true when an RMC (speed) sentence was completed.
  bool push(char c) {
    if (c == '$') { n = 0; on = true; return false; }
    if (!on) return false;
    if (c == '\r' || c == '\n') { on = false; buf[n] = 0; return line(); }
    if (n < (int)sizeof(buf) - 1) buf[n++] = c; else on = false;
    return false;
  }

private:
  char buf[100];
  int n = 0;
  bool on = false;
  static int hex(char c) { return c >= 'A' ? c - 'A' + 10 : c - '0'; }
  bool line() {
    char *star = strchr(buf, '*');
    if (!star || star[1] == 0 || star[2] == 0) return false;
    uint8_t x = 0;
    for (char *p = buf; p < star; p++) x ^= (uint8_t)*p;
    if (x != (hex(star[1]) << 4 | hex(star[2]))) return false;
    *star = 0;
    const char *f[20];
    int nf = 0;
    f[nf++] = buf;
    for (char *p = buf; *p && nf < 20; p++)
      if (*p == ',') { *p = 0; f[nf++] = p + 1; }
    if (strlen(f[0]) != 5) return false;
    const char *t = f[0] + 2;
    fix.sentences++;
    if (!strcmp(t, "RMC") && nf > 9) {
      fix.valid = f[2][0] == 'A';
      fix.kmh = fix.valid ? (float)atof(f[7]) * 1.852f : 0;
      fix.utcHhmmss = (uint32_t)atol(f[1]);
      fix.dateDdmmyy = (uint32_t)atol(f[9]);
      return true;
    }
    if (!strcmp(t, "GGA") && nf > 8) {
      fix.sats = atoi(f[7]);
      fix.hdop = f[8][0] ? (float)atof(f[8]) : 99;
    }
    if (!strcmp(t, "VTG") && nf > 7 && f[7][0]) fix.kmh = (float)atof(f[7]);
    return false;
  }
};

}  // namespace gps
