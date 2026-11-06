#!/bin/sh
# run.sh [eden-tree]: 04.78's test of the native memcpy/memmove/memset (Eden's
# src/core/arm/dynarmic/native_functions.h, Move and Fill) against memmove/memset on a flat buffer,
# with pages plain (a host pointer) or not (GPU-cached: ReadBlock/WriteBlock only), random addresses,
# sizes and overlaps. 04.75-04.77's copies through CopyBlock got 22% of these wrong.
set -e
EDEN=${1:-$HOME/eden}
D=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
H="$EDEN/src/core/arm/dynarmic/native_functions.h"
sed -n "$(grep -n '^// Plain memory (its page table entry' "$H" | cut -d: -f1),\$p" "$H" |
    sed '/^} \/\/ namespace Core::NativeFunctions/,$d' > "$T/move_fill.inc"
clang++-21 -std=c++20 -O1 -I"$T" "$D/test.cpp" -o "$T/test"
"$T/test"
