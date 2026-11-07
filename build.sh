#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build PATCHWERK with fm1-base (base/, a git submodule; see BUILDING.md).
#   ./build.sh [--release X.Y] [--host]
#   JIELI_TOOLCHAIN  JieLi Linux toolchain (default: ~/.jieli/toolchain)
#   AC79_SDK         JieLi AC79 SDK checkout (default: ~/fw-AC79_AIoT_SDK)
#   PYTHON           a Python with Pillow
set -e
cd "$(dirname "$0")"
export JIELI_TOOLCHAIN="${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}"
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
PY="${PYTHON:-python3}"
[ -f base/tools/build.py ] || { echo "build.sh: no fm1-base in base/ (git submodule update --init --recursive)"; exit 1; }
# PATCHWERK's changes to felucca-bsp (base/bsp) and fm1-base (base), patches/, until upstream: applied once
for p in patches/felucca-bsp-*.patch; do
    if git -C base/bsp apply --check "../../$p" 2>/dev/null; then git -C base/bsp apply "../../$p" && echo "build.sh: applied $p"; fi
done
for p in patches/fm1-base-*.patch; do
    if git -C base apply --check "../$p" 2>/dev/null; then git -C base apply "../$p" && echo "build.sh: applied $p"; fi
done
"$PY" -c 'import PIL' 2>/dev/null || { echo "build.sh: $PY has no Pillow (pip3 install Pillow)"; exit 1; }
# The fonts are drawn with Pillow's text layout (raqm), which loads FriBiDi at run time. On a Mac that is
# Homebrew's (brew install fribidi); on Apple silicon its folder is not where the loader looks by itself.
if [ "$(uname -s)" = Darwin ] && command -v brew >/dev/null 2>&1; then
    export DYLD_FALLBACK_LIBRARY_PATH="$(brew --prefix)/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
fi
"$PY" -c 'from PIL import features; raise SystemExit(0 if features.check("raqm") else 1)' 2>/dev/null || {
    echo "build.sh: Pillow can't lay out text (no raqm/FriBiDi). On a Mac: brew install fribidi"
    echo "          then run ./build.sh again. (Check: $PY -c 'from PIL import features; print(features.check(\"raqm\"))')"
    exit 1
}
"$PY" base/tools/build.py app "$@" || exit $?
# keep every firmware you build, with its version in the file name (firmware/patchwerk-v0.99999.fwsc): the web page lists them to download or flash an older one
case " $* " in *" --host "*) ;; *)
    NEW="$(ls -t app/build/patchwerk*.fwsc 2>/dev/null | head -1)"
    VER="$(sed -n 's/.*#define PW_VERSION "\([^"]*\)".*/\1/p' app/core/pw_ui.h)"
    if [ -n "$NEW" ] && [ -n "$VER" ] && [ "$NEW" -nt app/core/pw_ui.h ]; then
        mkdir -p firmware && cp "$NEW" "firmware/patchwerk-$VER.fwsc" && echo "build.sh: kept firmware/patchwerk-$VER.fwsc"
    fi ;;
esac
# the web page carries the firmware just built, so its installer needs no file chosen
if [ -f web/build_page.py ] && [ -f web/build/patchwerk.wasm ]; then
    "$PY" web/build_page.py >/dev/null 2>&1 && echo "build.sh: emulator.html now installs this build" || true
fi
