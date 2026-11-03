# Build anti-aliased VLW fonts (LovyanGFX "smooth fonts") from Inter.
# Output: include/fonts_vlw.h  (also read back by tools/render_screens.py so the
# preview PNGs use exactly the same glyph bitmaps as the board).
import struct
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FD = ROOT / 'assets/fonts'
ASCII = ''.join(chr(c) for c in range(32, 127)) + '°Δ●○★·±−—'
DIGITS = '0123456789.-s '
UPPER = ' 0123456789.-sABCDEFGHIJKLMNOPQRSTUVWXYZ'
# Thai (Noto Sans Thai, variable font) is merged into the small text fonts.
# The board cannot shape Thai, so marks get extra pre-positioned copies in the
# private-use area; include/thai_shape.h picks them (same table as below).
THAI_FONT = FD / 'NotoSansThai-VF.ttf'
THAI_WEIGHT = {'Medium': 500, 'SemiBold': 600, 'Bold': 700, 'ExtraBold': 800}
THAI_IN = {'F10', 'F11', 'F11B', 'F13', 'F13B', 'F15B'}
THAI = [chr(c) for c in range(0x0E01, 0x0E5C) if not 0x0E3B <= c <= 0x0E3E]
UP = '\u0E31\u0E34\u0E35\u0E36\u0E37\u0E47\u0E4D'   # upper vowels / nikhahit
TONE = '\u0E48\u0E49\u0E4A\u0E4B\u0E4C'             # tone marks + thanthakhat
# PUA: F700+i tone raised · F710+j upper vowel left · F720+i tone left · F730+i tone raised+left
# id: (name, weight file, px, charset); Thai is added to the fonts in THAI_IN
FONTS = [
    ('F10', 'SemiBold', 10, ASCII),
    ('F11', 'Medium', 11, ASCII),
    ('F11B', 'Bold', 11, ASCII),
    ('F13', 'Medium', 13, ASCII),
    ('F13B', 'Bold', 13, ASCII),
    ('F15B', 'Bold', 15, ASCII),
    ('F18B', 'ExtraBold', 18, ASCII),
    ('F24B', 'ExtraBold', 24, ASCII),
    ('F38B', 'ExtraBold', 38, ASCII),
    ('F56B', 'ExtraBold', 56, UPPER),
    ('F72B', 'ExtraBold', 72, DIGITS),
    ('F86B', 'ExtraBold', 86, DIGITS),
    ('F150B', 'ExtraBold', 150, DIGITS),
]

def ink(f, text, px, dx=0, dy=0):
    # glyph bitmap with the pen at (0, baseline): returns (bitmap, x0, top)
    P = px * 2
    im = Image.new('L', (P * 4, P * 4), 0)
    ImageDraw.Draw(im).text((P + dx, P * 2 + dy), text, font=f, fill=255, anchor='ls')
    bb = im.getbbox()
    if not bb: return None
    return im.crop(bb), bb[0] - P, bb[1] - P * 2

def thai_font(weight, px, shaped):
    f = ImageFont.truetype(str(THAI_FONT), px, layout_engine=ImageFont.Layout.RAQM if shaped else ImageFont.Layout.BASIC)
    f.set_variation_by_axes([THAI_WEIGHT[weight], 100])
    return f

def ctx_mark(weight, px, prefix, mark):
    # Where the font's own shaping puts `mark` after `prefix`, relative to the
    # pen at the end of `prefix`. The difference image only tells WHERE (parts
    # overlapping the vowel cancel out), so the complete stand-alone glyph is
    # slid over it and placed at the best match.
    import numpy as np
    f = thai_font(weight, px, True)
    P = px * 3
    def draw(t):
        im = Image.new('L', (P * 4, P * 3), 0)
        ImageDraw.Draw(im).text((P, P * 2), t, font=f, fill=255, anchor='ls')
        return np.asarray(im, dtype=np.int32)
    D = np.clip(draw(prefix + mark) - draw(prefix), 0, 255)
    base = ink(thai_font(weight, px, False), mark, px)
    if base is None or D.max() == 0: return base
    g, gx, gy = base
    G = np.asarray(g, dtype=np.int32)
    pen = round(P + f.getlength(prefix))
    ys, xs = np.nonzero(D)
    cy, cx = int(ys.mean()), int(xs.mean())
    best, bo = -1, (0, 0)
    for oy in range(-px, px + 1):
        for ox in range(-px, px + 1):
            X = cx - G.shape[1] // 2 + ox; Y = cy - G.shape[0] // 2 + oy
            if X < 0 or Y < 0 or Y + G.shape[0] > D.shape[0] or X + G.shape[1] > D.shape[1]: continue
            sc = (D[Y:Y + G.shape[0], X:X + G.shape[1]] * G).sum()
            if sc > best: best, bo = sc, (X, Y)
    return g, bo[0] - pen, bo[1] - P * 2

