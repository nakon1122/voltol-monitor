// VOTOL UART link: same request sequence as the official PC tool.
//  - LDGET (parameter read) is repeated every 500 ms, up to 9 times
//  - SHOW is then polled back-to-back (next request as soon as the answer
//    arrives, 150 ms timeout) with the check values from the parameter read
//  - writes: P1..P7 100 ms apart, wait for "PN", read back and compare
// Baud rate is auto-detected (9600 default, 115200, 256000).
// Realtime: the UART hands bytes over 1 character after a frame ends (not
// after the default ~10 idle characters), and live frames go through a queue
// to their own task, so this loop never waits for the screen or the web page.
#include <Arduino.h>
#include <driver/uart.h>
#include "config.h"
#include "link.h"
#include "gps.h"
#include "uart_swap.h"
#include <Preferences.h>

namespace vlink {
using namespace votol;

static HardwareSerial &U = Serial2;
static const uint32_t BAUDS[3] = {9600, 115200, 256000};
static int baudIx = 0;
static LiveFn onLive;
static ParamsFn onParams;
static WriteFn onWrite;
static Framer fr;
static ParamImage img;
static bool haveParams = false;
static volatile bool wantRead = true, wantWrite = false, wantClear = false, isBusy = false;
static uint8_t writeImg[PARAM_BYTES];
static uint32_t lastLive = 0, lastAny = 0, hzCount = 0, hzAt = 0;
static float hzVal = 0;
static int16_t lastRpm = 0;
static volatile bool ackPN = false;
static volatile int gpsCfgHz = 0;  // >0: send the u-blox setup on our TX line
static UartSwap sw;                 // automatic TX/RX swap of the VOTOL wires
static volatile bool swappedNow = false, gpsBlocked = false;
static bool uartOpen = false;
static void openUart(bool swap) {
  int rx = swap ? VOTOL_TX_PIN : VOTOL_RX_PIN, tx = swap ? VOTOL_RX_PIN : VOTOL_TX_PIN;
  if (uartOpen) U.end();
  uartOpen = true;
  pinMode(rx, INPUT);  // the old TX pin stops driving before it becomes our RX
  U.setRxBufferSize(1024);  // must be set before begin()
  U.begin(BAUDS[baudIx], SERIAL_8N1, rx, tx);
  uart_set_rx_timeout(UART_NUM_2, 1);  // deliver received bytes 1 character after the line goes quiet
  swappedNow = swap;
}
struct LiveItem { Live d; uint32_t ms; };
static QueueHandle_t liveQ;
static void liveTask(void *) {  // runs onLive (which waits for the shared lock) away from the UART loop
  LiveItem it;
  for (;;) if (xQueueReceive(liveQ, &it, portMAX_DELAY) == pdTRUE && onLive) onLive(it.d, it.ms);
}

// The GPS module's RX shares our TX wire with the controller's RX. The
// controller ignores UBX bytes (wrong header / baud) and the GPS ignores the
// controller frames, so the GPS can be set up through the same pin.
static void sendGpsConfig(int hz) {
  static uint8_t buf[640];
  if (!sw.gpsSetupPossible()) return;     // wires confirmed swapped: our TX does not reach the GPS
  bool back = swappedNow;
  if (back) openUart(false);              // an unconfirmed guess: send it the way the guide wires it
  const uint32_t tryBauds[3] = {9600, 38400, 115200};
  for (uint32_t b : tryBauds) {
    U.updateBaudRate(b);
    int n = gps::ubxBaudCmd(buf);
    U.write(buf, n); U.flush();
    delay(60);
  }
  U.updateBaudRate(115200);
  delay(150);
  int n = gps::ubxSetupCmd(buf, hz);
  for (int i = 0; i < 2; i++) { U.write(buf, n); U.flush(); delay(80); }
  U.updateBaudRate(BAUDS[baudIx]);
  if (back) openUart(true);
  while (U.available()) U.read();
}

static void sendFrame(const uint8_t *f) { U.write(f, FRAME_LEN); U.flush(); }

// Read bytes for up to `ms`; returns the kind of the last frame handled when
// stopAt matched, else K_NONE.
static Kind pump(uint32_t ms, Kind stopAt) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    while (U.available()) {
      if (!fr.push((uint8_t)U.read())) continue;
      lastAny = millis();
      Kind k = kindOf(fr.buf);
      if (k == K_LIVE) {
        Live d;
        parseLive(fr.buf, d);
        lastRpm = d.rpm;
        lastLive = millis();
        hzCount++;
        LiveItem it{d, lastLive};
        if (xQueueSend(liveQ, &it, 0) != pdTRUE) {  // consumer behind: keep the newest frame
          LiveItem old;
          xQueueReceive(liveQ, &old, 0);
          xQueueSend(liveQ, &it, 0);
        }
      } else if (k == K_PARAM) {
        img.storePage(fr.buf);
      } else if (k == K_WRITE_ACK) {
        ackPN = true;
      }
      if (k == stopAt || (stopAt == K_PARAM && img.complete())) return k;
    }
    vTaskDelay(1);
  }
  return K_NONE;
}

