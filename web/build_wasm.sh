#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The two wasms the tests need (plain clang, no zig, no wasm2js): the main one, which also generates the page's visualizations, and the FM-1-limits one.
set -eu
cd "$(dirname "$0")"
CORE=../app/core
mkdir -p build
clang --target=wasm32 -std=c11 -O2 -ffreestanding -nostdlib -fno-builtin-memset -fwrapv -fno-fast-math -ffp-contract=off \
  -DPW_WEB -Wall -Wextra -Werror -Iinclude -I. -I$CORE -Wl,--no-entry -Wl,--export-dynamic -Wl,-z,stack-size=262144 -Wl,--strip-all \
  -mbulk-memory -mnontrapping-fptoint -Wl,--initial-memory=33554432 -Wl,--max-memory=67108864 -o build/patchwerk.wasm \
  pw_web.c pw_store_flash.c $CORE/pw_engine.c $CORE/pw_modules.c $CORE/pw_presets.c $CORE/pw_ui.c $CORE/pw_link.c
# the FM-1's limits (24 modules, 64 cables): the link test's device
clang --target=wasm32 -std=c11 -O2 -ffreestanding -nostdlib -fno-builtin-memset -fwrapv -fno-fast-math -ffp-contract=off \
  -DPW_WEB -DPW_FM1_CAPS -Wall -Wextra -Werror -Iinclude -I. -I$CORE -Wl,--no-entry -Wl,--export-dynamic -Wl,-z,stack-size=262144 -Wl,--strip-all \
  -mbulk-memory -mnontrapping-fptoint -Wl,--initial-memory=33554432 -Wl,--max-memory=67108864 -o build/patchwerk-fm1caps.wasm \
  pw_web.c pw_store_flash.c $CORE/pw_engine.c $CORE/pw_modules.c $CORE/pw_presets.c $CORE/pw_ui.c $CORE/pw_link.c
