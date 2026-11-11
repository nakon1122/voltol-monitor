// VOLTOL Monitor screens. Pure drawing code (no Arduino): compiled for the
// board and, unchanged, for the PC preview renderer (tools/sim/sim_main.cpp).
#include "ui.h"
#include "vlw_metrics.h"
#include "i18n.h"
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
  char kmh[24], rpm[24], volt[24], amp[24], kw[24], soc[24], tc[24], tm[24];
  bool ok;
};
static Vals vals(const UiState &s) {
  Vals v;
  v.ok = s.linked;
  if (s.speedOk) snprintf(v.kmh, sizeof v.kmh, T("%d"), (int)lroundf(s.kmh)); else strcpy(v.kmh, "--");
  if (!s.linked) {
    for (char *p : {v.rpm, v.volt, v.amp, v.kw, v.soc, v.tc, v.tm}) strcpy(p, "--");
    return v;
  }
  snprintf(v.rpm, sizeof v.rpm, T("%d"), s.live.rpm);
  fmt1(v.volt, 8, s.live.volt);
  fmt1(v.amp, 8, s.live.amp);
  snprintf(v.kw, sizeof v.kw, T("%.2f"), s.kw);
  snprintf(v.soc, sizeof v.soc, T("%d%%"), s.soc);
  snprintf(v.tc, sizeof v.tc, T("%d"), s.live.tCtrl);
  snprintf(v.tm, sizeof v.tm, T("%d"), s.live.tMotor);
  return v;
}
static void statusText(const UiState &s, char *out, uint32_t &col, uint32_t &bg) {
  if (!s.linked && s.speedOk) { strcpy(out, "GPS"); col = BLUE; bg = 0x13202A; return; }
  if (!s.linked) { strcpy(out, "NO LINK"); col = MUTED2; bg = CHIP; return; }
  if (s.live.faults) { strcpy(out, "FAULT"); col = TEXT; bg = RED; return; }
  uint8_t st = s.live.status < 8 ? s.live.status : 0;
  snprintf(out, 36, T("%s"), STATUS_NAMES[st]);
  col = st == ST_RUN ? GREEN : st == ST_BRAKE ? BLUE : SUB;
  bg = st == ST_RUN ? DARKGREEN : CHIP;
}
// faultText with translated names
static void faultTextT(uint32_t f, char *out, int cap) {
  if (!f) { snprintf(out, cap, "%s", T("None")); return; }
  int len = 0;
  uint32_t known = 0;
  for (int i = 0; i < FAULT_COUNT; i++)
    if (f & FAULTS[i].mask) {
      known |= FAULTS[i].mask;
      len += snprintf(out + len, len < cap ? cap - len : 0, "%s%s", len ? ", " : "", T(FAULTS[i].name));
    }
  if (f & ~known && len < cap) snprintf(out + len, cap - len, "%s0x%08X", len ? ", " : "", (unsigned)(f & ~known));
}
static void faultLine(const UiState &s, char *out, int n) {
  if (!s.linked && s.speedOk) { snprintf(out, n, T("GPS only · %d sats · no controller"), s.gpsSats); return; }
  if (!s.linked) { snprintf(out, n, T("Waiting for controller (%u baud)"), (unsigned)s.baud); return; }
  if (!s.live.faults) { snprintf(out, n, "%s", T("No fault")); return; }
  faultTextT(s.live.faults, out, n);
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
  float f = s.speedOk ? s.kmh / c.maxKmh : 0;
  return 135 + 270 * fminf(fmaxf(f, 0), 1);
}
static void dashA(Gfx &g, const UiState &s, const Settings &c, bool full) {
  Vals v = vals(s);
  char st[48]; uint32_t sc, sb;
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
        char t[24]; snprintf(t, sizeof t, T("%d"), c.maxKmh * i / 12);
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
    g.text(AX, AY + 20, v.kmh, s.kmh >= 99.5f ? F72B : F86B, s.speedOk ? TEXT : DIM, AL_C);
  });
  g.boxed(100, 258, 120, 22, BG, [&] {
    char t[72]; snprintf(t, sizeof t, T("%s rpm"), v.rpm);
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
    char t[36];
    snprintf(t, sizeof t, T("%s°C"), v.tc); g.text(X + 10, 272, t, F18B, s.live.tCtrl > 80 ? RED : TEXT);
    snprintf(t, sizeof t, T("%s°C"), v.tm); g.text(X + 82, 272, t, F18B, s.live.tMotor > 100 ? RED : TEXT);
  });
  g.boxed(0, 292, 480, 28, BG, [&] {
    char t[192];
    snprintf(t, sizeof t, T("TRIP %.1f km   ODO %.0f km"), s.trip.distM / 1000, s.odoKm);
    g.text(10, 310, t, F11, MUTED);
    char f[256]; faultLine(s, f, sizeof f);
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
    char t[72]; snprintf(t, sizeof t, T("RPM %s"), v.rpm);
    g.text(8, 40, t, F11B, SUB);
  });
  char st[48]; uint32_t sc, sb;
  statusText(s, st, sc, sb);
  g.boxed(10, 56, 70, 120, BBG, [&] {
    g.fillRoundRect(10, 56, 70, 120, 10, 0x0E1318);
    g.drawRoundRect(10, 56, 70, 120, 10, YELLOW);
    g.text(45, 78, "GEAR", F10, MUTED, AL_C);
    char t[2] = {s.linked ? GEAR_CH[s.live.gear()] : '-', 0};
    g.text(45, 148, t, F56B, YELLOW, AL_C);
    g.text(45, 168, st, F10, sc == SUB ? MUTED2 : sc, AL_C);
  });
  g.boxed(88, 52, 300, 122, BBG, [&] { g.text(238, 168, v.kmh, F150B, s.speedOk ? TEXT : DIM, AL_C); });
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
    char t[24]; snprintf(t, sizeof t, T("%d"), c.maxAmp);
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
      char val[48]; const char *u = ""; uint32_t col = TEXT;
      if (i == 0) { strcpy(val, v.volt); u = "V"; }
      if (i == 1) { strcpy(val, v.amp); u = "A"; col = s.live.amp < 0 ? GREEN : YELLOW; }
      if (i == 2) { strcpy(val, v.kw); u = "kW"; col = BLUE; }
      if (i == 3) { snprintf(val, sizeof val, T("%s/%s"), v.tc, v.tm); u = "°C"; }
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
    char t[192]; snprintf(t, sizeof t, T("TRIP %.1f km · ODO %.0f km"), s.trip.distM / 1000, s.odoKm);
    g.text(8, 312, t, F10, MUTED);
    char f[256]; faultLine(s, f, sizeof f);
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
      char b[24]; snprintf(b, sizeof b, T("%d"), (int)t[i]);
      g.textMid(x, y, b, F10, 0x5E6C79, AL_C);
    }
  };
  float a1 = 140 + 260 * (s.speedOk ? fminf(s.kmh / c.maxKmh, 1) : 0);
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
  char st[48]; uint32_t sc, sb;
  statusText(s, st, sc, sb);
  g.boxed(190, 4, 100, 20, BAR, [&] {
    g.fillCircle(200, 14, 5, sc == SUB ? MUTED2 : sc == TEXT ? RED : sc);  // GPS = blue
    g.textMid(210, 14, st, F11B, sc == TEXT ? RED : sc);
  });
  g.boxed(356, 4, 116, 20, BAR, [&] { gears(g, 356, 5, 26, s); });
  g.boxed(LX - 46, Y0 - 44, 92, 60, 0x0B1117, [&] { g.text(LX, Y0 + 14, v.kmh, s.kmh >= 99.5f ? F38B : F56B, s.speedOk ? TEXT : DIM, AL_C); });
  g.boxed(RX - 46, Y0 - 44, 92, 60, 0x0B1117, [&] {
    char t[24]; snprintf(t, sizeof t, T("%d"), s.linked ? (int)lroundf(s.live.amp) : 0);
    g.text(RX, Y0 + 14, s.linked ? t : "--", fabsf(s.live.amp) >= 99.5f ? F38B : F56B, s.linked ? TEXT : DIM, AL_C);
  });
  g.boxed(LX - 44, Y0 + 66, 88, 18, 0x0B1117, [&] {
    char t[72]; snprintf(t, sizeof t, T("%s rpm"), v.rpm); g.text(LX, Y0 + 80, t, F13B, SUB, AL_C);
  });
  g.boxed(RX - 44, Y0 + 66, 88, 18, 0x0B1117, [&] {
    char t[72]; snprintf(t, sizeof t, T("%s kW"), v.kw); g.text(RX, Y0 + 80, t, F13B, BLUE, AL_C);
  });
  g.boxed(224, 64, 32, 126, BG, [&] {
    int fh = s.linked ? 118 * s.soc / 100 : 0;
    if (fh > 0) g.fillRoundRect(228, 186 - fh, 24, fh, 3, s.soc < 15 ? RED : GREEN);
  });
  g.boxed(200, 196, 80, 44, BG, [&] {
    g.text(240, 214, v.soc, F15B, TEXT, AL_C);
    char t[36]; snprintf(t, sizeof t, T("%s V"), v.volt); g.text(240, 232, t, F13, SUB, AL_C);
  });
  char trip[48], odo[48], tc[36], tm[36], f[256];
  snprintf(tc, sizeof tc, T("%s°C"), v.tc); snprintf(tm, sizeof tm, T("%s°C"), v.tm);
  snprintf(trip, sizeof trip, T("%.1f km"), s.trip.distM / 1000); snprintf(odo, sizeof odo, T("%.0f km"), s.odoKm);
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
  // run distance; tap to change (not while running)
  using ride::DragTimer;
  int st = s.drag->state;
  bool ran = st == DragTimer::RUN || st == DragTimer::DONE || st == DragTimer::ABORTED;
  const char *tn = ride::TARGET_NAME[ride::targetIx(ran ? s.drag->r.n : s.best.n)];
  g.fillRoundRect(68, 5, 70, 18, 9, 0x13202A);
  g.textMid(103, 14, tn, F11B, YELLOW, AL_C);
  g.fillRoundRect(196, 5, 88, 18, 9, col);
  g.fillRoundRect(197, 6, 86, 16, 8, BAR);
  g.textMid(240, 14, state, F11B, col, AL_C);
  char src[60];
  snprintf(src, sizeof src, s.src ? "SRC GPS %.0fHz" : "SRC RPM %.0fHz", s.src ? s.gpsHz : s.hz);
  chip(g, 330, 5, 78, src, 0x13202A, BLUE);
  char gp[48];
  if (s.gpsFix) snprintf(gp, sizeof gp, T("GPS %d"), s.gpsSats); else strcpy(gp, s.gpsSeen ? "GPS ..." : "GPS -");
  chip(g, 412, 5, 60, gp, 0x161B21, s.gpsFix ? GREEN : DIM);
}
// One line under the big READY/STOP text describing source and launch rule.
static void dragHint(Gfx &g, const UiState &s, const Settings &c, int state) {
  using ride::DragTimer;
  char t[256];
  const char *roll = c.rollout ? T(" · rollout 1 ft") : "";
  if (s.src) {
    if (state == DragTimer::ARMED && s.dragWaitGps)
      snprintf(t, sizeof t, T("Waiting for GPS: %d sats, ±%.1f km/h (need 6+, ±1.0)"), s.gpsSats, s.gpsSAcc);
    else if (state == DragTimer::ARMED) snprintf(t, sizeof t, T("GPS: hold still for 1 s%s"), roll);
    else if (state == DragTimer::READY)
      snprintf(t, sizeof t, s.linked ? T("HYBRID: launch from RPM, distance from GPS %.0f Hz%s") : T("GPS %.0f Hz · starts above 3 km/h%s"), s.gpsHz, roll);
    else snprintf(t, sizeof t, T("GPS %.0f Hz · %d sats · ±%.1f km/h%s"), s.gpsHz, s.gpsSats, s.gpsSAcc, roll);
  } else {
    if (state == DragTimer::ARMED) snprintf(t, sizeof t, T("Bring the bike to a full stop (0 RPM)%s"), roll);
    else if (state == DragTimer::READY) snprintf(t, sizeof t, T("0 RPM · Timer starts when the wheel turns%s"), roll);
    else snprintf(t, sizeof t, T("Press ARM, then launch from a standstill%s"), roll);
  }
  g.text(240, 114, t, F13, s.dragWaitGps ? YELLOW : SUB, AL_C);
}
static void track(Gfx &g, int y, double dist, int n) {
  double end = ride::SPLIT_M[n - 1];
  g.fillRoundRect(14, y, 452, 8, 4, TRACK);
  int w = (int)(452 * fmin(dist, end) / end);
  if (w > 0) g.fillRoundRect(14, y, w < 8 ? 8 : w, 8, 4, GREEN);
  int lastX = -100;
  for (int i = n - 1; i >= 0; i--) {  // right to left: skip labels that would overlap
    int x = 14 + (int)(452 * ride::SPLIT_M[i] / end);
    g.fillRect(x, y - 4, 2, 16, 0x9AA7B3);
    if (lastX - x < 40 && i != n - 1) continue;
    g.text(x > 452 ? 452 : x, y + 24, ride::SPLIT_NAME[i], F10, MUTED, AL_C);
    lastX = x;
  }
}
static void splitTable(Gfx &g, int y, const ride::DragResult &r, const ride::DragResult *best, bool peak, const char *title) {
  const int n = r.n >= 1 && r.n <= ride::SPLITS ? r.n : 4, rh = n > 4 ? 20 : 24;
  g.fillRoundRect(10, y, 460, 22 + n * rh, 8, CARD2);
  const int cols[5] = {22, 120, 210, 300, 400};
  const char *head[5] = {title, "TIME", "TRAP", best ? "Δ BEST" : "", peak ? "PEAK A" : ""};
  for (int i = 0; i < 5; i++) g.text(cols[i], y + 15, head[i], F10, MUTED);
  for (int i = 0; i < n; i++) {
    int yy = y + 38 + i * rh;
    const ride::Split &sp = r.s[i];
    g.text(cols[0], yy, ride::SPLIT_NAME[i], F13, TEXT);
    char t[72];
    if (!sp.done) { g.text(cols[1], yy, "—", F13, MUTED); g.text(cols[2], yy, "—", F13, MUTED); continue; }
    snprintf(t, sizeof t, T("%.2f s"), sp.t); g.text(cols[1], yy, t, F15B, TEXT);
    snprintf(t, sizeof t, T("%.1f km/h"), sp.trapKmh); g.text(cols[2], yy, t, F13, TEXT);
    if (best && best->s[i].done) {
      double d = sp.t - best->s[i].t;
      snprintf(t, sizeof t, T("%+.2f"), d); g.text(cols[3], yy, t, F13B, d <= 0 ? GREEN : RED);
    }
    if (peak) { snprintf(t, sizeof t, T("%.0f A"), sp.peakA); g.text(cols[4], yy, t, F13, TEXT); }
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
      char t[48]; snprintf(t, sizeof t, T("%.2f"), d.elapsed(s.now));
      g.text(20, 96, t, F72B, TEXT);
      g.text(26 + g.textWidth(t, F72B), 96, "s", F24B, MUTED2);
    });
    g.boxed(320, 44, 150, 60, BG, [&] {
      g.text(460, 62, "SPEED", F10, MUTED, AL_R);
      char t[48]; fmt1(t, 16, d.speedKmh());
      int w = g.textWidth(" km/h", F13);
      g.text(460 - w, 96, t, F38B, TEXT, AL_R);
      g.text(460, 96, " km/h", F13, MUTED2, AL_R);
    });
    g.boxed(10, 106, 460, 54, BG, [&] {
      char t[240];
      if (s.slipOk) snprintf(t, sizeof t, T("Distance %.1f m  ·  %.0f A  ·  slip %.0f%%"), d.distance(), s.live.amp, s.slipPct);
    else if (s.linked) snprintf(t, sizeof t, T("Distance %.1f m  ·  %.0f A  ·  %.1f kW"), d.distance(), s.live.amp, s.kw);
    else snprintf(t, sizeof t, T("Distance %.1f m  ·  GPS %d sats"), d.distance(), s.gpsSats);
      g.text(20, 120, t, F13, SUB);
      track(g, 128, d.distance(), d.r.n);
    });
    g.boxed(10, 158, 460, 120, BG, [&] { splitTable(g, 158, d.r, best, false, "SPLIT"); });
    return;
  }
  if (!full) {
    if (d.state == DragTimer::ARMED || d.state == DragTimer::READY || d.state == DragTimer::IDLE)
      g.boxed(0, 100, 480, 42, BG, [&] {
        dragHint(g, s, c, d.state);
        char t[192];
        if (s.linked)
          snprintf(t, sizeof t, T("%.1f V · %d%% · CTRL %d°C · Gear %c · %d rpm"), s.live.volt, s.soc, s.live.tCtrl,
                   GEAR_CH[s.live.gear()], s.live.rpm);
        else snprintf(t, sizeof t, T("%.1f km/h · no controller connected"), s.kmh);
        g.text(240, 134, t, F11, MUTED, AL_C);
      });
    return;
  }
  g.fillRect(0, 0, 480, 320, BG);
  if (d.state == DragTimer::DONE || d.state == DragTimer::ABORTED) {
    bool done = d.state == DragTimer::DONE;
    dragTop(g, s, done ? "FINISHED" : "ABORTED", done ? BLUE : RED);
    char t[144];
    const ride::Split &last = d.r.last();
    const char *tn = ride::TARGET_NAME[ride::targetIx(d.r.n)];
    if (done) {
      snprintf(t, sizeof t, T("%.2f"), last.t);
      g.text(20, 84, t, F56B, TEXT);
      g.text(26 + g.textWidth(t, F56B), 84, "s", F24B, MUTED2);
      snprintf(t, sizeof t, T("%s @ %.1f km/h"), tn, last.trapKmh);
      g.text(20, 108, t, F13B, TEXT);
      if (!d.r.spinCorrected && best && best->n == d.r.n && (!best->finished() || last.t < best->last().t)) {
        snprintf(t, sizeof t, best->finished() ? T("★ NEW BEST %+.2f") : T("★ NEW BEST"), best->finished() ? last.t - best->last().t : 0.0);
        g.fillRoundRect(330, 46, 140, 30, 15, YELLOW);
        g.textMid(400, 61, t, F13B, 0x000000, AL_C);
      }
    } else {
      g.text(20, 84, "ABORTED", F38B, RED);
      snprintf(t, sizeof t, T("Run stopped before %s · saved to history"), tn);
      g.text(20, 108, t, F13, SUB);
    }
    if (d.r.peakA > 0) snprintf(t, sizeof t, T("Peak %.0f A · %.1f kW · min %.1f V"), d.r.peakA, d.r.peakKw, d.r.minV);
    else snprintf(t, sizeof t, "%s", T("no controller data"));
    g.text(470, 96, t, F10, MUTED, AL_R);
    snprintf(t, sizeof t, T("%s%s"), s.src ? T("GPS timing") : T("RPM timing ±0.05 s"), c.rollout ? T(" · 1 ft rollout") : "");
    g.text(470, 110, t, F10, MUTED, AL_R);
    if (d.r.spinCorrected) {
      g.fillRoundRect(330, 46, 140, 30, 15, DARKRED);
      g.textMid(400, 61, "SPIN-CORRECTED", F13B, RED, AL_C);
      g.text(470, 124, "Not counted as best run", F10, RED, AL_R);
    } else if (d.r.maxSlip > 5) {
      snprintf(t, sizeof t, T("Max wheel slip %.0f%% (GPS)"), d.r.maxSlip);
      g.text(470, 124, t, F10, YELLOW, AL_R);
    } else if (d.r.wheelspin) g.text(470, 124, "Wheel spin detected", F10, YELLOW, AL_R);
    splitTable(g, 128, d.r, best, d.r.peakA > 0, "SPLIT");
    button(g, DB[0], "EXIT"); button(g, DB[1], "HISTORY"); button(g, DB[2], "WHEEL");
    button(g, DB[3], "AGAIN", GREEN, 0x000000);
    return;
  }
  // IDLE / ARMED / READY
  bool ready = d.state == DragTimer::READY, armed = d.state == DragTimer::ARMED;
  dragTop(g, s, ready ? "READY" : armed ? "ARMED" : "IDLE", ready ? GREEN : armed ? YELLOW : MUTED2);
  if (ready) g.text(240, 92, "READY", F56B, GREEN, AL_C);
  else if (armed) g.text(240, 92, s.dragWaitGps ? "GPS" : "STOP", F56B, YELLOW, AL_C);
  else g.text(240, 92, "DRAG", F56B, TEXT, AL_C);
  dragHint(g, s, c, d.state);
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
  const int cols[7] = {12, 106, 166, 226, 286, 346, 416};
  const char *h[7] = {"DATE", "DIST", "60 ft", "100 m", "201 m", "402 m", "1000 m"};
  for (int i = 0; i < 7; i++) g.text(cols[i], 52, h[i], F10, MUTED);
  if (!s.runN) g.text(240, 160, "No runs saved yet", F13, MUTED, AL_C);
  for (int r = 0; r < s.runN && r < 9; r++) {
    const DragRow &d = s.runs[r];
    int y = 76 + r * 26;
    if (r % 2 == 0) g.fillRect(6, y - 17, 468, 24, CARD2);
    g.text(cols[0], y, d.when, F11, SUB);
    g.text(cols[1], y, ride::TARGET_NAME[ride::targetIx(d.n)], F11B, d.done ? YELLOW : MUTED2);
    char t[48];
    for (int i = 0; i < 5; i++) {
      if (i < d.n && d.t[i] > 0) snprintf(t, sizeof t, T("%.2f"), d.t[i]); else strcpy(t, "—");
      g.text(cols[i + 2], y, t, i == d.n - 1 && d.done ? F13B : F13, i < d.n ? TEXT : DIM);
    }
  }
}

