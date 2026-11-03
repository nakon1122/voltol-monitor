// VOTOL UART link: same request sequence as the official PC tool.
//  - LDGET (parameter read) is repeated every 500 ms, up to 9 times
//  - SHOW is then polled back-to-back (next request as soon as the answer
//    arrives, 150 ms timeout) with the check values from the parameter read
//  - writes: P1..P7 100 ms apart, wait for "PN", read back and compare
// Baud rate is auto-detected (9600 default, 115200, 256000).
#include <Arduino.h>
#include "config.h"
#include "link.h"

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
        if (onLive) onLive(d, lastLive);
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
  U.setRxBufferSize(1024);  // must be set before begin()
  U.begin(BAUDS[baudIx], SERIAL_8N1, VOTOL_RX_PIN, VOTOL_TX_PIN);
  uint32_t silentSince = millis();
  for (;;) {
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
    if (millis() - hzAt >= 2000) {
      hzVal = hzCount * 1000.0f / (millis() - hzAt);
      hzCount = 0;
      hzAt = millis();
    }
    // nothing valid for 3 s: try the next baud rate, then read params again
    if (millis() - lastAny < 3000) silentSince = millis();
    else if (millis() - silentSince > 3000) {
      baudIx = (baudIx + 1) % 3;
      U.updateBaudRate(BAUDS[baudIx]);
      silentSince = millis();
      if (!haveParams) wantRead = true;
    }
  }
}

void begin(LiveFn live, ParamsFn params, WriteFn write) {
  onLive = live; onParams = params; onWrite = write;
  xTaskCreatePinnedToCore(task, "votol", 6144, nullptr, 3, nullptr, 0);
}
void requestRead() { wantRead = true; }
void requestWrite(const uint8_t *im) { memcpy(writeImg, im, PARAM_BYTES); wantWrite = true; }
void requestClearFaults() { if (lastRpm == 0) wantClear = true; }
bool busy() { return isBusy || wantRead || wantWrite; }
uint32_t baud() { return BAUDS[baudIx]; }
float hz() { return hzVal; }
uint32_t goodFrames() { return fr.good; }
uint32_t badFrames() { return fr.bad; }
bool alive() { return millis() - lastLive < 1000; }
}  // namespace vlink
