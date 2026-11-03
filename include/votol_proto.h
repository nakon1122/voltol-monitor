// VOTOL EM-series controller UART protocol (EM-30 .. EM-260, "EM-2 series").
// Decoded from the official PC tuning tools EM_V3-40 and EM_V3-EMsport.
// Pure C++ (no Arduino) so it is unit-tested on the PC (test/proto_test.cpp).
//
// Every frame is 24 bytes:
//   [0]  header   0xC9 host->controller, 0xC0 controller->host (0xC9 accepted too)
//   [1]  0x14     payload length (20) -> total = [1] + 4
//   [2]  0x02
//   [3..21]       command + data
//   [22] XOR of bytes 0..21
//   [23] 0x0D
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>

namespace votol {

static const int FRAME_LEN = 24;
static const int PAGES = 7;          // parameter pages P1..P7
static const int PAGE_BYTES = 17;    // frame bytes 5..21
static const int PARAM_BYTES = PAGES * PAGE_BYTES;  // 119

inline uint8_t xorSum(const uint8_t *f, int n) {
  uint8_t x = 0;
  for (int i = 0; i < n; i++) x ^= f[i];
  return x;
}
inline void seal(uint8_t *f) {
  f[0] = 0xC9; f[1] = 0x14; f[2] = 0x02;
  f[22] = xorSum(f, 22);
  f[23] = 0x0D;
}
inline void cmdFrame(uint8_t *f, const char *cmd) {
  memset(f, 0, FRAME_LEN);
  memcpy(f + 3, cmd, strlen(cmd));
  seal(f);
}

// --- requests --------------------------------------------------------------
// Live-data poll. chkA/chkB come from P1[18..21], chk3 from P5[21]; the official
// tool fills them from the last parameter read. Remote control is always OFF
// (0xAA) so the dashboard can never move the motor.
inline void buildShow(uint8_t *f, uint16_t chkA, uint16_t chkB, uint8_t chk3) {
  cmdFrame(f, "SHOW");
  f[12] = 0xAA;            // remote control off
  f[16] = chk3;
  f[17] = 0xAA;
  f[18] = chkA >> 8; f[19] = chkA & 0xFF;
  f[20] = chkB >> 8; f[21] = chkB & 0xFF;
  seal(f);
}
inline void buildRead(uint8_t *f) { cmdFrame(f, "LDGET"); }
inline void buildClearFaults(uint8_t *f) { cmdFrame(f, "CLERR"); }
// Write one parameter page (1..7) from the 119-byte parameter image.
inline void buildWrite(uint8_t *f, int page, const uint8_t *params) {
  memset(f, 0, FRAME_LEN);
  f[3] = 'P'; f[4] = (uint8_t)page;
  memcpy(f + 5, params + (page - 1) * PAGE_BYTES, PAGE_BYTES);
  seal(f);
}

// --- receive framer ---------------------------------------------------------
// Feed bytes one by one; returns true when a complete, valid frame is in buf.
struct Framer {
  uint8_t buf[64];
  int n = 0, need = 0;
  uint32_t good = 0, bad = 0;
  bool push(uint8_t b) {
    if (n == 0) {
      if (b == 0xC0 || b == 0xC9) buf[n++] = b;
      return false;
    }
    if (n == 1) {
      if (b < 4 || b > 56) { n = 0; bad++; return push(b); }
      buf[n++] = b; need = b + 4;
      return false;
    }
    buf[n++] = b;
    if (n < need) return false;
    bool ok = buf[need - 1] == 0x0D && buf[need - 2] == xorSum(buf, need - 2);
    n = 0;
    if (ok) good++; else bad++;
    return ok;
  }
};

enum Kind { K_NONE, K_LIVE, K_PARAM, K_WRITE_ACK, K_OTHER };
inline Kind kindOf(const uint8_t *f) {
  char a = f[3], b = f[4];
  if (a == 'R' && b >= 1 && b <= PAGES) return K_PARAM;
  if (a == 'P' && b == 'N') return K_WRITE_ACK;
  if ((a == 'Y' && b == 'B') || (a == 'E' && (b == '1' || b == '2'))) return K_LIVE;
  return K_OTHER;
}

// --- live data --------------------------------------------------------------
enum Status { ST_IDLE, ST_INIT, ST_START, ST_RUN, ST_STOP, ST_BRAKE, ST_WAIT, ST_FAULT };
static const char *const STATUS_NAMES[8] = {"IDLE", "INIT", "START", "RUN",
                                            "STOP", "BRAKE", "WAIT", "FAULT"};
struct Live {
  float volt = 0, amp = 0;   // V, A (A negative while regenerating)
  int16_t rpm = 0;
  int16_t tCtrl = 0, tMotor = 0;  // deg C
  uint16_t tCoef = 0;
  uint32_t faults = 0;
  uint8_t gearBits = 0, status = 0;
  uint8_t gear() const { return gearBits & 3; }  // 0 L 1 M 2 H 3 S
};
inline int16_t be16s(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }
inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline void parseLive(const uint8_t *f, Live &d) {
  d.volt = be16(f + 5) / 10.0f;
  d.amp = be16s(f + 7) / 10.0f;
  d.faults = ((uint32_t)f[10] << 24) | ((uint32_t)f[11] << 16) | (f[12] << 8) | f[13];
  d.rpm = be16s(f + 14);
  d.tCtrl = (int)f[16] - 50;
  d.tMotor = (int)f[17] - 50;
  d.tCoef = be16(f + 18);
  d.gearBits = f[20];
  d.status = f[21];
}

// Fault bits as labelled by the official tool's "Fault display" box.
struct FaultDef { uint32_t mask; const char *name; };
static const FaultDef FAULTS[] = {
    {0x00000002, "Overcurrent"},  {0x00000004, "Undervoltage"},
    {0x00000010, "Overvoltage"},  {0x00001000, "Overheating"},
    {0x00000080, "Hall fault"},   {0x00000008, "Motor hall"},
    {0x00000001, "Brake"},        {0x00000800, "Out of control"},
    {0x00800200, "Controller failure"},
};
static const int FAULT_COUNT = sizeof(FAULTS) / sizeof(FAULTS[0]);
// Writes "Overcurrent, Hall fault" (or "0x00400000" for unknown bits).
inline void faultText(uint32_t f, char *out, int cap) {
  out[0] = 0;
  if (!f) { snprintf(out, cap, "None"); return; }
  uint32_t known = 0;
  int len = 0;
  for (int i = 0; i < FAULT_COUNT; i++)
    if (f & FAULTS[i].mask) {
      known |= FAULTS[i].mask;
      len += snprintf(out + len, len < cap ? cap - len : 0, "%s%s", len ? ", " : "", FAULTS[i].name);
    }
  if (f & ~known && len < cap)
    snprintf(out + len, cap - len, "%s0x%08X", len ? ", " : "", (unsigned)(f & ~known));
}

// --- parameter image --------------------------------------------------------
// params[] holds frame bytes 5..21 of pages P1..P7 back to back.
inline int pIndex(int page, int frameOff) { return (page - 1) * PAGE_BYTES + frameOff - 5; }

struct ParamImage {
  uint8_t b[PARAM_BYTES];
  uint8_t havePages = 0;  // bit n-1 set when page n received
  bool complete() const { return havePages == 0x7F; }
  void storePage(const uint8_t *f) {
    int p = f[4];
    if (p < 1 || p > PAGES) return;
    memcpy(b + (p - 1) * PAGE_BYTES, f + 5, PAGE_BYTES);
    havePages |= 1 << (p - 1);
  }
  uint16_t chkA() const { return be16(b + pIndex(1, 18)); }
  uint16_t chkB() const { return be16(b + pIndex(1, 20)); }
  uint8_t chk3() const { return b[pIndex(5, 21)]; }
  // The "current check result value" the PC tool shows: byte sum used to tell
  // two parameter sets apart (not sent to the controller).
  uint32_t checkSum() const {
    uint32_t s = 0;
    for (int p = 1; p <= 6; p++)
      for (int o = 5; o <= 21; o++) s += b[pIndex(p, o)];
    s -= b[pIndex(3, 17)];  // tool skips P3[17]
    s -= b[pIndex(4, 21)];
    s -= b[pIndex(6, 21)];
    for (int o = 5; o <= 14; o++) s += b[pIndex(7, o)];
    return s;
  }
};

// Field types for the on-screen editor.
enum FType : uint8_t { T_U8, T_S16, T_U16, T_THR };
struct ParamDef {
  const char *group, *name, *unit;
  uint8_t page, off;  // frame offset 5..21
  FType type;
  uint8_t dec;        // decimals shown; raw = value * 10^dec
  int32_t min, max;   // raw limits accepted by the editor
  int32_t step;       // raw step of the +/- buttons
};
// Groups mirror the PC tool's tabs. T_THR = throttle voltage byte:
// volts = floor(raw*550/255 + 2.2) / 100.
static const ParamDef PARAMS[] = {
    {"Battery", "Overvoltage", "V", 1, 7, T_S16, 1, 100, 2000, 1},
    {"Battery", "Undervoltage", "V", 1, 16, T_S16, 1, 100, 2000, 1},
    {"Battery", "Soft undervoltage", "V", 1, 9, T_S16, 1, 100, 2000, 1},
    {"Battery", "Undervoltage variation", "V", 1, 11, T_U8, 1, 0, 255, 1},
    {"Battery", "Busbar current", "A", 1, 12, T_U16, 0, 0, 1000, 1},
    {"Battery", "Phase current", "A", 1, 14, T_U16, 0, 0, 2000, 1},
    {"Sport", "Sport current limit", "A", 2, 5, T_S16, 0, 0, 1000, 1},
    {"Sport", "Sport flux-weakening", "", 2, 10, T_S16, 0, 0, 10000, 10},
    {"Sport", "Flux-weak compensation", "", 2, 21, T_U8, 0, 0, 255, 1},
    {"Sport", "Speed ratio", "%", 2, 17, T_U8, 0, 0, 100, 1},
    {"Sport", "Soft-start grade", "", 2, 18, T_U8, 0, 1, 16, 1},
    {"Sport", "Logout time", "s", 2, 19, T_U8, 0, 0, 255, 1},
    {"Sport", "Recovery time", "s", 2, 20, T_U8, 0, 0, 255, 1},
    {"3-Speed", "Low speed", "%", 2, 7, T_U8, 0, 0, 100, 1},
    {"3-Speed", "Mid speed", "%", 2, 8, T_U8, 0, 0, 100, 1},
    {"3-Speed", "High speed", "%", 2, 9, T_U8, 0, 0, 100, 1},
    {"3-Speed", "Mid flux-weakening", "", 2, 14, T_S16, 0, 0, 10000, 10},
    {"3-Speed", "High flux-weakening", "", 2, 12, T_S16, 0, 0, 10000, 10},
    {"3-Speed", "Low speed (2)", "%", 4, 12, T_U8, 0, 0, 100, 1},
    {"3-Speed", "Mid speed (2)", "%", 4, 13, T_U8, 0, 0, 100, 1},
    {"3-Speed", "High speed (2)", "%", 4, 14, T_U8, 0, 0, 100, 1},
    {"3-Speed", "Flux-weakening (2)", "", 4, 15, T_S16, 0, 0, 10000, 10},
    {"3-Speed", "Mid flux-weak (2)", "", 4, 19, T_S16, 0, 0, 10000, 10},
    {"3-Speed", "High flux-weak (2)", "", 4, 17, T_S16, 0, 0, 10000, 10},
    {"3-Speed", "HDC lowest speed", "", 4, 10, T_S16, 0, 0, 3000, 10},
    {"Motor", "Pole pairs", "", 3, 18, T_U8, 0, 1, 64, 1},
    {"Motor", "Hall shift angle", "", 3, 5, T_S16, 0, -360, 360, 1},
    {"Motor", "Reverse speed limit", "%", 3, 16, T_U8, 0, 0, 100, 1},
    {"Motor", "EBS ratio", "%", 3, 19, T_U8, 0, 0, 100, 1},
    {"Temperature", "Initial temperature", "C", 3, 7, T_U8, 0, 0, 200, 1},
    {"Temperature", "Over-temperature", "C", 3, 8, T_U8, 0, 0, 200, 1},
    {"Temperature", "Max temperature", "C", 3, 9, T_U8, 0, 0, 200, 1},
    {"Temperature", "T.C.1", "", 3, 10, T_S16, 0, -32000, 32000, 1},
    {"Temperature", "T.C.2", "", 3, 12, T_S16, 0, -32000, 32000, 1},
    {"Temperature", "T.C.3", "", 3, 14, T_S16, 0, -32000, 32000, 1},
    {"Throttle", "Start voltage", "V", 7, 5, T_THR, 2, 0, 255, 1},
    {"Throttle", "End voltage", "V", 7, 6, T_THR, 2, 0, 255, 1},
    {"Throttle", "Low protect", "V", 7, 7, T_THR, 2, 0, 255, 1},
    {"Throttle", "High protect", "V", 7, 8, T_THR, 2, 0, 255, 1},
    {"Throttle", "Start torque", "", 7, 11, T_U16, 0, 0, 5000, 10},
    {"Throttle", "Combinative torque", "", 7, 13, T_U16, 0, 0, 5000, 10},
    {"Throttle", "Rate of rise", "", 7, 10, T_U8, 0, 0, 255, 1},
    {"Throttle", "Rate of decline", "", 7, 9, T_U8, 0, 0, 255, 1},
};
static const int PARAM_COUNT = sizeof(PARAMS) / sizeof(PARAMS[0]);
static const char *const GROUPS[] = {"Battery", "Sport", "3-Speed", "Motor", "Temperature", "Throttle"};
static const int GROUP_COUNT = 6;

inline int32_t getRaw(const uint8_t *img, const ParamDef &d) {
  const uint8_t *p = img + pIndex(d.page, d.off);
  switch (d.type) {
  case T_S16: return be16s(p);
  case T_U16: return be16(p);
  default: return p[0];
  }
}
inline void setRaw(uint8_t *img, const ParamDef &d, int32_t v) {
  if (v < d.min) v = d.min;
  if (v > d.max) v = d.max;
  uint8_t *p = img + pIndex(d.page, d.off);
  if (d.type == T_S16 || d.type == T_U16) { p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF; }
  else p[0] = (uint8_t)v;
}
// Value as shown on screen (e.g. 84.0 V, 4.30 V throttle).
inline float shown(const uint8_t *img, const ParamDef &d) {
  int32_t r = getRaw(img, d);
  if (d.type == T_THR) return (int)(r * 550 / 255 + 2.2) / 100.0f;
  float v = (float)r;
  for (int i = 0; i < d.dec; i++) v /= 10;
  return v;
}
inline void formatValue(const uint8_t *img, const ParamDef &d, char *out, int cap) {
  snprintf(out, cap, "%.*f", d.dec, shown(img, d));
}

// Default-gear and direction flags (read-only on screen).
inline int defaultGear(const uint8_t *img) {  // 0 Low 1 Mid 2 High, -1 unknown
  uint8_t f = img[pIndex(2, 16)];
  if (f & 0x08) return 0;
  if (f & 0x04) return 1;
  if (f & 0x02) return 2;
  return -1;
}
inline bool motorReversed(const uint8_t *img) { return img[pIndex(3, 17)] & 0x80; }

}  // namespace votol
