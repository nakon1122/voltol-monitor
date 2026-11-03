// g++ -std=c++17 -Iinclude -o proto_test test/proto_test.cpp && ./proto_test
#include "votol_proto.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace votol;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static void hexEq(const uint8_t *a, const char *hex) {
  for (int i = 0; i < 24; i++) {
    unsigned v; sscanf(hex + i * 3, "%2x", &v);
    if (a[i] != v) { printf("byte %d: got %02x want %02x\n", i, a[i], v); fails++; }
  }
}
int main() {
  uint8_t f[24];
  // SHOW poll captured from the official tool (endless-sphere thread 112970).
  buildShow(f, 0x0467, 0x00f3, 0x1e);
  hexEq(f, "c9 14 02 53 48 4f 57 00 00 00 00 00 aa 00 00 00 1e aa 04 67 00 f3 52 0d");
  // Page-2 write with current limit 30 A, same source.
  uint8_t img[PARAM_BYTES] = {0};
  const uint8_t p2[17] = {0x00,0x1e,0x2d,0x50,0x64,0x0f,0xa0,0x0f,0xa0,0x0b,0xb8,0x39,0x28,0x10,0x14,0x1e,0x3c};
  memcpy(img + PAGE_BYTES, p2, 17);
  buildWrite(f, 2, img);
  hexEq(f, "c9 14 02 50 02 00 1e 2d 50 64 0f a0 0f a0 0b b8 39 28 10 14 1e 3c 0e 0d");
  // ...and 31 A gives the second captured frame.
  const ParamDef *lim = nullptr;
  for (auto &d : PARAMS) if (!strcmp(d.name, "Sport current limit")) lim = &d;
  CHECK(lim && getRaw(img, *lim) == 30);
  setRaw(img, *lim, 31);
  buildWrite(f, 2, img);
  hexEq(f, "c9 14 02 50 02 00 1f 2d 50 64 0f a0 0f a0 0b b8 39 28 10 14 1e 3c 0f 0d");
  for (auto &d : PARAMS) {
    if (!strcmp(d.name, "Low speed")) CHECK(getRaw(img, d) == 45);
    if (!strcmp(d.name, "Speed ratio")) CHECK(getRaw(img, d) == 40);
    if (!strcmp(d.name, "Soft-start grade")) CHECK(getRaw(img, d) == 16);
  }
  CHECK(defaultGear(img) == 0);  // 0x39 -> bit 0x08 -> Low
  // Framer: garbage, then a live frame split across pushes.
  Framer fr;
  uint8_t live[24] = {0xC0, 0x14, 0x02, 'Y', 'B', 0x03, 0x10, 0x01, 0xE6, 0, 0, 0, 0x00, 0x02,
                      0x10, 0x18, 92, 105, 0, 0, 0x02, 3, 0, 0x0D};
  live[22] = xorSum(live, 22);
  int got = 0;
  const uint8_t junk[] = {0x00, 0xC0, 0x55, 0x13};
  for (uint8_t b : junk) got += fr.push(b);
  for (uint8_t b : live) got += fr.push(b);
  CHECK(got == 1);
  CHECK(kindOf(fr.buf) == K_LIVE);
  Live d;
  parseLive(fr.buf, d);
  CHECK(fabs(d.volt - 78.4f) < 0.01 && fabs(d.amp - 48.6f) < 0.01 && d.rpm == 4120);
  CHECK(d.tCtrl == 42 && d.tMotor == 55 && d.gear() == 2 && d.status == ST_RUN && d.faults == 2);
  char t[64];
  faultText(0x00400802, t, sizeof t);
  CHECK(!strcmp(t, "Overcurrent, Out of control, 0x00400000"));
  // Regen current is negative.
  live[7] = 0xFF; live[8] = 0x38; live[22] = xorSum(live, 22);
  for (uint8_t b : live) fr.push(b);
  parseLive(fr.buf, d);
  CHECK(fabs(d.amp + 20.0f) < 0.01);
  // Param pages assemble into a complete image.
  ParamImage pi;
  for (int p = 1; p <= 7; p++) {
    uint8_t r[24] = {0xC0, 0x14, 0x02, 'R', (uint8_t)p};
    for (int i = 5; i < 22; i++) r[i] = p * 16 + i;
    r[22] = xorSum(r, 22); r[23] = 0x0D;
    CHECK(kindOf(r) == K_PARAM);
    pi.storePage(r);
  }
  CHECK(pi.complete());
  CHECK(pi.chkA() == ((1 * 16 + 18) << 8 | (1 * 16 + 19)));
  CHECK(pi.chk3() == 5 * 16 + 21);
  // Throttle voltage scaling of the PC tool: floor(raw*550/255 + 2.2) / 100.
  ParamDef thr = {"Throttle", "x", "V", 7, 5, T_THR, 2, 0, 255, 1};
  uint8_t im2[PARAM_BYTES] = {0};
  im2[pIndex(7, 5)] = 51;  // 51*550/255 = 110 -> 112.2 -> 1.12 V
  CHECK(fabs(shown(im2, thr) - 1.12f) < 0.001);
  printf(fails ? "proto_test: %d FAILED\n" : "proto_test: all passed\n", fails);
  return fails ? 1 : 0;
}
