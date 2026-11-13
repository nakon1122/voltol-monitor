// ============================================================ BMS =========
// Smart BMS over Bluetooth: overview, cell voltages, settings, write check,
// finding / choosing the BMS. Included by ui.cpp (shares its helpers).
using bms::Link;
static int bmsSel = 0, bmsTop = 0, bmsStep = 1;
static uint32_t bmsDrawAt = 0;
static const int BROWS = 6;

static void icBatt(Gfx &g, int cx, int cy, uint32_t c, uint32_t bg, float f = 0.6f) {
  g.fillRoundRect(cx - 11, cy - 7, 20, 14, 3, c);
  g.fillRoundRect(cx - 9, cy - 5, 16, 10, 2, bg);
  g.fillRect(cx + 9, cy - 3, 3, 6, c);
  int w = (int)(14 * f);
  if (w > 0) g.fillRect(cx - 8, cy - 4, w, 8, c);
}
static const char *bmsLinkText(const BmsUi &b, uint32_t &c) {
  switch (b.link) {
  case bms::L_ONLINE: c = GREEN; return "ONLINE";
  case bms::L_SCAN: c = BLUE; return "SEARCHING";
  case bms::L_CONNECT: c = YELLOW; return "CONNECTING";
  case bms::L_LOST: c = RED; return "NO SIGNAL";
  case bms::L_NOBLE: c = MUTED2; return "NO BLUETOOTH";
  default: c = MUTED2; return "NOT SET";
  }
}
static uint32_t socColor(float soc) { return soc < 20 ? RED : soc < 40 ? YELLOW : GREEN; }
static uint32_t diffColor(float mv) { return mv < 20 ? GREEN : mv < 50 ? YELLOW : RED; }
static void bmsHeader(Gfx &g, const UiState &s, const char *title) {
  uint32_t c;
  const char *t = bmsLinkText(s.bms, c);
  header(g, title, t, c);
}
static void tile(Gfx &g, int x, int y, int w, const char *label, const char *val, const char *unit, uint32_t vc = TEXT) {
  g.fillRoundRect(x, y, w, 54, 8, CARD);
  g.text(x + 10, y + 17, label, F10, MUTED);
  g.text(x + 10, y + 45, val, F24B, vc);
  if (unit && *unit) g.text(x + 14 + g.textWidth(val, F24B), y + 45, unit, F11, MUTED2);
}

