#!/usr/bin/env bash
# Builds breakout.nro, blocks.nro, padtest.nro, gputest.nro, stacktest.nro and nativetest(_gnu).nro (in OUT, default this
# folder) with libnx, built from source with clang (../nx/build.sh makes that toolchain; NX_BUILD
# points at its nx-build).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE}"
NX_BUILD="${NX_BUILD:-$HOME/nx/devkitpro/libnx-clang/nx-build}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
"$NX_BUILD" "$work/breakout.nro" "Breakout" "Eden for PS4" "1.0" "$HERE/breakout.c" -- -Wall -Wextra >/dev/null
"$NX_BUILD" "$work/blocks.nro" "Blocks" "Eden for PS4" "1.0" "$HERE/blocks.c" -- -Wall -Wextra >/dev/null
"$NX_BUILD" "$work/padtest.nro" "Controller test" "Eden for PS4" "1.0" "$HERE/padtest.c" -- -Wall -Wextra >/dev/null
"$NX_BUILD" "$work/gputest.nro" "GPU test" "Eden for PS4" "1.0" "$HERE/gputest.c" -- -Wall -Wextra >/dev/null
"$NX_BUILD" "$work/stacktest.nro" "Stack test" "Eden for PS4" "1.0" "$HERE/stacktest.c" -- -Wall -Wextra >/dev/null
# (04.80) its symbols exported, as a game's SDK module has them: a SysV hash table, then a GNU one
NX_LDFLAGS="--export-dynamic --hash-style=sysv" \
    "$NX_BUILD" "$work/nativetest.nro" "Native test" "Eden for PS4" "1.0" "$HERE/nativetest.c" -- -Wall -Wextra >/dev/null
NX_LDFLAGS="--export-dynamic --hash-style=gnu" \
    "$NX_BUILD" "$work/nativetest_gnu.nro" "Native test" "Eden for PS4" "1.0" "$HERE/nativetest.c" -- -Wall -Wextra >/dev/null
mkdir -p "$OUT"
for nro in breakout blocks padtest gputest stacktest nativetest nativetest_gnu; do
    cp "$work/$nro.nro" "$OUT/$nro.nro"
    echo "$OUT/$nro.nro ($(stat -c %s "$OUT/$nro.nro") bytes)"
done
