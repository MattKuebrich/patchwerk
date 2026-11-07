#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Builds PATCHWERK's built-in CZ tone bank as a .syx file in assets/cz/ (run once; the result is checked in):
     INTERNAL.syx  64 tones picked from the CZ-230S presets (github.com/boffinry CZ-Presets: 99 tones as 128-byte CZ-101 dumps)
     (an INIT tone first, then the CZ-230S picks, then 16 'PW' tones made from the CZ module's own knob model)
   python3 tools/make_cz_banks.py /path/to/CZ-Presets-master/CZ-230S
   tools/gen_cz_inc.py then turns the two .syx files into app/core/pw_czbank.inc."""
import math, os, re, sys

KW = [0, 31, 44, 57, 70, 83, 96, 111, 146, 255]   # the CZ-101's DCW key follow breakpoints (as web/page.html's parseCZ)

PICK = [  # (preset number, name <= 16 chars): keys and plucks, mallets, basses, strings and pads, winds, organs and voices, percussion and fx
    (50, "PIANO 1"), (51, "PIANO 2"), (53, "HONKY-TONK"), (55, "HARPSICHORD"), (57, "SYNTH CLAVI"), (41, "PEARL DROP"), (47, "PLUNK EXTEND"), (18, "SYNTH HARP"),
    (10, "LIGHT HARP"), (37, "KOTO"), (38, "SHAMISEN"), (70, "ACO GUITAR"), (72, "SEMIACO GUITAR"), (74, "ELEC GUITAR"), (49, "SYNTH SITAR"), (39, "QANUN"),
    (60, "BELLS"), (62, "SYNTH CELESTA"), (64, "SYNTH VIBES"), (66, "BELL LYRA"), (67, "XYLOPHONE"), (68, "SOFT XYLOPHONE"), (89, "STEEL DRUM"), (61, "CARILLON"),
    (76, "ELEC BASS 1"), (77, "ELEC BASS 2"), (78, "SLAP BASS"), (79, "METAL BASS"),
    (16, "SWEET STRINGS"), (6, "STRING ENS"), (3, "SYMPHONIC ENS"), (8, "SYNTH ENS"), (0, "BRASS ENS"), (27, "SPACE VOICE"), (45, "FANTASY"), (12, "SOUTHERN WING"),
    (13, "MAGICAL WIND"), (33, "VIOLIN"), (34, "CELLO"), (17, "LIGHT ATTACK"),
    (30, "TRUMPET"), (31, "FLUTE"), (32, "WHISTLE"), (48, "SLASH REED"), (36, "SHAKUHACHI"), (40, "SYNTH REED"), (42, "DOUBLE REED"), (14, "FUNKY HORN"),
    (24, "ACCORDION"), (43, "MEOW ATTACK"),
    (20, "JAZZ ORGAN"), (22, "PIPE ORGAN"), (25, "FEMALE CHORUS"), (26, "MALE CHORUS"), (29, "WAH VOICE"),
    (80, "SYNTH DRUMS"), (83, "SYNTH CLAPPER"), (85, "COWBELL"), (86, "CONGA"), (87, "TABLA"), (91, "JET ROAR"), (96, "COMPUTER GAME"), (97, "LASER GUN"), (99, "SWEEP"),
]


def nibbles(b):
    out = bytearray()
    for x in b:
        out += bytes((x & 15, x >> 4))
    return bytes(out)


def message(tone, i):
    return bytes([0xF0, 0x44, 0x00, 0x00, 0x70, 0x21, 0x20 + (i & 15)]) + nibbles(tone) + b"\xF7"


def from_cz01(d, name):   # a CZ-101 dump (256 nibbles) -> the CZ's 144 bytes: the key follow derived, the name added
    n = d.index(0xF7)
    p = n - 256
    b = bytearray(144)
    for k in range(128):
        b[k] = (d[p + 2 * k] & 15) | ((d[p + 2 * k + 1] & 15) << 4)
    for o in (14, 71):
        k = min(range(len(KW)), key=lambda q: abs(KW[q] - b[o + 5]))
        b[o + 4] = (b[o + 4] & 0xF0) | k
    b[128:144] = name.upper().ljust(16)[:16].encode("ascii")
    return bytes(b)


def valid(b):
    ok = not (b[0] & 0xF0) and (b[0] >> 2) <= 2 and b[1] <= 1 and b[3] <= 47
    for o in (0, 57):
        ok = ok and (b[16 + o] & 15) <= 9 and (b[16 + o] >> 4) <= 14 and b[18 + o] <= 9
    return ok


# ---- the knob model (pw_modules.c: cz_custom), for the USER bank ----
def cz_rate(raw, e):
    chip = (8 | (raw & 7)) << ((raw & 127) >> 3)
    return chip * 40000.0 * ((1.0 / (8 * 131072.0)) if e == 0 else (1.0 / (4 * 16384.0)) if e == 1 else (1.0 / (2 * 131072.0)))


def cz_raw(per_s, e):
    c = per_s / cz_rate(0, e) * 8.0
    if not c > 8.0:
        return 0
    ex = max(0, min(15, int(math.floor(math.log2(c))) - 3))
    mt = int(c / (1 << ex) + 0.5) - 8
    if mt > 7:
        if ex < 15:
            ex += 1
            mt = 0
        else:
            mt = 7
    return ex * 8 + max(0, mt)


def cz_level(x):
    if not x > 0.003:
        return 0
    l = 127.0 + 9.81 * math.log2(min(x, 1.0))
    return int(max(1.0, l))


WMAP = [(0, 0), (1, 0), (2, 0), (4, 0), (5, 0), (6, 1), (6, 2), (6, 3)]


def knob_tone(name, w1, w2=0, dcw=0.6, d=(0.005, 0.6, 0.3, 0.3), a=(0.005, 1.0, 0.7, 0.3), pit=(0, 0.1), vib=0.0, line=0, detune=0.0, mod=0, octave=0):
    b = bytearray(144)
    win = WMAP[w1][1] or (WMAP[w2 - 1][1] if w2 else 0)
    word = WMAP[w1][0] << 13 | ((512 | WMAP[w2 - 1][0] << 10) if w2 else 0) | win << 6 | mod << 3
    b[14], b[15] = word >> 8, word & 255
    sl = cz_level(a[2])
    ls = [127, sl, 0]
    rs = [127.0 / max(a[0], 0.001), (127.0 - sl) / max(a[1], 0.01), max(sl, 8) / max(a[3], 0.01)]
    b[20] = 2
    for i in range(3):
        b[21 + 2 * i], b[22 + 2 * i] = cz_raw(rs[i], 2), ls[i]
    b[24] |= 128
    pk = int(max(0.0, min(1.0, dcw)) * 127 + 0.5)
    ps = int(pk * max(0.0, min(1.0, d[2])) + 0.5)
    ls = [pk, ps, 0]
    rs = [pk * 8.0 / max(d[0], 0.001), (pk - ps) * 8.0 / max(d[1], 0.01), max(ps, 1) * 8.0 / max(d[3], 0.01)]
    b[37] = 2
    for i in range(3):
        b[38 + 2 * i], b[39 + 2 * i] = cz_raw(rs[i], 1), ls[i]
    b[41] |= 128
    if pit[0] > 0.05:
        amt = max(0.0, min(24.0, pit[0]))
        b[54], b[55] = 1, 127
        b[56] = int(amt * 16 + 0.5) if amt <= 3.9375 else 64 + int(amt * 0.5 + 0.5)
        b[57] = cz_raw(amt / max(pit[1], 0.005), 0)
    b[4], b[6], b[9], b[10] = 8, 20, 0x60, 0x0A
    b[12] = int(2 + 98 * vib) if vib > 0.01 else 1
    b[71:128] = b[14:71]
    b[0] = (line & 3) | ({0: 0, 12: 1, -12: 2}[octave] << 2)
    if detune:
        b[1] = 1 if detune < 0 else 0
        b[2] = (int(abs(detune) / 100.0 * 64) & 63) << 2
    b[128:144] = name.upper().ljust(16)[:16].encode("ascii")
    assert valid(b), name
    return bytes(b)


USER = [
    knob_tone("PW SAW BASS", 0, 0, 0.35, (0.005, 0.3, 0.15, 0.2), (0.005, 0.5, 0.4, 0.1)),
    knob_tone("PW GLASS KEYS", 3, 0, 0.5, (0.005, 0.9, 0.2, 0.5), (0.005, 1.6, 0.25, 0.4), vib=0.1),
    knob_tone("PW SQUARE LEAD", 1, 0, 0.45, (0.01, 0.5, 0.6, 0.2), (0.01, 0.3, 0.9, 0.15), vib=0.35, line=2, detune=7),
    knob_tone("PW RESO PAD", 5, 0, 0.7, (0.6, 1.0, 0.6, 1.2), (0.5, 1.0, 0.8, 1.5), vib=0.2, line=2, detune=9),
    knob_tone("PW PLUCK", 2, 0, 0.5, (0.002, 0.18, 0.05, 0.15), (0.002, 0.45, 0.0, 0.2)),
    knob_tone("PW BRASS", 4, 0, 0.6, (0.08, 0.4, 0.7, 0.2), (0.05, 0.4, 0.9, 0.2), vib=0.2, line=2, detune=5),
    knob_tone("PW BELL", 3, 7, 0.8, (0.002, 2.5, 0.0, 2.0), (0.002, 3.0, 0.0, 2.0), pit=(0.3, 0.05)),
    knob_tone("PW KICK", 0, 0, 0.25, (0.002, 0.12, 0.0, 0.1), (0.002, 0.28, 0.0, 0.1), pit=(18, 0.08), octave=-12),
    knob_tone("PW SNARE", 1, 0, 0.8, (0.002, 0.15, 0.0, 0.1), (0.002, 0.22, 0.0, 0.1), pit=(5, 0.05), mod=3),
    knob_tone("PW ACID", 0, 0, 0.3, (0.002, 0.2, 0.1, 0.1), (0.002, 0.3, 0.7, 0.1), octave=-12),
    knob_tone("PW CHOIR", 6, 0, 0.55, (0.5, 0.8, 0.7, 0.9), (0.45, 1.0, 0.85, 1.0), vib=0.3, line=2, detune=11),
    knob_tone("PW WOBBLE", 0, 0, 0.5, (0.01, 0.6, 0.6, 0.3), (0.01, 0.5, 0.9, 0.2), vib=0.9, octave=-12),
    knob_tone("PW ORGAN", 4, 0, 0.3, (0.003, 0.1, 1.0, 0.08), (0.003, 0.1, 1.0, 0.08), line=3, detune=0.0),
    knob_tone("PW ZAP", 5, 0, 0.9, (0.002, 0.2, 0.0, 0.1), (0.002, 0.3, 0.0, 0.1), pit=(24, 0.15)),
    knob_tone("PW SWEEP PAD", 0, 5, 0.8, (3.0, 2.0, 0.4, 2.5), (1.5, 1.0, 0.85, 2.0), vib=0.15, line=2, detune=6),
    knob_tone("PW E PIANO", 3, 0, 0.45, (0.003, 1.2, 0.15, 0.4), (0.003, 1.8, 0.2, 0.35), vib=0.05, line=2, detune=4),
]


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "."
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "cz")
    os.makedirs(here, exist_ok=True)
    idx = open(os.path.join(src, "CZ230Sindex.txt")).read()
    internal = []
    for n, name in PICK:
        d = open(os.path.join(src, "presets", "%02d.syx" % n), "rb").read()
        t = from_cz01(d, name)
        assert valid(t), (n, name)
        internal.append(t)
    init = knob_tone("INIT", 0, 0, 0.5, (0.005, 0.5, 0.6, 0.3), (0.005, 0.5, 0.8, 0.3))   # a plain saw: the place to start your own sound
    tones = [init] + internal + USER
    assert len(tones) <= 128
    fn = "INTERNAL.syx"
    with open(os.path.join(here, fn), "wb") as f:
        for i, t in enumerate(tones):
            f.write(message(t, i))
    print(fn, len(tones), "tones")
    if os.path.exists(os.path.join(here, "USER.syx")): os.remove(os.path.join(here, "USER.syx"))


if __name__ == "__main__":
    main()
