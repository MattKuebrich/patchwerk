#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Builds the factory patches from tools/factory_src.inc and packs them into app/core/pw_factory_data.inc.
#   tools/gen_factory.sh          write the data file
#   tools/gen_factory.sh --check  fail if the data file is not what the source gives (run_tests.sh does this)
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cc -std=c11 -O1 -DPW_HOST -DPW_FACTORY_GEN -Iapp/core tools/gen_factory.c app/core/pw_engine.c app/core/pw_modules.c app/core/pw_presets.c -lm -o "$T/gen"
"$T/gen" "$T"
python3 tools/gen_factory.py "$T" "$T/data.inc"
if [ "$1" = "--check" ]; then
    cmp -s "$T/data.inc" app/core/pw_factory_data.inc || { echo "app/core/pw_factory_data.inc is out of date: run tools/gen_factory.sh"; exit 1; }
else
    cp "$T/data.inc" app/core/pw_factory_data.inc
fi
