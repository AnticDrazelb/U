#!/usr/bin/env bash
# (04.44) Builds and runs desccheck.c: the descriptor check from port/mesa/ac_ps4_drm.c (between
# "descriptor check: begin" and "descriptor check: end"), with the sanitizers, on Linux.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/desccheck"
mkdir -p "$OUT"
sed -n '/---- descriptor check: begin/,/---- descriptor check: end/p' "$HERE/../../../port/mesa/ac_ps4_drm.c" > "$OUT/desccheck.inc"
"${CC:-clang-21}" -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$OUT" "$HERE/desccheck.c" -o "$OUT/desccheck"
"$OUT/desccheck" "$@"