def vlw(weight, px, chars, name=''):
    f = ImageFont.truetype(str(FD / f'Inter-{weight}.otf'), px)
    asc, desc = f.getmetrics()
    glyphs, bitmaps = [], []
    def add(code, res, adv):
        if res is None: glyphs.append((code, 1, 1, adv, 0, 0)); bitmaps.append(b'\0'); return
        im, x0, top = res
        glyphs.append((code, im.height, im.width, adv, -top, x0)); bitmaps.append(im.tobytes())
    for ch in sorted(set(chars), key=ord):
        if ch == ' ': add(32, None, round(f.getlength(' '))); continue
        add(ord(ch), ink(f, ch, px), round(f.getlength(ch)))
    if name in THAI_IN:
        tpx = max(px, 11)  # Thai is unreadable below 11 px
        t = thai_font(weight, tpx, False)
        BELOW = '\u0E38\u0E39\u0E3A'
        K, TALL = '\u0E01', '\u0E1B'   # ko kai (normal), po pla (tall)
        for ch in THAI:
            if ch in UP or ch in TONE or ch in BELOW:
                add(ord(ch), ctx_mark(weight, tpx, K, ch), 0)
            else:
                add(ord(ch), ink(t, ch, tpx), round(t.getlength(ch)))
        for i, m in enumerate(TONE):
            add(0xF700 + i, ctx_mark(weight, tpx, K + '\u0E37', m), 0)       # above an upper vowel (tallest: ื)
            add(0xF720 + i, ctx_mark(weight, tpx, TALL, m), 0)                # after a tall consonant
            add(0xF730 + i, ctx_mark(weight, tpx, TALL + '\u0E37', m), 0)    # both
        for j, m in enumerate(UP):
            add(0xF710 + j, ctx_mark(weight, tpx, TALL, m), 0)
    order = sorted(range(len(glyphs)), key=lambda i: glyphs[i][0])
    glyphs = [glyphs[i] for i in order]; bitmaps = [bitmaps[i] for i in order]
    out = struct.pack('>6i', len(glyphs), 11, px, 0, asc, desc)
    for u, h, w, adv, dy, dx in glyphs:
        out += struct.pack('>7i', u, h, w, adv, dy, dx, 0)
    return out + b''.join(bitmaps)

def main():
    head = ['// Generated by tools/make_fonts.py from Inter + Noto Sans Thai (SIL OFL 1.1, assets/fonts). Do not edit.',
            '#pragma once', '#include <stdint.h>',
            'enum FontId { ' + ', '.join(f[0] for f in FONTS) + ', FONT_COUNT };',
            'extern const uint8_t *const FONT_VLW[FONT_COUNT];']
    lines = ['// Generated by tools/make_fonts.py from Inter + Noto Sans Thai (SIL OFL 1.1, assets/fonts). Do not edit.',
             '#include "fonts_vlw.h"']
    total = 0
    for name, w, px, cs in FONTS:
        b = vlw(w, px, cs, name); total += len(b)
        lines.append(f'static const uint8_t {name}_VLW[{len(b)}] = {{')
        for i in range(0, len(b), 32):
            lines.append(','.join(str(x) for x in b[i:i + 32]) + ',')
        lines.append('};')
    lines.append('const uint8_t *const FONT_VLW[FONT_COUNT] = { ' + ', '.join(f'{f[0]}_VLW' for f in FONTS) + ' };')
    (ROOT / 'include/fonts_vlw.h').write_text('\n'.join(head) + '\n')
    (ROOT / 'src/fonts_vlw.cpp').write_text('\n'.join(lines) + '\n')
    print(f'fonts: {total/1024:.0f} KB')

if __name__ == '__main__':
    main()
