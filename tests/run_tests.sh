#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# PATCHWERK's tests: the engine on the host, the app in fm1-base's simulator, the live link between two engines,
# the browser emulator. (The browser tests: tests/*.py with Playwright, the page served from web/build on :8766.)
set -e
cd "$(dirname "$0")/.."
mkdir -p tests/out
cc -std=c11 -O2 -Wall -Wextra -DPW_HOST -Iapp/core tests/host_test.c app/core/pw_engine.c app/core/pw_modules.c \
   app/core/pw_presets.c -lm -o tests/out/host_test
(cd tests && ./out/host_test | tail -1)
cc -std=c11 -O2 -Wall -Wextra -DPW_HOST -Iapp/core tests/bank_test.c app/core/pw_bank.c app/core/pw_engine.c app/core/pw_modules.c \
   app/core/pw_presets.c -lm -o tests/out/bank_test
(cd tests && ./out/bank_test | tail -1)
cc -std=c11 -O2 -Wall -Wextra -Ibase/bsp/chip tests/ota_check_test.c -o tests/out/ota_check_test
(cd tests && ./out/ota_check_test | tail -1)   # (the interrupted-install check, against the real header entries)
cc -std=c11 -O2 -Wall -Wextra -Wno-unused-function -Ibase/bsp/chip tests/ota_ident_test.c -o tests/out/ota_ident_test
./tests/out/ota_ident_test | tail -1   # (the handshake: a short identity for M-UPGRADE 3.11, the whole one for our installers)
node tests/ident_test.mjs | tail -1
python3 tests/ident_py_test.py | tail -1
python3 tests/ota_header_test.py | tail -1   # (the build's mirror of that check)
python3 tests/make_bank.py >/dev/null
./build.sh --host >/dev/null
fails=0
sh tools/gen_factory.sh --check || fails=1   # (the packed factory patches are what their source gives)
sh tools/gen_brand.sh --check || fails=1   # (the brand colors in the C headers and the page are what brand/brand.json gives)
python3 tools/gen_examples.py --check || fails=1   # (the packed example texts are what their source gives)
python3 tools/gen_cz_inc.py --check || fails=1   # (the CZ banks in the firmware are what the .syx files in assets/cz say)
python3 tools/make_dx_bank.py --check || fails=1   # (FM-DX's INTERNAL.syx is what tools/dx_voices.py says)
node tools/gen_internal.mjs --check || fails=1   # (the INTERNAL sample bank in the firmware is what web/factory_bank.json gives)
node tools/gen_break.mjs --check || fails=1   # (the SAMPLER's starting sound, from web/break_src.json)
python3 tools/gen_dx_inc.py --check || fails=1   # (the FM-DX bank in the firmware is what that .syx says)
for s in tests/scenarios/*.txt; do
    n=$(basename "$s" .txt); mkdir -p "app/build/scenarios/$n"
    r=$(app/build/host/patchwerk_host "$s" "app/build/scenarios/$n" | tail -1)
    echo "$n: $r"
    case "$r" in *FAILED*) fails=1 ;; esac
done
if command -v clang >/dev/null; then sh web/build_wasm.sh >/dev/null 2>&1 || { echo "web/build_wasm.sh FAILED (the wasm builds use -Werror)"; fails=1; }; fi
if [ -f web/build/patchwerk.wasm ] && command -v node >/dev/null; then
    node tests/link_test.mjs | tail -1
    node tests/user_pal_test.mjs || fails=1   # (the USER palette: kept in the settings, read back, removed)
    node tests/midi_set_test.mjs || fails=1   # (MIDI clock out, program change, the settings as a backup)
    node tests/sea_test.mjs || fails=1   # (web/sea.js: the decoder against the reference decoder, the encoder against its fixture)
    node tests/help_fit.mjs || fails=1
    (cd tests && mkdir -p lcd && node patchview_test.mjs | tail -1 && node screen_test.mjs)
fi
exit $fails
