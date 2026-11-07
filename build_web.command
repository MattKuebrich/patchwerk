#!/bin/bash
# Double-click on a Mac: builds PATCHWERK's browser emulator with Zig (web/build_zig.sh), refreshes the module
# visualization stills if Node is installed, and writes the page to emulator.html. Opens it when done.
# First time: it installs what it needs (ziglang and Pillow with pip; wasm2js with Homebrew: brew install binaryen).
cd "$(dirname "$0")" || exit 1
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
pause() { [ -n "$NOPAUSE" ] && return; echo; read -n 1 -s -r -p "Press any key to close."; echo; }
fail() { echo; echo "STOPPED: $1"; pause; exit 1; }

command -v python3 >/dev/null || fail "python3 is not installed (xcode-select --install, or brew install python)."
python3 -c "import ziglang" 2>/dev/null || { echo "Installing ziglang..."; python3 -m pip install --user ziglang || python3 -m pip install --user --break-system-packages ziglang || fail "could not install ziglang (pip3 install ziglang)."; }
python3 -c "import PIL" 2>/dev/null || { echo "Installing Pillow..."; python3 -m pip install --user Pillow || python3 -m pip install --user --break-system-packages Pillow || fail "could not install Pillow (pip3 install Pillow)."; }
command -v wasm2js >/dev/null || fail "wasm2js is missing: brew install binaryen"
[ -d base/lib ] || echo "(note: the base/ folder looks empty; the page build may need it)"

echo "Building the browser emulator..."
sh web/build_zig.sh || fail "the build failed (see above)."

if command -v node >/dev/null && [ -f tests/gen_vis.mjs ]; then
    echo "Refreshing the module visualization stills..."
    rm -rf web/vis_raw
    if node tests/gen_vis.mjs web/vis_raw >/dev/null 2>&1 && python3 tests/vis_png.py web/vis_raw web/vis >/dev/null 2>&1; then
        python3 web/build_page.py >/dev/null 2>&1 && echo "  stills refreshed, page rebuilt."
    else
        echo "  (skipped: the stills were not refreshed; the page still works)"
    fi
    rm -rf web/vis_raw
fi

echo
echo "Done: emulator.html."
[ -n "$NOPAUSE" ] || open emulator.html 2>/dev/null
pause
