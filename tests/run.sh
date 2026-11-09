#!/bin/bash
# Runs the kext's call-site code on this Mac's kernel collection, outside the kernel.
#   bash tests/run.sh [collection]
# tools/check_collection.py finds the two calls with its own code and writes the bytes of setPowerState;
# callsite_test then runs callsite.hpp on them and on altered copies. Builds with the address and undefined behaviour
# sanitizers. Nothing is installed and nothing outside a temporary folder is written.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
OUT=$(python3 -I "$HERE/tools/check_collection.py" ${1:+"$1"} --dump "$T/setPowerState.bin")
echo "$OUT"
read -r _ BYTES ADDRESS CLAMP ON OFF TIMER1 TIMER2 <<< "$(echo "$OUT" | grep '^DUMP ')"
xcrun clang++ -std=c++14 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  "$HERE/tests/callsite_test.cpp" -o "$T/callsite_test"
"$T/callsite_test" "$BYTES" "$ADDRESS" "$CLAMP" "$ON" "$OFF" "${TIMER1:-0}" "${TIMER2:-0}"
