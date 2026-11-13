// PC test for the automatic VOTOL TX/RX swap and the GPS fallback (real headers: uart_swap.h,
// gps_sched.h, gps.h). Simulates the link task loop (poll + up to 150 ms wait), the wiring, the
// controller and a u-blox MAX-M10S (factory: 9600 baud NMEA 1/s; after setup: 115200 UBX 10/s).
//   g++ -std=c++17 -Iinclude test/swap_test.cpp -o swap_test && ./swap_test
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "uart_swap.h"
#include "gps_sched.h"
#include "gps.h"
constexpr uint32_t GpsSched::BAUDS[3];

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---------------------------------------------------------------- link loop
struct LinkResult { int firstFrameMs = -1; bool saved = false; int switches = 0; int contentionMs = 0; int gpsCfgSent = 0, gpsCfgReached = 0; };

// wiredSwapped: the installer swapped the two VOTOL wires. ctlOnAt: controller powered at this time.
static LinkResult runLink(bool wiredSwapped, bool savedSwap, uint32_t ctlOnAt, uint32_t simMs, std::vector<uint32_t> gpsCfgAt = {0}) {
  LinkResult r;
  UartSwap sw;
  uint32_t t = 0;
  sw.begin(savedSwap, t);
  size_t gi = 0;
  while (t < simMs) {
    // GPS setup request from main (first one right after boot); link.cpp sendGpsConfig()
    if (gi < gpsCfgAt.size() && t >= gpsCfgAt[gi]) {
      gi++;
      if (sw.gpsSetupPossible()) {
        r.gpsCfgSent++;
        // sent in the guide orientation: it reaches the GPS only if the GPS RX really sits on that wire,
        // i.e. always (the GPS RX is wired to the guide's "board TX" pin); but if the VOTOL wires are
        // swapped, that pin is also the controller's TX -> both drive the wire while the controller is on
        r.gpsCfgReached++;
        if (wiredSwapped && t >= ctlOnAt) r.contentionMs += 900;
        t += 900;  // the setup takes ~0.9 s of the link loop
      }
    }
    bool orientOk = sw.swapped == wiredSwapped;
    bool ctlOn = t >= ctlOnAt;
    if (!orientOk && ctlOn) r.contentionMs += 35;  // our 33-byte poll at 9600 goes out on the controller's TX wire
    bool got = orientOk && ctlOn;
    t += got ? 75 : 150;                          // answer after ~75 ms, else the 150 ms timeout
    if (got && r.firstFrameMs < 0) r.firstFrameMs = (int)t;
    UartSwap::Act a = sw.update(t, got);
    if (a == UartSwap::SWITCH) r.switches++;
    if (a == UartSwap::SAVE) r.saved = sw.swapped;
  }
  return r;
}

// ------------------------------------------------------------- GPS module
static std::string nmea(const char *body) {
  uint8_t x = 0;
  for (const char *p = body; *p; p++) x ^= (uint8_t)*p;
  char b[160];
  snprintf(b, sizeof b, "$%s*%02X\r\n", body, x);
  return b;
}
static std::vector<uint8_t> pvtFrame(uint32_t itow) {
  std::vector<uint8_t> f = {0xB5, 0x62, 0x01, 0x07, 92, 0};
  std::vector<uint8_t> p(92, 0);
  memcpy(&p[0], &itow, 4);
  p[20] = 3; p[21] = 0x01; p[23] = 12;  // 3D fix, gnssFixOK, 12 satellites
  f.insert(f.end(), p.begin(), p.end());
  uint8_t a = 0, b = 0;
  for (size_t i = 2; i < f.size(); i++) { a += f[i]; b += a; }
  f.push_back(a); f.push_back(b);
  return f;
}
struct GpsResult { int firstMs = -1; float hz = 0; int samples = 0; bool ubx = false; int lostMs = 0; };