// ============================================================ LOG =========
static const Btn LB[3] = {{10, 282, 148, 30}, {166, 282, 148, 30}, {322, 282, 148, 30}};
static void logPage(Gfx &g, const UiState &s, const Settings &c) {
  char r[144];
  snprintf(r, sizeof r, T("%s"), s.sd ? T("SD card") : T("internal"));
  header(g, "LOG", r);
  char t[256];
  snprintf(t, sizeof t, T("Storage: %s · %u KB free of %u KB · rate %s"), s.sd ? T("SD card") : T("internal flash"),
           (unsigned)s.fsFreeKB, (unsigned)s.fsTotalKB, c.logRate ? "" : T("max"));
  if (c.logRate) snprintf(t, sizeof t, T("Storage: %s · %u KB free of %u KB · %u Hz"), s.sd ? T("SD card") : T("internal flash"),
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
    char b[48]; snprintf(b, sizeof b, i == 0 ? "%.2f" : "%.0f", val[i]);
    g.text(x, 104, b, F18B, i == 3 ? YELLOW : TEXT);
    g.text(x, 115, lab[i], F10, MUTED);
  }
  g.text(14, 140, "TRIPS", F10, MUTED);
  if (!s.tripN) g.text(240, 200, "No trips recorded yet", F13, MUTED, AL_C);
  for (int i = 0; i < s.tripN && i < 5; i++) {
    const TripRow &x = s.trips[i];
    int y = 160 + i * 24;
    if (i % 2 == 0) g.fillRect(6, y - 16, 468, 23, CARD2);
    char b[256];
    snprintf(b, sizeof b, T("#%u"), x.id); g.text(14, y, b, F13B, TEXT);
    g.text(56, y, x.when, F11, SUB);
    snprintf(b, sizeof b, T("%.1f km · %.0f km/h · %.0f A · %.0f Wh/km"), x.km, x.maxKmh, x.maxA, x.whKm);
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
    char b[36];
    snprintf(b, sizeof b, T("%.0f"), mk * i / 4); g.text(X - 4, y + 4, b, F10, GREEN, AL_R);
    snprintf(b, sizeof b, T("%.0f"), mi + (ma - mi) * i / 4); g.text(X + W + 4, y + 4, b, F10, YELLOW);
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
  char b[144]; snprintf(b, sizeof b, T("%d s shown"), s.gN); g.text(470, 300, b, F11, MUTED, AL_R);
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
    char t[256], n[240];
    faultTextT(f.code, n, sizeof n);
    snprintf(t, sizeof t, T("%s %s"), f.set ? "●" : "○", f.set ? n : T("cleared"));
    g.text(cols[1], y, t, F11B, f.set ? RED : GREEN);
    snprintf(t, sizeof t, T("%.0f"), f.kmh); g.text(cols[2], y, t, F11, TEXT);
    snprintf(t, sizeof t, T("%.0f"), f.amp); g.text(cols[3], y, t, F11, TEXT);
    snprintf(t, sizeof t, T("%.1f"), f.volt); g.text(cols[4], y, t, F11, TEXT);
  }
  char now[256]; faultLine(s, now, sizeof now);
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
static bool showAdv = false;
static int groupFields(int grp, int *idx) {
  int n = 0;
  for (int i = 0; i < PARAM_COUNT; i++)
    if (!strcmp(PARAMS[i].group, GROUPS[grp]) && (showAdv || !PARAMS[i].adv)) idx[n++] = i;
  return n;
}
static const Btn ADV_BTN = {360, 0, 120, 32};
static const Btn TB[3] = {{10, 282, 148, 30}, {166, 282, 148, 30}, {322, 282, 148, 30}};
static Btn groupBtn(int i) { return {10 + (i % 3) * 154, 102 + (i / 3) * 62, 146, 56}; }
static const Btn LAUNCH_BAR = {10, 228, 460, 46};
static void tunePage(Gfx &g, const UiState &s) {
  header(g, "TUNE CONTROLLER", showAdv ? "ADVANCED: ON" : "ADVANCED: OFF", showAdv ? YELLOW : MUTED2);
  bool have = s.params.complete();
  g.fillRoundRect(10, 40, 460, 56, 8, CARD);
  char t[256];
  if (have) {
    snprintf(t, sizeof t, T("Parameters read %s · check %05X"), s.paramsWhen, (unsigned)s.params.checkSum());
    g.text(22, 60, t, F13B, TEXT);
    snprintf(t, sizeof t, s.backupN ? T("Backup #%u saved before any change · %d backups kept") : T("No backup yet"), s.backupN ? s.backups[0].id : 0, s.backupN);
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
    g.text(b.x + 12, b.y + 24, GROUPS[i], F15B, have ? TEXT : DIM);
    char c[96]; snprintf(c, sizeof c, T("%d settings"), n);
    if (ch) snprintf(c, sizeof c, T("%d settings · %d changed"), n, ch);
    g.text(b.x + 12, b.y + 44, c, F10, ch ? YELLOW : MUTED);
  }
  g.fillRoundRect(LAUNCH_BAR.x, LAUNCH_BAR.y, LAUNCH_BAR.w, LAUNCH_BAR.h, 8, have ? 0x1A1408 : CARD2);
  g.text(22, 247, "LAUNCH PROFILE", F13B, have ? YELLOW : DIM);
  g.text(22, 264, "Street / Launch throttle settings against wheel spin", F10, MUTED);
  g.text(458, 256, ">", F18B, have ? YELLOW : DIM, AL_R);
  int ch = changedCount(s);
  button(g, TB[0], s.busy ? "READING..." : "READ", s.busy ? CARD2 : CHIP, TEXT);
  button(g, TB[1], "BACKUPS");
  char w[72]; snprintf(w, sizeof w, ch ? T("WRITE (%d)") : T("WRITE"), ch);
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
    char v[72], u[96];
    formatValue(s.edit, d, v, sizeof v);
    snprintf(u, sizeof u, T("%s %s"), v, d.unit);
    g.textMid(326, y + 15, u, F13B, ch ? YELLOW : TEXT, AL_R);
  }
  if (n > ROWS) {
    char p[120]; snprintf(p, sizeof p, T("%d-%d / %d"), editTop + 1, editTop + ROWS < n ? editTop + ROWS : n, n);
    g.text(14, 300, p, F11, MUTED);
    button(g, {190, 282, 70, 30}, "UP"); button(g, EB_UP, "DOWN");
  }
  // right panel: selected value
  const ParamDef &d = PARAMS[idx[editSel]];
  g.fillRoundRect(346, 40, 124, 110, 8, CARD);
  g.text(354, 58, d.name, F10, MUTED);
  char v[72]; formatValue(s.edit, d, v, sizeof v);
  g.text(408, 104, v, strlen(v) > 5 ? F24B : F38B, getRaw(s.edit, d) != getRaw(s.params.b, d) ? YELLOW : TEXT, AL_C);
  g.text(408, 124, d.unit, F13, MUTED2, AL_C);
  char was[96], o[72]; formatValue(s.params.b, d, o, sizeof o);
  snprintf(was, sizeof was, T("was %s"), o);
  g.text(408, 142, was, F10, MUTED, AL_C);
  button(g, EB_MINUS, "-", CHIP, TEXT, F24B);
  button(g, EB_PLUS, "+", CHIP, TEXT, F24B);
  char st[48]; snprintf(st, sizeof st, T("x%d"), editStep);
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
    char a[72], b[72], t[192];
    formatValue(s.params.b, d, a, sizeof a); formatValue(s.edit, d, b, sizeof b);
    if (shown % 2 == 0) g.fillRect(6, y - 15, 468, 22, CARD2);
    g.text(14, y, d.group, F11, MUTED);
    g.text(100, y, d.name, F13, TEXT);
    snprintf(t, sizeof t, T("%s  >  %s %s"), a, b, d.unit);
    g.text(466, y, t, F13B, YELLOW, AL_R);
    y += 22; shown++;
  }
  if (n > shown) { char t[96]; snprintf(t, sizeof t, T("+ %d more"), n - shown); g.text(14, y, t, F11, MUTED); }
  g.fillRoundRect(10, 214, 460, 58, 8, 0x1A1408);
  bool stopped = s.linked && s.live.rpm == 0;
  g.text(22, 234, stopped ? "Bike stopped (0 RPM)" : "Stop the bike first: writing needs 0 RPM", F13B, stopped ? GREEN : RED);
  char t[256];
  snprintf(t, sizeof t, T("A backup of the current values is kept (#%u). Values are read back to verify."), s.backupN ? s.backups[0].id : 0);
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
    char t[144];
    snprintf(t, sizeof t, T("#%u"), b.id); g.textMid(22, y + 12, t, F13B, TEXT);
    g.textMid(70, y + 12, b.when, F13, SUB);
    snprintf(t, sizeof t, T("check %05X"), (unsigned)b.sum); g.textMid(260, y + 12, t, F11, MUTED);
    if (s.params.complete() && b.sum == s.params.checkSum()) g.textMid(458, y + 12, "= controller", F11B, GREEN, AL_R);
  }
  button(g, BB_RESTORE, "RESTORE", backupSel >= 0 ? YELLOW : CARD2, backupSel >= 0 ? 0x000000 : DIM);
  g.text(14, 300, "Restore loads the backup, then WRITE asks for confirmation.", F10, MUTED);
}

