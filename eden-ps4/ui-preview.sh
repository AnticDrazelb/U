#!/usr/bin/env bash
# Builds the HOME menu's desktop preview (src/ui/preview.cpp) and draws each of its screens to a PNG
# in OUT (default ./ui-preview-out), as the PS4 shows them (1920x1080).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$PWD/ui-preview-out}"
CXX="${CXX:-clang++}"
mkdir -p "$OUT"
bin="$OUT/ui-preview"
"$CXX" -std=c++20 -O2 -I"$HERE/src" -DEDEN_PS4_UI_FONTS="\"$HERE/ui-fonts\"" \
    "$HERE/src/ui/canvas.cpp" "$HERE/src/ui/fonts.cpp" "$HERE/src/ui/art.cpp" "$HERE/src/ui/home_menu.cpp" \
    "$HERE/src/ui/preview.cpp" -o "$bin"
"$bin" "$OUT"
