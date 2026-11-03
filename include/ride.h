// Speed / distance maths, trip statistics, 1-second summaries and the
// 402 m drag timer. Pure C++ (unit-tested on the PC: test/ride_test.cpp).
//
// The drag timer is fed (time, speed) samples from whichever speed source is
// active: controller RPM over UART today, a GPS module later. Between samples
// the speed is taken as linear, so split times and trap speeds are
// interpolated instead of snapping to the ~10 Hz sample grid.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace ride {

// km/h from motor RPM: wheel circumference in mm, gear ratio motor:wheel.
inline float rpmToKmh(float rpm, float circMm, float ratio) {
  if (ratio <= 0) ratio = 1;
  return rpm / ratio * circMm * 60.0f / 1e6f;
}
// Tyre circumference from a size like 130/70-17 (width mm, aspect %, rim inch).
inline float tyreCircMm(int width, int aspect, int rimInch) {
  return (float)M_PI * (rimInch * 25.4f + 2.0f * width * aspect / 100.0f);
}

// ---------------------------------------------------------------- drag ----
static const int SPLITS = 4;
static const double SPLIT_M[SPLITS] = {18.288, 100.0, 201.168, 402.336};
static const char *const SPLIT_NAME[SPLITS] = {"60 ft", "100 m", "201 m", "402 m"};

struct Split {
  bool done = false;
  double t = 0;        // s from launch
  double trapKmh = 0;  // speed when crossing
  float peakA = 0;     // highest current inside this section
};
struct DragResult {
  Split s[SPLITS];
  float peakA = 0, peakKw = 0, minV = 0;
  bool wheelspin = false;  // acceleration > 1.2 g seen (wheel slip suspected)
  bool finished() const { return s[SPLITS - 1].done; }
};

class DragTimer {
public:
  enum State { IDLE, ARMED, READY, RUN, DONE, ABORTED };
  State state = IDLE;
  DragResult r;

  void arm() { state = ARMED; reset(); }
  void cancel() { state = IDLE; }
  // t in seconds (monotonic), v in km/h, plus current/voltage for statistics.
  void feed(double t, double kmh, float amp = 0, float volt = 0) {
    double v = kmh / 3.6;
    switch (state) {
    case ARMED:
      if (v <= 0.05) { state = READY; tPrev = t; }
      return;
    case READY:
      if (v <= 0.05) { tPrev = t; return; }
      state = RUN;
      launchLo = tPrev;           // still at rest here
      first = true; ft = t; fv = v; fa = amp;
      ts = tPrev;                 // provisional launch until the 2nd sample
      stats(amp, volt);
      return;
    case RUN:
      stats(amp, volt);
      if (first) {
        // Constant-acceleration launch: extrapolate back to v = 0.
        double est = ft - (v > fv ? fv * (t - ft) / (v - fv) : (ft - launchLo));
        if (est < launchLo) est = launchLo;
        if (est > ft) est = ft;
        ts = est;
        first = false;
        dist = 0; lt = ts; lv = 0;
        segment(ft, fv, fa);
      }
      segment(t, v, amp);
      if (v <= 0.05) { if (stopAt < 0) stopAt = t; else if (t - stopAt > 2) state = ABORTED; }
      else stopAt = -1;
      if (t - ts > 60) state = ABORTED;
      if (r.finished()) state = DONE;
      return;
    default: return;
    }
  }
  // Live read-outs while running.
  double elapsed(double tNow) const { return state == RUN || state == DONE ? (state == DONE ? r.s[SPLITS - 1].t : tNow - ts) : 0; }
  double distance() const { return dist; }
  double speedKmh() const { return lv * 3.6; }

private:
  double tPrev = 0, launchLo = 0, ts = 0, ft = 0, fv = 0, lt = 0, lv = 0, dist = 0, stopAt = -1;
  float fa = 0, secPeak = 0;
  int next = 0;
  bool first = false;
  void reset() { r = DragResult(); next = 0; dist = 0; lv = 0; stopAt = -1; secPeak = 0; }
  void stats(float amp, float volt) {
    if (amp > r.peakA) r.peakA = amp;
    if (amp * volt / 1000 > r.peakKw) r.peakKw = amp * volt / 1000;
    if (volt > 1 && (r.minV == 0 || volt < r.minV)) r.minV = volt;
    if (amp > secPeak) secPeak = amp;
  }
  void segment(double t, double v, float amp) {
    double dt = t - lt;
    if (dt <= 0) return;
    if (amp > secPeak) secPeak = amp;
    double k = (v - lv) / dt;
    if (k > 1.2 * 9.81) r.wheelspin = true;
    double dEnd = dist + (lv + v) * 0.5 * dt;
    while (next < SPLITS && SPLIT_M[next] <= dEnd) {
      double need = SPLIT_M[next] - dist, tau;
      if (fabs(k) < 1e-9) tau = lv > 0 ? need / lv : dt;
      else {
        double disc = lv * lv + 2 * k * need;
        tau = disc > 0 ? (-lv + sqrt(disc)) / k : dt;
      }
      if (tau < 0) tau = 0;
      if (tau > dt) tau = dt;
      Split &s = r.s[next];
      s.done = true;
      s.t = lt + tau - ts;
      s.trapKmh = (lv + k * tau) * 3.6;
      s.peakA = secPeak;
      secPeak = 0;
      next++;
    }
    dist = dEnd; lt = t; lv = v;
  }
};

