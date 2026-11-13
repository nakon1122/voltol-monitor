// PC test of the BMS engine (include/bms_engine.h): brand detection, live values,
// settings read, write + read-back check, write refusal, alarms.
//   g++ -std=c++17 -Iinclude -o bms_test test/bms_test.cpp src/bms_defs.cpp && ./bms_test
#include <cassert>
#include <cstdio>
#include <vector>
#include "bms_engine.h"
using namespace bmse;
// Real JK02_24S settings frame (0x01) quoted in syssi/esphome-jk-bms (OVP 4.30, UVP 2.90 ...)
static const uint8_t JK_SET[] = {0x55,0xAA,0xEB,0x90,0x01,0x4F,0x58,0x02,0x00,0x00,0x54,0x0B,0x00,0x00,0x80,0x0C,0x00,0x00,0xCC,0x10,0x00,0x00,0x68,0x10,0x00,0x00,0x0A,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF0,0x0A,0x00,0x00,0xA8,0x61,0x00,0x00,0x1E,0x00,0x00,0x00,0x3C,0x00,0x00,0x00,0xF0,0x49,0x02,0x00,0x2C,0x01,0x00,0x00,0x3C,0x00,0x00,0x00,0x3C,0x00,0x00,0x00,0xD0,0x07,0x00,0x00,0xBC,0x02,0x00,0x00,0x58,0x02,0x00,0x00,0xBC,0x02,0x00,0x00,0x58,0x02,0x00,0x00,0x38,0xFF,0xFF,0xFF,0x9C,0xFF,0xFF,0xFF,0x84,0x03,0x00,0x00,0xBC,0x02,0x00,0x00,0x0D,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x88,0x13,0x00,0x00,0xDC,0x05,0x00,0x00,0xE4,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40};
static bool near(float a, float b, float e = 1e-3f) { return fabsf(a - b) < e; }
static void fix(uint8_t *b) { uint8_t s = 0; for (int i = 0; i < 299; i++) s += b[i]; b[299] = s; }
static void le32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
// JK02_24S cell frame: 20 cells around 3.92 V, -48.6 A, SOC 78 %
static void jkCells(uint8_t *b, int n = 20) {
  memset(b, 0, 300);
  b[0] = 0x55; b[1] = 0xAA; b[2] = 0xEB; b[3] = 0x90; b[4] = 2;
  uint32_t tot = 0;
  for (int i = 0; i < n; i++) { uint16_t mv = 3920 + (i % 5) * 3; b[6 + i * 2] = mv & 255; b[7 + i * 2] = mv >> 8; tot += mv; }
  le32(b + 54, (1u << n) - 1);
  le32(b + 118, tot);
  le32(b + 126, (uint32_t)(int32_t)-48600);
  b[130] = 330 & 255; b[131] = 330 >> 8; b[132] = 340 & 255; b[133] = 340 >> 8; b[134] = 380 & 255; b[135] = 380 >> 8;
  b[141] = 78; le32(b + 142, 31200); le32(b + 146, 40000); le32(b + 150, 41);
  b[166] = 1; b[167] = 1;
  fix(b);
}
static void feedChunks(Engine &e, const uint8_t *p, size_t n, uint32_t now, size_t mtu = 20) {
  for (size_t i = 0; i < n; i += mtu) e.feed(p + i, n - i < mtu ? n - i : mtu, now);
}
static std::vector<uint8_t> jbdFrame(uint8_t cmd, const std::vector<uint8_t> &d) {
  std::vector<uint8_t> f = {0xDD, cmd, 0x00, (uint8_t)d.size()};
  f.insert(f.end(), d.begin(), d.end());
  uint16_t c = bms::jbdChecksum(f.data() + 2, d.size() + 2);
  f.push_back(c >> 8); f.push_back(c & 255); f.push_back(0x77);
  return f;
}
int main() {
  // ---------------- JK: identification, live data, settings
  Engine e;
  e.reset(true, false, false, false, 0);
  Out o;
  uint32_t t = 0;
  assert(!e.next(t, o));                       // settle first
  t += 1300; assert(e.next(t, o) && o.n == 20 && o.d[0] == 0xAA && o.d[4] == 0x97 && o.resp == R_WITHOUT);
  t += 1300; assert(e.next(t, o) && o.d[4] == 0x96);
  uint8_t cf[300]; jkCells(cf);
  for (int k = 0; k < 3; k++) feedChunks(e, cf, 300, t += 300, 128);
  assert(e.brand == bms::BR_JK && e.have && e.s.count == 20);
  assert(near(e.s.current, -48.6f) && near(e.s.soc, 78) && near(e.s.capacity, 31.2f) && e.s.cycles == 41);
  assert(e.pv[0] == 1 && e.pv[1] == 1);        // MOS from the live data
  t += 10; assert(e.next(t, o) && o.d[4] == 0x97);  // device info once
  t += 1600; assert(e.next(t, o) && o.d[4] == 0x96);  // starts the stream
  feedChunks(e, JK_SET, 300, t += 200);
  assert(e.settingsDone && near(e.pv[3], 4.30f) && near(e.pv[5], 2.90f) && e.pv[2] == 1);
  assert(bms::paramCount(bms::BR_JK) == 18);
  // check rules on the unified list
  assert(!bms::check(bms::BR_JK, e.pv, 3, 4.25f));
  assert(bms::check(bms::BR_JK, e.pv, 3, 4.21f));   // too close to OVP recovery 4.20
  assert(bms::check(bms::BR_JK, e.pv, 3, 4.6f));    // above the safe maximum
  // ---------------- JK write: frame, read back, verified
  assert(e.queueWrite(3, 4.25f));
  e.startWrites(t);
  t += 200; assert(e.next(t, o) && o.d[4] == bmsw::JK_PARAMS[bmsw::P_OVP].reg24 && o.d[6] == (4250 & 255) && o.d[7] == (4250 >> 8));
  t += 700; assert(e.next(t, o) && o.d[4] == 0x96);  // ask again
  uint8_t s2[300]; memcpy(s2, JK_SET, 300); le32(s2 + 18, 4250); fix(s2);
  feedChunks(e, s2, 300, t += 300);
  assert(!e.next(t += 10, o)); assert(!e.next(t += 10, o));
  assert(e.writeDone && e.writeOk && e.writeGood == 1 && near(e.pv[3], 4.25f));
  printf("JK ok: %s\n", e.writeMsg);
  // ---------------- JK write not taken by the BMS
  assert(e.queueWrite(4, 4.15f)); e.startWrites(t);
  for (int k = 0; k < 100; k++) e.next(t += 100, o);   // 10 s, the BMS keeps 4.20
  assert(e.writeDone && !e.writeOk && strstr(e.writeMsg, "kept the old value"));
  printf("JK refused: %s\n", e.writeMsg);
  // switch: discharge off
  assert(e.queueWrite(1, 0)); e.startWrites(t);
  t += 200; assert(e.next(t, o) && o.d[4] == 0x1E && o.d[6] == 0);
  // ---------------- JBD: brand from service FF00, live data, settings read
  Engine j;
  j.reset(true, true, false, false, 0);
  assert(j.brand == bms::BR_JBD);
  std::vector<uint8_t> basic(29, 0);
  basic[0] = 7740 >> 8; basic[1] = 7740 & 255;
  int16_t cur = -1250; basic[2] = uint16_t(cur) >> 8; basic[3] = uint16_t(cur) & 255;
  basic[4] = 3450 >> 8; basic[5] = 3450 & 255; basic[19] = 87; basic[20] = 0x03; basic[21] = 20; basic[22] = 3;
  for (int i = 0; i < 3; i++) { uint16_t k = 2731 + 280 + i * 10; basic[23 + i * 2] = k >> 8; basic[24 + i * 2] = k & 255; }
  std::vector<uint8_t> cells; for (int i = 0; i < 20; i++) { cells.push_back(3870 >> 8); cells.push_back(3870 & 255); }
  auto fb = jbdFrame(3, basic), fc = jbdFrame(4, cells);
  t = 0; assert(j.next(t += 600, o) && o.ch == CH_JBD && o.d[0] == 0xDD && o.d[2] == 3);
  feedChunks(j, fb.data(), fb.size(), t); feedChunks(j, fc.data(), fc.size(), t);
  assert(j.have && j.s.count == 20 && near(j.s.voltage, 77.4f) && j.pv[0] == 1);
  // settings read: factory, every register, exit
  int reads = 0; bool sawFactory = false, sawExit = false;
  for (int k = 0; k < 60 && !j.settingsDone; k++) {
    if (!j.next(t += 160, o)) continue;
    if (o.d[1] == 0x5A && o.d[2] == 0x00) sawFactory = true;
    if (o.d[1] == 0x5A && o.d[2] == 0x01) sawExit = true;
    if (o.d[1] == 0xA5 && o.d[2] >= 0x10 && o.d[2] <= 0x2F) {  // answer the register read
      reads++;
      uint16_t raw = o.d[2] == 0x24 ? 4200 : o.d[2] == 0x25 ? 4100 : 100;
      auto r = jbdFrame(o.d[2], {uint8_t(raw >> 8), uint8_t(raw & 255)});
      j.feed(r.data(), r.size(), t);
    }
  }
  assert(sawFactory && sawExit && reads == bset::JBD_N && j.settingsDone && near(j.pv[2], 4.2f) && near(j.pv[3], 4.1f));
  printf("JBD ok: %d registers\n", reads);
  // ---------------- alarms
  bms::Live L; L.nCells = 3; L.cell[0] = 3.90f; L.cell[1] = 4.31f; L.cell[2] = 3.91f; L.nTemp = 1; L.temp[0] = 30; L.mos = 85;
  uint32_t a = bms::alarms(L, bms::BR_JK, e.pv, e.pvHave);
  assert((a & bms::A_CELL_OV) && (a & bms::A_CELL_DIFF) && (a & bms::A_MOS_OT) && !(a & bms::A_CELL_UV));
  puts("bms_test PASS");
}
