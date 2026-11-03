// ESP32-S3 build without a screen: the web page is the user interface, so the
// display and on-screen UI become no-ops (src/display.cpp, src/ui.cpp and the
// fonts are not compiled in this build - see platformio.ini).
#ifdef VOLTOL_HEADLESS
#include "display.h"
#include "ui.h"

struct NullGfx : Gfx {
  void fillRect(int, int, int, int, uint32_t) override {}
  void fillRoundRect(int, int, int, int, int, uint32_t) override {}
  void drawRoundRect(int, int, int, int, int, uint32_t) override {}
  void fillCircle(int, int, int, uint32_t) override {}
  void drawLine(int, int, int, int, uint32_t) override {}
  void fillTriangle(int, int, int, int, int, int, uint32_t) override {}
  void fillArc(int, int, int, int, float, float, uint32_t) override {}
  void textRaw(int, int, const char *, int, uint32_t) override {}
};
static NullGfx nullGfx;
void displayBegin() {}
Gfx &displayGfx() { return nullGfx; }
void displayBacklight(uint8_t) {}
bool displayTouch(int &, int &) { return false; }
void displayCalibrate(bool) {}

namespace ui {
Page page = PG_DASH;
bool menuOpen = false;
int editGroup = 0;
void draw(Gfx &, const UiState &, const Settings &) {}
void update(Gfx &, const UiState &, const Settings &) {}
bool touch(int, int, UiState &, Settings &, UiActions &) { return false; }
bool tick(uint32_t) { return false; }
void go(Page p) { page = p; }
}  // namespace ui
#endif
