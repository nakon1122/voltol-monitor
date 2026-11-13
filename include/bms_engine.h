// One smart BMS over Bluetooth, without the radio: bytes from the BMS go into
// feed(), frames to send come out of next(). Works out the brand by itself
// (JK, ANT, JBD/Xiaoxiang, Daly), polls the live values, reads the settings
// and writes them one by one, checking each by reading it back.
// Same protocol handling as the BattLab firmware (src/main.cpp bleWorker).
// Pure C++: the PC test (test/bms_test.cpp) drives it with recorded frames.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bms.h"
#include "bms_daly.h"
#include "bms_other.h"
#include "bms_params.h"
#include "bms_settings.h"
#include "jk_protocol.h"

namespace bmse {
enum Ch : uint8_t { CH_MAIN, CH_JBD, CH_DALY };  // FFE1 (JK/ANT), FF02 (JBD), FFF2 (Daly)
enum Resp : uint8_t { R_AUTO, R_WITH, R_WITHOUT };
struct Out { uint8_t ch = CH_MAIN, resp = R_AUTO, n = 0; uint8_t d[24]; };

class Engine {
public:
  // ---- results
  uint8_t brand = bms::BR_NONE;
  jk::Sample s;              // last live sample
  bool have = false;
  uint32_t sampleAt = 0, samples = 0;
  float pv[bms::MAX_PARAMS] = {0};
  uint64_t pvHave = 0;       // bit per setting read
  bool settingsDone = false; // a full settings read finished
  uint32_t settingsSeq = 0;  // +1 every time the settings changed
  // write job: result once finished
  bool writing = false, writeDone = false, writeOk = false;
  char writeMsg[96] = "";
  int writeN = 0, writeGood = 0;

  void reset(bool mainCh, bool jbdCh, bool dalyCh, bool peerAnt, uint8_t forced) {
    *this = Engine();
    hasMain = mainCh; hasJbd = jbdCh; hasDaly = dalyCh; antName = peerAnt;
    brand = forced;
    if (!brand && hasJbd) brand = bms::BR_JBD;  // service FF00 only exists on JBD
  }
  int nsw() const { return brand == bms::BR_JK || brand == bms::BR_DALY ? 3 : brand ? 2 : 0; }

  // ---------------------------------------------------------------- input
  void feed(const uint8_t *p, size_t n, uint32_t now) {
    jkp.feed(p, n, [&](const uint8_t *b) { onJk(b, now); });
    antF.feed(p, n, [&](const uint8_t *f, size_t k) { onAnt(f, k, now); });
    jbdF.feed(p, n, [&](uint8_t cmd, const uint8_t *d, size_t k) { onJbd(cmd, d, k, now); });
    d2.feed(p, n, [&](uint8_t st, uint8_t fn, const uint8_t *d, size_t k) {
      if (st != 0xD2 || fn != 3) return;
      jk::Sample x; uint8_t bal;
      if (daly::decodeBle(d, k, x, bal) && pick(bms::BR_DALY)) { dalySub = 1; balOn = bal; publish(x, now); }
    });
    a5.feed(p, n, [&](uint8_t id, const uint8_t *d) {
      if (a5s.add(id, d)) { if (pick(bms::BR_DALY)) { dalySub = 2; publish(a5s.s, now); } }
      else if (!brand && id == 0x90) dalySub = 2;
    });
  }

  // ---------------------------------------------------------------- requests
  void readSettings() { wantRead = true; }
  // queue a write of unified setting i (switches first, see bms::param)
  bool queueWrite(int i, float v) {
    if (wq >= MAXW || i < 0 || i >= bms::paramCount(brand)) return false;
    wi[wq] = i; wv[wq] = v; wq++;
    return true;
  }
  void startWrites(uint32_t now) {
    if (!wq) return;
    writing = true; writeDone = false; writeOk = false; writeN = wq; writeGood = 0; wpos = 0; wState = 0; wAt = now;
    writeMsg[0] = 0;
  }

