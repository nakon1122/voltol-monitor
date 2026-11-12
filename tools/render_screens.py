# Render the CYD 3.5" screen previews from the REAL screen code.
#   1. tools/sim/sim_main.cpp (compiled with src/ui.cpp) prints every draw call
#   2. this script replays them pixel by pixel at 480x320 with the board's own
#      VLW glyph bitmaps (include/fonts_vlw.h) and RGB565 colours,
#   3. saves assets/screens/<name>.png at 2x (nearest) + ALL_SCREENS.png.
# Usage: python tools/render_screens.py            (builds the simulator with g++)
import math, re, struct, subprocess, sys, tempfile
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'assets/screens'
W, H = 480, 320

def load_fonts():
    src = (ROOT / 'src/fonts_vlw.cpp').read_text()
    fonts = []
    for m in re.finditer(r'static const uint8_t (\w+)_VLW\[\d+\] = \{(.*?)\};', src, re.S):
        b = bytes(int(x) for x in m.group(2).replace('\n', '').split(',') if x.strip())
        n = struct.unpack('>i', b[:4])[0]
        glyphs, off = {}, 24 + n * 28
        max_asc = 0
        for i in range(n):
            u, h, w, adv, dy, dx, _ = struct.unpack('>7i', b[24 + i * 28:52 + i * 28])
            glyphs[u] = (h, w, adv, dy, dx, np.frombuffer(b[off:off + w * h], np.uint8).reshape(h, w))
            off += w * h
        fonts.append((glyphs, struct.unpack('>i', b[8:12])[0]))
    return fonts

def rgb565(c):
    r, g, b = (c >> 16) & 255, (c >> 8) & 255, c & 255
    r, g, b = r >> 3, g >> 2, b >> 3
    return np.array([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], np.float32)

YY, XX = np.mgrid[0:H, 0:W]

