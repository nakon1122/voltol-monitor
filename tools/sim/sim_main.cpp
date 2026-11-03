// PC build of the real screen code (src/ui.cpp). Prints every drawing call;
// tools/render_screens.py replays them into PNGs with the board's own fonts.
//   g++ -std=c++17 -Iinclude -o sim tools/sim/sim_main.cpp src/ui.cpp src/fonts_vlw.cpp && ./sim > screens.txt
#include "ui.h"
#include "vlw_metrics.h"
#include <cstdio>
#include <cstring>
#include <cmath>

struct Rec : Gfx {
  void fillRect(int x, int y, int w, int h, uint32_t c) override { printf("R %d %d %d %d %06X\n", x, y, w, h, c); }
  void fillRoundRect(int x, int y, int w, int h, int r, uint32_t c) override { printf("RR %d %d %d %d %d %06X\n", x, y, w, h, r, c); }
  void drawRoundRect(int x, int y, int w, int h, int r, uint32_t c) override { printf("RO %d %d %d %d %d %06X\n", x, y, w, h, r, c); }
  void fillCircle(int x, int y, int r, uint32_t c) override { printf("C %d %d %d %06X\n", x, y, r, c); }
  void drawLine(int x0, int y0, int x1, int y1, uint32_t c) override { printf("L %d %d %d %d %06X\n", x0, y0, x1, y1, c); }
  void fillTriangle(int a, int b, int c2, int d, int e, int f, uint32_t c) override { printf("T %d %d %d %d %d %d %06X\n", a, b, c2, d, e, f, c); }
  void fillArc(int x, int y, int r0, int r1, float a0, float a1, uint32_t c) override { printf("A %d %d %d %d %.2f %.2f %06X\n", x, y, r0, r1, a0, a1, c); }
  void textRaw(int x, int y, const char *s, int font, uint32_t c) override { printf("S %d %d %d %06X %s\n", x, y, font, c, s); }
};

