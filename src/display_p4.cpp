// KSDIY ESP32-P4C5: the UI is drawn at its native 480x320 into a PSRAM canvas, then
// scaled x1.5 (bilinear) and rotated into the 480x800 portrait MIPI panel, which is
// used as 800x480 landscape (720x480 picture, 40 px bars left/right).
#ifdef VOTOL_P4C5
#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include "display.h"
#include "p4_panel.h"
#include "vlw_metrics.h"

static const int UW = 480, UH = 320;   // UI size
static const int LW = 800, LH = 480;   // landscape panel
static const int OX = 40;              // left bar
static lgfx::LGFX_Sprite canvas;
static bool flip = false;              // rotate the picture 180 degrees (Setup > ROTATE)
static bool panelOk = false;
static int dx0 = UW, dy0 = UH, dx1 = 0, dy1 = 0;  // dirty box in UI coordinates
static uint16_t *line = nullptr;                   // output block (portrait), PSRAM

static inline void mark(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (x < dx0) dx0 = x < 0 ? 0 : x;
  if (y < dy0) dy0 = y < 0 ? 0 : y;
  if (x + w > dx1) dx1 = x + w > UW ? UW : x + w;
  if (y + h > dy1) dy1 = y + h > UH ? UH : y + h;
}
static uint16_t c565(uint32_t c) { return lgfx::color565((c >> 16) & 255, (c >> 8) & 255, c & 255); }