class Canvas:
    def __init__(self, fonts):
        self.px = np.zeros((H, W, 3), np.float32)
        self.fonts = fonts
    def mask(self, m, c): self.px[m] = rgb565(c)
    def rect(self, x, y, w, h, c):
        x0, y0, x1, y1 = max(0, x), max(0, y), min(W, x + w), min(H, y + h)
        if x1 > x0 and y1 > y0: self.px[y0:y1, x0:x1] = rgb565(c)
    def rrect_mask(self, x, y, w, h, r):
        r = max(0, min(r, w // 2, h // 2))
        m = (XX >= x) & (XX < x + w) & (YY >= y) & (YY < y + h)
        cx = np.clip(XX, x + r, x + w - 1 - r); cy = np.clip(YY, y + r, y + h - 1 - r)
        return m & ((XX - cx) ** 2 + (YY - cy) ** 2 <= r * r + r)
    def rrect(self, x, y, w, h, r, c): self.mask(self.rrect_mask(x, y, w, h, r), c)
    def rrect_outline(self, x, y, w, h, r, c):
        self.mask(self.rrect_mask(x, y, w, h, r) & ~self.rrect_mask(x + 1, y + 1, w - 2, h - 2, max(0, r - 1)), c)
    def circle(self, x, y, r, c): self.mask((XX - x) ** 2 + (YY - y) ** 2 <= r * r + r, c)
    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), -abs(y1 - y0); sx = 1 if x0 < x1 else -1; sy = 1 if y0 < y1 else -1
        err = dx + dy; col = rgb565(c)
        while True:
            if 0 <= x0 < W and 0 <= y0 < H: self.px[y0, x0] = col
            if x0 == x1 and y0 == y1: break
            e2 = 2 * err
            if e2 >= dy: err += dy; x0 += sx
            if e2 <= dx: err += dx; y0 += sy
    def tri(self, pts, c):
        im = Image.new('L', (W, H), 0); ImageDraw.Draw(im).polygon(pts, fill=255)
        self.mask(np.array(im) > 0, c)
    def arc(self, x, y, r0, r1, a0, a1, c):
        d2 = (XX - x) ** 2 + (YY - y) ** 2
        m = (d2 >= r0 * r0) & (d2 <= r1 * r1 + r1)
        ang = np.degrees(np.arctan2(YY - y, XX - x)) % 360
        a0m, span = a0 % 360, a1 - a0
        rel = (ang - a0m) % 360
        self.mask(m & (rel <= span) if span < 360 else m, c)
    def text(self, x, y, font, c, s):
        glyphs, size = self.fonts[font]
        col = rgb565(c)
        for ch in s:
            g = glyphs.get(ord(ch))
            if not g: x += size * 2 // 7; continue
            h, w, adv, dy, dx, bm = g
            gx, gy = x + dx, y - dy
            x0, y0, x1, y1 = max(0, gx), max(0, gy), min(W, gx + w), min(H, gy + h)
            if x1 > x0 and y1 > y0:
                a = (bm[y0 - gy:y1 - gy, x0 - gx:x1 - gx].astype(np.float32) + 1) / 257.0
                a[bm[y0 - gy:y1 - gy, x0 - gx:x1 - gx] == 0] = 0
                reg = self.px[y0:y1, x0:x1]
                self.px[y0:y1, x0:x1] = reg * (1 - a[..., None]) + col * a[..., None]
            x += adv
    def image(self): return Image.fromarray(np.clip(self.px, 0, 255).astype(np.uint8))

def replay(lines, fonts):
    screens, cur, name = [], None, None
    for ln in lines:
        p = ln.rstrip('\n').split(' ')
        op = p[0]
        if op == 'SCREEN':
            if cur: screens.append((name, cur.image()))
            cur, name = Canvas(fonts), p[1]; continue
        if op == 'S':
            x, y, f = int(p[1]), int(p[2]), int(p[3]); c = int(p[4], 16)
            cur.text(x, y, f, c, ln.rstrip('\n').split(' ', 5)[5] if len(p) > 5 else ''); continue
        n = [float(v) for v in p[1:-1]]; c = int(p[-1], 16); i = [int(v) for v in n]
        if op == 'R': cur.rect(*i, c)
        elif op == 'RR': cur.rrect(*i, c)
        elif op == 'RO': cur.rrect_outline(*i, c)
        elif op == 'C': cur.circle(*i, c)
        elif op == 'L': cur.line(*i, c)
        elif op == 'T': cur.tri([(i[0], i[1]), (i[2], i[3]), (i[4], i[5])], c)
        elif op == 'A': cur.arc(i[0], i[1], i[2], i[3], n[4], n[5], c)
    if cur: screens.append((name, cur.image()))
    return screens

def render(flags=()):
    with tempfile.TemporaryDirectory() as td:
        exe = Path(td) / 'sim'
        subprocess.run(['g++', '-std=c++17', '-O1', *flags, '-I', str(ROOT / 'include'), '-o', str(exe),
                        str(ROOT / 'tools/sim/sim_main.cpp'), str(ROOT / 'src/ui.cpp'), str(ROOT / 'src/fonts_vlw.cpp'), str(ROOT / 'src/i18n.cpp')], check=True)
        lines = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.splitlines()
        lines += subprocess.run([str(exe), 'en'], check=True, capture_output=True, text=True).stdout.splitlines()
    return replay(lines, load_fonts())

def main():
    OUT.mkdir(parents=True, exist_ok=True)
    shots = render()
    for f in OUT.glob('*.png'):
        if not f.name.startswith(('web_', 's3web_')): f.unlink()
    for name, im in shots:
        im.resize((W * 2, H * 2), Image.NEAREST).save(OUT / f'{name}.png')
    sheet(OUT / 'ALL_SCREENS.png', [x for x in shots if not x[0].startswith('en_')])
    sheet(OUT / 'ALL_SCREENS_EN.png', [x for x in shots if x[0].startswith('en_')])
    # ESP32-S3 4.0" board: same 480x320 screens; pages that differ are saved as s340_<name>.png
    base = dict(shots)
    s40 = render(['-DVOLTOL_S340'])
    for name, im in s40:
        if name in base and np.array_equal(np.asarray(base[name]), np.asarray(im)): continue
        im.resize((W * 2, H * 2), Image.NEAREST).save(OUT / f's340_{name}.png')
    sheet(OUT / 'ALL_SCREENS_S3_40.png', [x for x in s40 if not x[0].startswith('en_')])
    print(f'{len(shots)} screens -> {OUT} (+ S3 4.0")')

def sheet(path, shots):
    # contact sheet
    cols, sw, sh = 4, 480, 320
    rows = math.ceil(len(shots) / cols)
    sheet = Image.new('RGB', (cols * (sw + 16) + 16, rows * (sh + 40) + 16), (24, 28, 34))
    d = ImageDraw.Draw(sheet)
    try: lab = ImageFont.truetype(str(ROOT / 'assets/fonts/Inter-SemiBold.otf'), 16)
    except Exception: lab = None
    for k, (name, im) in enumerate(shots):
        x, y = 16 + (k % cols) * (sw + 16), 16 + (k // cols) * (sh + 40)
        d.text((x, y), name, fill=(200, 210, 220), font=lab)
        sheet.paste(im, (x, y + 24))
    sheet.save(path)

if __name__ == '__main__':
    main()
