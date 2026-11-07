#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Quick check after editing pw_help.inc or pw_examples.inc: repack the example texts, rebuild the wasm and run only the
# add-list fit check (descriptions 8 lines, examples 7). Seconds instead of the whole suite. Run it before run_tests.sh.
set -e
cd "$(dirname "$0")/.."
python3 tools/gen_examples.py --fast | tail -1
sh web/build_wasm.sh 2>&1 | grep -E "error|warning" && exit 1
node tests/help_fit.mjs
echo '(fast pack: run tools/gen_examples.py plain before the full suite)'