class GfxP4 : public Gfx {
public:
  int font = -1;
  void fillRect(int x, int y, int w, int h, uint32_t c) override { canvas.fillRect(x, y, w, h, c565(c)); mark(x, y, w, h); }
  void fillRoundRect(int x, int y, int w, int h, int r, uint32_t c) override { canvas.fillRoundRect(x, y, w, h, r, c565(c)); mark(x, y, w, h); }
  void drawRoundRect(int x, int y, int w, int h, int r, uint32_t c) override { canvas.drawRoundRect(x, y, w, h, r, c565(c)); mark(x, y, w, h); }
  void fillCircle(int x, int y, int r, uint32_t c) override { canvas.fillCircle(x, y, r, c565(c)); mark(x - r, y - r, 2 * r + 1, 2 * r + 1); }
  void drawLine(int x0, int y0, int x1, int y1, uint32_t c) override {
    canvas.drawLine(x0, y0, x1, y1, c565(c));
    mark(min(x0, x1), min(y0, y1), abs(x1 - x0) + 1, abs(y1 - y0) + 1);
  }
  void fillTriangle(int a, int b, int c2, int d, int e, int f, uint32_t c) override {
    canvas.fillTriangle(a, b, c2, d, e, f, c565(c));
    int x0 = min(a, min(c2, e)), x1 = max(a, max(c2, e)), y0 = min(b, min(d, f)), y1 = max(b, max(d, f));
    mark(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
  }
  void fillArc(int x, int y, int r0, int r1, float a0, float a1, uint32_t c) override {
    canvas.fillArc(x, y, r1, r0, a0, a1, c565(c));
    int r = max(r0, r1);
    mark(x - r, y - r, 2 * r + 1, 2 * r + 1);
  }
  void textRaw(int x, int y, const char *s, int f, uint32_t c) override {
    if (f != font) { canvas.loadFont(FONT_VLW[f]); font = f; }
    canvas.setTextColor(c565(c));
    canvas.setTextDatum(lgfx::baseline_left);
    int w = canvas.textWidth(s), h = canvas.fontHeight();
    canvas.drawString(s, x, y);
    mark(x - 2, y - h, w + 4, h + h / 2 + 2);
  }
  // the canvas is already off-screen: draw the box in place, it reaches the panel on flush
  void boxed(int x, int y, int w, int h, uint32_t bg, const std::function<void()> &fn) override {
    canvas.setClipRect(x, y, w, h);
    canvas.fillRect(x, y, w, h, c565(bg));
    fn();
    canvas.clearClipRect();
    mark(x, y, w, h);
  }
};
static GfxP4 gfx;
Gfx &displayGfx() { return gfx; }

// landscape (X,Y) -> portrait (px,py)
static inline void l2p(int X, int Y, int &px, int &py) {
  if (!flip) { px = LH - 1 - Y; py = X; }
  else { px = Y; py = LW - 1 - X; }
}

static inline uint16_t px565(const uint16_t *b, int x, int y) {
  uint16_t v = b[y * UW + x];
  return (uint16_t)((v >> 8) | (v << 8));  // the sprite keeps big-endian RGB565
}
static inline uint16_t blend(uint16_t a, uint16_t b, uint16_t c, uint16_t d, int fx, int fy) {
  // fx, fy in 0..255
  int w00 = (256 - fx) * (256 - fy), w10 = fx * (256 - fy), w01 = (256 - fx) * fy, w11 = fx * fy;
  int r = (((a >> 11) & 31) * w00 + ((b >> 11) & 31) * w10 + ((c >> 11) & 31) * w01 + ((d >> 11) & 31) * w11) >> 16;
  int g = (((a >> 5) & 63) * w00 + ((b >> 5) & 63) * w10 + ((c >> 5) & 63) * w01 + ((d >> 5) & 63) * w11) >> 16;
  int bl = ((a & 31) * w00 + (b & 31) * w10 + (c & 31) * w01 + (d & 31) * w11) >> 16;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// copy a UI rectangle to the panel: landscape block -> rotated portrait block
static void push(int ux0, int uy0, int ux1, int uy1) {
  if (!panelOk || !line) return;
  const uint16_t *src = (const uint16_t *)canvas.getBuffer();
  int X0 = OX + (ux0 * 3) / 2 - 1, X1 = OX + (ux1 * 3 + 1) / 2 + 1;
  int Y0 = (uy0 * 3) / 2 - 1, Y1 = (uy1 * 3 + 1) / 2 + 1;
  if (X0 < OX) X0 = OX;
  if (X1 > OX + UW * 3 / 2) X1 = OX + UW * 3 / 2;
  if (Y0 < 0) Y0 = 0;
  if (Y1 > LH) Y1 = LH;
  // walk the block in strips of landscape columns so the temp buffer stays small
  const int STRIP = 64;
  for (int xs = X0; xs < X1; xs += STRIP) {
    int xe = xs + STRIP < X1 ? xs + STRIP : X1;
    int pw = Y1 - Y0, ph = xe - xs;  // portrait block: width = landscape height span, height = landscape width span
    for (int X = xs; X < xe; X++) {
      int ufp = ((X - OX) * 512) / 3;  // u * 256
      int u = ufp >> 8, fu = ufp & 255;
      int u1 = u + 1 < UW ? u + 1 : u;
      for (int Y = Y0; Y < Y1; Y++) {
        int vfp = (Y * 512) / 3;
        int v = vfp >> 8, fv = vfp & 255;
        if (v >= UH) { v = UH - 1; fv = 0; }
        int v1 = v + 1 < UH ? v + 1 : v;
        uint16_t c = blend(px565(src, u, v), px565(src, u1, v), px565(src, u, v1), px565(src, u1, v1), fu, fv);
        int px, py;
        l2p(X, Y, px, py);
        // position inside the portrait block
        int bx, by;
        if (!flip) { bx = px - (LH - Y1); by = py - xs; }
        else { bx = px - Y0; by = py - (LW - xe); }
        line[by * pw + bx] = c;
      }
    }
    int px0, py0;
    if (!flip) { px0 = LH - Y1; py0 = xs; }
    else { px0 = Y0; py0 = LW - xe; }
    p4_panel_draw(px0, py0, px0 + pw, py0 + ph, line);
  }
}

static void bars() {  // black side bars once
  if (!panelOk || !line) return;
  memset(line, 0, LH * OX * 2);
  p4_panel_draw(0, 0, LH, OX, line);
  p4_panel_draw(0, LW - OX, LH, LW, line);
}

void displayBegin() {
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  canvas.createSprite(UW, UH);
  canvas.fillScreen(0);
  line = (uint16_t *)heap_caps_malloc(LH * 64 * 2 + 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  Preferences p;
  p.begin("votol-disp", true);
  flip = p.getBool("flip", false);
  p.end();
  panelOk = p4_panel_init() == 0;
  bars();
  ledcAttach(6, 10000, 10);
  ledcWrite(6, 800);
}
void displayBacklight(uint8_t level) {
  static const uint16_t L[6] = {0, 120, 300, 520, 800, 1023};
  ledcWrite(6, L[level > 5 ? 5 : level]);
}
bool displayTouch(int &x, int &y) {
  int tx, ty;
  if (!p4_touch_read(&tx, &ty)) return false;
  int X, Y;  // portrait -> landscape
  if (!flip) { X = ty; Y = LH - 1 - tx; }
  else { X = LW - 1 - ty; Y = tx; }
  X = ((X - OX) * 2) / 3;
  Y = (Y * 2) / 3;
  if (X < 0 || X >= UW || Y < 0 || Y >= UH) return false;
  x = X; y = Y;
  return true;
}
// no calibration on capacitive touch: the Setup button rotates the picture 180 degrees
void displayCalibrate(bool force) {
  if (!force) return;
  flip = !flip;
  Preferences p;
  p.begin("votol-disp", false);
  p.putBool("flip", flip);
  p.end();
  bars();
  mark(0, 0, UW, UH);
  gfx.font = -1;
}
void displayFlush(bool full) {
  if (full) mark(0, 0, UW, UH);
  if (dx1 <= dx0 || dy1 <= dy0) return;
  push(dx0, dy0, dx1, dy1);
  dx0 = UW; dy0 = UH; dx1 = dy1 = 0;
}
#endif
