#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Where a factory patch's CPU goes: instructions per function (valgrind's callgrind: exact counts, no timing noise).
#   tools/cpu_profile.sh 48        (the number is the patch's place in the list, from 0: SONG DUB is 48)
# What matters is the share of each function and how it changes with an edit; the FM-1's own cost per instruction is
# higher (no hardware divide, slower floats), so the totals do not carry over, the proportions mostly do.
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cc -std=c11 -O2 -g -DPW_HOST -Iapp/core -Itests tools/prof_run.c app/core/pw_engine.c app/core/pw_modules.c app/core/pw_presets.c -lm -o "$T/run"
valgrind --tool=callgrind --callgrind-out-file="$T/cg.out" "$T/run" "${1:-0}" 2>&1 | grep 'refs'
callgrind_annotate "$T/cg.out" 2>/dev/null | sed -n '/file:function/,$p' | sed -n 3,24p | cut -c1-110
