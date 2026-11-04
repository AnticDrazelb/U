#!/usr/bin/env bash
# (04.46) Builds and runs preflight.c: the pre-flight record from port/mesa/ac_ps4_drm.c (the lines
# between "pre-flight record: begin" and "end", with the hang walker's for the shader dump), with
# the sanitizers; then decodes its output with gpu-preflight.py.
#   preflight.sh [garbage rounds]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/preflight"
mkdir -p "$OUT"
SRC="$HERE/../../../port/mesa/ac_ps4_drm.c"
sed -n '/---- hang walker: begin/,/---- hang walker: end/p' "$SRC" \
    | grep -v '^static const void \*ps4_hw_read(uint64_t va, uint64_t bytes);$' \
    | grep -v '^void ps4_boot_trace(const char \*stage);' > "$OUT/walker.inc"
sed -n '/---- pre-flight record: begin/,/---- pre-flight record: end/p' "$SRC" > "$OUT/preflight.inc"
"${CC:-clang-21}" -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$OUT" "$HERE/preflight.c" -o "$OUT/preflight"
"$OUT/preflight" "$OUT" "$@"
python3 "$HERE/../gpu-preflight.py" "$OUT/preflight.txt" "$OUT/progress.txt" > "$OUT/decoded.txt"
grep -q "the CP passed trace id 7 last, in submission 2" "$OUT/decoded.txt" || { cat "$OUT/decoded.txt"; echo "decode: FAILED"; exit 1; }
grep -q "DISPATCH_DIRECT" "$OUT/decoded.txt" && grep -q "CS 0x4400300000" "$OUT/decoded.txt" \
    || { cat "$OUT/decoded.txt"; echo "decode: FAILED (the packet after it)"; exit 1; }
echo "decode: ok"