// setupReaches: the u-blox setup sent by the link task arrives at the GPS RX (false when the VOTOL wires
// are swapped: then it is never sent). Runs main.cpp's GPS reading + 2-second housekeeping.
static GpsResult runGps(bool setupReaches, bool blocked, uint32_t simMs, uint32_t measureFrom) {
  GpsResult r;
  gps::Nmea nm;
  gps::Ubx ub;
  GpsSched sch;
  sch.cfgAt = 0;                            // setup requested at boot (main.cpp setup())
  uint32_t gpsBaud = 9600;                  // MAX-M10S factory
  bool gpsUbx = false, S_ubx = false;
  int gpsRate = 1;                          // updates per second
  uint32_t last = 0, count = 0, hzAt = 0, nextMsg = 1000, itow = 0;
  float hz = 0;
  uint32_t samplesMeasured = 0, lastSample = 0;
  bool pendingCfg = setupReaches && !blocked;  // the boot-time request
  uint32_t cfgDoneAt = 200;
  for (uint32_t t = 0; t < simMs; t += 10) {
    if (pendingCfg && t >= cfgDoneAt) {      // GPS switches to 115200 + NAV-PVT 10/s
      pendingCfg = false; gpsBaud = 115200; gpsUbx = true; gpsRate = 10; nextMsg = t + 100;
    }
    if (t >= nextMsg) {
      nextMsg += 1000 / gpsRate;
      uint32_t boardBaud = GpsSched::BAUDS[sch.baudIx];
      if (boardBaud == gpsBaud) {
        bool sample = false;
        if (gpsUbx) {
          for (uint8_t c : pvtFrame(itow += 100)) if (ub.push(c)) { sample = true; S_ubx = true; }
        } else {
          std::string s = nmea("GPGGA,010203.00,1345.0000,N,10030.0000,E,1,09,0.9,10.0,M,0.0,M,,") +
                          nmea("GPRMC,010203.00,A,1345.0000,N,10030.0000,E,25.0,0.0,131126,,,A");
          for (char c : s) if (nm.push(c) && !S_ubx) sample = true;
        }
        if (sample) {
          last = t; count++;
          if (r.firstMs < 0) r.firstMs = (int)t;
          if (t >= measureFrom) {
            samplesMeasured++;
            if (lastSample && t - lastSample > 1500) r.lostMs += t - lastSample;
            lastSample = t;
          }
        }
      }
    }
    if (t - hzAt >= 2000) {
      hz = count * 1000.0f / (t - hzAt);
      count = 0; hzAt = t;
      bool cfg;
      sch.tick(t, last, S_ubx, hz, 7, blocked, cfg);
      if (cfg && setupReaches) { pendingCfg = true; cfgDoneAt = t + 200; }
    }
  }
  r.samples = samplesMeasured;
  r.hz = samplesMeasured * 1000.0f / (simMs - measureFrom);
  r.ubx = S_ubx;
  return r;
}

int main() {
  printf("== VOTOL TX/RX auto swap ==\n");
  struct { const char *name; bool wired, saved; uint32_t on; } L[] = {
    {"wired as in the guide, first boot", false, false, 0},
    {"wires swapped, first boot", true, false, 0},
    {"wires swapped, later boots (remembered)", true, true, 0},
    {"wired as in the guide, but board remembered swapped (re-wired)", false, true, 0},
    {"wires swapped, controller switched on after 20 s", true, false, 20000},
    {"wired as in the guide, controller switched on after 20 s", false, false, 20000},
  };
  for (auto &c : L) {
    LinkResult r = runLink(c.wired, c.saved, c.on, 60000);
    int after = r.firstFrameMs < 0 ? -1 : r.firstFrameMs - (int)c.on;
    printf("%-64s data after %5.2f s  saved=%s  wire fight %.2f s  GPS setup sent %d\n", c.name, after / 1000.0,
           r.saved ? "swapped" : "normal ", r.contentionMs / 1000.0, r.gpsCfgSent);
    CHECK(r.firstFrameMs >= 0, "no data");
    CHECK(after <= 3600, "too slow: %d ms", after);
    CHECK(r.saved == c.wired, "remembered the wrong orientation");
    if (c.wired && c.saved) CHECK(r.contentionMs == 0 && r.gpsCfgSent == 0, "a remembered swapped board must never drive the controller's TX wire");
  }

  printf("\n== GPS (MAX-M10S) update rate ==\n");
  GpsResult n = runGps(true, false, 120000, 30000);
  GpsResult s = runGps(false, true, 120000, 30000);
  printf("wired as in the guide: first data %.1f s, %s, %.1f updates/s, gaps %.1f s\n", n.firstMs / 1000.0, n.ubx ? "u-blox NAV-PVT" : "NMEA", n.hz, n.lostMs / 1000.0);
  printf("VOTOL wires swapped:   first data %.1f s, %s, %.1f updates/s, gaps %.1f s\n", s.firstMs / 1000.0, s.ubx ? "u-blox NAV-PVT" : "NMEA", s.hz, s.lostMs / 1000.0);
  CHECK(n.ubx && n.hz > 9.5f, "configured GPS should give ~10/s");
  CHECK(!s.ubx && s.hz > 0.95f && s.hz < 1.05f, "swapped: factory 1/s");
  CHECK(s.lostMs == 0, "swapped: the baud hunt must not drop the GPS every 15 s");
  CHECK(s.firstMs >= 0 && s.firstMs < 6000, "swapped: GPS found within 6 s");
  // old behaviour (setup re-sent every 15 s although it can never arrive) for comparison
  GpsResult o = runGps(false, false, 120000, 30000);
  printf("(old firmware, swapped: %.2f updates/s, gaps %.1f s in 90 s)\n", o.hz, o.lostMs / 1000.0);

  printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
  return fails ? 1 : 0;
}
