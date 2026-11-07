#!/bin/sh
# the brand colors (brand/brand.json) into the code that needs them; --check: fail if they are out of date
cd "$(dirname "$0")/.." && python3 tools/gen_brand.py "$@"