// ======================================================= LAUNCH PROFILE ===
static int profParam(int f) {  // index into PARAMS of profile field f
  for (int i = 0; i < PARAM_COUNT; i++) if (!strcmp(PARAMS[i].name, PROF_FIELDS[f])) return i;
  return 0;
}
static int profSel = 0;  // 0..5: profile*3 + field
static const Btn LP_MINUS = {346, 132, 58, 40}, LP_PLUS = {412, 132, 58, 40}, LP_CAP0 = {10, 194, 160, 34},
                 LP_CAP1 = {176, 194, 160, 34}, LP_AP0 = {10, 282, 225, 30}, LP_AP1 = {245, 282, 225, 30};
static void launchPage(Gfx &g, const UiState &s) {
  header(g, "LAUNCH PROFILE");
  g.text(14, 52, "Lower throttle rise / start torque = less wheel spin at launch.", F11, SUB);
  const int cx[2] = {196, 280};
  g.text(cx[0], 72, "STREET", F10, MUTED, AL_R);
  g.text(cx[1], 72, "LAUNCH", F10, YELLOW, AL_R);
  g.text(334, 72, "NOW", F10, MUTED, AL_R);
  for (int f = 0; f < 3; f++) {
    const ParamDef &d = PARAMS[profParam(f)];
    int y = 80 + f * 36;
    g.fillRoundRect(10, y, 326, 32, 6, CARD2);
    g.textMid(20, y + 16, PROF_FIELDS[f], F13, TEXT);
    for (int p = 0; p < 2; p++) {
      bool sel = profSel == p * 3 + f;
      if (sel) g.drawRoundRect(cx[p] - 64, y + 3, 70, 26, 5, BLUE);
      char t[48];
      if (s.prof.have[p]) snprintf(t, sizeof t, T("%ld"), (long)s.prof.v[p][f]); else strcpy(t, "-");
      g.textMid(cx[p], y + 16, t, F13B, p ? YELLOW : TEXT, AL_R);
    }
    char now[48]; snprintf(now, sizeof now, T("%ld"), (long)getRaw(s.params.b, d));
    g.textMid(330, y + 16, now, F13, MUTED2, AL_R);
  }
  int p = profSel / 3, f = profSel % 3;
  g.fillRoundRect(346, 80, 124, 46, 8, CARD);
  g.text(354, 96, p ? "LAUNCH" : "STREET", F10, p ? YELLOW : MUTED);
  g.text(354, 116, PROF_FIELDS[f], F11B, TEXT);
  button(g, LP_MINUS, "-", CHIP, TEXT, F24B);
  button(g, LP_PLUS, "+", CHIP, TEXT, F24B);
  button(g, LP_CAP0, "SAVE NOW AS STREET");
  button(g, LP_CAP1, "SAVE NOW AS LAUNCH");
  g.text(14, 250, "Apply = load into the editor, then confirm WRITE (backup kept).", F10, MUTED);
  g.text(14, 266, "Raw values as the PC tool shows them.", F10, MUTED);
  button(g, LP_AP0, "APPLY STREET", s.prof.have[0] ? CHIP : CARD2, s.prof.have[0] ? TEXT : DIM);
  button(g, LP_AP1, "APPLY LAUNCH", s.prof.have[1] ? YELLOW : CARD2, s.prof.have[1] ? 0x000000 : DIM);
}

