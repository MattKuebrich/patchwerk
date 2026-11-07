#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# One command for a delivery:  tools/release.sh 114
# Bumps the version (pw_ui.h, README, SUBMISSION, emulator.html) from the one now in pw_ui.h, checks CHANGELOG.txt has the entry,
# repacks the examples and factory patches, runs the whole suite (it builds the wasm), builds the page, and zips the tree to
# /tmp/patchwerk-v1_0_0-rcNNN.zip. Stops at the first failure. Send the zip afterwards.
set -e
cd "$(dirname "$0")/.."
new=$1; [ -n "$new" ] || { echo "usage: tools/release.sh NNN   (a release candidate: rcNNN)  or  tools/release.sh 1.0.1   (a version)"; exit 2; }
cur=$(grep -o '"v[^"]*"' app/core/pw_ui.h | head -1 | tr -d '"')
if echo "$new" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    # a version: v1.0.1 replaces the one now in pw_ui.h in the source, README, SUBMISSION and the page; the CHANGELOG's first line is "v1.0.1"
    ver="v$new"
    sed -i "s/$cur/$ver/g" app/core/pw_ui.h README.md submission/SUBMISSION.md emulator.html
    head -1 CHANGELOG.txt | grep -qx "$ver" || { echo "CHANGELOG.txt has no entry for $ver at the top (its first line is just: $ver): add it, then run again"; exit 1; }
    tag=$(echo "$ver" | tr . _); out=/tmp/patchwerk-$tag; dir=patchwerk-$tag; new=$ver
else
    old=$(echo "$cur" | grep -o 'rc[0-9]*' | tr -d 'rc')
    if [ "$old" != "$new" ]; then
        sed -i "s/rc$old/rc$new/g" app/core/pw_ui.h README.md submission/SUBMISSION.md
        sed -i "s/v1.0.0-rc$old/v1.0.0-rc$new/g" emulator.html
    fi
    head -1 CHANGELOG.txt | grep -q "rc$new" || { echo "CHANGELOG.txt has no entry for rc$new at the top: add it, then run again"; exit 1; }
    out=/tmp/patchwerk-v1_0_0-rc$new; dir=patchwerk-v1_0_0-rc$new
fi
python3 tools/gen_brand.py | tail -1
python3 tools/gen_examples.py | tail -1
sh tools/gen_factory.sh | tail -1
sh tests/run_tests.sh > /tmp/rt$new.log 2>&1 || { grep -vE "host: .*ms simulated" /tmp/rt$new.log | tail -25; echo "TESTS FAILED (log: /tmp/rt$new.log)"; exit 1; }
for w in "ALL TESTS PASSED" "BANK TESTS PASSED" "LINK TESTS PASSED" "SEA TESTS PASSED" "EXAMPLES FIT" "DESCRIPTIONS FIT"; do
    grep -q "$w" /tmp/rt$new.log || { echo "missing: $w (log: /tmp/rt$new.log)"; exit 1; }
done
rm -rf web/vis_raw && node tests/gen_vis.mjs web/vis_raw | tail -1 && rm -rf web/vis && python3 tests/vis_png.py web/vis_raw web/vis | tail -1 && rm -rf web/vis_raw   # (the web page's module pictures, drawn by the firmware: every module has one, every release)
python3 web/build_page.py 2>&1 | tail -1
d=/tmp/stage/$dir
rm -rf /tmp/stage && mkdir -p /tmp/stage && cp -a . "$d"
(cd "$d" && rm -rf tests/out tests/_bt_* app/build/scenarios app/build/host app/build/o2 && find . -name __pycache__ -prune -exec rm -rf {} +)
rm -f $out.zip
(cd /tmp/stage && zip -qr $out.zip $dir)
ls -la $out.zip
