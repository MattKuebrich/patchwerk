#!/bin/sh
# Build PATCHWERK's browser emulator with Zig's bundled clang + wasm-ld instead of Homebrew's LLVM.
# Save this as patchwerk/web/build_zig.sh, then from the patchwerk folder:
#     pip3 install ziglang Pillow
#     sh web/build_zig.sh
# Needs wasm2js (brew install binaryen) and the base/ folder (git submodule) for build_page.py.
set -eu
cd "$(dirname "$0")"
CORE=../app/core
SRCS="pw_web.c pw_store_flash.c $CORE/pw_engine.c $CORE/pw_modules.c $CORE/pw_presets.c $CORE/pw_ui.c $CORE/pw_link.c"
CC="python3 -m ziglang cc -target wasm32-freestanding"
# (no -Werror: Zig's clang is newer than the one this was written with, and may warn about other things)
FLAGS="-std=c11 -O2 -ffreestanding -nostdlib -fno-builtin-memset -fwrapv -fno-fast-math -ffp-contract=off \
  -DPW_WEB -Wall -Wextra -Iinclude -I. -I$CORE \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,-z,stack-size=262144 -Wl,--strip-all"
mkdir -p build
$CC $FLAGS -mbulk-memory -mnontrapping-fptoint \
  -Wl,--initial-memory=33554432 -Wl,--max-memory=67108864 -o build/patchwerk.wasm $SRCS
$CC $FLAGS -mcpu=mvp \
  -Wl,--initial-memory=33554432 -Wl,--max-memory=33554432 -o build/patchwerk-mvp.wasm $SRCS
wasm2js build/patchwerk-mvp.wasm -O2 -o build/patchwerk.wasm2js.mjs
ls -l build/patchwerk.wasm build/patchwerk.wasm2js.mjs
python3 build_page.py
echo "Done: emulator.html"