// ============================================================ SETUP =======
struct Row { const char *name; };
static const int NSROW = 16;
static const char *const SROW[NSROW] = {"Language", "Dashboard", "Speed source", "Log rate", "Battery cells", "Cell empty/full",
                                        "Gauge max km/h", "Gauge max A", "Backlight", "Wi-Fi hotspot", "Time zone (UTC)",
                                        "Drag rollout", "GPS rate", "Max traction (spin)", "Drag distance", "Internet Wi-Fi"};
static const char *netName = "";
static void setupValue(const Settings &c, int i, char *t) {
  switch (i) {
  case 0: snprintf(t, 72, T("%s"), DASH_NAME[c.dash]); break;
  case 1: snprintf(t, 72, T("%s"), c.speedSrc == 0 ? "RPM" : c.speedSrc == 1 ? "GPS" : "AUTO"); break;
  case 2: if (c.logRate) snprintf(t, 72, T("%u Hz"), LOG_RATES[c.logRate]); else strcpy(t, "MAX"); break;
  case 3: snprintf(t, 72, T("%uS"), c.cells); break;
  case 4: snprintf(t, 72, T("%.2f/%.2f"), c.cellEmptyMv / 1000.0, c.cellFullMv / 1000.0); break;
  case 5: snprintf(t, 72, T("%u"), c.maxKmh); break;
  case 6: snprintf(t, 72, T("%u"), c.maxAmp); break;
  case 7: snprintf(t, 72, T("%u/5"), c.backlight); break;
  case 8: snprintf(t, 72, T("%s"), c.wifi ? "ON" : "OFF"); break;
  case 9: snprintf(t, 72, T("UTC%+d"), c.tzHours); break;
  case 10: snprintf(t, 72, T("%s"), c.rollout ? "1 ft (strip)" : "OFF (0 ft)"); break;
  case 11: snprintf(t, 72, T("%s"), c.gpsRate ? "25 Hz (M10)" : "10 Hz"); break;
  case 12: snprintf(t, 72, T("%.1f g"), c.maxG10 / 10.0); break;
  case 13: snprintf(t, 72, T("%s"), ride::TARGET_NAME[c.dragTarget <= 2 ? c.dragTarget : 1]); break;
  case 14: snprintf(t, 72, T("%s"), netName[0] ? netName : "set on web page"); break;
  }
}
static Btn srow(int i) { return {10 + (i / 8) * 234, 37 + (i % 8) * 30, 226, 28}; }
static const Btn SB[3] = {{10, 282, 148, 30}, {166, 282, 148, 30}, {322, 282, 148, 30}};
static void setupPage(Gfx &g, const UiState &s, const Settings &c) {
  header(g, "SETUP");
  netName = s.netOn ? s.netSsid : "";
  for (int i = 0; i < NSROW; i++) {
    Btn b = srow(i);
    g.fillRoundRect(b.x, b.y, b.w, b.h, 6, i == 0 ? 0x13202A : CARD);
    g.text(b.x + 10, b.y + 11, SROW[i], F10, MUTED);
    char t[96];
    if (i == 0) strcpy(t, c.lang ? "English" : "ไทย"); else setupValue(c, i - 1, t);
    g.text(b.x + 10, b.y + 25, t, F13B, i == 15 && !s.netOn ? MUTED2 : TEXT);
    if (i == 15) continue;  // typed on the web page
    g.fillRoundRect(b.x + b.w - 72, b.y + 2, 32, 24, 6, CHIP);
    g.textMid(b.x + b.w - 56, b.y + 14, "-", F18B, TEXT, AL_C);
    g.fillRoundRect(b.x + b.w - 37, b.y + 2, 32, 24, 6, CHIP);
    g.textMid(b.x + b.w - 21, b.y + 14, "+", F18B, TEXT, AL_C);
  }
  button(g, SB[0], "WHEEL"); button(g, SB[1], "TOUCH CAL"); button(g, SB[2], "INFO");
}
static void adjust(Settings &c, int i, int d) {
  if (i == 0) { c.lang = !c.lang; return; }
  i--;
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
  case 5: clampAdd(c.maxKmh, 10, 40, 400); break;
  case 6: clampAdd(c.maxAmp, 50, 50, 1500); break;
  case 7: clampAdd(c.backlight, 1, 1, 5); break;
  case 8: c.wifi = !c.wifi; break;
  case 9: clampAdd(c.tzHours, 1, -12, 14); break;
  case 10: c.rollout = !c.rollout; break;
  case 11: c.gpsRate = !c.gpsRate; break;
  case 12: clampAdd(c.maxG10, 1, 5, 25); break;
  case 13: cyc(c.dragTarget, 3); break;
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
    char t[72];
    if (i == 0) snprintf(t, sizeof t, T("%u"), c.tyreW);
    if (i == 1) snprintf(t, sizeof t, T("%u"), c.tyreA);
    if (i == 2) snprintf(t, sizeof t, T("%u"), c.tyreR);
    if (i == 3) snprintf(t, sizeof t, T("%+.1f"), c.wheelCorr / 10.0);
    if (i == 4) snprintf(t, sizeof t, T("%.2f : 1"), c.ratioX100 / 100.0);
    g.text(b.x + 10, b.y + 33, t, F13B, TEXT);
    g.fillRoundRect(b.x + b.w - 74, b.y + 6, 32, 28, 6, CHIP);
    g.textMid(b.x + b.w - 58, b.y + 20, "-", F18B, TEXT, AL_C);
    g.fillRoundRect(b.x + b.w - 38, b.y + 6, 32, 28, 6, CHIP);
    g.textMid(b.x + b.w - 22, b.y + 20, "+", F18B, TEXT, AL_C);
  }
  g.fillRoundRect(320, 40, 150, 224, 8, CARD2);
  char t[144];
  snprintf(t, sizeof t, T("%u/%u-%u"), c.tyreW, c.tyreA, c.tyreR);
  g.text(395, 64, t, F15B, TEXT, AL_C);
  g.text(395, 92, "circumference", F10, MUTED, AL_C);
  snprintf(t, sizeof t, T("%.0f mm"), c.circMm());
  g.text(395, 116, t, F24B, YELLOW, AL_C);
  g.text(395, 148, "at 1000 rpm", F10, MUTED, AL_C);
  snprintf(t, sizeof t, T("%.1f km/h"), ride::rpmToKmh(1000, c.circMm(), c.ratioX100 / 100.0f));
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
static const Btn IB_CHECK = {10, 282, 225, 30}, IB_INSTALL = {245, 282, 225, 30};
static void infoLine(Gfx &g, int &y, const char *k, const char *v, uint32_t c = TEXT) {
  g.text(14, y, k, F11, MUTED); g.text(130, y, v, F13B, c); y += 21;
}
static void updateLine(Gfx &g, const UiState &s) {
  char t[256];
  uint32_t c = s.updState == 4 ? RED : s.updAvailable ? YELLOW : s.netConnected ? GREEN : MUTED2;
  if (s.updState == 2) snprintf(t, sizeof t, T("Downloading %s... %d%%"), s.updLatest, s.updProgress);
  else if (s.updFmt && s.updFmt[0]) snprintf(t, sizeof t, T(s.updFmt), s.updArg);
  else if (s.updMsg[0]) snprintf(t, sizeof t, "%s", s.updMsg);
  else snprintf(t, sizeof t, "%s", s.netConnected ? T("Not checked yet") : T("Join a Wi-Fi network (web page > SETUP)"));
  g.text(14, 262, "Update", F11, MUTED);
  g.text(130, 262, t, F13B, c);
  if (s.updState == 2) {
    g.fillRoundRect(130, 268, 336, 6, 3, TRACK);
    g.fillRoundRect(130, 268, 336 * s.updProgress / 100, 6, 3, YELLOW);
  }
}
static void infoPage(Gfx &g, const UiState &s) {
  header(g, "INFO");
  char t[256];
  int y = 54;
  infoLine(g, y, "Firmware", s.fw);
  if (s.rolledBack) infoLine(g, y, "Last update", "failed to start - previous version restored", RED);
  infoLine(g, y, "Hotspot", s.ssid);
  if (s.netOn) snprintf(t, sizeof t, T("%s · %s"), s.netSsid, s.netConnected ? s.netIp : T("connecting..."));
  else snprintf(t, sizeof t, "%s", T("not set (web page > SETUP)"));
  infoLine(g, y, "Internet", t, s.netConnected ? GREEN : MUTED2);
  snprintf(t, sizeof t, T("%s · %u baud · %.1f Hz"), s.linked ? T("connected") : T("no data"), (unsigned)s.baud, s.hz);
  infoLine(g, y, "Controller", t, s.linked ? GREEN : RED);
  snprintf(t, sizeof t, T("%u good · %u bad"), (unsigned)s.frames, (unsigned)s.errors);
  infoLine(g, y, "Frames", t);
  if (s.gpsSeen) snprintf(t, sizeof t, T("%s%s · %d sats · %.0f Hz · ±%.1f km/h"), s.gpsUbx ? "u-blox " : "NMEA ", s.gpsFix ? T("fix") : T("no fix"), s.gpsSats, s.gpsHz, s.gpsSAcc);
  else snprintf(t, sizeof t, "%s", T("not connected (optional)"));
  infoLine(g, y, "GPS", t, s.gpsFix ? GREEN : TEXT);
  snprintf(t, sizeof t, T("%s · %u KB free · RAM %u KB"), s.sd ? T("SD card") : T("internal"), (unsigned)s.fsFreeKB, (unsigned)s.heapKB);
  infoLine(g, y, "Storage", t);
  infoLine(g, y, "Wiring", "VOTOL TX>IO35 · RX<IO22 · GPS TX>IO21, RX<IO22", MUTED2);
  g.boxed(0, 248, 480, 30, BG, [&] { updateLine(g, s); });
  bool stopped = !s.linked || s.live.rpm == 0;
  button(g, IB_CHECK, "CHECK FOR UPDATE", s.netConnected ? CHIP : CARD2, s.netConnected ? TEXT : DIM);
  char b[96]; snprintf(b, sizeof b, s.updAvailable ? T("INSTALL %s") : T("INSTALL"), s.updLatest);
  bool can = s.updAvailable && stopped && s.updState != 2;
  button(g, IB_INSTALL, b, can ? YELLOW : CARD2, can ? 0x000000 : DIM);
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
  i18n::lang = c.lang;
  showAdv = c.advanced;
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
  case PG_SETUP: setupPage(g, s, c); break;
  case PG_LAUNCH: launchPage(g, s); break;
  case PG_WHEEL: wheelPage(g, c); break;
  case PG_INFO: infoPage(g, s); break;
  default: break;
  }
}
void update(Gfx &g, const UiState &s, const Settings &c) {
  i18n::lang = c.lang;
  if (page == PG_DASH && !menuOpen) dash(g, s, c, false);
  else if (page == PG_DRAG) dragPage(g, s, c, false);
  else if (page == PG_INFO) g.boxed(0, 248, 480, 30, BG, [&] { updateLine(g, s); });
}
bool tick(uint32_t ms) {
  nowMs = ms;
  if (menuOpen && ms - menuAt > 6000) { menuOpen = false; return true; }
  if (menuDirty) { menuDirty = false; return true; }
  return false;
}
bool touch(int x, int y, UiState &s, Settings &c, UiActions &a) {
  showAdv = c.advanced;
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
    if (hit({60, 0, 86, 28}, x, y)) { c.dragTarget = (uint8_t)((c.dragTarget + 1) % 3); a.settingsChanged = true; return true; }
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
    if (hit(ADV_BTN, x, y)) { c.advanced = !c.advanced; showAdv = c.advanced; a.settingsChanged = true; return true; }
    if (hit(TB[0], x, y) && !s.busy) { a.readParams = true; s.msg[0] = 0; return true; }
    if (hit(TB[1], x, y)) { backupSel = -1; go(PG_BACKUPS); return true; }
    if (hit(TB[2], x, y) && changedCount(s)) { s.msg[0] = 0; go(PG_CONFIRM); return true; }
    for (int i = 0; i < GROUP_COUNT; i++)
      if (hit(groupBtn(i), x, y) && s.params.complete()) { editGroup = i; editSel = 0; editTop = 0; go(PG_EDIT); return true; }
    if (hit(LAUNCH_BAR, x, y) && s.params.complete()) { profSel = 3; go(PG_LAUNCH); return true; }
    return false;
  case PG_LAUNCH: {
    if (hit(BACK, x, y)) { go(PG_TUNE); return true; }
    int p = profSel / 3, f = profSel % 3;
    const ParamDef &d = PARAMS[profParam(f)];
    auto capture = [&](int q) {
      for (int k = 0; k < 3; k++) s.prof.v[q][k] = getRaw(s.params.b, PARAMS[profParam(k)]);
      s.prof.have[q] = 1; a.profChanged = true;
    };
    if (hit(LP_MINUS, x, y) || hit(LP_PLUS, x, y)) {
      if (!s.prof.have[p]) capture(p);
      int32_t v = s.prof.v[p][f] + (hit(LP_PLUS, x, y) ? 1 : -1) * d.step;
      s.prof.v[p][f] = v < d.min ? d.min : v > d.max ? d.max : v;
      a.profChanged = true;
      return true;
    }
    if (hit(LP_CAP0, x, y)) { capture(0); return true; }
    if (hit(LP_CAP1, x, y)) { capture(1); return true; }
    if (hit(LP_AP0, x, y) || hit(LP_AP1, x, y)) {
      int q = hit(LP_AP1, x, y) ? 1 : 0;
      if (!s.prof.have[q]) return false;
      for (int k = 0; k < 3; k++) setRaw(s.edit, PARAMS[profParam(k)], s.prof.v[q][k]);
      s.msg[0] = 0;
      go(changedCount(s) ? PG_CONFIRM : PG_TUNE);
      return true;
    }
    for (int f2 = 0; f2 < 3; f2++)
      for (int q = 0; q < 2; q++)
        if (hit({q ? 216 : 132, 80 + f2 * 36, 70, 32}, x, y)) { profSel = q * 3 + f2; return true; }
    return false;
  }
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
    for (int i = 0; i < NSROW - 1; i++) {
      Btn b = srow(i);
      if (hit({b.x + b.w - 74, b.y, 37, b.h}, x, y)) { adjust(c, i, -1); a.settingsChanged = true; return true; }
      if (hit({b.x + b.w - 37, b.y, 37, b.h}, x, y)) { adjust(c, i, +1); a.settingsChanged = true; return true; }
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
  case PG_INFO:
    if (hit(IB_CHECK, x, y)) { if (s.netConnected) a.checkUpdate = true; return true; }
    if (hit(IB_INSTALL, x, y)) { if (s.updAvailable) a.installUpdate = true; return true; }
    if (hit(BACK, x, y)) { go(PG_SETUP); return true; }
    return false;
  default: return false;
  }
}
}  // namespace ui
