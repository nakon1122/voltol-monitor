// Optional GPS module. Best: u-blox M8 / M10 - the firmware configures it at
// every start // (115200 baud, 10 or 25 Hz, airborne <2g model so hard launches are not
// smoothed, UBX NAV-PVT) and then uses NAV-PVT: speed in mm/s with its accuracy, and the GPS time of each
// fix (iTOW) so drag splits are timed from the satellites' clock, not from when
// the bytes arrived. Any other NMEA receiver still works through RMC/GGA.
// Pure C++ (unit-tested: test/ride_test.cpp).
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

// ------------------------------------------------------------------ UBX ----
struct Pvt {
  uint32_t iTOW = 0;      // GPS time of week, ms
  bool fixOk = false;     // gnssFixOK and 3D fix
  uint8_t numSV = 0;
  float kmh = 0, sAccKmh = 99, hAccM = 99;
  bool timeValid = false;
  uint16_t year = 0;
  uint8_t mon = 0, day = 0, hour = 0, min = 0, sec = 0;
};

class Ubx {
public:
  Pvt pvt;
  uint32_t frames = 0;
  // Returns true when a NAV-PVT message was completed.
  bool push(uint8_t b) {
    switch (st) {
    case 0: st = b == 0xB5 ? 1 : 0; return false;
    case 1: st = b == 0x62 ? 2 : (b == 0xB5 ? 1 : 0); return false;
    case 2: cls = b; ca = cb = 0; sum(b); st = 3; return false;
    case 3: id = b; sum(b); st = 4; return false;
    case 4: len = b; sum(b); st = 5; return false;
    case 5: len |= b << 8; sum(b); n = 0; st = len > sizeof(buf) ? 0 : (len ? 6 : 7); return false;
    case 6: buf[n++] = b; sum(b); if (n == len) st = 7; return false;
    case 7: st = b == ca ? 8 : 0; return false;
    case 8:
      st = 0;
      if (b != cb) return false;
      frames++;
      if (cls == 0x01 && id == 0x07 && len >= 92) { parsePvt(); return true; }
      return false;
    }
    st = 0;
    return false;
  }

private:
  uint8_t buf[100], cls = 0, id = 0, ca = 0, cb = 0;
  uint16_t len = 0, n = 0;
  int st = 0;
  void sum(uint8_t b) { ca += b; cb += ca; }
  uint32_t u32(int o) const { return buf[o] | buf[o + 1] << 8 | buf[o + 2] << 16 | (uint32_t)buf[o + 3] << 24; }
  void parsePvt() {
    pvt.iTOW = u32(0);
    pvt.year = buf[4] | buf[5] << 8;
    pvt.mon = buf[6]; pvt.day = buf[7]; pvt.hour = buf[8]; pvt.min = buf[9]; pvt.sec = buf[10];
    pvt.timeValid = (buf[11] & 3) == 3;
    pvt.fixOk = (buf[21] & 1) && buf[20] >= 3 && buf[20] <= 4;
    pvt.numSV = buf[23];
    pvt.hAccM = u32(40) / 1000.0f;
    int32_t g = (int32_t)u32(60);
    pvt.kmh = g * 0.0036f;
    pvt.sAccKmh = u32(68) * 0.0036f;
  }
};

// UBX message with sync bytes and Fletcher checksum; returns its length.
inline int ubxMsg(uint8_t *o, uint8_t cls, uint8_t id, const uint8_t *p, int len) {
  o[0] = 0xB5; o[1] = 0x62; o[2] = cls; o[3] = id; o[4] = len & 0xFF; o[5] = len >> 8;
  if (len) memcpy(o + 6, p, len);
  uint8_t a = 0, b = 0;
  for (int i = 2; i < 6 + len; i++) { a += o[i]; b += a; }
  o[6 + len] = a; o[7 + len] = b;
  return len + 8;
}
// u-blox M10 style key/value config (RAM layer only, re-sent at every start).
struct ValSet {
  uint8_t p[96];
  int n = 4;
  ValSet() { p[0] = 0; p[1] = 0x01; p[2] = p[3] = 0; }
  void add(uint32_t key, uint32_t v) {
    for (int i = 0; i < 4; i++) p[n++] = (key >> (8 * i)) & 0xFF;
    int sz = (key >> 28) & 7;
    int bytes = sz == 4 ? 4 : sz == 3 ? 2 : 1;
    for (int i = 0; i < bytes; i++) p[n++] = (v >> (8 * i)) & 0xFF;
  }
  int build(uint8_t *o) const { return ubxMsg(o, 0x06, 0x8A, p, n); }
};
// Step 1 (sent at the module's current baud): switch UART1 to 115200.
inline int ubxBaudCmd(uint8_t *o) {
  int k = 0;
  uint8_t prt[20] = {1, 0, 0, 0, 0xD0, 0x08, 0, 0, 0x00, 0xC2, 0x01, 0x00, 0x03, 0, 0x03, 0, 0, 0, 0, 0};
  k += ubxMsg(o + k, 0x06, 0x00, prt, 20);  // M8: CFG-PRT
  ValSet v;
  v.add(0x40520001, 115200);                // M10: CFG-UART1-BAUDRATE
  k += v.build(o + k);
  return k;
}
// Step 2 (at 115200): rate, automotive model, NAV-PVT on, chatty NMEA off.
inline int ubxSetupCmd(uint8_t *o, int hz) {
  int k = 0;
  uint16_t ms = hz >= 25 ? 40 : hz >= 20 ? 50 : 100;
  uint8_t rate[6] = {(uint8_t)(hz >= 25 ? 100 : ms), 0, 1, 0, 1, 0};  // M8 tops out at 10 Hz
  k += ubxMsg(o + k, 0x06, 0x08, rate, 6);  // CFG-RATE
  // Airborne <2g: does not smooth away the > 1 g acceleration of a drag launch
  uint8_t nav5[36] = {0x01, 0x00, 7};
  k += ubxMsg(o + k, 0x06, 0x24, nav5, 36); // CFG-NAV5 dynModel 7
  const uint8_t msgs[5][3] = {{0x01, 0x07, 1}, {0xF0, 0x03, 0}, {0xF0, 0x02, 0}, {0xF0, 0x01, 0}, {0xF0, 0x05, 0}};
  for (auto &m : msgs) k += ubxMsg(o + k, 0x06, 0x01, m, 3);  // CFG-MSG: PVT on, GSV/GSA/GLL/VTG off
  ValSet v;
  v.add(0x30210001, ms);   // CFG-RATE-MEAS
  v.add(0x30210002, 1);    // CFG-RATE-NAV
  v.add(0x20110021, 7);    // CFG-NAVSPG-DYNMODEL airborne <2g
  v.add(0x20910007, 1);    // CFG-MSGOUT-UBX_NAV_PVT_UART1
  v.add(0x209100c5, 0);    // GSV off
  v.add(0x209100c0, 0);    // GSA off
  v.add(0x209100ca, 0);    // GLL off
  v.add(0x209100b1, 0);    // VTG off
  k += v.build(o + k);
  return k;
}

}  // namespace gps