// --------------------------------------------------------- 1 s summary ----
struct SecAgg {
  uint32_t n = 0;
  double sKmh = 0, sRpm = 0, sA = 0, sV = 0;
  float maxKmh = 0, maxA = -1e9f, minA = 1e9f, minV = 1e9f, maxV = 0;
  int maxRpm = 0, maxTc = -99, maxTm = -99;
  uint32_t faults = 0;
  void add(float kmh, int rpm, float a, float v, int tc, int tm, uint32_t f) {
    n++; sKmh += kmh; sRpm += rpm; sA += a; sV += v;
    if (kmh > maxKmh) maxKmh = kmh;
    if (rpm > maxRpm) maxRpm = rpm;
    if (a > maxA) maxA = a;
    if (a < minA) minA = a;
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
    if (tc > maxTc) maxTc = tc;
    if (tm > maxTm) maxTm = tm;
    faults |= f;
  }
  float avgKmh() const { return n ? sKmh / n : 0; }
  float avgRpm() const { return n ? sRpm / n : 0; }
  float avgA() const { return n ? sA / n : 0; }
  float avgV() const { return n ? sV / n : 0; }
};

// ---------------------------------------------------------------- trip ----
struct Trip {
  double distM = 0, wh = 0, ah = 0, whRegen = 0;
  double movingS = 0;
  float maxKmh = 0, maxA = 0, minV = 0, maxKw = 0;
  double lastT = -1;
  void add(double t, float kmh, float a, float v) {
    if (lastT >= 0) {
      double dt = t - lastT;
      if (dt > 0 && dt < 2) {
        distM += kmh / 3.6 * dt;
        double w = a * v;
        wh += w * dt / 3600.0;
        ah += a * dt / 3600.0;
        if (w < 0) whRegen += -w * dt / 3600.0;
        if (kmh > 0.5) movingS += dt;
      }
    }
    lastT = t;
    if (kmh > maxKmh) maxKmh = kmh;
    if (a > maxA) maxA = a;
    if (a * v / 1000 > maxKw) maxKw = a * v / 1000;
    if (v > 1 && (minV == 0 || v < minV)) minV = v;
  }
  float avgKmh() const { return movingS > 1 ? distM / movingS * 3.6 : 0; }
  float whPerKm() const { return distM > 50 ? wh / (distM / 1000) : 0; }
};

// Battery % from pack voltage (linear between empty and full, per cell).
inline int socFromVolt(float v, int cells, float vEmpty, float vFull) {
  if (cells <= 0) return 0;
  float c = v / cells;
  int p = (int)lroundf((c - vEmpty) / (vFull - vEmpty) * 100);
  return p < 0 ? 0 : p > 100 ? 100 : p;
}

}  // namespace ride