// ---- overview
static const Btn BM[3] = {{10, 282, 148, 30}, {166, 282, 148, 30}, {322, 282, 148, 30}};
static void bmsEmpty(Gfx &g, const UiState &s) {
  const BmsUi &b = s.bms;
  g.fillCircle(240, 112, 52, mix(BG, BLUE, 0.10f));
  g.fillRoundRect(206, 92, 62, 40, 8, b.link == bms::L_NOBLE ? MUTED2 : BLUE);
  g.fillRoundRect(211, 97, 52, 30, 5, mix(BG, BLUE, 0.10f));
  g.fillRect(268, 104, 7, 16, b.link == bms::L_NOBLE ? MUTED2 : BLUE);
  if (b.link == bms::L_SCAN) { float a = (nowMs % 1200) * 0.3f; g.fillArc(240, 112, 52, 57, a, a + 80, BLUE); }
  const char *h = b.link == bms::L_NOBLE ? "This screen has no Bluetooth" : b.link == bms::L_SCAN ? "Looking for BMS nearby..." : "No BMS connected yet";
  const char *l = b.link == bms::L_NOBLE ? "BMS data is not available on this board" : "Switch the BMS on and close its phone app";
  g.text(240, 196, h, F15B, TEXT, AL_C);
  g.text(240, 220, l, F11, MUTED2, AL_C);
  if (b.link != bms::L_NOBLE) g.text(240, 242, "Then press FIND BMS - the screen does the rest", F11, MUTED2, AL_C);
}
static void bmsBody(Gfx &g, const UiState &s) {
  const BmsUi &b = s.bms;
  const bms::Live &L = b.live;
  bool stale = b.link != bms::L_ONLINE;
  char t[96], u[96];
  // SOC ring
  g.fillRoundRect(10, 40, 196, 236, 10, CARD);
  const int cx = 108, cy = 128;
  uint32_t sc = stale ? MUTED2 : socColor(L.soc);
  g.fillArc(cx, cy, 62, 74, 135, 405, TRACK);
  float f = L.soc / 100; if (f < 0) f = 0; if (f > 1) f = 1;
  if (f > 0) g.fillArc(cx, cy, 62, 74, 135, 135 + 270 * f, sc);
  snprintf(t, sizeof t, "%d", (int)lroundf(L.soc));
  g.text(cx - 6, cy + 14, t, F38B, stale ? MUTED2 : TEXT, AL_C);
  g.text(cx + g.textWidth(t, F38B) / 2 - 2, cy + 14, "%", F13B, MUTED2);
  bool chg = L.amp > 0.3f, dsg = L.amp < -0.3f;
  g.text(cx, cy + 40, chg ? "CHARGING" : dsg ? "DISCHARGING" : "STANDBY", F10, chg ? GREEN : dsg ? YELLOW : MUTED2, AL_C);
  snprintf(t, sizeof t, T("%.1f / %.0f Ah"), L.remainAh, L.fullAh);
  g.text(cx, 232, t, F13B, TEXT, AL_C);
  snprintf(t, sizeof t, T("%u cycles"), (unsigned)L.cycles);
  g.text(cx, 254, t, F10, MUTED, AL_C);
  // tiles
  fmt1(t, 16, L.volt); tile(g, 216, 40, 124, "PACK", t, "V");
  snprintf(t, sizeof t, "%+.1f", L.amp); tile(g, 346, 40, 124, "CURRENT", t, "A", chg ? GREEN : TEXT);
  snprintf(t, sizeof t, "%.2f", fabsf(L.volt * L.amp) / 1000); tile(g, 216, 100, 124, "POWER", t, "kW");
  int lo, hi;
  float mn = L.cellMin(&lo), mx = L.cellMax(&hi), dmv = (mx - mn) * 1000;
  snprintf(t, sizeof t, "%d", (int)lroundf(dmv)); tile(g, 346, 100, 124, "CELL DIFF", t, "mV", stale ? TEXT : diffColor(dmv));
  snprintf(t, sizeof t, "%d", (int)lroundf(L.tempMax())); tile(g, 216, 160, 124, "TEMP MAX", t, "C", L.tempMax() >= 50 ? RED : L.tempMax() >= 40 ? YELLOW : TEXT);
  g.fillRoundRect(346, 160, 124, 54, 8, CARD);
  snprintf(t, sizeof t, T("CELLS %dS"), L.nCells);
  g.text(356, 177, t, F10, MUTED);
  snprintf(t, sizeof t, "%.3f", mx); g.text(356, 196, t, F13B, stale ? TEXT : RED);
  snprintf(t, sizeof t, "%.3f", mn); g.text(356, 212, t, F13B, stale ? TEXT : BLUE);
  g.text(460, 196, "highest", F10, MUTED2, AL_R); g.text(460, 212, "lowest", F10, MUTED2, AL_R);
  // MOS + alarm / link card
  uint32_t al = L.alarms;
  bool bad = al || b.link == bms::L_LOST;
  g.fillRoundRect(216, 220, 254, 56, 8, bad ? DARKRED : CARD);
  if (b.link == bms::L_LOST) {
    snprintf(t, sizeof t, T("Signal lost %d s - retrying"), (int)b.age);
    g.text(228, 240, t, F13B, 0xFF8A80);
  } else if (al) {
    int k = 0; while (k < bms::ALARM_N && !(al >> k & 1)) k++;
    int more = 0; for (int i = k + 1; i < bms::ALARM_N; i++) more += al >> i & 1;
    if (more) snprintf(t, sizeof t, T("%s +%d"), T(bms::ALARM[k]), more); else snprintf(t, sizeof t, "%s", T(bms::ALARM[k]));
    g.fillCircle(232, 235, 7, RED); g.text(232, 239, "!", F11B, 0xFFFFFF, AL_C);
    g.text(246, 240, t, F13B, 0xFF8A80);
  } else {
    struct { const char *n; bool on; } mos[3] = {{"CHG", L.chg}, {"DSG", L.dsg}, {"BAL", L.bal}};
    for (int i = 0; i < 3; i++) {
      int x = 224 + i * 82;
      g.fillRoundRect(x, 228, 78, 20, 5, mos[i].on ? DARKGREEN : CHIP);
      g.fillCircle(x + 10, 238, 4, mos[i].on ? GREEN : DIM);
      g.text(x + 18, 243, mos[i].n, F11B, mos[i].on ? GREEN : MUTED2);
      g.text(x + 73, 243, mos[i].on ? "ON" : "OFF", F10, mos[i].on ? GREEN : MUTED2, AL_R);
    }
  }
  snprintf(u, sizeof u, T("%s · %s · %d dBm"), bms::BRAND[b.brand], b.name, b.rssi);
  g.text(228, 266, u, F10, MUTED);
}
static void bmsButtons(Gfx &g, const UiState &s) {
  bool have = s.bms.have;
  button(g, BM[0], "CELLS", have ? CHIP : CARD2, have ? TEXT : DIM);
  button(g, BM[1], "SETTINGS", have ? CHIP : CARD2, have ? TEXT : DIM);
  button(g, BM[2], s.bms.link == bms::L_OFF ? "FIND BMS" : "BMS DEVICE", s.bms.link == bms::L_OFF ? BLUE : CHIP, s.bms.link == bms::L_OFF ? 0x001018 : TEXT);
}
static void bmsPage(Gfx &g, const UiState &s) {
  bmsHeader(g, s, "BATTERY BMS");
  if (!s.bms.have) bmsEmpty(g, s); else bmsBody(g, s);
  bmsButtons(g, s);
}

