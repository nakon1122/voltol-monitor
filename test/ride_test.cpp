// g++ -std=c++17 -Iinclude -o ride_test test/ride_test.cpp && ./ride_test
#include "ride.h"
#include "gps.h"
#include <cstdio>
#include <cmath>
#include <cstring>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
using namespace ride;

// Ideal run: constant acceleration a until vmax, sampled every dt starting at phase.
static double worst = 0;
static void run(double a, double vmaxKmh, double dt, double phase, double launch) {
  DragTimer d;
  d.arm();
  double vmax = vmaxKmh / 3.6;
  for (double t = phase; t < launch + 40; t += dt) {
    double tau = t - launch, v = tau <= 0 ? 0 : std::min(a * tau, vmax);
    d.feed(t, v * 3.6, 100, 72);
    if (d.state == DragTimer::DONE) break;
  }
  CHECK(d.state == DragTimer::DONE);
  double tAcc = vmax / a, dAcc = 0.5 * a * tAcc * tAcc;
  for (int i = 0; i < SPLITS; i++) {
    double D = SPLIT_M[i];
    double want = D <= dAcc ? sqrt(2 * D / a) : tAcc + (D - dAcc) / vmax;
    double vWant = (D <= dAcc ? a * want : vmax) * 3.6;
    double err = fabs(d.r.s[i].t - want);
    if (err > worst) worst = err;
    CHECK(err < 0.02);
    CHECK(fabs(d.r.s[i].trapKmh - vWant) < 1.0);
  }
}
int main() {
  CHECK(fabs(rpmToKmh(1000, 1884.96f, 1) - 113.1f) < 0.1);
  CHECK(fabs(tyreCircMm(130, 70, 17) - 1928.4f) < 1);
  for (double ph = 0; ph < 0.1; ph += 0.013) {
    run(3.0, 200, 0.1, ph, 1.537);   // 10 Hz UART
    run(5.5, 110, 0.1, ph, 2.011);   // hits top speed before 402 m
    run(4.0, 150, 0.04, ph, 0.777);  // 25 Hz GPS
  }
  printf("drag worst split error: %.4f s\n", worst);
  // Not armed -> nothing happens; stop before the end -> aborted.
  DragTimer d;
  d.feed(0, 0); d.feed(1, 50);
  CHECK(d.state == DragTimer::IDLE);
  d.arm();
  d.feed(0, 0); d.feed(0.1, 5); d.feed(0.2, 10);
  for (double t = 0.3; t < 4; t += 0.1) d.feed(t, 0);
  CHECK(d.state == DragTimer::ABORTED);
  // Wheelspin flag on an impossible jump.
  d.arm(); d.feed(0, 0); d.feed(0.1, 20); d.feed(0.2, 80);
  CHECK(d.r.wheelspin);
  // Trip energy: 100 A at 72 V for 36 s = 72 Wh, 1 Ah.
  Trip tr;
  for (int i = 0; i <= 360; i++) tr.add(i * 0.1, 36, 100, 72);
  CHECK(fabs(tr.wh - 72) < 0.01 && fabs(tr.ah - 1) < 0.001 && fabs(tr.distM - 360) < 0.01);
  CHECK(socFromVolt(84.0f, 20, 3.0f, 4.2f) == 100 && socFromVolt(72.0f, 20, 3.0f, 4.2f) == 50);
  // GPS NMEA.
  gps::Nmea g;
  auto feed = [&](const char *body) {
    unsigned char x = 0; for (const char *p = body; *p; p++) x ^= *p;
    char s[128]; snprintf(s, sizeof s, "$%s*%02X\r\n", body, x);
    bool r = false; for (char *p = s; *p; p++) r |= g.push(*p);
    return r;
  };
  CHECK(feed("GNRMC,123519.00,A,1345.123,N,10030.456,E,54.0,84.4,031126,,,A"));
  CHECK(g.fix.valid && fabs(g.fix.kmh - 100.008f) < 0.01 && g.fix.dateDdmmyy == 31126);
  feed("GPGGA,123519.00,1345.123,N,10030.456,E,1,12,0.8,10.0,M,0.0,M,,");
  CHECK(g.fix.sats == 12 && fabs(g.fix.hdop - 0.8f) < 0.01);
  CHECK(!g.push('$') && !g.push('X'));
  printf(fails ? "ride_test: %d FAILED\n" : "ride_test: all passed\n", fails);
  return fails ? 1 : 0;
}