  // ---------------------------------------------------------------- output
  // Next frame to send (call often). Returns false when nothing is due.
  bool next(uint32_t now, Out &o) {
    o = Out();
    if (writing) return writeStep(now, o);
    if (!brand) return ident(now, o);
    switch (brand) {
    case bms::BR_JK: return jkPoll(now, o);
    case bms::BR_ANT: return antPoll(now, o);
    case bms::BR_JBD: return jbdPoll(now, o);
    default: return dalyPoll(now, o);
    }
  }
  bool is32() const { return jkLayout == 32; }

private:
  bool hasMain = false, hasJbd = false, hasDaly = false, antName = false;
  jk::Parser jkp;
  jk::AutoDetector det;
  int jkLayout = 0;
  bms::AntFramer antF;
  bms::JbdFramer jbdF;
  daly::D2Framer d2;
  daly::A5Framer a5;
  daly::A5State a5s;
  uint8_t dalySub = 0, dalyIdx = 0, balOn = 0;
  jk::Sample jbdS;
  bool jbdBasic = false;
  uint32_t identAt = 0, identStart = 0, pollAt = 0, infoAt = 0, cellReqAt = 0, cellAt = 0;
  uint8_t identIdx = 0, jbdTog = 0;
  bool cellReqOn = false;
  bool jkInfo = false, wantRead = false, identOn = false, infoSent = false;
  int prStep = -1;        // ANT / JBD settings read position
  uint32_t prAt = 0;
  // writes
  static const int MAXW = 64;
  int wi[MAXW], wq = 0, wpos = 0, wState = 0;
  float wv[MAXW];
  uint32_t wAt = 0;
  uint32_t seqBefore = 0;

  bool pick(uint8_t b) { if (!brand) brand = b; return brand == b; }
  void publish(const jk::Sample &x, uint32_t now) {
    s = x; have = true; sampleAt = now; samples++;
    int ns = nsw();
    // MOS switches come with every sample: keep the switch settings in step
    if (ns >= 2) { setPv(0, x.charge ? 1 : 0); setPv(1, x.discharge ? 1 : 0); }
    if (brand == bms::BR_DALY && ns >= 3) setPv(2, balOn ? 1 : 0);
  }
  void setPv(int i, float v) {
    if (i < 0 || i >= bms::MAX_PARAMS) return;
    if (!(pvHave >> i & 1) || fabsf(pv[i] - v) > 1e-4f) settingsSeq++;
    pv[i] = v; pvHave |= uint64_t(1) << i;
  }
  void onJk(const uint8_t *b, uint32_t now) {
    if (b[4] == 2 || b[4] == 3) pick(bms::BR_JK);
    if (brand != bms::BR_JK) return;
    if (b[4] == 3) jkInfo = true;
    else if (b[4] == 2) {
      cellAt = now;
      jk::Sample x;
      int lay = det.accept(b, x);
      if (lay) jkLayout = lay;
      if (jkLayout && jk::decode(b, jkLayout == 32, x)) publish(x, now);
    } else if (b[4] == 1) {
      bmsw::JkSettings js;
      if (bmsw::parseJkSettings(b, js)) {
        for (int i = 0; i < bmsw::SW_COUNT; i++) setPv(i, js.sw[i] ? 1 : 0);
        for (int i = 0; i < bmsw::JK_PARAM_COUNT; i++) setPv(3 + i, js.v[i]);
        settingsDone = true;
        settingsSeq++;  // a full read finished (even when nothing changed)
      }
    }
  }
  void onAnt(const uint8_t *f, size_t n, uint32_t now) {
    if (n >= 10 && f[2] == 0x12) {  // settings register read back
      if (brand != bms::BR_ANT) return;
      uint16_t addr = f[3] | uint16_t(f[4]) << 8;
      uint8_t len = f[5];
      if (len == 2 || len == 4) {
        int32_t raw = len == 4 ? int32_t(f[6] | f[7] << 8 | f[8] << 16 | uint32_t(f[9]) << 24) : int32_t(int16_t(f[6] | f[7] << 8));
        for (int i = 0; i < bset::ANT_N; i++)
          if (bset::ANT[i].reg == addr) setPv(2 + i, bset::decode(bset::ANT[i], raw));
      }
      return;
    }
    jk::Sample x;
    if (bms::decodeAnt(f, n, x) && pick(bms::BR_ANT)) publish(x, now);
  }
  void onJbd(uint8_t cmd, const uint8_t *d, size_t n, uint32_t now) {
    if (brand == bms::BR_JBD && cmd >= 0x10 && cmd <= 0x2F && n == 2) {
      for (int i = 0; i < bset::JBD_N; i++)
        if (bset::JBD[i].reg == cmd) {
          const bset::Def &df = bset::JBD[i];
          int32_t raw = df.sgn ? int32_t(int16_t(d[0] << 8 | d[1])) : int32_t(d[0] << 8 | d[1]);
          setPv(2 + i, bset::decode(df, raw));
        }
      return;
    }
    bool ok = cmd == 3 ? bms::decodeJbdBasic(d, n, jbdS) : cmd == 4 ? bms::decodeJbdCells(d, n, jbdS) : false;
    if (!ok || !pick(bms::BR_JBD)) return;
    if (cmd == 3) jbdBasic = true;
    if (jbdBasic && jbdS.count) {
      float total = 0;
      for (unsigned i = 0; i < jbdS.count; i++) total += jbdS.cells[i];
      if (fabsf(jbdS.voltage - total) <= fmaxf(2.f, total * .05f)) publish(jbdS, now);
    }
  }

