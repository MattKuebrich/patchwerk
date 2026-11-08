#!/bin/sh
# The screen pictures for the page, README and reference, taken raw (the colors the firmware sends, no panel lift):
# web/shots/*.png (480x480, 2x, sharp) and docs/img/settings.png. Needs the host build and Pillow.
#   sh tools/gen_shots.sh
set -e
cd "$(dirname "$0")/.."
[ -x app/build/host/patchwerk_host ] || ./build.sh --host >/dev/null
out=$(mktemp -d)
app/build/host/patchwerk_host tests/scenarios/web_shots.txt "$out" | tail -1
app/build/host/patchwerk_host tests/scenarios/web_shots2.txt "$out" | tail -1
python3 - "$out" <<'PY'
import sys
sys.path.insert(0, 'tools')
import panel_look   # the pictures show the screen as the unit's panel does (the emulator's "Match the unit's panel")
from PIL import Image
out = sys.argv[1]
for n, dst in [('add', 'web/shots'), ('cable', 'web/shots'), ('knobs', 'web/shots'), ('matrix', 'web/shots'), ('patches', 'web/shots'),
               ('perform', 'web/shots'), ('seq', 'web/shots'), ('song', 'web/shots'), ('mutes', 'web/shots'), ('mutate', 'web/shots'), ('groups', 'web/shots'), ('settings', 'docs/img')]:
    im = panel_look.image(Image.open(f'{out}/{n}.png').convert('RGB'))
    im.resize((im.width * 2, im.height * 2), Image.NEAREST).save(f'{dst}/{n}.png', optimize=True)
print('12 pictures')
PY
app/build/host/patchwerk_host tests/scenarios/og_shots.txt "$out" | tail -1
python3 - "$out" <<'PY'
import sys
sys.path.insert(0, 'tools')
import panel_look
from PIL import Image
for n in ('splash', 'mseq'): panel_look.image(Image.open(f'{sys.argv[1]}/{n}.png').convert('RGB')).save(f'brand/{n}.png')
PY
python3 tools/gen_logo.py && python3 tools/gen_og.py
rm -rf "$out"
