// LovyanGFX driver for the ESP32-3248S035 + the Gfx back-end used by ui.cpp.
// Panel/touch settings are the ones proven on the BattLab CYD35 firmware.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include "display.h"
#include "vlw_metrics.h"

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Touch_XPT2046 touch_;

public:
  LGFX() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = HSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 27000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.readable = true;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      panel_.config(cfg);
    }
    {
      auto cfg = light_.config();
      cfg.pin_bl = 27;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    {
      auto cfg = touch_.config();
      cfg.x_min = 300;
      cfg.x_max = 3900;
      cfg.y_min = 200;
      cfg.y_max = 3900;
      cfg.pin_int = 36;
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;
      cfg.spi_host = HSPI_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_cs = 33;
      touch_.config(cfg);
      panel_.setTouch(&touch_);
    }
    setPanel(&panel_);
  }
};

static LGFX tft;

static uint16_t c565(uint32_t c) { return lgfx::color565((c >> 16) & 255, (c >> 8) & 255, c & 255); }

// Draws either straight to the panel or into an off-screen strip.
class GfxLgfx : public Gfx {
public:
  lgfx::LovyanGFX *t = &tft;
  int ox = 0, oy = 0;
  int font = -1;
  void fillRect(int x, int y, int w, int h, uint32_t c) override { t->fillRect(x - ox, y - oy, w, h, c565(c)); }
  void fillRoundRect(int x, int y, int w, int h, int r, uint32_t c) override { t->fillRoundRect(x - ox, y - oy, w, h, r, c565(c)); }
  void drawRoundRect(int x, int y, int w, int h, int r, uint32_t c) override { t->drawRoundRect(x - ox, y - oy, w, h, r, c565(c)); }
  void fillCircle(int x, int y, int r, uint32_t c) override { t->fillCircle(x - ox, y - oy, r, c565(c)); }
  void drawLine(int x0, int y0, int x1, int y1, uint32_t c) override { t->drawLine(x0 - ox, y0 - oy, x1 - ox, y1 - oy, c565(c)); }
  void fillTriangle(int a, int b, int c2, int d, int e, int f, uint32_t c) override {
    t->fillTriangle(a - ox, b - oy, c2 - ox, d - oy, e - ox, f - oy, c565(c));
  }
  void fillArc(int x, int y, int r0, int r1, float a0, float a1, uint32_t c) override {
    t->fillArc(x - ox, y - oy, r1, r0, a0, a1, c565(c));
  }
  void textRaw(int x, int y, const char *s, int f, uint32_t c) override {
    if (f != font) { t->loadFont(FONT_VLW[f]); font = f; }
    t->setTextColor(c565(c));
    t->setTextDatum(lgfx::baseline_left);
    t->drawString(s, x - ox, y - oy);
  }
  void boxed(int x, int y, int w, int h, uint32_t bg, const std::function<void()> &fn) override {
    if (t != &tft) { Gfx::boxed(x, y, w, h, bg, fn); return; }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 480) w = 480 - x;
    if (y + h > 320) h = 320 - y;
    if (w <= 0 || h <= 0) return;
    int rows = 30000 / (w * 2);
    if (rows < 1) rows = 1;
    if (rows > h) rows = h;
    lgfx::LGFX_Sprite spr(&tft);
    spr.setColorDepth(16);
    if (!spr.createSprite(w, rows)) {  // out of memory: draw directly
      Gfx::boxed(x, y, w, h, bg, fn);
      return;
    }
    for (int sy = 0; sy < h; sy += rows) {
      int sh = rows < h - sy ? rows : h - sy;
      spr.fillScreen(c565(bg));
      t = &spr; ox = x; oy = y + sy; font = -1;
      fn();
      t = &tft; ox = oy = 0;
      tft.setClipRect(x, y + sy, w, sh);  // last strip may be shorter
      spr.pushSprite(x, y + sy);
      tft.clearClipRect();
    }
    spr.deleteSprite();
    font = -1;
  }
};
static GfxLgfx gfx;

Gfx &displayGfx() { return gfx; }

void displayBegin() {
  tft.init();
  tft.setRotation(1);
  tft.setBrightness(200);
  tft.fillScreen(0);
}
void displayBacklight(uint8_t level) {
  static const uint8_t L[6] = {0, 40, 90, 140, 200, 255};
  tft.setBrightness(L[level > 5 ? 5 : level]);
}
bool displayTouch(int &x, int &y) {
  int32_t tx, ty;
  if (!tft.getTouch(&tx, &ty)) return false;
  x = tx; y = ty;
  return true;
}
void displayCalibrate(bool force) {
  Preferences p;
  p.begin("voltol-disp", false);
  uint16_t cal[8];
  bool have = p.getBytes("cal", cal, sizeof cal) == sizeof cal;
  if (have && !force) {
    tft.setTouchCalibrate(cal);
  } else {
    tft.fillScreen(0);
    tft.loadFont(FONT_VLW[F15B]);
    tft.setTextColor(0xFFFF);
    tft.setTextDatum(lgfx::middle_center);
    tft.drawString("Touch calibration", 240, 140);
    tft.loadFont(FONT_VLW[F13]);
    tft.drawString("Tap the arrow in each corner", 240, 172);
    tft.calibrateTouch(cal, 0xFE00, 0x0000, 24);
    tft.setTouchCalibrate(cal);
    p.putBytes("cal", cal, sizeof cal);
  }
  p.end();
  gfx.font = -1;
}
