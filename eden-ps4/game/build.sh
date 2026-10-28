#!/usr/bin/env bash
# Builds breakout.nro (in OUT, default this folder) with libnx, built from source with clang
# (../nx/build.sh makes that toolchain; NX_BUILD points at its nx-build).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE}"
NX_BUILD="${NX_BUILD:-$HOME/nx/devkitpro/libnx-clang/nx-build}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
"$NX_BUILD" "$work/breakout.nro" "Breakout" "Eden for PS4" "1.0" "$HERE/breakout.c" -- -Wall -Wextra >/dev/null
mkdir -p "$OUT"
cp "$work/breakout.nro" "$OUT/breakout.nro"
echo "$OUT/breakout.nro ($(stat -c %s "$OUT/breakout.nro") bytes)"