// ---- cells
static const Btn BC[2] = {{10, 282, 225, 30}, {245, 282, 225, 30}};
static void cellsBody(Gfx &g, const UiState &s) {
  const bms::Live &L = s.bms.live;
  int n = L.nCells; if (n < 1) n = 1;
  int cols = n <= 16 ? 4 : n <= 20 ? 5 : n <= 24 ? 6 : 8, rows = (n + cols - 1) / cols;
  int w = (460 - (cols - 1) * 6) / cols, h = (200 - (rows - 1) * 6) / rows;
  int lo, hi;
  float mn = L.cellMin(&lo), mx = L.cellMax(&hi), span = mx - mn;
  for (int i = 0; i < L.nCells; i++) {
    int x = 10 + (i % cols) * (w + 6), y = 40 + (i / cols) * (h + 6);
    uint32_t bg = i == lo ? 0x0D2030 : i == hi ? 0x2A1414 : CARD;
    g.fillRoundRect(x, y, w, h, 6, bg);
    char t[16];
    snprintf(t, sizeof t, "%d", i + 1);
    g.text(x + 7, y + 15, t, F10, i == lo ? BLUE : i == hi ? RED : MUTED);
    if (L.balMask >> i & 1) { g.fillCircle(x + w - 10, y + 10, 4, YELLOW); }
    snprintf(t, sizeof t, "%.3f", L.cell[i]);
    g.text(x + w - 7, y + h - 12, t, w < 70 ? F13B : F15B, i == lo ? BLUE : i == hi ? RED : TEXT, AL_R);
    float f = span > 0.0005f ? (L.cell[i] - mn) / span : 0.5f;
    g.fillRect(x + 6, y + h - 6, w - 12, 2, TRACK);
    g.fillRect(x + 6, y + h - 6, (int)((w - 12) * (0.1f + 0.9f * f)), 2, i == lo ? BLUE : i == hi ? RED : mix(DIM, GREEN, f));
  }
  char t[96];
  g.fillRoundRect(10, 246, 460, 30, 8, CARD2);
  snprintf(t, sizeof t, T("MIN %.3f V #%d"), mn, lo + 1); g.text(20, 266, t, F11B, BLUE);
  snprintf(t, sizeof t, T("MAX %.3f V #%d"), mx, hi + 1); g.text(150, 266, t, F11B, RED);
  float sum = 0; for (int i = 0; i < L.nCells; i++) sum += L.cell[i];
  snprintf(t, sizeof t, T("AVG %.3f V"), L.nCells ? sum / L.nCells : 0); g.text(276, 266, t, F11B, SUB);
  if (L.balMask) { g.fillCircle(382, 261, 4, YELLOW); g.text(390, 266, "balancing", F10, YELLOW); }
}
static void cellsPage(Gfx &g, const UiState &s) {
  const bms::Live &L = s.bms.live;
  char t[64];
  snprintf(t, sizeof t, T("%dS · diff %d mV"), L.nCells, (int)lroundf((L.cellMax() - L.cellMin()) * 1000));
  header(g, "CELL VOLTAGES", t, diffColor((L.cellMax() - L.cellMin()) * 1000));
  cellsBody(g, s);
  button(g, BC[0], "BMS OVERVIEW");
  button(g, BC[1], "SETTINGS");
}

