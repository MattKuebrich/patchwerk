#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""RGB565 screens (tests/gen_vis.mjs) -> small palette PNGs, one per module, for the web page.
   python3 tests/vis_png.py web/vis_raw web/vis      (needs Pillow)"""
import json, os, re, struct, sys
from PIL import Image
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import panel_look   # the pictures show the screen as the unit's panel does (the emulator's "Match the unit's panel")

src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
manifest = json.load(open(os.path.join(src, 'manifest.json')))
CROP = (0, 18, 240, 176)    # the module page's strip (its title row begins at y 19) and its picture, ending above the knob row's caption (y 177)
total = 0
for m in manifest:
    raw = open(os.path.join(src, f"{m['i']}.bin"), 'rb').read()
    px = struct.unpack('<57600H', raw)
    img = Image.new('RGB', (240, 240))
    img.putdata([(((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31) for v in px])
    img = panel_look.image(img.crop(CROP)).quantize(colors=48, method=Image.MEDIANCUT, dither=Image.NONE)
    img = img.resize((img.width * 3, img.height * 3), Image.NEAREST)   # (3x, so the page's big view stays sharp: no smoothing of 240 px up to a dialog)
    name = re.sub(r'[^A-Za-z0-9]+', '_', m['name']).strip('_') + '.png'   # the page looks them up by this
    img.save(os.path.join(dst, name), optimize=True)
    total += os.path.getsize(os.path.join(dst, name))
print(f"{len(manifest)} pictures, {total // 1024} KB")
