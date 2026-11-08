"""PATCHWERK's logo for the README: the power-on mark (five cables) over the name, on the screen's black.

    python3 tools/gen_logo.py      -> brand/logo.png
"""
import json, pathlib
from PIL import Image, ImageDraw, ImageFont

root = pathlib.Path(__file__).resolve().parent.parent
brand = json.load(open(root / 'brand' / 'brand.json'))
rgb = lambda h: tuple(int(h.lstrip('#')[i:i + 2], 16) for i in (0, 2, 4))
CABLES = [rgb(brand['accent'] if k == 'accent' else brand['cats'][k]) for k in brand['cables']]


def cdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def mark(scale):   # the mark as pw_ui.c's draw_boot draws it (x 24..215, y 46..113 on the FM-1)
    X0, Y0_, W, H = 24, 46, 192, 68
    im = Image.new('RGBA', (int(W * scale) + 1, int(H * scale) + 1), (0, 0, 0, 0)); d = ImageDraw.Draw(im)

    def fill(x, y, w, h, c):
        x0, y0 = int((x - X0) * scale), int((y - Y0_) * scale)
        d.rectangle([x0, y0, x0 + max(1, int(w * scale)) - 1, y0 + max(1, int(h * scale)) - 1], fill=c)
    TO = [3, 4, 0, 1, 2]
    XA, XB, Y0, DY = 30, 210, 52, 14
    N = XB - XA
    for i, c in enumerate(CABLES):
        ya, yb = Y0 + i * DY, Y0 + TO[i] * DY
        for t in range(0, N + 1, 2):
            s_ = 3 * t * t * N - 2 * t * t * t
            fill(XA + t, ya + cdiv((yb - ya) * s_, N * N * N) - 1, 2, 3, c)
    for i, c in enumerate(CABLES):
        for side in range(2):
            x = (XB if side else XA) - 6; y = Y0 + (TO[i] if side else i) * DY - 6
            fill(x + 2, y + 2, 8, 8, c)
    return im


W, H = 720, 390
img = Image.new('RGB', (W, H), (0, 0, 0))
m = mark(3)
img.paste(m, ((W - m.width) // 2, 30), m)
d = ImageDraw.Draw(img)
bold = ImageFont.truetype(str(root / 'base/assets/fonts/BarlowSemiCondensed-Bold.ttf'), 84)
small = ImageFont.truetype(str(root / 'base/assets/fonts/BarlowSemiCondensed-SemiBold.ttf'), 30)
d.text((W / 2, 290), 'PATCHWERK', font=bold, fill=(255, 255, 255), anchor='mm')
d.text((W / 2, 356), 'a modular synth for the FM-1', font=small, fill=(150, 150, 140), anchor='mm')
(root / 'brand').mkdir(exist_ok=True)
img.save(root / 'brand' / 'logo.png', optimize=True)
print('brand/logo.png', img.size)