// ---- settings
static const Btn BS_READ = {10, 282, 100, 30}, BS_UP = {116, 282, 70, 30}, BS_DN = {192, 282, 70, 30},
                 BS_WRITE = {346, 282, 124, 30}, BS_MINUS = {346, 158, 58, 46}, BS_PLUS = {412, 158, 58, 46},
                 BS_STEP = {346, 212, 58, 30}, BS_UNDO = {412, 212, 58, 30};
static bool changedAt(const BmsUi &b, int i) { return fabsf(b.pe[i] - b.pv[i]) > 1e-4f; }
static void bmsSetPage(Gfx &g, const UiState &s) {
  const BmsUi &b = s.bms;
  int n = bms::paramCount(b.brand);
  char t[128], v[32], o[32];
  if (b.msg[0]) header(g, "BMS SETTINGS", b.msg, b.msgKind == 1 ? GREEN : b.msgKind == 2 ? RED : YELLOW);
  else bmsHeader(g, s, "BMS SETTINGS");
  if (!b.pHave || !n) {
    g.fillRoundRect(10, 40, 460, 236, 10, CARD);
    g.text(240, 140, b.busy ? "Reading settings from the BMS..." : "Settings not read yet", F15B, b.busy ? YELLOW : TEXT, AL_C);
    g.text(240, 166, "Press READ. Values can also be changed on votol.battlab.net", F11, MUTED2, AL_C);
    button(g, BS_READ, b.busy ? "..." : "READ", b.busy ? CARD2 : BLUE, b.busy ? DIM : 0x001018);
    return;
  }
  if (bmsSel >= n) bmsSel = 0;
  if (bmsSel < bmsTop) bmsTop = bmsSel;
  if (bmsSel >= bmsTop + BROWS) bmsTop = bmsSel - BROWS + 1;
  for (int r = 0; r < BROWS && bmsTop + r < n; r++) {
    int i = bmsTop + r, y = 40 + r * 34;
    bms::ParamDef d = bms::param(b.brand, i);
    bool sel = i == bmsSel, ch = changedAt(b, i), bad = ch && bms::check(b.brand, b.pe, i, b.pe[i]);
    g.fillRoundRect(10, y, 326, 30, 6, sel ? 0x13202A : CARD2);
    if (sel) g.drawRoundRect(10, y, 326, 30, 6, bad ? RED : BLUE);
    g.textMid(20, y + 15, d.name, F13, TEXT);
    bms::format(d, b.pe[i], v, sizeof v);
    if (bms::isSwitch(d)) {
      bool on = b.pe[i] > 0.5f;
      g.fillRoundRect(270, y + 6, 58, 18, 9, on ? DARKGREEN : DARKRED);
      g.textMid(299, y + 15, v, F10, on ? GREEN : 0xFF8A80, AL_C);
      if (ch) g.fillCircle(262, y + 15, 3, YELLOW);
    } else {
      snprintf(t, sizeof t, T("%s %s"), v, d.unit);
      g.textMid(326, y + 15, t, F13B, bad ? RED : ch ? YELLOW : TEXT, AL_R);
    }
  }
  // status strip under the list
  bms::ParamDef d = bms::param(b.brand, bmsSel);
  const char *why = changedAt(b, bmsSel) ? bms::check(b.brand, b.pe, bmsSel, b.pe[bmsSel]) : nullptr;
  g.fillRoundRect(10, 246, 326, 30, 6, why ? DARKRED : CARD2);
  if (why) g.textMid(20, 261, why, F11B, 0xFF8A80);
  else if (b.cloudNote[0]) { icCloud(g, 26, 260, 18, BLUE); g.textMid(42, 261, b.cloudNote, F10, BLUE); }
  else {
    snprintf(t, sizeof t, T("%d-%d / %d · read %s"), bmsTop + 1, bmsTop + BROWS < n ? bmsTop + BROWS : n, n, b.pWhen);
    g.textMid(20, 261, t, F10, MUTED);
  }
  // selected value
  g.fillRoundRect(346, 40, 124, 110, 8, CARD);
  g.text(354, 58, d.name, F10, MUTED);
  bms::format(d, b.pe[bmsSel], v, sizeof v);
  bool ch = changedAt(b, bmsSel);
  g.text(408, 104, v, strlen(v) > 5 ? F24B : F38B, why ? RED : ch ? YELLOW : TEXT, AL_C);
  if (!bms::isSwitch(d)) g.text(408, 124, d.unit, F13, MUTED2, AL_C);
  bms::format(d, b.pv[bmsSel], o, sizeof o);
  snprintf(t, sizeof t, T("was %s"), bms::isSwitch(d) ? T(o) : o);
  g.text(408, 142, t, F10, MUTED, AL_C);
  button(g, BS_MINUS, bms::isSwitch(d) ? "OFF" : "-", CHIP, TEXT, bms::isSwitch(d) ? F13B : F24B);
  button(g, BS_PLUS, bms::isSwitch(d) ? "ON" : "+", CHIP, TEXT, bms::isSwitch(d) ? F13B : F24B);
  snprintf(t, sizeof t, T("x%d"), bmsStep);
  button(g, BS_STEP, t, CHIP, bms::isSwitch(d) ? DIM : BLUE);
  button(g, BS_UNDO, "UNDO", CHIP, MUTED2);
  if (!bms::isSwitch(d)) {
    char a[16], z[16];
    bms::format(d, d.min, a, sizeof a); bms::format(d, d.max, z, sizeof z);
    snprintf(t, sizeof t, T("range %s-%s"), a, z);
    g.text(408, 264, t, F10, MUTED, AL_C);
  }
  button(g, BS_READ, b.busy ? "..." : "READ", b.busy ? CARD2 : CHIP, b.busy ? DIM : TEXT);
  button(g, BS_UP, "UP"); button(g, BS_DN, "DOWN");
  int c = b.changed();
  bool anyBad = false;
  for (int i = 0; i < n; i++) if (changedAt(b, i) && bms::check(b.brand, b.pe, i, b.pe[i])) anyBad = true;
  snprintf(t, sizeof t, c ? T("WRITE (%d)") : T("WRITE"), c);
  button(g, BS_WRITE, t, c && !anyBad ? YELLOW : CARD2, c && !anyBad ? 0x000000 : DIM);
}

