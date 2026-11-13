// Smart BMS over Bluetooth (JK, ANT, JBD/Xiaoxiang, Daly): what the screen,
// the module web page and the cloud show. The protocol decoders and the
// protection-setting tables come from the BattLab project (jk_protocol.h,
// bms_other.h, bms_daly.h, bms_params.h, bms_settings.h).
#pragma once
#include <stdint.h>

namespace bms {
enum Brand : uint8_t { BR_NONE, BR_JK, BR_ANT, BR_JBD, BR_DALY, BR_COUNT };
extern const char *const BRAND[BR_COUNT];
// link state shown on every page
enum Link : uint8_t {
  L_OFF,      // no BMS chosen yet
  L_SCAN,     // looking for BMS nearby
  L_CONNECT,  // connecting to the chosen BMS
  L_ONLINE,   // data arriving
  L_LOST,     // chosen BMS not reachable, retrying
  L_NOBLE     // this board has no Bluetooth
};
// alarm bits (same meaning for every brand)
enum : uint32_t {
  A_CELL_OV = 1, A_CELL_UV = 2, A_PACK_OV = 4, A_PACK_UV = 8, A_CHG_OC = 16, A_DSG_OC = 32,
  A_SHORT = 64, A_CHG_OT = 128, A_DSG_OT = 256, A_MOS_OT = 512, A_LOW_T = 1024, A_CELL_DIFF = 2048
};
static const int ALARM_N = 12;
extern const char *const ALARM[ALARM_N];

struct Found { char name[24]; char mac[18]; int8_t rssi; uint8_t brand; uint8_t type = 0; };  // type = BLE address type

struct Live {
  float volt = 0, amp = 0, soc = 0, remainAh = 0, fullAh = 0, mos = 0;
  float temp[4] = {0, 0, 0, 0};
  uint8_t nTemp = 0, nCells = 0;
  float cell[32] = {0};
  uint32_t cycles = 0, balMask = 0, alarms = 0;
  bool chg = false, dsg = false, bal = false;  // MOS / balancer switched on
  float cellMin(int *at = nullptr) const;
  float cellMax(int *at = nullptr) const;
  float tempMax() const;
};

// One writable setting: protection values or an on/off switch (unit "sw").
struct ParamDef {
  const char *name, *unit;
  float min, max, step;
  uint8_t dec;
  int8_t below = -1, above = -1;  // must stay below / above this other setting (unified index)
};
static const int MAX_PARAMS = 64;
int paramCount(uint8_t brand);
ParamDef param(uint8_t brand, int i);
bool isSwitch(const ParamDef &d);
// nullptr when value v may be written to setting i (cur = all current values)
const char *check(uint8_t brand, const float *cur, int i, float v);
void format(const ParamDef &d, float v, char *out, int n);
// Alarm bits worked out from the live values (and the protection settings when read).
uint32_t alarms(const Live &L, uint8_t brand, const float *pv, uint64_t pvHave);
}  // namespace bms

// Everything the screens need about the BMS.
struct BmsUi {
  uint8_t link = bms::L_OFF, brand = bms::BR_NONE;
  char name[24] = "", mac[18] = "";
  int8_t rssi = 0;
  float age = 0;               // seconds since the last frame
  bms::Live live;
  bool have = false;           // live is valid
  // devices found nearby (find page)
  bms::Found found[8];
  int foundN = 0, sel = -1;
  // settings
  bool pHave = false;
  char pWhen[16] = "";
  float pv[bms::MAX_PARAMS] = {0};  // values read from the BMS
  float pe[bms::MAX_PARAMS] = {0};  // working copy being edited
  bool busy = false;
  char msg[64] = "";
  uint8_t msgKind = 0;         // 0 info, 1 ok, 2 error
  // cloud
  uint32_t logN = 0;           // samples sent to the cloud today
  char cloudNote[128] = "";     // last change made from the cloud page
  int changed() const;
};