static Rec g;
static UiState s;
static Settings cfg;
static ride::DragTimer drag;
static const char *prefix = "";
static void shot(const char *name) {
  printf("SCREEN %s%s\n", prefix, name);
  ui::draw(g, s, cfg);
}
static void live(float kmh) {
  s.linked = true; s.hz = 10.2f; s.baud = 9600; s.frames = 18342; s.errors = 3;
  s.live.volt = 78.4f; s.live.amp = 48.6f; s.live.rpm = (int)lroundf(kmh / ride::rpmToKmh(1, cfg.circMm(), 1));
  s.live.tCtrl = 42; s.live.tMotor = 55; s.live.gearBits = 2; s.live.status = votol::ST_RUN; s.live.faults = 0;
  s.kmh = kmh; s.kw = s.live.volt * s.live.amp / 1000; s.soc = 85; s.speedOk = true; s.src = 0; s.dragWaitGps = false;
  s.trip = ride::Trip(); s.trip.distM = 12400; s.trip.maxKmh = 98; s.trip.maxA = 186; s.trip.wh = 508; s.trip.movingS = 1210;
  s.odoKm = 1284;
}
static ride::DragResult runResult(double a, double vmax, double tEnd, ride::DragTimer *d) {
  d->arm();
  for (double t = 0; t < tEnd; t += 0.1) {
    double tau = t - 0.53, v = tau <= 0 ? 0 : fmin(a * tau, vmax / 3.6);
    d->feed(t, v * 3.6, tau > 0 ? 210 - 4 * tau : 0, 76 - (tau > 0 ? 4 : 0));
    s.now = t;
  }
  return d->r;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "en")) { cfg.lang = 1; prefix = "en_"; }
  s.fw = "VOLTOL Monitor 1.0.0";
  strcpy(s.ssid, "VOLTOL-3F2A"); strcpy(s.ip, "http://192.168.4.1");
  s.heapKB = 142; s.fsFreeKB = 512; s.fsTotalKB = 640; strcpy(s.clock, "18:42");
  s.drag = &drag;
  // ---- boot
  ui::page = PG_BOOT; shot("01_boot");
  // ---- dashboards
  ui::page = PG_DASH;
  live(62);
  for (int d = 0; d < 3; d++) {
    cfg.dash = d;
    char n[32]; snprintf(n, 32, "%02d_dash_%c", 2 + d, 'A' + d); shot(n);
  }
  cfg.dash = 0; live(38); s.live.amp = -21.5f; s.kw = s.live.amp * s.live.volt / 1000; s.live.status = votol::ST_BRAKE;
  shot("05_dash_A_regen");
  live(0); s.live.amp = 0; s.kw = 0; s.live.status = votol::ST_FAULT; s.live.faults = 0x00000082;
  cfg.dash = 1; shot("06_dash_B_fault");
  cfg.dash = 2; live(105); s.live.amp = 172; s.kw = 172 * 77.1f / 1000; s.live.volt = 77.1f; s.live.gearBits = 3; shot("07_dash_C_sport");
  s.linked = false; s.speedOk = false; cfg.dash = 0; shot("08_dash_A_nolink");
  live(62); cfg.dash = 0; ui::menuOpen = true; shot("09_menu"); ui::menuOpen = false;
  // ---- drag
  ride::DragTimer bestT; s.best = runResult(3.15, 140, 25, &bestT); s.haveBest = true; strcpy(s.bestWhen, "03 Nov 18:12");
  live(0); s.live.amp = 0; s.kw = 0; s.live.gearBits = 3; s.live.status = votol::ST_IDLE;
  ui::page = PG_DRAG; drag.cancel(); shot("10_drag_idle");
  drag.arm(); drag.feed(0, 0); shot("11_drag_ready");
  ride::DragTimer run; runResult(3.3, 140, 7.95, &run); drag = run; live(83.6); s.live.amp = 186; s.kw = 14.2f;
  shot("12_drag_run");
  runResult(3.3, 140, 25, &run); drag = run; shot("13_drag_result");
  // ---- GPS only (no controller wired)
  auto gpsOnly = [&](float kmh) {
    live(kmh); s.linked = false; s.speedOk = true; s.src = 1; s.gpsSeen = true; s.gpsFix = true; s.gpsUbx = true;
    s.gpsSats = 14; s.gpsHz = 25; s.gpsSAcc = 0.3f; s.kmh = kmh;
  };
  gpsOnly(57); ui::page = PG_DASH; cfg.dash = 1; shot("13b_dash_B_gps_only");
  cfg.rollout = 1; ui::page = PG_DRAG;
  gpsOnly(0); s.gpsSats = 4; s.gpsSAcc = 2.4f; s.dragWaitGps = true;
  drag = ride::DragTimer(); drag.cfg = ride::DragCfg::gps(true); drag.arm(); shot("13c_drag_gps_waiting");
  gpsOnly(0.4f); s.dragWaitGps = false;
  for (double t = 0; t < 1.2; t += 0.04) drag.feed(t, 0.4); shot("13d_drag_gps_ready");
  { ride::DragTimer gr; gr.cfg = ride::DragCfg::gps(true); gr.arm();
    for (double t = 0; t < 30; t += 0.04) { double tau = t - 1.5, v = tau <= 0 ? 0.08 : fmin(3.3 * tau, 140 / 3.6); gr.feed(t, v * 3.6); }
    drag = gr; gpsOnly(0); shot("13e_drag_gps_result"); }
  cfg.rollout = 0; cfg.dash = 0;
  // ---- hybrid (controller + GPS) run with wheel slip, then an RPM-only spin-corrected result
  { ride::DragTimer h; h.cfg = ride::DragCfg::gps(false); h.arm();
    for (double t = 0; t < 4.6; t += 0.04) { double tau = t - 1.5, v = tau <= 0 ? 0.1 : 1.35 * 9.81 * tau; h.feed(t, v * 3.6, tau > 0 ? 420 : 0, 80); if (fabs(t - 1.52) < 0.02) h.launchHint(1.5); h.slip(tau > 0.3 && tau < 0.9 ? 14 : 3); s.now = t; }
    drag = h; live(146); s.live.amp = 418; s.kw = 31.5f; s.src = 1; s.gpsSeen = s.gpsFix = s.gpsUbx = true; s.gpsSats = 15; s.gpsHz = 25; s.gpsSAcc = 0.2f;
    s.slipOk = true; s.slipPct = 6; shot("13f_drag_hybrid_run"); s.slipOk = false; }
  { ride::DragTimer r; r.cfg = ride::DragCfg::rpm(false, 1.6); r.arm();
    for (double t = 0; t < 12; t += 0.1) { double tau = t - 0.5, real = tau <= 0 ? 0 : fmin(1.3 * 9.81 * tau, 70); double w = real + (tau > 0.2 && tau < 1.0 ? 25 * sin((tau - 0.2) / 0.8 * M_PI) : 0); r.feed(t, w * 3.6, tau > 0 ? 520 : 0, 82); if (r.state == ride::DragTimer::DONE) break; }
    drag = r; live(0); s.live.amp = 0; s.src = 0; s.gpsSeen = false; shot("13g_drag_spin_corrected"); }
  ui::page = PG_DRAG_HIST;
  const char *dates[5] = {"03 Nov 18:31", "03 Nov 18:12", "03 Nov 17:58", "02 Nov 19:20", "02 Nov 19:05"};
  for (int i = 0; i < 5; i++) {
    DragRow &r = s.runs[i]; strcpy(r.when, dates[i]);
    for (int k = 0; k < 4; k++) { r.t[k] = (float)(i == 0 ? drag.r.s[k].t : s.best.s[k].t) + i * 0.05f * (k + 1); r.trap[k] = (float)drag.r.s[k].trapKmh - i; }
  }
  s.runN = 5; shot("14_drag_history");
  // ---- logs
  live(0); s.live.status = votol::ST_IDLE; s.live.amp = 0;
  s.trip.distM = 12400; s.trip.maxKmh = 98; s.trip.maxA = 186; s.trip.wh = 508; s.trip.movingS = 1210;
  const char *tw[5] = {"03 Nov 17:40", "02 Nov 19:01", "02 Nov 08:12", "01 Nov 18:30", "01 Nov 07:55"};
  float km[5] = {18.2f, 22.7f, 9.4f, 31.0f, 8.8f};
  for (int i = 0; i < 5; i++) {
    TripRow &t = s.trips[i]; t.id = 41 - i; strcpy(t.when, tw[i]); t.km = km[i];
    t.maxKmh = 92 + i * 3; t.maxA = 170 + i * 6; t.whKm = 38 + i;
  }
  s.tripN = 5;
  ui::page = PG_LOG; shot("15_log");
  s.gN = 180;
  for (int i = 0; i < s.gN; i++) {
    float t = i / 180.0f;
    s.gKmh[i] = 60 + 35 * sinf(t * 9) * (t < .5f ? t * 2 : 1) + (i > 150 ? -50 * (i - 150) / 30.0f : 0);
    if (s.gKmh[i] < 0) s.gKmh[i] = 0;
    s.gAmp[i] = 60 + 110 * cosf(t * 9 - 0.6f) + (i % 7) * 4;
    if (i > 150) s.gAmp[i] = -25;
  }
  ui::page = PG_GRAPH; shot("16_graph");
  const char *fw[4] = {"18:31:07", "18:31:09", "18:02:44", "17:50:12"};
  uint32_t fc[4] = {0x02, 0x02, 0x80, 0x04};
  bool fs[4] = {true, false, true, true};
  for (int i = 0; i < 4; i++) {
    FaultRow &f = s.faults[i]; strcpy(f.when, fw[i]); f.code = fc[i]; f.set = fs[i];
    f.kmh = 64 - i * 9; f.amp = 205 - i * 30; f.volt = 74.2f + i; f.rpm = 900;
  }
  s.faultN = 4; ui::page = PG_FAULTS; shot("17_faults");
  // ---- tune
  for (int p = 1; p <= 7; p++) {
    uint8_t f[24] = {0xC0, 0x14, 0x02, 'R', (uint8_t)p};
    s.params.storePage(f);
  }
  using namespace votol;
  auto put = [&](const char *name, float v) {
    for (auto &d : PARAMS) if (!strcmp(d.name, name)) {
      int32_t r = d.type == T_THR ? (int32_t)lroundf((v * 100 - 2.2f) * 255 / 550) : (int32_t)lroundf(v * powf(10, d.dec));
      setRaw(s.params.b, d, r);
    }
  };
  put("Overvoltage", 90); put("Undervoltage", 60); put("Soft undervoltage", 63); put("Undervoltage variation", 2);
  put("Busbar current", 180); put("Phase current", 450); put("Sport current limit", 30); put("Sport flux-weakening", 4000);
  put("Flux-weak compensation", 60); put("Speed ratio", 40); put("Soft-start grade", 16); put("Logout time", 20);
  put("Recovery time", 30); put("Low speed", 45); put("Mid speed", 80); put("High speed", 100);
  put("Mid flux-weakening", 3000); put("High flux-weakening", 4000); put("Pole pairs", 4); put("Hall shift angle", 240);
  put("Initial temperature", 80); put("Over-temperature", 100); put("Max temperature", 120);
  put("Start voltage", 1.1f); put("End voltage", 4.2f); put("Low protect", 0.5f); put("High protect", 4.6f);
  memcpy(s.edit, s.params.b, PARAM_BYTES);
  strcpy(s.paramsWhen, "18:20");
  s.backupN = 3;
  for (int i = 0; i < 3; i++) {
    s.backups[i].id = 4 - i; strcpy(s.backups[i].when, i == 0 ? "03 Nov 18:20" : i == 1 ? "01 Nov 09:12" : "28 Oct 20:40");
    s.backups[i].sum = i == 0 ? s.params.checkSum() : 0x1A2B3 + i * 977;
  }
  ui::page = PG_TUNE; shot("18_tune");
  for (auto &d : PARAMS) {
    if (!strcmp(d.name, "Sport current limit")) setRaw(s.edit, d, 45);
    if (!strcmp(d.name, "Speed ratio")) setRaw(s.edit, d, 60);
  }
  ui::page = PG_TUNE; shot("19_tune_changed");
  ui::editGroup = 1; ui::page = PG_EDIT; shot("20_edit_sport");
  ui::page = PG_CONFIRM; shot("21_confirm");
  strcpy(s.msg, "Written and verified OK"); s.msgKind = 1;
  ui::page = PG_TUNE; memcpy(s.params.b, s.edit, PARAM_BYTES); shot("22_tune_written"); s.msg[0] = 0;
  ui::page = PG_BACKUPS; shot("23_backups");
  s.prof.have[0] = s.prof.have[1] = 1;
  s.prof.v[0][0] = 40; s.prof.v[0][1] = 300; s.prof.v[0][2] = 16;
  s.prof.v[1][0] = 18; s.prof.v[1][1] = 150; s.prof.v[1][2] = 10;
  ui::page = PG_LAUNCH; shot("23b_launch_profile");
  // ---- setup
  ui::page = PG_WHEEL; shot("25_wheel");
  live(0); s.gpsSeen = true; s.gpsFix = true; s.gpsSats = 11; s.gpsHz = 10;
  s.netOn = true; s.netConnected = true; strcpy(s.netSsid, "Home-WiFi"); strcpy(s.netIp, "192.168.1.57");
  s.updAvailable = true; strcpy(s.updLatest, "1.3.0"); strcpy(s.updMsg, "Version 1.3.0 available"); s.updFmt = "Version %s available"; strcpy(s.updArg, "1.3.0");
  ui::page = PG_SETUP; shot("24_setup");
  ui::page = PG_INFO; shot("26_info_update");
  s.updState = 2; s.updProgress = 62; shot("27_info_downloading");
  return 0;
}
