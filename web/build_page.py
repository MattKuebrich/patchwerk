"""Assemble emulator.html (next to README.md): embed the WebAssembly build (base64) and the
plain-JavaScript build of the same firmware (wasm2js) for pages without wasm."""
import base64, pathlib, re
here = pathlib.Path(__file__).parent
w = base64.b64encode((here/'build/patchwerk.wasm').read_bytes()).decode()
jsf = here/'build/patchwerk.wasm2js.mjs'
if jsf.exists():
    js = jsf.read_text()
    cut = js.index('var retasmFunc = asmFunc(')
    body = js[:cut]
    assert 'import ' not in body.split('function asmFunc')[0], 'unexpected imports'
    factory = 'function PATCHWERK_JS_FACTORY() {\n' + body + '\nreturn asmFunc({});\n}\n'
else:   # no wasm2js (binaryen) build: the page runs on WebAssembly only
    print('warning: web/build/patchwerk.wasm2js.mjs missing (run web/build.sh with binaryen): no plain-JS fallback in this page')
    factory = 'function PATCHWERK_JS_FACTORY() { throw new Error("this page has no plain-JavaScript engine"); }\n'
base_web = here.parent / 'base' / 'web'
ota = ''
for name in ('fm1pkg.js', 'fm1ota.js'):
    ota += re.sub(r'^export\s+', '', (base_web / name).read_text(), flags=re.M) + '\n'
import os
html = (here/os.environ.get('PATCHWERK_PAGE', 'page.html')).read_text()
html = html.replace('/*__FM1OTA__*/', ota)
html = html.replace('/*__SEA_JS__*/', (here/'sea.js').read_text())   # the SEA codec (trim, compress, decode: core/pw_sea.inc is its firmware side)
ver = re.search(r'#define PW_VERSION "([^"]+)"', (here.parent/'app/core/pw_ui.h').read_text()).group(1)
html = html.replace('__PW_VERSION__', ver)
def cz_names(fn):   # the tone names of assets/cz/<fn> (144-byte tones as 288 nibbles: tools/make_cz_banks.py)
    d = (here.parent/'assets'/'cz'/fn).read_bytes(); out = []; i = 0
    while i < len(d):
        j = d.index(0xF7, i); n = d[i + 7:j]
        out.append(bytes((n[2*k] & 15) | ((n[2*k+1] & 15) << 4) for k in range(128, 144)).decode('ascii').strip()); i = j + 1
    return out
import json as _j
html = html.replace('/*__CZ_BUILTIN__*/[]', _j.dumps(cz_names('INTERNAL.syx')))
def dx_names():   # the voice names of assets/dx/INTERNAL.syx (a 32-voice TX81Z dump: tools/make_dx_bank.py)
    d = (here.parent/'assets'/'dx'/'INTERNAL.syx').read_bytes()[6:6 + 4096]
    return [d[k*128 + 57:k*128 + 67].decode('ascii').strip() for k in range(32)]
html = html.replace('/*__DX_BUILTIN__*/[]', _j.dumps(dx_names()))
html = html.replace('/*__FACTORY_BANK__*/[]', (here/'factory_bank.json').read_text().strip())
import json
vis = {f.stem: 'data:image/png;base64,' + base64.b64encode(f.read_bytes()).decode() for f in sorted((here/'vis').glob('*.png'))} if (here/'vis').is_dir() else {}
html = html.replace('/*__VIS_IMAGES__*/{}', json.dumps(vis))
for f in sorted((here/'shots').glob('*.png')) if (here/'shots').is_dir() else []:   # the FM-1's screens in the page's guide (renders of the firmware's own screen)
    html = html.replace('__SHOT_' + f.stem + '__', 'data:image/png;base64,' + base64.b64encode(f.read_bytes()).decode())