// ---- write check
static const Btn BW_CANCEL = {10, 282, 148, 30}, BW_WRITE = {322, 282, 148, 30};
static bool bikeStopped(const UiState &s) { return !s.speedOk || s.kmh < 1; }
static void bmsConfirmPage(Gfx &g, const UiState &s) {
  const BmsUi &b = s.bms;
  header(g, "WRITE TO BMS");
  g.text(14, 54, "These values will be written to the BMS:", F13, SUB);
  int y = 76, shown = 0, n = bms::paramCount(b.brand);
  bool dsgOff = false;
  for (int i = 0; i < n && shown < 5; i++) {
    if (!changedAt(b, i)) continue;
    bms::ParamDef d = bms::param(b.brand, i);
    char a[32], z[32], t[96];
    bms::format(d, b.pv[i], a, sizeof a); bms::format(d, b.pe[i], z, sizeof z);
    if (bms::isSwitch(d)) { snprintf(a, sizeof a, "%s", T(a)); snprintf(z, sizeof z, "%s", T(z)); }
    if (bms::isSwitch(d) && i == 1 && b.pe[i] < 0.5f) dsgOff = true;
    if (shown % 2 == 0) g.fillRect(6, y - 15, 468, 22, CARD2);
    g.text(14, y, d.name, F13, TEXT);
    snprintf(t, sizeof t, T("%s  >  %s %s"), a, z, bms::isSwitch(d) ? "" : d.unit);
    g.text(466, y, t, F13B, YELLOW, AL_R);
    y += 22; shown++;
  }
  if (b.changed() > shown) { char t[64]; snprintf(t, sizeof t, T("+ %d more"), b.changed() - shown); g.text(14, y, t, F11, MUTED); }
  bool stopped = bikeStopped(s);
  g.fillRoundRect(10, 196, 460, 76, 8, 0x1A1408);
  g.text(22, 216, stopped ? "Bike stopped" : "Stop the bike first", F13B, stopped ? GREEN : RED);
  g.text(22, 238, dsgOff ? "Discharge OFF cuts the power to the motor" : "Wrong protection values can damage the battery", F11, dsgOff ? 0xFF8A80 : SUB);
  g.text(22, 258, "Each value is read back to check it. Old values are kept for UNDO.", F11, SUB);
  if (b.msg[0]) g.text(240, 302, b.msg, F11B, b.msgKind == 1 ? GREEN : b.msgKind == 2 ? RED : YELLOW, AL_C);
  button(g, BW_CANCEL, "CANCEL");
  button(g, BW_WRITE, b.busy ? "WRITING..." : "WRITE", stopped && !b.busy ? YELLOW : CARD2, stopped && !b.busy ? 0x000000 : DIM);
}

