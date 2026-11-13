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
  if (b == BR_JK) {
    const bmsw::JkParam &p = bmsw::JK_PARAMS[i];
    return {p.name, p.unit, p.min, p.max, p.step, p.decimals};
  }
  int n;
  const bset::Def *t = table(b, n);
  if (t && i < n) return {t[i].name, t[i].unit, t[i].min, t[i].max, t[i].step, t[i].dec};
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
  if (b == BR_JK) {  // OVP > OVP recovery, UVP < UVP recovery, power off < UVP
    const float *c = cur + ns;
    int k = i - ns;
    using namespace bmsw;
    if (k == P_OVP && v <= c[P_OVPR]) return "must be above its release value";
    if (k == P_OVPR && v >= c[P_OVP]) return "must be below its protection value";
    if (k == P_UVP && v >= c[P_UVPR]) return "must be below its release value";
    if (k == P_UVPR && v <= c[P_UVP]) return "must be above its protection value";
    if (k == P_UVP && v <= c[P_POWER_OFF]) return "must be above power off";
    if (k == P_POWER_OFF && v >= c[P_UVP]) return "must be below cell UVP";
    return nullptr;
  }
  uint8_t fam = b == BR_ANT ? 2 : 3;
  return bset::check(fam, cur + ns, ~0ull, i - ns, v);
}

void format(const ParamDef &d, float v, char *out, int n) {
  if (isSwitch(d)) snprintf(out, n, "%s", v > 0.5f ? "ON" : "OFF");
  else snprintf(out, n, "%.*f", d.dec, v);
}
}  // namespace bms

int BmsUi::changed() const {
  int n = 0, k = bms::paramCount(brand);
  for (int i = 0; i < k; i++) if (fabsf(pe[i] - pv[i]) > 1e-4f) n++;
  return n;
}
