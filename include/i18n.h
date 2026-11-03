#pragma once
#include <stdint.h>
// On-screen language. Texts are written in English in the code and looked up
// in a Thai table (src/i18n.cpp) when Thai is selected.
namespace i18n {
enum : uint8_t { LANG_TH = 0, LANG_EN = 1 };
extern uint8_t lang;
const char *tr(const char *en);
}  // namespace i18n
#define T(s) i18n::tr(s)