// ---- find / choose the BMS
static const Btn BF[2] = {{10, 282, 225, 30}, {245, 282, 225, 30}};
static void findBody(Gfx &g, const UiState &s) {
  const BmsUi &b = s.bms;
  const int cx = 92, cy = 120;
  uint32_t c;
  bmsLinkText(b, c);
  g.fillCircle(cx, cy, 64, mix(BG, c, 0.10f));
  g.fillArc(cx, cy, 56, 62, 0, 360, mix(BG, c, 0.25f));
  const char *h = "", *l = "";
  if (b.link == bms::L_SCAN || b.link == bms::L_CONNECT) {
    float a = (nowMs % 1200) * 0.3f;
    g.fillArc(cx, cy, 56, 62, a, a + 80, c);
    if (b.link == bms::L_SCAN) { icBt(g, cx, cy, 20, c); h = "Searching..."; l = "BMS on, phone app closed"; }
    else {
      uint32_t in = mix(BG, c, 0.10f);
      g.fillRoundRect(cx - 26, cy - 16, 48, 32, 6, c); g.fillRoundRect(cx - 22, cy - 12, 40, 24, 4, in);
      g.fillRect(cx + 22, cy - 7, 5, 14, c);
      int k = (nowMs / 400) % 4;
      for (int j = 0; j < k; j++) g.fillRect(cx - 19 + j * 12, cy - 9, 10, 18, c);
      h = "Connecting..."; l = b.name;
    }
  } else if (b.link == bms::L_ONLINE) {
    g.fillCircle(cx, cy, 44, c); icCheck(g, cx, cy + 2, 20, BG); h = "Connected!"; l = b.name;
  } else if (b.link == bms::L_LOST) {
    g.fillCircle(cx, cy, 44, c); icCross(g, cx, cy, 16, BG); h = "BMS not reachable"; l = "Too far, or a phone is connected";
  } else if (b.link == bms::L_NOBLE) {
    icBt(g, cx, cy, 22, c); h = "No Bluetooth"; l = "This board cannot reach a BMS";
  } else {
    icBt(g, cx, cy, 22, c); h = "Pick your BMS"; l = "Tap a BMS in the list";
  }
  g.text(cx, 214, h, F13B, b.link == bms::L_OFF ? TEXT : c, AL_C);
  g.text(cx, 234, l, F10, MUTED2, AL_C);
  if (!b.foundN) {
    g.fillRoundRect(180, 40, 290, 232, 10, CARD);
    g.text(325, 140, b.link == bms::L_SCAN ? "No BMS found yet" : "Press SCAN to look for BMS", F13B, MUTED2, AL_C);
    g.text(325, 162, "Supports JK, ANT, JBD / Xiaoxiang, Daly", F10, MUTED, AL_C);
    return;
  }
  for (int i = 0; i < b.foundN && i < 5; i++) {
    const bms::Found &f = b.found[i];
    int y = 40 + i * 47;
    bool cur = !strcmp(f.mac, b.mac) && b.link == bms::L_ONLINE, sel = i == b.sel;
    g.fillRoundRect(180, y, 290, 42, 8, cur ? 0x0B2218 : sel ? 0x13202A : CARD);
    if (sel || cur) g.drawRoundRect(180, y, 290, 42, 8, cur ? GREEN : BLUE);
    chip(g, 190, y + 6, 44, bms::BRAND[f.brand], f.brand ? 0x13283A : CHIP, f.brand ? BLUE : MUTED2);
    g.text(242, y + 19, f.name, F13B, TEXT);
    g.text(242, y + 36, f.mac, F10, MUTED);
    if (cur) chip(g, 380, y + 12, 58, "IN USE", DARKGREEN, GREEN);
    sigBars(g, 446, y + 28, f.rssi, f.rssi > -70 ? GREEN : f.rssi > -85 ? YELLOW : RED);
  }
}
static void findButtons(Gfx &g, const UiState &s) {
  const BmsUi &b = s.bms;
  bool scanning = b.link == bms::L_SCAN;
  button(g, BF[0], scanning ? "SCANNING..." : "SCAN AGAIN", scanning ? CARD2 : CHIP, scanning ? MUTED2 : TEXT);
  bool cur = b.sel >= 0 && b.sel < b.foundN && !strcmp(b.found[b.sel].mac, b.mac) && b.mac[0];
  if (cur) button(g, BF[1], "FORGET THIS BMS", CHIP, 0xFF8A80);
  else if (b.sel >= 0) button(g, BF[1], "CONNECT", BLUE, 0x001018);
  else button(g, BF[1], "CONNECT", CARD2, DIM);
}
static void findPage(Gfx &g, const UiState &s) {
  bmsHeader(g, s, "FIND BMS");
  findBody(g, s);
  findButtons(g, s);
}

