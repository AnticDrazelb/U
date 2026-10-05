#!/usr/bin/env bash
# Build the Wii U test homebrew (tests/*/) with devkitPPC + wut inside the devkitpro/devkitppc
# Docker image. Output: tests/out/<name>.rpx
#   tests/build-tests.sh            (run inside the container, see build-tests.cmd)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$HERE/out"
for dir in "$HERE"/*/; do
    name="$(basename "$dir")"
    [ -f "$dir/CMakeLists.txt" ] || continue
    rm -rf "/tmp/build-$name"
    /opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S "$dir" -B "/tmp/build-$name" -DCMAKE_BUILD_TYPE=Release
    cmake --build "/tmp/build-$name" -j4
    cp "/tmp/build-$name/$name.rpx" "$HERE/out/"
    echo "built tests/out/$name.rpx"
done
