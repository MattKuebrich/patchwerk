"""PATCHWERK's logo for the README and the link preview: the device's own power-on screen (no version), cropped and enlarged.

    ./build.sh --host && printf 'wait 2500\nexpect logo 1\nshot logo.png\n' > /tmp/logo.txt && mkdir -p /tmp/lg && \
      app/build/host/patchwerk_host /tmp/logo.txt /tmp/lg && cp /tmp/lg/logo.png brand/splash.png
    python3 tools/gen_logo.py      -> brand/logo.png (and brand/og.png via tools/gen_og.py)
"""
import pathlib
from PIL import Image, ImageChops

root = pathlib.Path(__file__).resolve().parent.parent
im = Image.open(root / 'brand' / 'splash.png').convert('RGB')
bb = ImageChops.difference(im, Image.new('RGB', im.size, im.getpixel((0, 0)))).getbbox()
pad = 14
bb = (max(0, bb[0] - pad), max(0, bb[1] - pad), min(im.width, bb[2] + pad), min(im.height, bb[3] + pad))
S = 4
out = im.crop(bb).resize(((bb[2] - bb[0]) * S, (bb[3] - bb[1]) * S), Image.NEAREST)
out.save(root / 'brand' / 'logo.png', optimize=True)
print('brand/logo.png', out.size)
