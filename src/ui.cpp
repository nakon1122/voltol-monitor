// VOLTOL Monitor screens. Pure drawing code (no Arduino): compiled for the
// board and, unchanged, for the PC preview renderer (tools/sim/sim_main.cpp).
#include "ui.h"
#include "vlw_metrics.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace votol;

namespace ui {
Page page = PG_BOOT;
bool menuOpen = false;
int editGroup = 0;
static int editSel = 0, editTop = 0, editStep = 1, backupSel = -1;
static uint32_t menuAt = 0, nowMs = 0;
static bool menuDirty = false;

// ------------------------------------------------------------- colours ----
static const uint32_t BG = 0x05070A, BAR = 0x0B1016, CARD = 0x0F161E, CARD2 = 0x0E1419,
                      TRACK = 0x18212B, LINE = 0x2A3640, TEXT = 0xE8EDF2, MUTED = 0x6E7C89,
                      MUTED2 = 0x7F8C99, SUB = 0xB8C4CF, DIM = 0x4A5763, CHIP = 0x1A222B,
                      GREEN = 0x00E08A, YELLOW = 0xFFC400, RED = 0xFF3B30, BLUE = 0x4FC3FF,
                      DARKRED = 0x3A1414, DARKGREEN = 0x00331F;
static const char *const GEAR_CH = "LMHS";
static const char *const DASH_NAME[3] = {"A Sport Arc", "B Digital Race", "C Twin Gauge"};

static uint32_t mix(uint32_t a, uint32_t b, float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  int r = ((a >> 16) & 255) + (int)((((int)(b >> 16) & 255) - (int)((a >> 16) & 255)) * t);
  int g = ((a >> 8) & 255) + (int)((((int)(b >> 8) & 255) - (int)((a >> 8) & 255)) * t);
  int bl = (a & 255) + (int)((((int)b & 255) - (int)(a & 255)) * t);
  return (uint32_t)(r << 16 | g << 8 | bl);
}
static uint32_t speedColor(float f) {  // green -> yellow -> red
  return f < 0.6f ? mix(GREEN, YELLOW, f / 0.6f) : mix(YELLOW, RED, (f - 0.6f) / 0.4f);
}
static void pt(int cx, int cy, float r, float deg, int &x, int &y) {
  x = cx + (int)lroundf(r * cosf(deg * (float)M_PI / 180));
  y = cy + (int)lroundf(r * sinf(deg * (float)M_PI / 180));
}
static void bolt(Gfx &g, int x, int y, float s, uint32_t c) {
  // lightning bolt logo (same outline as the mock-ups)
  auto X = [&](float v) { return x + (int)lroundf(v * s); };
  auto Y = [&](float v) { return y + (int)lroundf(v * s); };
  g.fillTriangle(X(7), Y(0), X(0), Y(11), X(5), Y(11), c);
  g.fillTriangle(X(7), Y(0), X(5), Y(11), X(8), Y(0), c);
  g.fillTriangle(X(5), Y(11), X(0), Y(11), X(6), Y(7), c);
  g.fillTriangle(X(3), Y(20), X(11), Y(7), X(6), Y(7), c);
  g.fillTriangle(X(3), Y(20), X(6), Y(7), X(5), Y(11), c);
}
static void fmt1(char *b, int n, float v) { snprintf(b, n, "%.1f", v); }

struct Btn { int x, y, w, h; };
static bool hit(const Btn &b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }
static void button(Gfx &g, const Btn &b, const char *t, uint32_t bg = CHIP, uint32_t fg = TEXT, int font = F13B) {
  g.fillRoundRect(b.x, b.y, b.w, b.h, 8, bg);
  g.textMid(b.x + b.w / 2, b.y + b.h / 2, t, font, fg, AL_C);
}
static void chip(Gfx &g, int x, int y, int w, const char *t, uint32_t bg, uint32_t fg) {
  g.fillRoundRect(x, y, w, 18, 4, bg);
  g.textMid(x + w / 2, y + 9, t, F10, fg, AL_C);
}

// Live values as text ("--" while the controller is silent).
struct Vals {
  char kmh[8], rpm[8], volt[8], amp[8], kw[8], soc[8], tc[8], tm[8];
  bool ok;
};
static Vals vals(const UiState &s) {
  Vals v;
  v.ok = s.linked;
  if (!s.linked) {
    for (char *p : {v.kmh, v.rpm, v.volt, v.amp, v.kw, v.soc, v.tc, v.tm}) strcpy(p, "--");
    return v;
  }
  snprintf(v.kmh, 8, "%d", (int)lroundf(s.kmh));
  snprintf(v.rpm, 8, "%d", s.live.rpm);
  fmt1(v.volt, 8, s.live.volt);
  fmt1(v.amp, 8, s.live.amp);
  snprintf(v.kw, 8, "%.2f", s.kw);
  snprintf(v.soc, 8, "%d%%", s.soc);
  snprintf(v.tc, 8, "%d", s.live.tCtrl);
  snprintf(v.tm, 8, "%d", s.live.tMotor);
  return v;
}
static void statusText(const UiState &s, char *out, uint32_t &col, uint32_t &bg) {
  if (!s.linked) { strcpy(out, "NO LINK"); col = MUTED2; bg = CHIP; return; }
  if (s.live.faults) { strcpy(out, "FAULT"); col = TEXT; bg = RED; return; }
  uint8_t st = s.live.status < 8 ? s.live.status : 0;
  snprintf(out, 12, "%s", STATUS_NAMES[st]);
  col = st == ST_RUN ? GREEN : st == ST_BRAKE ? BLUE : SUB;
  bg = st == ST_RUN ? DARKGREEN : CHIP;
}
static void faultLine(const UiState &s, char *out, int n) {
  if (!s.linked) { snprintf(out, n, "Waiting for controller (%u baud)", (unsigned)s.baud); return; }
  if (!s.live.faults) { snprintf(out, n, "No fault"); return; }
  faultText(s.live.faults, out, n);
}
static void gears(Gfx &g, int x, int y, int w, const UiState &s) {
  for (int i = 0; i < 4; i++) {
    bool on = s.linked && s.live.gear() == i;
    int xx = x + i * (w + 4);
    g.fillRoundRect(xx, y, w, 18, 4, on ? YELLOW : CHIP);
    char t[2] = {GEAR_CH[i], 0};
    g.textMid(xx + w / 2, y + 9, t, F13B, on ? 0x000000 : 0x5B6B7B, AL_C);
  }
}
static void brand(Gfx &g, int x, int y) {  // y = top of header
  bolt(g, x, y + 5, 1, YELLOW);
  g.text(x + 16, y + 20, "VOLTOL", F13B, TEXT);
  g.text(x + 16 + g.textWidth("VOLTOL", F13B) + 6, y + 20, "Monitor", F11, MUTED2);
}

// Gauge band drawn in small slices so it can be updated incrementally
// (only the part between the old and the new value is repainted).
typedef uint32_t (*SliceColor)(float frac, void *ctx);
static void band(Gfx &g, int cx, int cy, int r0, int r1, float a0, float a1, float from, float to,
                 float valDeg, uint32_t track, SliceColor fill, void *ctx, float zeroDeg = -1) {
  const float step = 3;
  if (from > to) { float t = from; from = to; to = t; }
  from = fmaxf(a0, from - step);
  to = fminf(a1, to + step);
  for (float a = from; a < to; a += step) {
    float b = fminf(a + step, to);
    float mid = (a + b) / 2;
    bool on = zeroDeg < 0 ? mid <= valDeg : (valDeg >= zeroDeg ? mid >= zeroDeg && mid <= valDeg : mid <= zeroDeg && mid >= valDeg);
    g.fillArc(cx, cy, r0, r1, a, b + 0.6f, on ? fill((mid - a0) / (a1 - a0), ctx) : track);
  }
}
static uint32_t gradFill(float f, void *) { return speedColor(f); }
static uint32_t solidFill(float, void *c) { return *(uint32_t *)c; }

// ============================================================ DASH A ======
static float lastA = -1, lastC1 = -1, lastC2 = -1;
static const int AX = 160, AY = 182, AR = 118;
static float angA(const UiState &s, const Settings &c) {
  float f = s.linked ? s.kmh / c.maxKmh : 0;
  return 135 + 270 * fminf(fmaxf(f, 0), 1);
}
static void dashA(Gfx &g, const UiState &s, const Settings &c, bool full) {
  Vals v = vals(s);
  char st[16]; uint32_t sc, sb;
  statusText(s, st, sc, sb);
  if (full) {
    g.fillRect(0, 0, 480, 320, BG);
    g.fillRect(0, 0, 480, 30, BAR);
    brand(g, 10, 0);
    // track + ticks
    g.fillArc(AX, AY, AR - 8, AR + 8, 135, 405, TRACK);
    int x, y;
    pt(AX, AY, AR, 405, x, y); g.fillCircle(x, y, 8, TRACK);
    pt(AX, AY, AR, 135, x, y); g.fillCircle(x, y, 8, GREEN);
    for (int i = 0; i <= 12; i++) {
      float a = 135 + 270 * i / 12.0f;
      bool big = i % 2 == 0;
      int x0, y0, x1, y1;
      pt(AX, AY, AR - 14, a, x0, y0);
      pt(AX, AY, AR - (big ? 24 : 19), a, x1, y1);
      g.drawLine(x0, y0, x1, y1, big ? 0x9AA7B3 : 0x3A4652);
      if (big) {
        char t[8]; snprintf(t, 8, "%d", c.maxKmh * i / 12);
        pt(AX, AY, AR - 36, a, x, y);
        g.textMid(x, y, t, F10, MUTED, AL_C);
      }
    }
    g.text(AX, AY + 44, "km/h", F13, MUTED2, AL_C);
    lastA = 135;
  }
  float a = angA(s, c);
  band(g, AX, AY, AR - 8, AR + 8, 135, 405, full ? 135 : lastA, a, a, TRACK, gradFill, nullptr);
  lastA = a;
  g.boxed(186, 6, 76, 18, BAR, [&] {
    g.fillRoundRect(186, 6, 76, 18, 9, sb);
    g.textMid(224, 15, st, F11B, sc, AL_C);
  });
  g.boxed(366, 6, 104, 18, BAR, [&] { gears(g, 366, 6, 22, s); });
  g.boxed(88, 128, 144, 80, BG, [&] {
    g.text(AX, AY + 20, v.kmh, s.kmh >= 99.5f ? F72B : F86B, s.linked ? TEXT : DIM, AL_C);
  });
  g.boxed(100, 258, 120, 22, BG, [&] {
    char t[24]; snprintf(t, 24, "%s rpm", v.rpm);
    g.text(AX, AY + 92, t, F13B, SUB, AL_C);
  });
  // right tiles
  const int X = 318, W = 152, BW = 132;
  auto tile = [&](int y, int h, const char *label, const char *val, const char *unit, uint32_t col) {
    g.fillRoundRect(X, y, W, h, 8, CARD);
    g.text(X + 10, y + 16, label, F10, MUTED);
    g.text(X + 10, y + 42, val, F24B, col);
    g.text(X + 14 + g.textWidth(val, F24B), y + 42, unit, F13, MUTED2);
  };
  g.boxed(X, 40, W, 62, BG, [&] {
    tile(40, 62, "BATTERY", v.volt, "V", TEXT);
    g.text(X + W - 10, 56, v.soc, F11B, GREEN, AL_R);
    g.fillRoundRect(X + 10, 90, BW, 6, 3, 0x1D2731);
    if (s.linked) g.fillRoundRect(X + 10, 90, BW * s.soc / 100, 6, 3, s.soc < 15 ? RED : GREEN);
  });
  g.boxed(X, 108, W, 62, BG, [&] {
    tile(108, 62, s.live.amp < 0 && s.linked ? "REGEN" : "CURRENT", v.amp, "A", s.live.amp < 0 ? GREEN : YELLOW);
    g.fillRoundRect(X + 10, 158, BW, 6, 3, 0x1D2731);
    float f = s.linked ? fminf(fabsf(s.live.amp) / c.maxAmp, 1) : 0;
    g.fillRoundRect(X + 10, 158, (int)(BW * f), 6, 3, s.live.amp < 0 ? GREEN : YELLOW);
  });
  g.boxed(X, 176, W, 52, BG, [&] { tile(176, 52, "POWER", v.kw, "kW", BLUE); });
  g.boxed(X, 234, W, 46, BG, [&] {
    g.fillRoundRect(X, 234, W, 46, 8, CARD);
    g.text(X + 10, 250, "CTRL", F10, MUTED);
    g.text(X + 82, 250, "MOTOR", F10, MUTED);
    char t[12];
    snprintf(t, 12, "%s°C", v.tc); g.text(X + 10, 272, t, F18B, s.live.tCtrl > 80 ? RED : TEXT);
    snprintf(t, 12, "%s°C", v.tm); g.text(X + 82, 272, t, F18B, s.live.tMotor > 100 ? RED : TEXT);
  });
  g.boxed(0, 292, 480, 28, BG, [&] {
    char t[64];
    snprintf(t, 64, "TRIP %.1f km   ODO %.0f km", s.trip.distM / 1000, s.odoKm);
    g.text(10, 310, t, F11, MUTED);
    char f[96]; faultLine(s, f, sizeof f);
    g.text(470, 310, f, s.live.faults && s.linked ? F11B : F11, s.live.faults && s.linked ? RED : MUTED, AL_R);
  });
}

// ============================================================ DASH B ======
static void dashB(Gfx &g, const UiState &s, const Settings &c, bool full) {
  Vals v = vals(s);
  const uint32_t BBG = 0x000000;
  if (full) {
    g.fillRect(0, 0, 480, 320, BBG);
    bolt(g, 392, 29, 0.7f, YELLOW);
    g.text(402, 40, "VOLTOL", F11B, TEXT);
    g.text(238, 190, "KM/H", F15B, MUTED2, AL_C);
  }
  // shift-light RPM bar
  float rpmMax = c.maxKmh / fmaxf(0.001f, ride::rpmToKmh(1, c.circMm(), c.ratioX100 / 100.0f));
  int lit = s.linked ? (int)lroundf(30.0f * s.live.rpm / fmaxf(1, rpmMax)) : 0;
  for (int i = 0; i < 30; i++) {
    float f = i / 30.0f;
    uint32_t col = f < .6f ? GREEN : f < .85f ? YELLOW : RED;
    g.fillRoundRect(8 + (int)(i * 15.47f), 8, 12, 16, 2, i < lit ? col : 0x141A20);
  }
  g.boxed(8, 28, 200, 16, BBG, [&] {
    char t[24]; snprintf(t, 24, "RPM %s", v.rpm);
    g.text(8, 40, t, F11B, SUB);
  });
  char st[16]; uint32_t sc, sb;
  statusText(s, st, sc, sb);
  g.boxed(10, 56, 70, 120, BBG, [&] {
    g.fillRoundRect(10, 56, 70, 120, 10, 0x0E1318);
    g.drawRoundRect(10, 56, 70, 120, 10, YELLOW);
    g.text(45, 78, "GEAR", F10, MUTED, AL_C);
    char t[2] = {s.linked ? GEAR_CH[s.live.gear()] : '-', 0};
    g.text(45, 148, t, F56B, YELLOW, AL_C);
    g.text(45, 168, st, F10, sc == SUB ? MUTED2 : sc, AL_C);
  });
  g.boxed(88, 52, 300, 122, BBG, [&] { g.text(238, 168, v.kmh, F150B, s.linked ? TEXT : DIM, AL_C); });
  // current bar (regen below zero)
  g.boxed(390, 50, 84, 152, BBG, [&] {
    const int bx = 420, by = 56, bh = 130;
    const float lo = -0.25f * c.maxAmp, hi = (float)c.maxAmp;
    g.fillRoundRect(bx, by, 22, bh, 4, 0x141A20);
    int zy = by + (int)(bh * hi / (hi - lo));
    float a = s.linked ? fmaxf(lo, fminf(hi, s.live.amp)) : 0;
    int vy = by + (int)(bh * (hi - a) / (hi - lo));
    if (vy < zy) g.fillRect(bx, vy, 22, zy - vy, YELLOW);
    else if (vy > zy) g.fillRect(bx, zy, 22, vy - zy, GREEN);
    g.drawLine(bx - 4, zy, bx + 26, zy, MUTED2);
    char t[8]; snprintf(t, 8, "%d", c.maxAmp);
    g.text(bx - 6, by + 9, t, F10, MUTED, AL_R);
    g.text(bx - 6, zy + 4, "0", F10, MUTED, AL_R);
    g.text(bx + 11, by + bh + 14, "AMP", F10, MUTED, AL_C);
  });
  // cards
  const char *lab[4] = {"VOLT", "AMP", "POWER", "TEMP"};
  for (int i = 0; i < 4; i++) {
    int x = 8 + i * 118;
    g.boxed(x, 216, 112, 76, BBG, [&] {
      g.fillRoundRect(x, 216, 112, 76, 8, 0x0E1318);
      g.text(x + 10, 234, lab[i], F10, MUTED);
      char val[16]; const char *u = ""; uint32_t col = TEXT;
      if (i == 0) { strcpy(val, v.volt); u = "V"; }
      if (i == 1) { strcpy(val, v.amp); u = "A"; col = s.live.amp < 0 ? GREEN : YELLOW; }
      if (i == 2) { strcpy(val, v.kw); u = "kW"; col = BLUE; }
      if (i == 3) { snprintf(val, 16, "%s/%s", v.tc, v.tm); u = "°C"; }
      g.text(x + 10, 264, val, F24B, col);
      g.text(x + 13 + g.textWidth(val, F24B), 264, u, F13, MUTED2);
      if (i == 0) {
        g.text(x + 102, 234, v.soc, F11B, GREEN, AL_R);
        g.fillRoundRect(x + 10, 276, 92, 5, 2, 0x1D2731);
        if (s.linked) g.fillRoundRect(x + 10, 276, 92 * s.soc / 100, 5, 2, s.soc < 15 ? RED : GREEN);
      }
      if (i == 3) g.text(x + 10, 284, "ctrl/motor", F10, MUTED);
    });
  }
  g.boxed(0, 298, 480, 22, BBG, [&] {
    char t[64]; snprintf(t, 64, "TRIP %.1f km · ODO %.0f km", s.trip.distM / 1000, s.odoKm);
    g.text(8, 312, t, F10, MUTED);
    char f[96]; faultLine(s, f, sizeof f);
    g.text(472, 312, f, F10, s.live.faults && s.linked ? RED : MUTED, AL_R);
  });
}

// ============================================================ DASH C ======
static void dashC(Gfx &g, const UiState &s, const Settings &c, bool full) {
  Vals v = vals(s);
  const int R = 86, Y0 = 146, LX = 110, RX = 370;
  const float lo = -0.25f * c.maxAmp, hi = (float)c.maxAmp;
  auto ticks = [&](int cx, float vmin, float vmax, const float *t, int n) {
    for (int i = 0; i < n; i++) {
      float a = 140 + 260 * (t[i] - vmin) / (vmax - vmin);
      int x, y; pt(cx, Y0, R - 20, a, x, y);
      char b[8]; snprintf(b, 8, "%d", (int)t[i]);
      g.textMid(x, y, b, F10, 0x5E6C79, AL_C);
    }
  };
  float a1 = 140 + 260 * (s.linked ? fminf(s.kmh / c.maxKmh, 1) : 0);
  float amp = s.linked ? fmaxf(lo, fminf(hi, s.live.amp)) : 0;
  float a2 = 140 + 260 * (amp - lo) / (hi - lo), z2 = 140 + 260 * (0 - lo) / (hi - lo);
  if (full) {
    g.fillRect(0, 0, 480, 320, BG);
    g.fillRect(0, 0, 480, 28, BAR);
    brand(g, 10, -1);
    for (int cx : {LX, RX}) {
      g.fillCircle(cx, Y0, R + 10, 0x0B1117);
      g.fillArc(cx, Y0, R - 5, R + 5, 140, 400, TRACK);
    }
    float t1[5] = {0, c.maxKmh * .25f, c.maxKmh * .5f, c.maxKmh * .75f, (float)c.maxKmh};
    ticks(LX, 0, c.maxKmh, t1, 5);
    float t2[5] = {lo, 0, roundf(hi / 30) * 10, roundf(hi / 15) * 10, hi};
    ticks(RX, lo, hi, t2, 5);
    int x0, y0, x1, y1;
    pt(RX, Y0, R + 9, z2, x0, y0); pt(RX, Y0, R - 9, z2, x1, y1);
    g.drawLine(x0, y0, x1, y1, 0x9AA7B3);
    g.text(LX, Y0 + 34, "km/h", F13, MUTED2, AL_C);
    g.text(RX, Y0 + 34, "A", F13, MUTED2, AL_C);
    g.fillRoundRect(222, 62, 36, 130, 6, LINE);
    g.fillRoundRect(224, 64, 32, 126, 5, BG);
    g.fillRoundRect(233, 56, 14, 6, 2, LINE);
    g.fillRect(0, 268, 480, 52, BAR);
    lastC1 = 140; lastC2 = z2;
  }
  uint32_t gcol = GREEN, acol = s.live.amp < 0 ? GREEN : YELLOW;
  band(g, LX, Y0, R - 5, R + 5, 140, 400, full ? 140 : lastC1, a1, a1, TRACK, solidFill, &gcol);
  band(g, RX, Y0, R - 5, R + 5, 140, 400, full ? 140 : lastC2, full ? 400 : a2, a2, TRACK, solidFill, &acol, z2);
  lastC1 = a1; lastC2 = a2;
  char st[16]; uint32_t sc, sb;
  statusText(s, st, sc, sb);
  g.boxed(190, 4, 100, 20, BAR, [&] {
    g.fillCircle(200, 14, 5, sc == SUB ? MUTED2 : sc == TEXT ? RED : sc);
    g.textMid(210, 14, st, F11B, sc == TEXT ? RED : sc);
  });
  g.boxed(356, 4, 116, 20, BAR, [&] { gears(g, 356, 5, 26, s); });
  g.boxed(LX - 52, Y0 - 44, 104, 60, 0x0B1117, [&] { g.text(LX, Y0 + 14, v.kmh, s.kmh >= 99.5f ? F38B : F56B, s.linked ? TEXT : DIM, AL_C); });
  g.boxed(RX - 52, Y0 - 44, 104, 60, 0x0B1117, [&] {
    char t[8]; snprintf(t, 8, "%d", s.linked ? (int)lroundf(s.live.amp) : 0);
    g.text(RX, Y0 + 14, s.linked ? t : "--", fabsf(s.live.amp) >= 99.5f ? F38B : F56B, s.linked ? TEXT : DIM, AL_C);
  });
  g.boxed(LX - 60, Y0 + 70, 120, 22, 0x0B1117, [&] {
    char t[24]; snprintf(t, 24, "%s rpm", v.rpm); g.text(LX, Y0 + 86, t, F13B, SUB, AL_C);
  });
  g.boxed(RX - 60, Y0 + 70, 120, 22, 0x0B1117, [&] {
    char t[24]; snprintf(t, 24, "%s kW", v.kw); g.text(RX, Y0 + 86, t, F13B, BLUE, AL_C);
  });
  g.boxed(224, 64, 32, 126, BG, [&] {
    int fh = s.linked ? 118 * s.soc / 100 : 0;
    if (fh > 0) g.fillRoundRect(228, 186 - fh, 24, fh, 3, s.soc < 15 ? RED : GREEN);
  });
  g.boxed(200, 196, 80, 44, BG, [&] {
    g.text(240, 214, v.soc, F15B, TEXT, AL_C);
    char t[12]; snprintf(t, 12, "%s V", v.volt); g.text(240, 232, t, F13, SUB, AL_C);
  });
  char trip[16], odo[16], tc[12], tm[12], f[96];
  snprintf(tc, 12, "%s°C", v.tc); snprintf(tm, 12, "%s°C", v.tm);
  snprintf(trip, 16, "%.1f km", s.trip.distM / 1000); snprintf(odo, 16, "%.0f km", s.odoKm);
  faultLine(s, f, sizeof f);
  const char *lab[5] = {"CTRL", "MOTOR", "TRIP", "ODO", "FAULT"};
  const char *val[5] = {tc, tm, trip, odo, s.live.faults && s.linked ? f : (s.linked ? "None" : "--")};
  for (int i = 0; i < 5; i++) {
    int x = 14 + i * 94;
    g.boxed(x - 4, 272, i == 4 ? 94 : 90, 44, BAR, [&] {
      g.text(x, 286, lab[i], F10, MUTED);
      bool bad = i == 4 && s.linked && s.live.faults;
      g.text(x, 306, val[i], bad ? F11B : F15B, i == 4 ? (bad ? RED : GREEN) : TEXT);
    });
  }
}

// ============================================================ MENU ========
static const char *const MENU[5] = {"STYLE", "DRAG", "LOG", "TUNE", "SETUP"};
static void drawMenu(Gfx &g, const Settings &c) {
  g.fillRect(0, 256, 480, 64, 0x0B1016);
  g.drawLine(0, 256, 480, 256, LINE);
  for (int i = 0; i < 5; i++) {
    Btn b = {6 + i * 95, 264, 88, 48};
    g.fillRoundRect(b.x, b.y, b.w, b.h, 8, i == 0 ? 0x13202A : CHIP);
    g.text(b.x + b.w / 2, b.y + 22, MENU[i], F13B, i == 0 ? BLUE : TEXT, AL_C);
    if (i == 0) g.text(b.x + b.w / 2, b.y + 38, DASH_NAME[c.dash] + 2, F10, MUTED2, AL_C);
  }
}

// ======================================================== page header =====
static const Btn BACK = {0, 0, 96, 32};
static void header(Gfx &g, const char *title, const char *right = nullptr, uint32_t rc = MUTED2) {
  g.fillRect(0, 0, 480, 320, BG);
  g.fillRect(0, 0, 480, 32, BAR);
  g.text(12, 21, "< DASH", F13B, BLUE);
  g.text(240, 21, title, F15B, TEXT, AL_C);
  if (right) g.text(470, 21, right, F11, rc, AL_R);
}

// ============================================================ DRAG ========
static const Btn DB[4] = {{10, 282, 110, 30}, {130, 282, 110, 30}, {250, 282, 110, 30}, {370, 282, 100, 30}};
static void dragTop(Gfx &g, const UiState &s, const char *state, uint32_t col) {
  g.fillRect(0, 0, 480, 28, BAR);
  bolt(g, 10, 4, 1, YELLOW);
  g.text(26, 19, "DRAG", F13B, TEXT);
  g.text(72, 19, "402 m", F11, MUTED2);
  g.fillRoundRect(196, 5, 88, 18, 9, col);
  g.fillRoundRect(197, 6, 86, 16, 8, BAR);
  g.textMid(240, 14, state, F11B, col, AL_C);
  char src[20];
  snprintf(src, 20, s.src ? "SRC GPS %.0fHz" : "SRC RPM %.0fHz", s.src ? s.gpsHz : s.hz);
  chip(g, 330, 5, 78, src, 0x13202A, BLUE);
  char gp[16];
  if (s.gpsFix) snprintf(gp, 16, "GPS %d", s.gpsSats); else strcpy(gp, s.gpsSeen ? "GPS ..." : "GPS -");
  chip(g, 412, 5, 60, gp, 0x161B21, s.gpsFix ? GREEN : DIM);
}
static void track(Gfx &g, int y, double dist) {
  g.fillRoundRect(14, y, 452, 8, 4, TRACK);
  int w = (int)(452 * fmin(dist, 402.336) / 402.336);
  if (w > 0) g.fillRoundRect(14, y, w < 8 ? 8 : w, 8, 4, GREEN);
  for (int i = 0; i < ride::SPLITS; i++) {
    int x = 14 + (int)(452 * ride::SPLIT_M[i] / 402.336);
    g.fillRect(x, y - 4, 2, 16, 0x9AA7B3);
    g.text(x > 452 ? 452 : x, y + 24, ride::SPLIT_NAME[i], F10, MUTED, AL_C);
  }
}
static void splitTable(Gfx &g, int y, const ride::DragResult &r, const ride::DragResult *best, bool peak, const char *title) {
  g.fillRoundRect(10, y, 460, 22 + 4 * 24, 8, CARD2);
  const int cols[5] = {22, 120, 210, 300, 400};
  const char *head[5] = {title, "TIME", "TRAP", best ? "Δ BEST" : "", peak ? "PEAK A" : ""};
  for (int i = 0; i < 5; i++) g.text(cols[i], y + 15, head[i], F10, MUTED);
  for (int i = 0; i < ride::SPLITS; i++) {
    int yy = y + 38 + i * 24;
    const ride::Split &sp = r.s[i];
    g.text(cols[0], yy, ride::SPLIT_NAME[i], F13, TEXT);
    char t[24];
    if (!sp.done) { g.text(cols[1], yy, "—", F13, MUTED); g.text(cols[2], yy, "—", F13, MUTED); continue; }
    snprintf(t, 24, "%.2f s", sp.t); g.text(cols[1], yy, t, F15B, TEXT);
    snprintf(t, 24, "%.1f km/h", sp.trapKmh); g.text(cols[2], yy, t, F13, TEXT);
    if (best && best->s[i].done) {
      double d = sp.t - best->s[i].t;
      snprintf(t, 24, "%+.2f", d); g.text(cols[3], yy, t, F13B, d <= 0 ? GREEN : RED);
    }
    if (peak) { snprintf(t, 24, "%.0f A", sp.peakA); g.text(cols[4], yy, t, F13, TEXT); }
  }
}
static int lastDragState = -1;
static void dragPage(Gfx &g, const UiState &s, const Settings &c, bool full) {
  using ride::DragTimer;
  const DragTimer &d = *s.drag;
  if (d.state != lastDragState) full = true;
  lastDragState = d.state;
  const ride::DragResult *best = s.haveBest ? &s.best : nullptr;
  if (d.state == DragTimer::RUN) {
    if (full) {
      g.fillRect(0, 0, 480, 320, BG);
      dragTop(g, s, "RUN", YELLOW);
      button(g, {10, 282, 460, 30}, "ABORT", DARKRED, RED);
    }
    g.boxed(10, 34, 300, 70, BG, [&] {
      char t[16]; snprintf(t, 16, "%.2f", d.elapsed(s.now));
      g.text(20, 96, t, F72B, TEXT);
      g.text(26 + g.textWidth(t, F72B), 96, "s", F24B, MUTED2);
    });
    g.boxed(320, 44, 150, 60, BG, [&] {
      g.text(460, 62, "SPEED", F10, MUTED, AL_R);
      char t[16]; fmt1(t, 16, d.speedKmh());
      int w = g.textWidth(" km/h", F13);
      g.text(460 - w, 96, t, F38B, TEXT, AL_R);
      g.text(460, 96, " km/h", F13, MUTED2, AL_R);
    });
    g.boxed(10, 106, 460, 54, BG, [&] {
      char t[80];
      snprintf(t, 80, "Distance %.1f m  ·  %.0f A  ·  %.1f kW", d.distance(), s.live.amp, s.kw);
      g.text(20, 120, t, F13, SUB);
      track(g, 128, d.distance());
    });
    g.boxed(10, 158, 460, 120, BG, [&] { splitTable(g, 158, d.r, best, false, "SPLIT"); });
    return;
  }
  if (!full) {
    if (d.state == DragTimer::ARMED || d.state == DragTimer::READY || d.state == DragTimer::IDLE)
      g.boxed(0, 120, 480, 22, BG, [&] {
        char t[64];
        snprintf(t, 64, "%.1f V · %d%% · CTRL %d°C · Gear %c · %d rpm", s.live.volt, s.soc, s.live.tCtrl,
                 s.linked ? GEAR_CH[s.live.gear()] : '-', s.live.rpm);
        g.text(240, 134, t, F11, MUTED, AL_C);
      });
    return;
  }
  g.fillRect(0, 0, 480, 320, BG);
  if (d.state == DragTimer::DONE || d.state == DragTimer::ABORTED) {
    bool done = d.state == DragTimer::DONE;
    dragTop(g, s, done ? "FINISHED" : "ABORTED", done ? BLUE : RED);
    char t[48];
    const ride::Split &last = d.r.s[ride::SPLITS - 1];
    if (done) {
      snprintf(t, 48, "%.2f", last.t);
      g.text(20, 84, t, F56B, TEXT);
      g.text(26 + g.textWidth(t, F56B), 84, "s", F24B, MUTED2);
      snprintf(t, 48, "402 m @ %.1f km/h", last.trapKmh);
      g.text(20, 108, t, F13B, TEXT);
      if (best && (!best->finished() || last.t < best->s[3].t)) {
        snprintf(t, 48, best->finished() ? "★ NEW BEST %+.2f" : "★ NEW BEST", best->finished() ? last.t - best->s[3].t : 0.0);
        g.fillRoundRect(330, 46, 140, 30, 15, YELLOW);
        g.textMid(400, 61, t, F13B, 0x000000, AL_C);
      }
    } else {
      g.text(20, 84, "ABORTED", F38B, RED);
      g.text(20, 108, "Run stopped before 402 m", F13, SUB);
    }
    snprintf(t, 48, "Peak %.0f A · %.1f kW", d.r.peakA, d.r.peakKw);
    g.text(470, 96, t, F10, MUTED, AL_R);
    snprintf(t, 48, "Min %.1f V · %s", d.r.minV, s.src ? "GPS" : "±0.05 s (RPM)");
    g.text(470, 110, t, F10, MUTED, AL_R);
    if (d.r.wheelspin) g.text(470, 124, "Wheel spin suspected", F10, YELLOW, AL_R);
    splitTable(g, 128, d.r, best, true, "SPLIT");
    button(g, DB[0], "EXIT"); button(g, DB[1], "HISTORY"); button(g, DB[2], "WHEEL");
    button(g, DB[3], "AGAIN", GREEN, 0x000000);
    return;
  }
  // IDLE / ARMED / READY
  bool ready = d.state == DragTimer::READY, armed = d.state == DragTimer::ARMED;
  dragTop(g, s, ready ? "READY" : armed ? "ARMED" : "IDLE", ready ? GREEN : armed ? YELLOW : MUTED2);
  if (ready) {
    g.text(240, 92, "READY", F56B, GREEN, AL_C);
    g.text(240, 114, "0 RPM · Timer starts when the wheel turns", F13, SUB, AL_C);
  } else if (armed) {
    g.text(240, 92, "STOP", F56B, YELLOW, AL_C);
    g.text(240, 114, "Bring the bike to a full stop (0 RPM)", F13, SUB, AL_C);
  } else {
    g.text(240, 92, "DRAG", F56B, TEXT, AL_C);
    g.text(240, 114, "Press ARM, then launch from a standstill", F13, SUB, AL_C);
  }
  g.text(240, 134, "", F11, MUTED, AL_C);
  if (best) {
    splitTable(g, 146, *best, nullptr, false, "BEST RUN");
    g.text(300, 161, s.bestWhen, F10, MUTED);
  } else {
    g.fillRoundRect(10, 146, 460, 118, 8, CARD2);
    g.text(240, 210, "No runs yet", F13, MUTED, AL_C);
  }
  button(g, DB[0], "EXIT"); button(g, DB[1], "HISTORY"); button(g, DB[2], "WHEEL");
  button(g, DB[3], armed || ready ? "DISARM" : "ARM", armed || ready ? CHIP : GREEN, armed || ready ? TEXT : 0x000000);
}
static void dragHist(Gfx &g, const UiState &s) {
  header(g, "DRAG HISTORY");
  const int cols[6] = {14, 120, 190, 260, 330, 405};
  const char *h[6] = {"DATE", "60 ft", "100 m", "201 m", "402 m", "TRAP"};
  for (int i = 0; i < 6; i++) g.text(cols[i], 52, h[i], F10, MUTED);
  if (!s.runN) g.text(240, 160, "No runs saved yet", F13, MUTED, AL_C);
  for (int r = 0; r < s.runN && r < 9; r++) {
    const DragRow &d = s.runs[r];
    int y = 76 + r * 26;
    if (r % 2 == 0) g.fillRect(6, y - 17, 468, 24, CARD2);
    g.text(cols[0], y, d.when, F11, SUB);
    char t[16];
    for (int i = 0; i < 4; i++) {
      if (d.t[i] > 0) snprintf(t, 16, "%.2f", d.t[i]); else strcpy(t, "—");
      g.text(cols[i + 1], y, t, i == 3 ? F13B : F13, TEXT);
    }
    snprintf(t, 16, "%.1f", d.trap[3]); g.text(cols[5], y, t, F13, TEXT);
  }
}

// ============================================================ LOG =========
static const Btn LB[3] = {{10, 282, 148, 30}, {166, 282, 148, 30}, {322, 282, 148, 30}};
static void logPage(Gfx &g, const UiState &s, const Settings &c) {
  char r[48];
  snprintf(r, 48, "%s", s.sd ? "SD card" : "internal");
  header(g, "LOG", r);
  char t[96];
  snprintf(t, 96, "Storage: %s · %u KB free of %u KB · rate %s", s.sd ? "SD card" : "internal flash",
           (unsigned)s.fsFreeKB, (unsigned)s.fsTotalKB, c.logRate ? "" : "max");
  if (c.logRate) snprintf(t, 96, "Storage: %s · %u KB free of %u KB · %u Hz", s.sd ? "SD card" : "internal flash",
                          (unsigned)s.fsFreeKB, (unsigned)s.fsTotalKB, LOG_RATES[c.logRate]);
  g.text(14, 52, t, F11, SUB);
  // current trip card
  g.fillRoundRect(10, 62, 460, 58, 8, CARD);
  g.text(22, 80, "THIS TRIP", F10, MUTED);
  const ride::Trip &tr = s.trip;
  const char *lab[6] = {"km", "max km/h", "avg km/h", "max A", "Wh", "Wh/km"};
  float val[6] = {(float)(tr.distM / 1000), tr.maxKmh, tr.avgKmh(), tr.maxA, (float)tr.wh, tr.whPerKm()};
  for (int i = 0; i < 6; i++) {
    int x = 22 + i * 76;
    char b[16]; snprintf(b, 16, i == 0 ? "%.2f" : "%.0f", val[i]);
    g.text(x, 104, b, F18B, i == 3 ? YELLOW : TEXT);
    g.text(x, 115, lab[i], F10, MUTED);
  }
  g.text(14, 140, "TRIPS", F10, MUTED);
  if (!s.tripN) g.text(240, 200, "No trips recorded yet", F13, MUTED, AL_C);
  for (int i = 0; i < s.tripN && i < 5; i++) {
    const TripRow &x = s.trips[i];
    int y = 160 + i * 24;
    if (i % 2 == 0) g.fillRect(6, y - 16, 468, 23, CARD2);
    char b[96];
    snprintf(b, 96, "#%u", x.id); g.text(14, y, b, F13B, TEXT);
    g.text(56, y, x.when, F11, SUB);
    snprintf(b, 96, "%.1f km · %.0f km/h · %.0f A · %.0f Wh/km", x.km, x.maxKmh, x.maxA, x.whKm);
    g.text(466, y, b, F11, TEXT, AL_R);
  }
  button(g, LB[0], "GRAPH"); button(g, LB[1], "FAULTS"); button(g, LB[2], "NEW TRIP");
}
static void graphPage(Gfx &g, const UiState &s) {
  header(g, "GRAPH", "last minutes · 1 point/s");
  const int X = 44, Y = 50, W = 410, H = 210;
  g.fillRoundRect(10, 40, 460, 236, 8, CARD2);
  float mk = 10, ma = 10, mi = 0;
  for (int i = 0; i < s.gN; i++) {
    mk = fmaxf(mk, s.gKmh[i]); ma = fmaxf(ma, s.gAmp[i]); mi = fminf(mi, s.gAmp[i]);
  }
  mk = ceilf(mk / 20) * 20; ma = ceilf(ma / 50) * 50; mi = floorf(mi / 50) * 50;
  for (int i = 0; i <= 4; i++) {
    int y = Y + H - H * i / 4;
    g.drawLine(X, y, X + W, y, 0x1C2630);
    char b[12];
    snprintf(b, 12, "%.0f", mk * i / 4); g.text(X - 4, y + 4, b, F10, GREEN, AL_R);
    snprintf(b, 12, "%.0f", mi + (ma - mi) * i / 4); g.text(X + W + 4, y + 4, b, F10, YELLOW);
  }
  for (int i = 1; i < s.gN; i++) {
    int x0 = X + W * (i - 1) / (s.gN > 1 ? s.gN - 1 : 1), x1 = X + W * i / (s.gN > 1 ? s.gN - 1 : 1);
    auto yk = [&](float v) { return Y + H - (int)(H * v / mk); };
    auto ya = [&](float v) { return Y + H - (int)(H * (v - mi) / (ma - mi)); };
    g.drawLine(x0, ya(s.gAmp[i - 1]), x1, ya(s.gAmp[i]), YELLOW);
    g.drawLine(x0, yk(s.gKmh[i - 1]), x1, yk(s.gKmh[i]), GREEN);
    g.drawLine(x0, yk(s.gKmh[i - 1]) + 1, x1, yk(s.gKmh[i]) + 1, GREEN);
  }
  if (s.gN < 2) g.text(240, 160, "Not enough data yet", F13, MUTED, AL_C);
  g.fillCircle(20, 296, 4, GREEN); g.text(30, 300, "km/h (left)", F11, SUB);
  g.fillCircle(140, 296, 4, YELLOW); g.text(150, 300, "current A (right)", F11, SUB);
  char b[48]; snprintf(b, 48, "%d s shown", s.gN); g.text(470, 300, b, F11, MUTED, AL_R);
}
static const Btn FB = {322, 282, 148, 30};
static void faultsPage(Gfx &g, const UiState &s) {
  header(g, "FAULT LOG");
  const int cols[5] = {14, 98, 330, 390, 440};
  const char *h[5] = {"TIME", "FAULT", "km/h", "A", "V"};
  for (int i = 0; i < 5; i++) g.text(cols[i], 52, h[i], F10, MUTED);
  if (!s.faultN) g.text(240, 160, "No faults recorded", F13, GREEN, AL_C);
  for (int i = 0; i < s.faultN && i < 8; i++) {
    const FaultRow &f = s.faults[i];
    int y = 76 + i * 25;
    if (i % 2 == 0) g.fillRect(6, y - 17, 468, 24, CARD2);
    g.text(cols[0], y, f.when, F11, SUB);
    char t[96], n[80];
    faultText(f.code, n, sizeof n);
    snprintf(t, 96, "%s %s", f.set ? "●" : "○", f.set ? n : "cleared");
    g.text(cols[1], y, t, F11B, f.set ? RED : GREEN);
    snprintf(t, 16, "%.0f", f.kmh); g.text(cols[2], y, t, F11, TEXT);
    snprintf(t, 16, "%.0f", f.amp); g.text(cols[3], y, t, F11, TEXT);
    snprintf(t, 16, "%.1f", f.volt); g.text(cols[4], y, t, F11, TEXT);
  }
  char now[96]; faultLine(s, now, sizeof now);
  g.text(14, 300, "Now:", F11, MUTED);
  g.text(50, 300, now, F11B, s.live.faults ? RED : GREEN);
  button(g, FB, "CLEAR FAULTS", s.live.faults ? DARKRED : CHIP, s.live.faults ? RED : MUTED2);
}

// ============================================================ TUNE ========
static int changedCount(const UiState &s) {
  if (!s.params.complete()) return 0;
  int n = 0;
  for (int i = 0; i < PARAM_COUNT; i++)
    if (getRaw(s.edit, PARAMS[i]) != getRaw(s.params.b, PARAMS[i])) n++;
  return n;
}
static int groupFields(int grp, int *idx) {
  int n = 0;
  for (int i = 0; i < PARAM_COUNT; i++)
    if (!strcmp(PARAMS[i].group, GROUPS[grp])) idx[n++] = i;
  return n;
}
static const Btn TB[3] = {{10, 282, 148, 30}, {166, 282, 148, 30}, {322, 282, 148, 30}};
static Btn groupBtn(int i) { return {10 + (i % 3) * 154, 104 + (i / 3) * 84, 146, 76}; }
static void tunePage(Gfx &g, const UiState &s) {
  header(g, "TUNE CONTROLLER");
  bool have = s.params.complete();
  g.fillRoundRect(10, 40, 460, 56, 8, CARD);
  char t[96];
  if (have) {
    snprintf(t, 96, "Parameters read %s · check %05X", s.paramsWhen, (unsigned)s.params.checkSum());
    g.text(22, 60, t, F13B, TEXT);
    snprintf(t, 96, s.backupN ? "Backup #%u saved before any change · %d backups kept" : "No backup yet", s.backupN ? s.backups[0].id : 0, s.backupN);
    g.text(22, 82, t, F11, s.backupN ? GREEN : YELLOW);
  } else {
    g.text(22, 60, s.busy ? "Reading parameters..." : "Parameters not read yet", F13B, s.busy ? YELLOW : TEXT);
    g.text(22, 82, "Stop the bike, switch on, then press READ", F11, SUB);
  }
  if (s.msg[0]) {
    uint32_t mc = s.msgKind == 1 ? GREEN : s.msgKind == 2 ? RED : YELLOW;
    g.text(458, 60, s.msg, F11B, mc, AL_R);
  }
  for (int i = 0; i < GROUP_COUNT; i++) {
    Btn b = groupBtn(i);
    int idx[64], n = groupFields(i, idx), ch = 0;
    for (int k = 0; k < n && have; k++)
      if (getRaw(s.edit, PARAMS[idx[k]]) != getRaw(s.params.b, PARAMS[idx[k]])) ch++;
    g.fillRoundRect(b.x, b.y, b.w, b.h, 8, have ? CHIP : CARD2);
    g.text(b.x + 12, b.y + 30, GROUPS[i], F15B, have ? TEXT : DIM);
    char c[32]; snprintf(c, 32, "%d settings", n);
    g.text(b.x + 12, b.y + 52, c, F11, MUTED);
    if (ch) { snprintf(c, 32, "%d changed", ch); g.text(b.x + 12, b.y + 66, c, F10, YELLOW); }
  }
  int ch = changedCount(s);
  button(g, TB[0], s.busy ? "READING..." : "READ", s.busy ? CARD2 : CHIP, TEXT);
  button(g, TB[1], "BACKUPS");
  char w[24]; snprintf(w, 24, ch ? "WRITE (%d)" : "WRITE", ch);
  button(g, TB[2], w, ch ? YELLOW : CARD2, ch ? 0x000000 : DIM);
}
static const Btn EB_MINUS = {346, 158, 58, 46}, EB_PLUS = {412, 158, 58, 46}, EB_STEP = {346, 212, 58, 30},
                 EB_RESET = {412, 212, 58, 30}, EB_UP = {270, 282, 60, 30}, EB_DN = {270, 246, 60, 30},
                 EB_DONE = {346, 282, 124, 30};
static void editPage(Gfx &g, const UiState &s) {
  header(g, GROUPS[editGroup]);
  int idx[64], n = groupFields(editGroup, idx);
  if (editSel >= n) editSel = 0;
  const int ROWS = 7;
  if (editSel < editTop) editTop = editSel;
  if (editSel >= editTop + ROWS) editTop = editSel - ROWS + 1;
  for (int r = 0; r < ROWS && editTop + r < n; r++) {
    int i = editTop + r;
    const ParamDef &d = PARAMS[idx[i]];
    int y = 40 + r * 34;
    bool sel = i == editSel, ch = getRaw(s.edit, d) != getRaw(s.params.b, d);
    g.fillRoundRect(10, y, 326, 30, 6, sel ? 0x13202A : CARD2);
    if (sel) g.drawRoundRect(10, y, 326, 30, 6, BLUE);
    g.textMid(20, y + 15, d.name, F13, TEXT);
    char v[24], u[32];
    formatValue(s.edit, d, v, sizeof v);
    snprintf(u, 32, "%s %s", v, d.unit);
    g.textMid(326, y + 15, u, F13B, ch ? YELLOW : TEXT, AL_R);
  }
  if (n > ROWS) {
    char p[40]; snprintf(p, 40, "%d-%d / %d", editTop + 1, editTop + ROWS < n ? editTop + ROWS : n, n);
    g.text(14, 300, p, F11, MUTED);
    button(g, {190, 282, 70, 30}, "UP"); button(g, EB_UP, "DOWN");
  }
  // right panel: selected value
  const ParamDef &d = PARAMS[idx[editSel]];
  g.fillRoundRect(346, 40, 124, 110, 8, CARD);
  g.text(354, 58, d.name, F10, MUTED);
  char v[24]; formatValue(s.edit, d, v, sizeof v);
  g.text(408, 104, v, strlen(v) > 5 ? F24B : F38B, getRaw(s.edit, d) != getRaw(s.params.b, d) ? YELLOW : TEXT, AL_C);
  g.text(408, 124, d.unit, F13, MUTED2, AL_C);
  char was[32], o[24]; formatValue(s.params.b, d, o, sizeof o);
  snprintf(was, 32, "was %s", o);
  g.text(408, 142, was, F10, MUTED, AL_C);
  button(g, EB_MINUS, "-", CHIP, TEXT, F24B);
  button(g, EB_PLUS, "+", CHIP, TEXT, F24B);
  char st[16]; snprintf(st, 16, "x%d", editStep);
  button(g, EB_STEP, st, CHIP, BLUE);
  button(g, EB_RESET, "UNDO", CHIP, MUTED2);
  button(g, EB_DONE, "DONE", 0x13202A, BLUE);
}
static const Btn CB_CANCEL = {10, 282, 148, 30}, CB_WRITE = {322, 282, 148, 30};
static void confirmPage(Gfx &g, const UiState &s) {
  header(g, "WRITE PARAMETERS");
  g.text(14, 54, "These values will be written to the controller:", F13, SUB);
  int y = 74, shown = 0, n = changedCount(s);
  for (int i = 0; i < PARAM_COUNT && shown < 6; i++) {
    const ParamDef &d = PARAMS[i];
    if (getRaw(s.edit, d) == getRaw(s.params.b, d)) continue;
    char a[24], b[24], t[64];
    formatValue(s.params.b, d, a, sizeof a); formatValue(s.edit, d, b, sizeof b);
    if (shown % 2 == 0) g.fillRect(6, y - 15, 468, 22, CARD2);
    g.text(14, y, d.group, F11, MUTED);
    g.text(100, y, d.name, F13, TEXT);
    snprintf(t, 64, "%s  >  %s %s", a, b, d.unit);
    g.text(466, y, t, F13B, YELLOW, AL_R);
    y += 22; shown++;
  }
  if (n > shown) { char t[32]; snprintf(t, 32, "+ %d more", n - shown); g.text(14, y, t, F11, MUTED); }
  g.fillRoundRect(10, 214, 460, 58, 8, 0x1A1408);
  bool stopped = s.linked && s.live.rpm == 0;
  g.text(22, 234, stopped ? "Bike stopped (0 RPM)" : "Stop the bike first: writing needs 0 RPM", F13B, stopped ? GREEN : RED);
  char t[96];
  snprintf(t, 96, "A backup of the current values is kept (#%u). Values are read back to verify.", s.backupN ? s.backups[0].id : 0);
  g.text(22, 256, t, F11, SUB);
  if (s.msg[0]) g.text(240, 300, s.msg, F11B, s.msgKind == 1 ? GREEN : s.msgKind == 2 ? RED : YELLOW, AL_C);
  button(g, CB_CANCEL, "CANCEL");
  button(g, CB_WRITE, s.busy ? "WRITING..." : "WRITE", stopped && !s.busy ? YELLOW : CARD2, stopped ? 0x000000 : DIM);
}
static const Btn BB_RESTORE = {322, 282, 148, 30};
static void backupsPage(Gfx &g, const UiState &s) {
  header(g, "PARAMETER BACKUPS");
  g.text(14, 54, "Saved automatically on every read that differs from the last backup.", F11, SUB);
  if (!s.backupN) g.text(240, 160, "No backups yet - press READ on the TUNE page", F13, MUTED, AL_C);
  for (int i = 0; i < s.backupN && i < 8; i++) {
    const BackupRow &b = s.backups[i];
    int y = 64 + i * 27;
    bool sel = i == backupSel;
    g.fillRoundRect(10, y, 460, 24, 5, sel ? 0x13202A : (i % 2 ? BG : CARD2));
    if (sel) g.drawRoundRect(10, y, 460, 24, 5, BLUE);
    char t[48];
    snprintf(t, 48, "#%u", b.id); g.textMid(22, y + 12, t, F13B, TEXT);
    g.textMid(70, y + 12, b.when, F13, SUB);
    snprintf(t, 48, "check %05X", (unsigned)b.sum); g.textMid(260, y + 12, t, F11, MUTED);
    if (s.params.complete() && b.sum == s.params.checkSum()) g.textMid(458, y + 12, "= controller", F11B, GREEN, AL_R);
  }
  button(g, BB_RESTORE, "RESTORE", backupSel >= 0 ? YELLOW : CARD2, backupSel >= 0 ? 0x000000 : DIM);
  g.text(14, 300, "Restore loads the backup, then WRITE asks for confirmation.", F10, MUTED);
}

// ============================================================ SETUP =======
struct Row { const char *name; };
static const char *const SROW[10] = {"Dashboard", "Speed source", "Log rate", "Battery cells", "Cell empty/full",
                                     "Gauge max km/h", "Gauge max A", "Backlight", "Wi-Fi export", "Time zone (UTC)"};
static void setupValue(const Settings &c, int i, char *t) {
  switch (i) {
  case 0: snprintf(t, 24, "%s", DASH_NAME[c.dash]); break;
  case 1: snprintf(t, 24, "%s", c.speedSrc == 0 ? "RPM" : c.speedSrc == 1 ? "GPS" : "AUTO"); break;
  case 2: if (c.logRate) snprintf(t, 24, "%u Hz", LOG_RATES[c.logRate]); else strcpy(t, "MAX"); break;
  case 3: snprintf(t, 24, "%uS", c.cells); break;
  case 4: snprintf(t, 24, "%.2f/%.2f", c.cellEmptyMv / 1000.0, c.cellFullMv / 1000.0); break;
  case 5: snprintf(t, 24, "%u", c.maxKmh); break;
  case 6: snprintf(t, 24, "%u", c.maxAmp); break;
  case 7: snprintf(t, 24, "%u/5", c.backlight); break;
  case 8: snprintf(t, 24, "%s", c.wifi ? "ON" : "OFF"); break;
  case 9: snprintf(t, 24, "UTC%+d", c.tzHours); break;
  }
}
static Btn srow(int i) { return {10 + (i / 5) * 234, 40 + (i % 5) * 46, 226, 40}; }
static const Btn SB[3] = {{10, 282, 148, 30}, {166, 282, 148, 30}, {322, 282, 148, 30}};
static void setupPage(Gfx &g, const Settings &c) {
  header(g, "SETUP");
  for (int i = 0; i < 10; i++) {
    Btn b = srow(i);
    g.fillRoundRect(b.x, b.y, b.w, b.h, 8, CARD);
    g.text(b.x + 10, b.y + 15, SROW[i], F10, MUTED);
    char t[24]; setupValue(c, i, t);
    g.text(b.x + 10, b.y + 33, t, F13B, TEXT);
    g.fillRoundRect(b.x + b.w - 74, b.y + 6, 32, 28, 6, CHIP);
    g.textMid(b.x + b.w - 58, b.y + 20, "-", F18B, TEXT, AL_C);
    g.fillRoundRect(b.x + b.w - 38, b.y + 6, 32, 28, 6, CHIP);
    g.textMid(b.x + b.w - 22, b.y + 20, "+", F18B, TEXT, AL_C);
  }
  button(g, SB[0], "WHEEL"); button(g, SB[1], "TOUCH CAL"); button(g, SB[2], "INFO");
}
static void adjust(Settings &c, int i, int d) {
  auto cyc = [&](uint8_t &v, int n) { v = (uint8_t)((v + d + n) % n); };
  auto clampAdd = [&](auto &v, int step, int lo, int hi) { int x = (int)v + d * step; v = x < lo ? lo : x > hi ? hi : x; };
  switch (i) {
  case 0: cyc(c.dash, 3); break;
  case 1: cyc(c.speedSrc, 3); break;
  case 2: cyc(c.logRate, 5); break;
  case 3: clampAdd(c.cells, 1, 6, 40); break;
  case 4: clampAdd(c.cellFullMv, 50, 3300, 4400);
    if (c.cellFullMv == 3650 || c.cellFullMv == 3600) c.cellEmptyMv = 2800;  // LiFePO4
    else if (c.cellFullMv >= 4000) c.cellEmptyMv = 3000;
    break;
  case 5: clampAdd(c.maxKmh, 10, 40, 300); break;
  case 6: clampAdd(c.maxAmp, 50, 50, 1000); break;
  case 7: clampAdd(c.backlight, 1, 1, 5); break;
  case 8: c.wifi = !c.wifi; break;
  case 9: clampAdd(c.tzHours, 1, -12, 14); break;
  }
}
static const char *const WROW[5] = {"Tyre width (mm)", "Tyre aspect (%)", "Rim (inch)", "Correction (%)", "Gear ratio"};
static Btn wrow(int i) { return {10, 40 + i * 46, 300, 40}; }
static void wheelPage(Gfx &g, const Settings &c) {
  header(g, "WHEEL / SPEED");
  for (int i = 0; i < 5; i++) {
    Btn b = wrow(i);
    g.fillRoundRect(b.x, b.y, b.w, b.h, 8, CARD);
    g.text(b.x + 10, b.y + 15, WROW[i], F10, MUTED);
    char t[24];
    if (i == 0) snprintf(t, 24, "%u", c.tyreW);
    if (i == 1) snprintf(t, 24, "%u", c.tyreA);
    if (i == 2) snprintf(t, 24, "%u", c.tyreR);
    if (i == 3) snprintf(t, 24, "%+.1f", c.wheelCorr / 10.0);
    if (i == 4) snprintf(t, 24, "%.2f : 1", c.ratioX100 / 100.0);
    g.text(b.x + 10, b.y + 33, t, F13B, TEXT);
    g.fillRoundRect(b.x + b.w - 74, b.y + 6, 32, 28, 6, CHIP);
    g.textMid(b.x + b.w - 58, b.y + 20, "-", F18B, TEXT, AL_C);
    g.fillRoundRect(b.x + b.w - 38, b.y + 6, 32, 28, 6, CHIP);
    g.textMid(b.x + b.w - 22, b.y + 20, "+", F18B, TEXT, AL_C);
  }
  g.fillRoundRect(320, 40, 150, 224, 8, CARD2);
  char t[48];
  snprintf(t, 48, "%u/%u-%u", c.tyreW, c.tyreA, c.tyreR);
  g.text(395, 64, t, F15B, TEXT, AL_C);
  g.text(395, 92, "circumference", F10, MUTED, AL_C);
  snprintf(t, 48, "%.0f mm", c.circMm());
  g.text(395, 116, t, F24B, YELLOW, AL_C);
  g.text(395, 148, "at 1000 rpm", F10, MUTED, AL_C);
  snprintf(t, 48, "%.1f km/h", ride::rpmToKmh(1000, c.circMm(), c.ratioX100 / 100.0f));
  g.text(395, 172, t, F18B, TEXT, AL_C);
  g.text(395, 206, "Tip: ride 100 m", F10, MUTED, AL_C);
  g.text(395, 220, "between 2 marks and", F10, MUTED, AL_C);
  g.text(395, 234, "adjust Correction", F10, MUTED, AL_C);
  g.text(395, 248, "until TRIP shows 0.10", F10, MUTED, AL_C);
  g.text(14, 300, "Speed = RPM / ratio x circumference. Hub motor: ratio 1.00", F11, SUB);
}
static void adjustWheel(Settings &c, int i, int d) {
  auto clampAdd = [&](auto &v, int step, int lo, int hi) { int x = (int)v + d * step; v = x < lo ? lo : x > hi ? hi : x; };
  switch (i) {
  case 0: clampAdd(c.tyreW, 5, 50, 300); break;
  case 1: clampAdd(c.tyreA, 5, 30, 100); break;
  case 2: clampAdd(c.tyreR, 1, 8, 21); break;
  case 3: clampAdd(c.wheelCorr, 1, -100, 100); break;
  case 4: clampAdd(c.ratioX100, 5, 50, 2000); break;
  }
}
static void infoPage(Gfx &g, const UiState &s) {
  header(g, "INFO");
  char t[96];
  const char *rows[][2] = {{"Firmware", s.fw}, {"Wi-Fi", s.ssid}, {"Web page", s.ip}};
  int y = 58;
  for (auto &r : rows) {
    g.text(14, y, r[0], F11, MUTED); g.text(140, y, r[1], F13B, TEXT); y += 24;
  }
  snprintf(t, 96, "%s · %u baud · %.1f Hz", s.linked ? "connected" : "no data", (unsigned)s.baud, s.hz);
  g.text(14, y, "Controller", F11, MUTED); g.text(140, y, t, F13B, s.linked ? GREEN : RED); y += 24;
  snprintf(t, 96, "%u good · %u bad", (unsigned)s.frames, (unsigned)s.errors);
  g.text(14, y, "Frames", F11, MUTED); g.text(140, y, t, F13B, TEXT); y += 24;
  if (s.gpsSeen) snprintf(t, 96, "%s · %d sats · %.0f Hz", s.gpsFix ? "fix" : "no fix", s.gpsSats, s.gpsHz);
  else snprintf(t, 96, "not connected (optional)");
  g.text(14, y, "GPS", F11, MUTED); g.text(140, y, t, F13B, s.gpsFix ? GREEN : TEXT); y += 24;
  snprintf(t, 96, "%s · %u KB free", s.sd ? "SD card" : "internal", (unsigned)s.fsFreeKB);
  g.text(14, y, "Log storage", F11, MUTED); g.text(140, y, t, F13B, TEXT); y += 24;
  snprintf(t, 96, "%u KB", (unsigned)s.heapKB);
  g.text(14, y, "Free memory", F11, MUTED); g.text(140, y, t, F13B, TEXT); y += 24;
  g.text(14, 300, "Wiring: VOTOL TX -> IO35, VOTOL RX <- IO22, GND - GND. GPS TX -> IO21.", F10, MUTED);
}
static void bootPage(Gfx &g, const UiState &s) {
  g.fillRect(0, 0, 480, 320, BG);
  bolt(g, 196, 70, 4.2f, YELLOW);
  g.text(240, 200, "VOLTOL", F38B, TEXT, AL_C);
  g.text(240, 224, "MONITOR", F13B, MUTED2, AL_C);
  g.text(240, 268, "Connecting to VOTOL controller...", F13, SUB, AL_C);
  g.text(240, 300, s.fw, F10, MUTED, AL_C);
}

// ============================================================ dispatch ====
void go(Page p) { page = p; menuOpen = false; }
static void dash(Gfx &g, const UiState &s, const Settings &c, bool full) {
  if (c.dash == 1) dashB(g, s, c, full);
  else if (c.dash == 2) dashC(g, s, c, full);
  else dashA(g, s, c, full);
}
void draw(Gfx &g, const UiState &s, const Settings &c) {
  switch (page) {
  case PG_BOOT: bootPage(g, s); break;
  case PG_DASH: dash(g, s, c, true); if (menuOpen) drawMenu(g, c); break;
  case PG_DRAG: lastDragState = -1; dragPage(g, s, c, true); break;
  case PG_DRAG_HIST: dragHist(g, s); break;
  case PG_LOG: logPage(g, s, c); break;
  case PG_GRAPH: graphPage(g, s); break;
  case PG_FAULTS: faultsPage(g, s); break;
  case PG_TUNE: tunePage(g, s); break;
  case PG_EDIT: editPage(g, s); break;
  case PG_CONFIRM: confirmPage(g, s); break;
  case PG_BACKUPS: backupsPage(g, s); break;
  case PG_SETUP: setupPage(g, c); break;
  case PG_WHEEL: wheelPage(g, c); break;
  case PG_INFO: infoPage(g, s); break;
  default: break;
  }
}
void update(Gfx &g, const UiState &s, const Settings &c) {
  if (page == PG_DASH && !menuOpen) dash(g, s, c, false);
  else if (page == PG_DRAG) dragPage(g, s, c, false);
}
bool tick(uint32_t ms) {
  nowMs = ms;
  if (menuOpen && ms - menuAt > 6000) { menuOpen = false; return true; }
  if (menuDirty) { menuDirty = false; return true; }
  return false;
}
bool touch(int x, int y, UiState &s, Settings &c, UiActions &a) {
  switch (page) {
  case PG_BOOT: go(PG_DASH); return true;
  case PG_DASH:
    if (!menuOpen) { menuOpen = true; menuAt = nowMs; return true; }
    if (y < 256) { menuOpen = false; return true; }
    menuAt = nowMs;
    for (int i = 0; i < 5; i++)
      if (hit({6 + i * 95, 264, 88, 48}, x, y)) {
        if (i == 0) { c.dash = (c.dash + 1) % 3; a.settingsChanged = true; return true; }
        go(i == 1 ? PG_DRAG : i == 2 ? PG_LOG : i == 3 ? PG_TUNE : PG_SETUP);
        if (i == 3 && !s.params.complete() && !s.busy) a.readParams = true;
        return true;
      }
    return false;
  case PG_DRAG: {
    using ride::DragTimer;
    if (s.drag->state == DragTimer::RUN) {
      if (hit({10, 282, 460, 30}, x, y)) { a.cancelDrag = true; return true; }
      return false;
    }
    if (hit(DB[0], x, y)) { a.cancelDrag = true; go(PG_DASH); return true; }
    if (hit(DB[1], x, y)) { go(PG_DRAG_HIST); return true; }
    if (hit(DB[2], x, y)) { go(PG_WHEEL); return true; }
    if (hit(DB[3], x, y)) {
      bool armed = s.drag->state == DragTimer::ARMED || s.drag->state == DragTimer::READY;
      if (armed) a.cancelDrag = true; else a.armDrag = true;
      return true;
    }
    return false;
  }
  case PG_DRAG_HIST: if (hit(BACK, x, y) || y > 40) { go(PG_DRAG); return true; } return false;
  case PG_GRAPH: case PG_FAULTS:
    if (page == PG_FAULTS && hit(FB, x, y)) { a.clearFaults = true; return true; }
    if (hit(BACK, x, y) || y > 280) { go(PG_LOG); return true; }
    return false;
  case PG_LOG:
    if (hit(BACK, x, y)) { go(PG_DASH); return true; }
    if (hit(LB[0], x, y)) { go(PG_GRAPH); return true; }
    if (hit(LB[1], x, y)) { go(PG_FAULTS); return true; }
    if (hit(LB[2], x, y)) { a.newTrip = true; return true; }
    return false;
  case PG_TUNE:
    if (hit(BACK, x, y)) { go(PG_DASH); return true; }
    if (hit(TB[0], x, y) && !s.busy) { a.readParams = true; s.msg[0] = 0; return true; }
    if (hit(TB[1], x, y)) { backupSel = -1; go(PG_BACKUPS); return true; }
    if (hit(TB[2], x, y) && changedCount(s)) { s.msg[0] = 0; go(PG_CONFIRM); return true; }
    for (int i = 0; i < GROUP_COUNT; i++)
      if (hit(groupBtn(i), x, y) && s.params.complete()) { editGroup = i; editSel = 0; editTop = 0; go(PG_EDIT); return true; }
    return false;
  case PG_EDIT: {
    int idx[64], n = groupFields(editGroup, idx);
    const ParamDef &d = PARAMS[idx[editSel]];
    if (hit(BACK, x, y) || hit(EB_DONE, x, y)) { go(PG_TUNE); return true; }
    if (hit(EB_MINUS, x, y) || hit(EB_PLUS, x, y)) {
      int dir = hit(EB_PLUS, x, y) ? 1 : -1;
      setRaw(s.edit, d, getRaw(s.edit, d) + dir * d.step * editStep);
      return true;
    }
    if (hit(EB_STEP, x, y)) { editStep = editStep == 1 ? 10 : editStep == 10 ? 100 : 1; return true; }
    if (hit(EB_RESET, x, y)) { setRaw(s.edit, d, getRaw(s.params.b, d)); return true; }
    if (n > 7 && hit({190, 282, 70, 30}, x, y)) { if (editTop > 0) editTop--; if (editSel > editTop + 6) editSel = editTop + 6; return true; }
    if (n > 7 && hit(EB_UP, x, y)) { if (editTop + 7 < n) editTop++; if (editSel < editTop) editSel = editTop; return true; }
    if (x < 336 && y >= 40 && y < 40 + 7 * 34) {
      int r = editTop + (y - 40) / 34;
      if (r < n) { editSel = r; return true; }
    }
    return false;
  }
  case PG_CONFIRM:
    if (hit(BACK, x, y) || hit(CB_CANCEL, x, y)) { go(PG_TUNE); return true; }
    if (hit(CB_WRITE, x, y) && s.linked && s.live.rpm == 0 && !s.busy) { a.writeParams = true; return true; }
    return false;
  case PG_BACKUPS:
    if (hit(BACK, x, y)) { go(PG_TUNE); return true; }
    if (hit(BB_RESTORE, x, y) && backupSel >= 0 && s.params.complete()) {
      a.restoreBackup = s.backups[backupSel].id;
      go(PG_CONFIRM);
      return true;
    }
    if (y >= 64 && y < 64 + 8 * 27) {
      int i = (y - 64) / 27;
      if (i < s.backupN) { backupSel = i; return true; }
    }
    return false;
  case PG_SETUP:
    if (hit(BACK, x, y)) { go(PG_DASH); return true; }
    if (hit(SB[0], x, y)) { go(PG_WHEEL); return true; }
    if (hit(SB[1], x, y)) { a.touchCal = true; return true; }
    if (hit(SB[2], x, y)) { go(PG_INFO); return true; }
    for (int i = 0; i < 10; i++) {
      Btn b = srow(i);
      if (hit({b.x + b.w - 76, b.y, 38, b.h}, x, y)) { adjust(c, i, -1); a.settingsChanged = true; return true; }
      if (hit({b.x + b.w - 38, b.y, 38, b.h}, x, y)) { adjust(c, i, +1); a.settingsChanged = true; return true; }
    }
    return false;
  case PG_WHEEL:
    if (hit(BACK, x, y)) { go(PG_SETUP); return true; }
    for (int i = 0; i < 5; i++) {
      Btn b = wrow(i);
      if (hit({b.x + b.w - 76, b.y, 38, b.h}, x, y)) { adjustWheel(c, i, -1); a.settingsChanged = true; return true; }
      if (hit({b.x + b.w - 38, b.y, 38, b.h}, x, y)) { adjustWheel(c, i, +1); a.settingsChanged = true; return true; }
    }
    return false;
  case PG_INFO: if (hit(BACK, x, y) || y > 40) { go(PG_SETUP); return true; } return false;
  default: return false;
  }
}
}  // namespace ui