fw = [f for f in (here.parent/'app/build').glob('patchwerk*.fwsc')] if (here.parent/'app/build').is_dir() else []
if fw:   # the firmware you last built is its own file beside the page, named with its version (patchwerk-v1.0.1.fwsc); the page fetches it and checks its SHA-256
    import hashlib, shutil
    fw.sort(key=lambda f: f.stat().st_mtime)
    data = fw[-1].read_bytes()
    pkg_name = 'patchwerk-' + ver + '.fwsc'
    meta = json.dumps({'pkg': pkg_name, 'sha256': hashlib.sha256(data).hexdigest(), 'version': ver})
    html = html.replace("'/*__FWSC__*/'", "'" + meta + "'")
    (here/'build').mkdir(exist_ok=True)
    for d in (here/'build', here.parent):   # next to the build files and next to emulator.html
        for old in list(d.glob('patchwerk*.fwsc')) + list(d.glob('patchwerk*.fwsc.js')):   # (an older version's file, or the one before names had versions: one firmware beside the page)
            old.unlink()
        (d/pkg_name).write_bytes(data)
        (d/(pkg_name + '.js')).write_text('window.PW_FWSC_B64="' + base64.b64encode(data).decode() + '";\n')   # (the same firmware as a script: a page opened from a folder cannot fetch a file, but it can load a script)
    print('firmware beside the page:', fw[-1].name, len(data), 'bytes')
html = html.replace('/*__ENGINE_JS__*/', factory).replace('__WASM_B64__', w)

# PATCHWERK's mark as the FM-1 draws it at power-on (core/pw_ui.c draw_boot): five cables drooping between their plugs,
# drawn here pixel for pixel (half size for the page's header, a 32 px one for the tab), kept sharp by the browser
import io
from PIL import Image, ImageDraw
import json
BRAND = json.load(open(here / 'brand.json'))   # brand/brand.json through tools/gen_brand.py: the one place the colors are chosen
_rgb = lambda h: tuple(int(h.lstrip('#')[i:i + 2], 16) for i in (0, 2, 4))
CABLES = [_rgb(BRAND['accent'] if k == 'accent' else BRAND['cats'][k]) for k in BRAND['cables']]   # the mark's cables (the power-on screen's)
BRAND_CSS = ('--accent: %s; --ok: %s; --warn: %s; --bad: %s;\n  --c-source: %s; --c-filter: %s; --c-fx: %s; --c-mod: %s;\n  --c-util: %s; --c-seq: %s; --c-io: %s;'
             % ((BRAND['accent'], BRAND['ok'], BRAND['warn'], BRAND['bad']) + tuple(BRAND['cats'][k] for k in ('source', 'filter', 'fx', 'mod', 'util', 'seq', 'io'))))
def cdiv(a, b):   # C's integer division (toward zero)
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q
def boot_cables(scale):
    """the boot screen's mark as pw_ui.c's draw_boot draws it (x 24..215, y 46..113 on the FM-1), at scale 1 or 0.5"""
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
def icon32():
    im = Image.new('RGBA', (32, 32), (14, 15, 18, 255)); d = ImageDraw.Draw(im)
    TO = [3, 4, 0, 1, 2]
    for i, c in enumerate(CABLES):
        ya, yb = 6 + i * 4, 6 + TO[i] * 4
        for x in range(5, 27):
            t = (x - 5) / 21.0; k = t * t * (3 - 2 * t)
            y = round(ya + (yb - ya) * k)
            d.rectangle([x, y, x, y + 1], fill=c)
        d.rectangle([2, ya - 1, 4, ya + 2], fill=c); d.rectangle([27, yb - 1, 29, yb + 2], fill=c)
    return im
def data_uri(im):
    b = io.BytesIO(); im.save(b, 'PNG'); return 'data:image/png;base64,' + base64.b64encode(b.getvalue()).decode()
mark = boot_cables(1)
html = html.replace('/*__BRAND_CSS__*/', BRAND_CSS).replace('__BRAND_ACCENT__', BRAND['accent']).replace('__BRAND_NAME__', BRAND['name'])
html = html.replace('__MARK_PNG__', data_uri(mark)).replace('__MARK_W__', str(mark.width)).replace('__MARK_H__', str(mark.height))
html = html.replace('__ICON_PNG__', data_uri(icon32()))
(here.parent/'emulator.html').write_text(html)
(here.parent/'index.html').write_text(html)   # (the same page under the name GitHub Pages serves)
print('emulator.html', len(html))
