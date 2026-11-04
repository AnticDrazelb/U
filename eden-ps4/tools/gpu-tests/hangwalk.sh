#!/usr/bin/env bash
# (04.39) Builds and runs hangwalk.c: the GPU hang report's walker from port/mesa/ac_ps4_drm.c (the
# lines between "hang walker: begin" and "hang walker: end"), with the sanitizers.
#   hangwalk.sh [eden-ps4-logs.txt with a gpu/gpu-hang.txt of Eden 04.38's Zelda hang] [garbage rounds]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/hangwalk"
mkdir -p "$OUT"
sed -n '/---- hang walker: begin/,/---- hang walker: end/p' "$HERE/../../../port/mesa/ac_ps4_drm.c" \
    | grep -v '^static const void \*ps4_hw_read(uint64_t va, uint64_t bytes);$' \
    | grep -v '^void ps4_boot_trace(const char \*stage);' > "$OUT/walker.inc"
"${CC:-clang-21}" -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$OUT" "$HERE/hangwalk.c" -o "$OUT/hangwalk"
"$OUT/hangwalk" "$@"