static bool readParams() {
  uint8_t f[FRAME_LEN];
  buildRead(f);
  img.havePages = 0;
  for (int i = 0; i < 9 && !img.complete(); i++) {
    sendFrame(f);
    uint32_t t0 = millis();
    while (millis() - t0 < 500 && !img.complete()) pump(500 - (millis() - t0), K_PARAM);
  }
  return img.complete();
}

static void doWrite() {
  if (!haveParams) { onWrite(false, "Read the parameters first"); return; }
  if (millis() - lastLive > 1000) { onWrite(false, "No live data from controller"); return; }
  if (lastRpm != 0) { onWrite(false, "Motor turning: stop the bike (0 RPM)"); return; }
  uint8_t f[FRAME_LEN];
  ackPN = false;
  for (int p = 1; p <= PAGES; p++) {
    buildWrite(f, p, writeImg);
    sendFrame(f);
    pump(100, K_WRITE_ACK);
  }
  uint32_t t0 = millis();
  while (!ackPN && millis() - t0 < 2000) pump(50, K_WRITE_ACK);
  bool acked = ackPN;
  // read back and compare every byte
  if (!readParams()) { onWrite(false, acked ? "Written, but read-back failed" : "No answer from controller"); return; }
  haveParams = true;
  bool same = memcmp(img.b, writeImg, PARAM_BYTES) == 0;
  onParams(img, true);
  if (same) onWrite(true, "Written and verified OK");
  else onWrite(false, acked ? "Read-back differs: not all values accepted" : "Controller did not accept the write");
}

static void task(void *) {
  uint8_t f[FRAME_LEN];
  {
    Preferences p;
    p.begin("votol", true);
    bool saved = p.getUChar("uswap", 0) == 1;
    p.end();
    sw.begin(saved, millis());
  }
  openUart(sw.swapped);
  uint32_t silentSince = millis(), seenGood = fr.good;
  for (;;) {
    if (gpsCfgHz) { int hz = gpsCfgHz; gpsCfgHz = 0; sendGpsConfig(hz); }
    if (wantRead) {
      wantRead = false;
      isBusy = true;
      bool ok = readParams();
      if (ok) haveParams = true;
      if (onParams) onParams(img, ok);
      isBusy = false;
    }
    if (wantWrite) {
      wantWrite = false;
      isBusy = true;
      doWrite();
      isBusy = false;
    }
    if (wantClear) {
      wantClear = false;
      buildClearFaults(f);
      sendFrame(f);
      pump(100, K_NONE);
    }
    // live poll
    buildShow(f, haveParams ? img.chkA() : 0, haveParams ? img.chkB() : 0, haveParams ? img.chk3() : 0);
    sendFrame(f);
    pump(150, K_LIVE);
    {  // wrong way round? try the pins the other way
      bool got = fr.good != seenGood;
      seenGood = fr.good;
      UartSwap::Act act = sw.update(millis(), got);
      if (act == UartSwap::SWITCH) openUart(sw.swapped);
      if (act == UartSwap::SAVE) {
        Preferences p;
        p.begin("votol", false);
        if (p.getUChar("uswap", 0) != (sw.swapped ? 1 : 0)) p.putUChar("uswap", sw.swapped ? 1 : 0);
        p.end();
      }
      gpsBlocked = !sw.gpsSetupPossible();
    }
    if (millis() - hzAt >= 2000) {
      hzVal = hzCount * 1000.0f / (millis() - hzAt);
      hzCount = 0;
      hzAt = millis();
    }
    // VOTOL EM talks 9600 baud only (v1.8.2: no more baud hopping, which made
    // the screen redraw every few seconds while the controller was off).
    // Nothing valid for 6 s: just ask for the parameters again.
    if (millis() - lastAny < 3000) silentSince = millis();
    else if (millis() - silentSince > 6000) {
      silentSince = millis();
      if (!haveParams) wantRead = true;
    }
  }
}

void begin(LiveFn live, ParamsFn params, WriteFn write) {
  onLive = live; onParams = params; onWrite = write;
  liveQ = xQueueCreate(8, sizeof(LiveItem));
  xTaskCreatePinnedToCore(liveTask, "votolLive", 6144, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(task, "votol", 6144, nullptr, 3, nullptr, 0);
}
void requestRead() { wantRead = true; }
void requestWrite(const uint8_t *im) { memcpy(writeImg, im, PARAM_BYTES); wantWrite = true; }
void requestGpsConfig(int hz) { gpsCfgHz = hz; }
void requestClearFaults() { if (lastRpm == 0) wantClear = true; }
bool busy() { return isBusy || wantRead || wantWrite; }
uint32_t baud() { return BAUDS[baudIx]; }
float hz() { return hzVal; }
uint32_t goodFrames() { return fr.good; }
uint32_t badFrames() { return fr.bad; }
bool alive() { return millis() - lastLive < 1000; }
bool swapped() { return swappedNow && sw.confirmed; }
bool gpsSetupBlocked() { return gpsBlocked; }
}  // namespace vlink