static bool bmsTouch(int x, int y, UiState &s, UiActions &a) {
  BmsUi &b = s.bms;
  int n = bms::paramCount(b.brand);
  switch (page) {
  case PG_BMS:
    if (hit(BACK, x, y)) { go(PG_DASH); return true; }
    if (hit(BM[0], x, y) && b.have) { go(PG_BMS_CELLS); return true; }
    if (hit(BM[1], x, y) && b.have) { go(PG_BMS_SET); if (!b.pHave && !b.busy) a.bmsRead = true; return true; }
    if (hit(BM[2], x, y)) { go(PG_BMS_FIND); if (b.link == bms::L_OFF) a.bmsScan = true; return true; }
    return false;
  case PG_BMS_CELLS:
    if (hit(BACK, x, y) || hit(BC[0], x, y)) { go(PG_BMS); return true; }
    if (hit(BC[1], x, y)) { go(PG_BMS_SET); if (!b.pHave && !b.busy) a.bmsRead = true; return true; }
    return false;
  case PG_BMS_SET: {
    if (hit(BACK, x, y)) { go(PG_BMS); return true; }
    if (hit(BS_READ, x, y) && !b.busy) { a.bmsRead = true; b.msg[0] = 0; return true; }
    if (!b.pHave || !n) return false;
    bms::ParamDef d = bms::param(b.brand, bmsSel);
    if (hit(BS_MINUS, x, y) || hit(BS_PLUS, x, y)) {
      int dir = hit(BS_PLUS, x, y) ? 1 : -1;
      float v = bms::isSwitch(d) ? (dir > 0 ? 1 : 0) : b.pe[bmsSel] + dir * d.step * bmsStep;
      if (v < d.min) v = d.min;
      if (v > d.max) v = d.max;
      b.pe[bmsSel] = roundf(v / d.step) * d.step;
      return true;
    }
    if (hit(BS_STEP, x, y)) { bmsStep = bmsStep == 1 ? 10 : bmsStep == 10 ? 100 : 1; return true; }
    if (hit(BS_UNDO, x, y)) { b.pe[bmsSel] = b.pv[bmsSel]; return true; }
    if (hit(BS_UP, x, y)) { if (bmsSel > 0) bmsSel--; return true; }
    if (hit(BS_DN, x, y)) { if (bmsSel + 1 < n) bmsSel++; return true; }
    if (hit(BS_WRITE, x, y) && b.changed()) {
      for (int i = 0; i < n; i++) if (changedAt(b, i) && bms::check(b.brand, b.pe, i, b.pe[i])) return false;
      b.msg[0] = 0; go(PG_BMS_CONFIRM); return true;
    }
    if (x < 336 && y >= 40 && y < 40 + BROWS * 34) {
      int r = bmsTop + (y - 40) / 34;
      if (r < n) { bmsSel = r; return true; }
    }
    return false;
  }
  case PG_BMS_CONFIRM:
    if (hit(BACK, x, y) || hit(BW_CANCEL, x, y)) { go(PG_BMS_SET); return true; }
    if (hit(BW_WRITE, x, y) && bikeStopped(s) && !b.busy) { a.bmsWrite = true; return true; }
    return false;
  case PG_BMS_FIND:
    if (hit(BACK, x, y)) { go(PG_BMS); return true; }
    if (hit(BF[0], x, y) && b.link != bms::L_SCAN) { a.bmsScan = true; return true; }
    if (hit(BF[1], x, y) && b.sel >= 0 && b.sel < b.foundN) {
      if (!strcmp(b.found[b.sel].mac, b.mac)) a.bmsForget = true; else a.bmsConnect = b.sel;
      return true;
    }
    if (x >= 180 && y >= 40 && y < 40 + 5 * 47) {
      int i = (y - 40) / 47;
      if (i < b.foundN) { b.sel = i; return true; }
    }
    return false;
  default: return false;
  }
}
