// Minimal Thai shaping for the board's bitmap fonts (no OpenType engine).
// Picks the pre-positioned mark copies that tools/make_fonts.py puts in the
// private-use area, so marks do not collide:
//   U+F700+i  tone mark above an upper vowel        (e.g. ที่ นี้)
//   U+F710+j  upper vowel after a tall consonant    (ป ฝ ฟ ฬ)
//   U+F720+i  tone mark after a tall consonant      (ป่)
//   U+F730+i  tone mark above an upper vowel after a tall consonant
// and splits SARA AM (ำ) into NIKHAHIT + SARA AA (น้ำ -> น ํ ้ า).
#pragma once
#include <stdint.h>
#include <string.h>

namespace thai {
static const char UP[] = {0x31, 0x34, 0x35, 0x36, 0x37, 0x47, 0x4D};  // low byte of U+0E xx
inline int upIndex(uint16_t u) {
  if ((u >> 8) != 0x0E) return -1;
  for (int j = 0; j < 7; j++) if ((u & 0xFF) == (uint8_t)UP[j]) return j;
  return -1;
}
inline int toneIndex(uint16_t u) { return u >= 0x0E48 && u <= 0x0E4C ? u - 0x0E48 : -1; }
inline bool isTall(uint16_t u) { return u == 0x0E1B || u == 0x0E1D || u == 0x0E1F || u == 0x0E2C; }
inline bool isConsonant(uint16_t u) { return u >= 0x0E01 && u <= 0x0E2E; }
// Any Thai letter in the string? (UTF-8 lead bytes E0 B8 / E0 B9)
inline bool needs(const char *s) {
  for (; *s; s++)
    if ((uint8_t)s[0] == 0xE0 && ((uint8_t)s[1] == 0xB8 || (uint8_t)s[1] == 0xB9)) return true;
  return false;
}
inline uint16_t next(const char *&s) {
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
inline int put(char *o, int n, int cap, uint16_t u) {
  if (u < 0x80) { if (n + 1 < cap) o[n++] = (char)u; return n; }
  if (u < 0x800) {
    if (n + 2 < cap) { o[n++] = (char)(0xC0 | (u >> 6)); o[n++] = (char)(0x80 | (u & 0x3F)); }
    return n;
  }
  if (n + 3 < cap) {
    o[n++] = (char)(0xE0 | (u >> 12));
    o[n++] = (char)(0x80 | ((u >> 6) & 0x3F));
    o[n++] = (char)(0x80 | (u & 0x3F));
  }
  return n;
}
inline const char *shape(const char *in, char *out, int cap) {
  int n = 0;
  bool tall = false, hasUp = false;
  const char *s = in;
  while (*s) {
    uint16_t u = next(s);
    int j = upIndex(u), t = toneIndex(u);
    if (isConsonant(u)) { tall = isTall(u); hasUp = false; n = put(out, n, cap, u); continue; }
    if (j >= 0) { n = put(out, n, cap, tall ? 0xF710 + j : u); hasUp = true; continue; }
    if (t >= 0) {
      const char *peek = s;
      bool am = *peek && next(peek) == 0x0E33;  // tone then SARA AM: nikhahit goes first
      if (am) { n = put(out, n, cap, tall ? 0xF710 + 6 : 0x0E4D); hasUp = true; }
      uint16_t v = hasUp ? (tall ? 0xF730 : 0xF700) + t : (tall ? 0xF720 + t : u);
      n = put(out, n, cap, v);
      if (am) { s = peek; n = put(out, n, cap, 0x0E32); }
      continue;
    }
    if (u == 0x0E33) {  // SARA AM -> NIKHAHIT + SARA AA
      n = put(out, n, cap, tall ? 0xF710 + 6 : 0x0E4D);
      n = put(out, n, cap, 0x0E32);
      continue;
    }
    if (u >= 0x0E00 && u <= 0x0E7F) { n = put(out, n, cap, u); continue; }
    tall = hasUp = false;
    n = put(out, n, cap, u);
  }
  out[n < cap ? n : cap - 1] = 0;
  return out;
}
}  // namespace thai
