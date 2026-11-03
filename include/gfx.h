// Tiny drawing interface used by all screens (src/ui.cpp).
// On the board it is backed by LovyanGFX (src/gfx_lgfx.cpp); on the PC by a
// recorder (tools/sim/sim_main.cpp) whose output tools/render_screens.py turns
// into the preview PNGs. Both share the same font metrics (vlw_metrics.h), so
// the previews show exactly what the screen draws.
#pragma once
#include <stdint.h>
#include <functional>
#include "fonts_vlw.h"

// Text anchors (x is left/centre/right, y is always the BASELINE).
enum Align : uint8_t { AL_L, AL_C, AL_R };

struct Gfx {
  virtual ~Gfx() {}
  virtual void fillRect(int x, int y, int w, int h, uint32_t c) = 0;
  virtual void fillRoundRect(int x, int y, int w, int h, int r, uint32_t c) = 0;
  virtual void drawRoundRect(int x, int y, int w, int h, int r, uint32_t c) = 0;
  virtual void fillCircle(int x, int y, int r, uint32_t c) = 0;
  virtual void drawLine(int x0, int y0, int x1, int y1, uint32_t c) = 0;
  virtual void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) = 0;
  // Ring sector; angles in degrees, 0 = 3 o'clock, clockwise.
  virtual void fillArc(int x, int y, int r0, int r1, float a0, float a1, uint32_t c) = 0;
  // Anti-aliased text, transparent background, y = baseline.
  virtual void textRaw(int x, int y, const char *s, int font, uint32_t c) = 0;
  // Flicker-free region: everything drawn inside fn() goes to an off-screen
  // buffer (in strips if large) that is then copied to the panel in one go.
  virtual void boxed(int x, int y, int w, int h, uint32_t bg, const std::function<void()> &fn) {
    fillRect(x, y, w, h, bg);
    fn();
  }

  // ---- helpers shared by both back-ends ----
  int textWidth(const char *s, int font);
  int capHeight(int font);  // height of 'H' above the baseline
  void text(int x, int y, const char *s, int font, uint32_t c, Align a = AL_L) {
    int w = a == AL_L ? 0 : textWidth(s, font);
    textRaw(a == AL_L ? x : a == AL_C ? x - w / 2 : x - w, y, s, font, c);
  }
  // Vertically centred on y (uses cap height).
  void textMid(int x, int y, const char *s, int font, uint32_t c, Align a = AL_L) {
    text(x, y + capHeight(font) / 2, s, font, c, a);
  }
};
#include "vlw_metrics.h"
