"""How the FM-1's panel shows a color: the port of simPx in web/page.html (the darks lifted, the colors turned toward grey, violets bluer).
The web pictures (module pictures, screenshots, the mark and the page's brand colors) go through it, so they match the emulator with
"Match the unit's panel" ticked.  LIFT 0.62, TOP 255, SAT 0.62, PUR 1 are the page's defaults."""
import colorsys, math
LIFT, TOP, SAT, PUR = 0.62, 255, 0.62, 1.0
_lut = [round((255 * math.pow(m / 255, LIFT) - m) * max(0.0, (TOP - m) / (TOP or 1))) for m in range(256)]

def sim(r, g, b, L=_lut, S=SAT, PU=PUR):
    """one color (0..255 floats or ints) -> (R, G, B) floats, unclamped like the page's (the page clamps when it writes pixels)"""
    mx = max(r, g, b)
    k = 1.0 if mx < 48 else 1 - (mx - min(r, g, b)) / mx
    add = L[int((r + g + b) / 3)] * k
    R, G, B = r + add * (1 - r / 255), g + add * (1 - g / 255), b + add * (1 - b / 255)
    if S != 1:
        Se = S
        if PU > 0:
            m0 = max(R, G, B); d0 = m0 - min(R, G, B)
            if d0 > 0.2 * m0:
                h0 = ((G - B) / d0 if m0 == R else 2 + (B - R) / d0 if m0 == G else 4 + (R - G) / d0) * 60
                h0 = (h0 + 360) % 360; w0 = 1 - abs(h0 - 262) / 45
                if w0 > 0: Se = S + (0.98 - S) * min(1, PU) * w0
        y = 0.299 * R + 0.587 * G + 0.114 * B
        R, G, B = y + (R - y) * Se, y + (G - y) * Se, y + (B - y) * Se
    if PU > 0:
        mx2 = max(R, G, B); dd = mx2 - min(R, G, B)
        if dd > 0.2 * mx2:
            h = (G - B) / dd if mx2 == R else 2 + (B - R) / dd if mx2 == G else 4 + (R - G) / dd
            h = ((h * 60) + 360) % 360; w = 1 - abs(h - 262) / 45
            if w > 0:
                h = (h - 35 * PU * w) / 60; sx = dd / mx2; i6 = math.floor(h); f = h - i6
                pp, qq, tt = mx2 * (1 - sx), mx2 * (1 - sx * f), mx2 * (1 - sx * (1 - f))
                R, G, B = (pp, qq, mx2) if i6 == 3 else (tt, pp, mx2) if i6 == 4 else (mx2, pp, qq)
    return R, G, B

def px(c):
    """an (r, g, b) color as the panel shows it, whole numbers 0..255"""
    return tuple(max(0, min(255, round(v))) for v in sim(*c))

def hexc(h):
    h = h.lstrip('#'); return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))

def tohex(c): return '#%02x%02x%02x' % c

def image(im):
    """a PIL RGB image -> the same through the panel (one lookup per distinct color)"""
    from PIL import Image
    im = im.convert('RGB'); cache = {}; data = []
    for c in im.getdata():
        if c not in cache: cache[c] = px(c)
        data.append(cache[c])
    out = Image.new('RGB', im.size); out.putdata(data); return out
