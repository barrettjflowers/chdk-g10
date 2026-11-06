#!/bin/bash
# Extract the 12 bit block from modules/curves.c and run the host test against it.
set -euo pipefail
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/curves12_test"
mkdir -p "$OUT"
sed -n '/12 bit sensor support\./,/^void curve_apply() {/p' ../chdk/modules/curves.c \
	| sed '1s|^|/*|; $d' > "$OUT/curves12_block.inc"
gcc -O2 -Wall -Wno-unused-function -I"$OUT" -o "$OUT/test" test_curves12.c
"$OUT/test"
