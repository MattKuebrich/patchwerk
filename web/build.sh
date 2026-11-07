#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the browser emulator: PATCHWERK's core (app/core, unchanged) plus web/pw_web.c, the FM-1's panel,
# screen and flash in memory. clang + wasm-ld, no libc; and a plain-JavaScript copy of the same code
# (binaryen's wasm2js) for pages whose security policy refuses WebAssembly.
#   WASM2JS=path/to/wasm2js web/build.sh && python3 web/build_page.py   -> emulator.html
set -eu
cd "$(dirname "$0")"
CORE=../app/core
SRCS="pw_web.c pw_store_flash.c $CORE/pw_engine.c $CORE/pw_modules.c $CORE/pw_presets.c $CORE/pw_ui.c $CORE/pw_link.c"
FLAGS="-std=c11 -O2 -ffreestanding -nostdlib -fno-builtin-memset -fwrapv -fno-fast-math -ffp-contract=off \
  -DPW_WEB -Wall -Wextra -Werror -Iinclude -I. -I$CORE \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,-z,stack-size=262144 -Wl,--strip-all"
mkdir -p build
clang --target=wasm32 $FLAGS -mbulk-memory -mnontrapping-fptoint \
  -Wl,--initial-memory=33554432 -Wl,--max-memory=67108864 -o build/patchwerk.wasm $SRCS
clang --target=wasm32 -mcpu=mvp $FLAGS \
  -Wl,--initial-memory=33554432 -Wl,--max-memory=33554432 -o build/patchwerk-mvp.wasm $SRCS
"${WASM2JS:-wasm2js}" build/patchwerk-mvp.wasm -O2 -o build/patchwerk.wasm2js.mjs
# the FM-1's limits (24 modules, 64 cables): the link test's device
clang --target=wasm32 $FLAGS -DPW_FM1_CAPS -mbulk-memory -mnontrapping-fptoint \
  -Wl,--initial-memory=33554432 -Wl,--max-memory=67108864 -o build/patchwerk-fm1caps.wasm $SRCS
ls -l build/patchwerk.wasm build/patchwerk.wasm2js.mjs
