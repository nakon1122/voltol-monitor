// Text width / cap height straight from the VLW font data, using the same
// rules as LovyanGFX (LGFXBase::text_width) so layout is identical on the PC.
#pragma once
#include <stdint.h>
#include "gfx.h"

namespace vlw {
inline int32_t be32(const uint8_t *p) {
  return (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
}
// Glyph record: unicode, height, width, xAdvance, dY, dX, pad.
inline const uint8_t *glyph(const uint8_t *f, uint16_t u) {
  int n = be32(f);
  for (int i = 0; i < n; i++) {
    const uint8_t *g = f + 24 + i * 28;
    if (be32(g) == u) return g;
  }
  return nullptr;
}
inline uint16_t nextUtf8(const char *&s) {
  uint8_t c = (uint8_t)*s++;
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0 && *s) return (uint16_t)(((c & 0x1F) << 6) | (*s++ & 0x3F));
  if ((c & 0xF0) == 0xE0 && s[0] && s[1]) {
    uint16_t u = (uint16_t)(((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F));
    s += 2;
    return u;
  }
  return '?';
}
inline int width(const uint8_t *f, const char *s) {
  int left = 0, right = 0;
  int spaceW = be32(f + 8) * 2 / 7;  // LovyanGFX guess for missing glyphs
  while (*s) {
    uint16_t u = nextUtf8(s);
    const uint8_t *g = glyph(f, u);
    int adv = g ? be32(g + 12) : spaceW, w = g ? be32(g + 8) : spaceW, dx = g ? (int8_t)be32(g + 20) : 0;
    if (left == 0 && right == 0 && dx < 0) left = right = -dx;
    right = left + (adv > w + dx ? adv : w + dx);
    left += adv;
  }
  return right;
}
inline int cap(const uint8_t *f) {
  const uint8_t *g = glyph(f, 'H');
  if (!g) g = glyph(f, '0');
  return g ? be32(g + 16) : be32(f + 8) * 7 / 10;
}
}  // namespace vlw

inline int Gfx::rawWidth(const char *s, int font) { return vlw::width(FONT_VLW[font], s); }
inline int Gfx::textWidth(const char *s, int font) {
  char buf[400];
  if (fontThai(font)) s = T(s);
  if (thai::needs(s)) s = thai::shape(s, buf, sizeof buf);
  return vlw::width(FONT_VLW[font], s);
}
inline int Gfx::capHeight(int font) { return vlw::cap(FONT_VLW[font]); }
