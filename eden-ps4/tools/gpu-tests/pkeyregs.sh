#!/bin/bash
# (04.15) Builds pkeyregs.cpp against Eden's fixed_pipeline_state.cpp (the squashed-blend setting
# replaced by a variable the test sets) and runs it: ./pkeyregs.sh [register files] [trials]
# Needs the PS4 build's include flags: the -I/-D of fixed_pipeline_state.cpp.obj in its build.ninja.
set -e
EDEN=${EDEN:-$HOME/eden}
BUILD=${BUILD:-/root/eden-ps4-build}
OUT=${OUT:-$(mktemp -d)}
here=$(cd "$(dirname "$0")" && pwd)
(cd "$BUILD" && ninja -t commands src/video_core/CMakeFiles/video_core.dir/renderer_vulkan/fixed_pipeline_state.cpp.obj |
    tail -1 | tr ' ' '\n' | grep -E '^-I|^-D' | grep -v OPENORBIS) > "$OUT/flags.txt"
sed 's/Settings::values\.use_squashed_iterated_blend/g_squashed_iterated_blend/' \
    "$EDEN/src/video_core/renderer_vulkan/fixed_pipeline_state.cpp" > "$OUT/fps_copy.cpp"
sed -i '0,/^namespace Vulkan {/s//extern bool g_squashed_iterated_blend;\nnamespace Vulkan {/' "$OUT/fps_copy.cpp"
clang++-21 -std=gnu++20 -O2 $(cat "$OUT/flags.txt") -Wno-everything "$here/pkeyregs.cpp" "$OUT/fps_copy.cpp" \
    "$here/pkeyregs_stubs.cpp" "$EDEN/src/common/cityhash.cpp" -o "$OUT/pkeyregs"
"$OUT/pkeyregs" "${1:-5000}" "${2:-20000}"
