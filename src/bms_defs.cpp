// Writable BMS settings for every brand, as one list per brand:
// MOS switches first, then the protection values (tables from BattLab).
#include "bms.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "bms_params.h"
#include "bms_settings.h"

namespace bms {
const char *const BRAND[BR_COUNT] = {"-", "JK", "ANT", "JBD", "Daly"};
const char *const ALARM[ALARM_N] = {"Cell over-voltage", "Cell under-voltage", "Pack over-voltage", "Pack under-voltage",
                                    "Charge over-current", "Discharge over-current", "Short circuit", "Charge over-temp",
                                    "Discharge over-temp", "MOS over-temp", "Too cold", "Cell difference"};

float Live::cellMin(int *at) const {
  float m = 99; int k = -1;
  for (int i = 0; i < nCells; i++) if (cell[i] > 0 && cell[i] < m) { m = cell[i]; k = i; }
  if (at) *at = k;
  return k < 0 ? 0 : m;
}
float Live::cellMax(int *at) const {
  float m = 0; int k = -1;
  for (int i = 0; i < nCells; i++) if (cell[i] > m) { m = cell[i]; k = i; }
  if (at) *at = k;
  return m;
}
float Live::tempMax() const {
  float m = mos;
  for (int i = 0; i < nTemp; i++) if (temp[i] > m) m = temp[i];
  return m;
}

static const ParamDef SW_CHG = {"Charge MOS", "sw", 0, 1, 1, 0}, SW_DSG = {"Discharge MOS", "sw", 0, 1, 1, 0},
                      SW_BAL = {"Balancer", "sw", 0, 1, 1, 0};
static int switches(uint8_t b) { return b == BR_JK || b == BR_DALY ? 3 : b == BR_ANT || b == BR_JBD ? 2 : 0; }
static const bset::Def *table(uint8_t b, int &n) {
  if (b == BR_ANT) return bset::table(2, n);
  if (b == BR_JBD) return bset::table(3, n);
  n = 0;
  return nullptr;
}

int paramCount(uint8_t b) {
  int n = 0;
  if (b == BR_JK) n = bmsw::JK_PARAM_COUNT;
  else table(b, n);
  n += switches(b);
  return n > MAX_PARAMS ? MAX_PARAMS : n;
}
ParamDef param(uint8_t b, int i) {
  int ns = switches(b);
  if (i < ns) return i == 0 ? SW_CHG : i == 1 ? SW_DSG : SW_BAL;
  i -= ns;
  if (i < 0) return {"?", "", 0, 0, 1, 0};
  if (b == BR_JK) {
    using namespace bmsw;
    const JkParam &p = JK_PARAMS[i];
    ParamDef d{p.name, p.unit, p.min, p.max, p.step, p.decimals};
    // OVP > OVP recovery > UVP recovery > UVP > power off
    auto at = [&](int k) { return (int8_t)(ns + k); };
    if (i == P_OVP) d.above = at(P_OVPR);
    if (i == P_OVPR) { d.below = at(P_OVP); d.above = at(P_UVPR); }
    if (i == P_UVPR) { d.below = at(P_OVPR); d.above = at(P_UVP); }
    if (i == P_UVP) { d.below = at(P_UVPR); d.above = at(P_POWER_OFF); }
    if (i == P_POWER_OFF) d.below = at(P_UVP);
    return d;
  }
  int n;
  const bset::Def *t = table(b, n);
  if (t && i < n) {
    ParamDef d{t[i].name, t[i].unit, t[i].min, t[i].max, t[i].step, t[i].dec};
    if (t[i].below >= 0) d.below = (int8_t)(ns + t[i].below);
    if (t[i].above >= 0) d.above = (int8_t)(ns + t[i].above);
    return d;
  }
  return {"?", "", 0, 0, 1, 0};
}
bool isSwitch(const ParamDef &d) { return !strcmp(d.unit, "sw"); }

const char *check(uint8_t b, const float *cur, int i, float v) {
  int n = paramCount(b);
  if (i < 0 || i >= n) return "unknown setting";
  ParamDef d = param(b, i);
  if (v < d.min - d.step / 2 || v > d.max + d.step / 2) return "outside the safe range";
  int ns = switches(b);
  if (i < ns) return nullptr;
  if (b == BR_JK) {  // BattLab rules: OVP > OVP recovery > UVP recovery > UVP > power off
    bmsw::JkSettings js;
    for (int k = 0; k < bmsw::JK_PARAM_COUNT; k++) js.v[k] = cur[ns + k];
    js.valid = true;
    return bmsw::checkJkValue(js, i - ns, v);
  }
  uint8_t fam = b == BR_ANT ? 2 : 3;
  return bset::check(fam, cur + ns, ~0ull, i - ns, v);
}

void format(const ParamDef &d, float v, char *out, int n) {
  if (isSwitch(d)) snprintf(out, n, "%s", v > 0.5f ? "ON" : "OFF");
  else snprintf(out, n, "%.*f", d.dec, v);
}
uint32_t alarms(const Live &L, uint8_t b, const float *pv, uint64_t have) {
  if (!L.nCells) return 0;
  uint32_t a = 0;
  float mn = L.cellMin(), mx = L.cellMax();
  auto P = [&](const char *name, float &v) {  // protection value by name, when read
    int n = paramCount(b);
    for (int i = 0; i < n; i++)
      if ((have >> i & 1) && !strcmp(param(b, i).name, name)) { v = pv[i]; return true; }
    return false;
  };
  float ov, uv;
  if (P("Cell OVP", ov) && mx >= ov - 0.005f) a |= A_CELL_OV;
  if (P("Cell UVP", uv) && mn > 0 && mn <= uv + 0.005f) a |= A_CELL_UV;
  if (mx - mn >= 0.10f) a |= A_CELL_DIFF;
  bool chg = L.amp > 0.3f;
  for (int i = 0; i < L.nTemp; i++) {
    if (L.temp[i] >= 60) a |= chg ? A_CHG_OT : A_DSG_OT;
    if (L.temp[i] <= 0 && chg) a |= A_LOW_T;
  }
  if (L.mos >= 80) a |= A_MOS_OT;
  return a;
}
}  // namespace bms

int BmsUi::changed() const {
  int n = 0, k = bms::paramCount(brand);
  for (int i = 0; i < k; i++) if (fabsf(pe[i] - pv[i]) > 1e-4f) n++;
  return n;
}
