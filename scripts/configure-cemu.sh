#!/usr/bin/env bash
# Configure Cemu for PS4: no vcpkg, no desktop GUI/input/audio backends; the PS4 frontend
# (src/gui/ps4) and Vulkan on DolphinPS4's RADV port. Run through ~/cemups4 (no space in the path).
#   configure-cemu.sh [extra cmake args...]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
SRC="${CEMU_SRC:-$PS4_BUILD_ROOT/src/Cemu}"
BUILD="$PS4_BUILD_ROOT/build/cemu"
ps4_cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-cemu.cmake" \
    "-DCMAKE_PROJECT_Cemu_INCLUDE=$HERE/../toolchain/ps4-love-style.cmake" \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=OFF \
    -DENABLE_VCPKG=OFF -DENABLE_WXWIDGETS=OFF -DENABLE_SDL=OFF -DENABLE_CUBEB=OFF \
    -DENABLE_HIDAPI=OFF -DENABLE_LIBUSB=OFF -DENABLE_OPENGL=OFF -DENABLE_METAL=OFF \
    -DENABLE_DISCORD_RPC=OFF -DENABLE_WAYLAND=OFF -DENABLE_BLUEZ=OFF -DENABLE_FERAL_GAMEMODE=OFF \
    -DENABLE_VULKAN=ON -DALLOW_PORTABLE=OFF \
    -DPS4_PAID="${PS4_PAID:-0x3800000000000035}" \
    "$@"