  static void put(Out &o, uint8_t ch, const uint8_t *d, int n, uint8_t resp = R_AUTO) {
    o.ch = ch; o.resp = resp; o.n = (uint8_t)(n > 24 ? 24 : n); memcpy(o.d, d, o.n);
  }
  uint8_t jbdCh() const { return hasJbd ? CH_JBD : CH_MAIN; }
  uint8_t dalyCh() const { return hasDaly ? CH_DALY : CH_MAIN; }

  // Quick identification: rotate the requests of every brand (JK first on FFE1).
  bool ident(uint32_t now, Out &o) {
    if (!identOn) { identOn = true; identStart = now; identAt = now; return false; }  // let the module settle 1 s
    if (now - identAt < 1200) return false;
    identAt = now;
    uint8_t idx = identIdx++;
    auto j7 = jk::request(0x97), j6 = jk::request(0x96);
    if (hasMain && !hasDaly && !antName && now - identStart < 60000) {
      // new JK modules answer only one write type: alternate it every two requests
      put(o, CH_MAIN, (idx & 1) ? j6.data() : j7.data(), 20, ((idx / 2) & 1) ? R_WITH : R_WITHOUT);
      return true;
    }
    uint8_t fr[16];
    switch (idx % (hasDaly ? 6 : 4)) {
    case 0: put(o, CH_MAIN, j7.data(), 20); break;
    case 1: put(o, CH_MAIN, bms::ANT_STATUS_REQUEST, 10); break;
    case 2: put(o, jbdCh(), bms::JBD_BASIC_REQUEST, 7); break;
    case 3: put(o, CH_MAIN, j6.data(), 20); break;
    case 4: put(o, CH_DALY, fr, daly::d2Frame(fr, 0xD2, 3, 0, daly::BLE_STATUS_REGS)); break;
    default: put(o, CH_DALY, fr, daly::a5Frame(fr, 0x80, 0x90)); break;
    }
    if (dalySub == 2 && !brand) brand = bms::BR_DALY;
    return true;
  }
  // JK: 0x97 (device info) once, 0x96 starts the stream (settings 0x01, then cells 0x02).
  bool jkPoll(uint32_t now, Out &o) {
    if (!infoSent) { infoSent = true; infoAt = now; auto b = jk::request(0x97); put(o, CH_MAIN, b.data(), 20); return true; }
    bool first = !cellReqOn && (jkInfo || now - infoAt > 1500);
    bool stalled = cellReqOn && now - (cellAt ? cellAt : cellReqAt) > 10000 && now - cellReqAt > 10000;
    bool poll = cellReqOn && now - cellReqAt > 2000 && (!cellAt || now - cellAt > 2000);
    if (first || stalled || poll || wantRead) {
      wantRead = false; cellReqAt = now; cellReqOn = true;
      auto b = jk::request(0x96); put(o, CH_MAIN, b.data(), 20);
      return true;
    }
    return false;
  }
  bool antPoll(uint32_t now, Out &o) {
    if (wantRead || (!settingsDone && prStep < 0 && samples > 0)) { wantRead = false; prStep = 0; }
    if (prStep >= 0) return antReadStep(now, o);
    if (now - pollAt < 1000) return false;
    pollAt = now;
    put(o, CH_MAIN, bms::ANT_STATUS_REQUEST, 10);
    return true;
  }
  bool antReadStep(uint32_t now, Out &o) {
    if (now - prAt < 150) return false;
    prAt = now; pollAt = now;
    uint8_t fr[20];
    put(o, CH_MAIN, fr, bset::antRead(fr, bset::ANT[prStep]));
    if (++prStep >= bset::ANT_N) { prStep = -1; settingsDone = true; settingsSeq++; }
    return true;
  }
  bool jbdPoll(uint32_t now, Out &o) {
    if (wantRead || (!settingsDone && prStep < 0 && samples > 0)) { wantRead = false; prStep = 0; }
    if (prStep >= 0) return jbdReadStep(now, o);
    if (now - pollAt < 500) return false;
    pollAt = now;
    jbdTog ^= 1;
    put(o, jbdCh(), (jbdTog || !jbdBasic) ? bms::JBD_BASIC_REQUEST : bms::JBD_CELLS_REQUEST, 7);
    return true;
  }
  bool jbdReadStep(uint32_t now, Out &o) {
    if (now - prAt < 150) return false;
    prAt = now; pollAt = now;
    uint8_t fr[20];
    int k;
    if (prStep == 0) k = bset::jbdFactory(fr);
    else if (prStep <= bset::JBD_N) k = bset::jbdRead(fr, (uint8_t)bset::JBD[prStep - 1].reg);
    else k = bset::jbdExit(fr, false);
    put(o, jbdCh(), fr, k);
    if (++prStep > bset::JBD_N + 1) { prStep = -1; settingsDone = true; settingsSeq++; }
    return true;
  }
  bool dalyPoll(uint32_t now, Out &o) {
    if (now - pollAt < (dalySub == 2 ? 250u : 500u)) return false;
    pollAt = now;
    uint8_t fr[16];
    if (dalySub == 2) put(o, dalyCh(), fr, daly::a5Frame(fr, 0x80, daly::A5_POLL[dalyIdx++ % 5]));
    else put(o, dalyCh(), fr, daly::d2Frame(fr, 0xD2, 3, 0, daly::BLE_STATUS_REGS));
    if (!settingsDone) settingsDone = true;  // Daly: switches only, they come with the live data
    return true;
  }

  // ---- writes: one setting at a time, then read it back
  // wState: 0 send (may take several frames), 1 wait, then ask for the value again, 2 check
  int sub = 0;
  bool writeStep(uint32_t now, Out &o) {
    if (wpos >= wq) {
      writing = false; writeDone = true; writeOk = writeGood == writeN;
      if (!writeMsg[0]) snprintf(writeMsg, sizeof writeMsg, writeOk ? "Written and verified OK" : "%d of %d values verified", writeGood, writeN);
      wq = 0;
      return false;
    }
    int i = wi[wpos];
    float v = wv[wpos];
    bms::ParamDef d = bms::param(brand, i);
    bool sw = bms::isSwitch(d);
    int ns = nsw();
    uint8_t fr[24];
    if (wState == 0) {  // send
      if (now - wAt < 150) return false;
      wAt = now;
      if (brand == bms::BR_JK) {
        std::array<uint8_t, 20> f = sw ? bmsw::jkWriteFrame(bmsw::JK_SW_REG[i], v > 0.5f ? 1 : 0)
                                       : bmsw::jkWriteFrame(is32() ? bmsw::JK_PARAMS[i - ns].reg32 : bmsw::JK_PARAMS[i - ns].reg24, bmsw::jkRaw(i - ns, v));
        put(o, CH_MAIN, f.data(), 20);
        wState = 1;
      } else if (brand == bms::BR_ANT) {
        if (sub == 0) { auto a = bmsw::antAuthFrame(); put(o, CH_MAIN, a.data(), (int)a.size()); sub = 1; return true; }
        if (sw) { auto f = bmsw::antWriteFrame(v > 0.5f ? bmsw::ANT_SW_ON[i] : bmsw::ANT_SW_OFF[i], 0); put(o, CH_MAIN, f.data(), (int)f.size()); }
        else put(o, CH_MAIN, fr, bset::antWrite(fr, bset::ANT[i - ns], bset::encode(bset::ANT[i - ns], v)));
        sub = 0; wState = 1;
      } else if (brand == bms::BR_JBD) {
        if (sw) {
          bool chg = i == 0 ? v > 0.5f : pv[0] > 0.5f, dsg = i == 1 ? v > 0.5f : pv[1] > 0.5f;
          auto f = bmsw::jbdMosFrame(chg, dsg);
          put(o, jbdCh(), f.data(), (int)f.size());
          wState = 1;
        } else {  // factory mode -> write -> save + exit
          const bset::Def &df = bset::JBD[i - ns];
          if (sub == 0) put(o, jbdCh(), fr, bset::jbdFactory(fr));
          else if (sub == 1) put(o, jbdCh(), fr, bset::jbdWrite(fr, (uint8_t)df.reg, bset::encode(df, v)));
          else put(o, jbdCh(), fr, bset::jbdExit(fr, true));
          if (++sub > 2) { sub = 0; wState = 1; }
          wAt = now + 150;  // 300 ms between the three steps
        }
      } else {  // Daly switches
        int k = 0;
        if (dalySub == 2) { if (i < 2) k = daly::a5Frame(fr, 0x80, i == 0 ? 0xDA : 0xD9, v > 0.5f ? 1 : 0); }
        else k = daly::d2Frame(fr, 0xD2, 6, daly::BLE_MOS_REG[i], v > 0.5f ? 1 : 0);
        if (!k) { failOne("not on this Daly"); return false; }
        put(o, dalyCh(), fr, k);
        wState = 1;
      }
      seqBefore = settingsSeq;
      return true;
    }
    if (wState == 1) {  // ask for the value again
      if (now - wAt < 600) return false;
      wAt = now;
      wState = 2;
      if (brand == bms::BR_JK) { auto b = jk::request(0x96); put(o, CH_MAIN, b.data(), 20); return true; }
      if (brand == bms::BR_ANT && !sw) { put(o, CH_MAIN, fr, bset::antRead(fr, bset::ANT[i - ns])); return true; }
      if (brand == bms::BR_JBD && !sw) {  // read just this register (factory, read, exit)
        const bset::Def &df = bset::JBD[i - ns];
        put(o, jbdCh(), fr, sub == 0 ? bset::jbdFactory(fr) : sub == 1 ? bset::jbdRead(fr, (uint8_t)df.reg) : bset::jbdExit(fr, false));
        if (++sub <= 2) { wState = 1; wAt = now - 450; } else sub = 0;
        return true;
      }
      // switches of ANT / JBD / Daly: the next live sample shows them
      if (brand == bms::BR_JBD) { put(o, jbdCh(), bms::JBD_BASIC_REQUEST, 7); return true; }
      if (brand == bms::BR_ANT) { put(o, CH_MAIN, bms::ANT_STATUS_REQUEST, 10); return true; }
      return dalyPoll(now + 1000, o);
    }
    // wState 2: check what the BMS reports now
    bool same = (pvHave >> i & 1) && fabsf(pv[i] - v) <= d.step / 2 + 1e-4f;
    if (same) { writeGood++; nextWrite(now); return false; }
    if (now - wAt > 6000) { failOne("BMS kept the old value"); return false; }
    // keep the live data coming while waiting
    if (brand != bms::BR_JK && now - pollAt > 700) {
      pollAt = now;
      if (brand == bms::BR_JBD) { put(o, jbdCh(), bms::JBD_BASIC_REQUEST, 7); return true; }
      if (brand == bms::BR_ANT) {
        if (sw) put(o, CH_MAIN, bms::ANT_STATUS_REQUEST, 10);
        else put(o, CH_MAIN, fr, bset::antRead(fr, bset::ANT[i - ns]));
        return true;
      }
      return dalyPoll(now + 1000, o);
    }
    return false;
  }
  void nextWrite(uint32_t now) { wpos++; wState = 0; sub = 0; wAt = now; }
  void failOne(const char *why) {
    bms::ParamDef d = bms::param(brand, wi[wpos]);
    snprintf(writeMsg, sizeof writeMsg, "%s: %s", d.name, why);
    nextWrite(wAt);
  }
};
}  // namespace bmse
