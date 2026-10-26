#!/usr/bin/env bash
# Builds the Switch CPU benchmark.
#   build.sh host   desktop build in ~/sb-host (run: ~/sb-host/switch_bench; BENCH_SCALE=0.1 for a quick run)
#   build.sh ps4    eboot.bin in ~/sb-ps4, packaged as out/SwitchCpuBench.pkg in this repo
# Needs Eden at EDEN_DIR with patches/dynarmic-ps4.patch applied, xbyak at XBYAK_DIR, and the Crema
# PS4 sysroot (docs/BUILDING.md) for the boost/fmt headers and the PS4 build.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EDEN_DIR="${EDEN_DIR:-$HOME/eden}"
XBYAK_DIR="${XBYAK_DIR:-$HOME/sbdeps/xbyak}"
SYSROOT="${PS4_SYSROOT:-$HOME/cemups4-build/sysroot}"
common=(-G Ninja -DEDEN_DIR="$EDEN_DIR" -DXBYAK_DIR="$XBYAK_DIR" -DHEADERS_DIR="$SYSROOT/include")

case "${1:-}" in
host)
    cmake -S "$HERE" -B "$HOME/sb-host" "${common[@]}" -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21
    ninja -C "$HOME/sb-host"
    ;;
ps4)
    # shellcheck disable=SC1091
    source "$HERE/../scripts/ps4-env.sh"
    ps4_cmake -S "$HERE" -B "$HOME/sb-ps4" "${common[@]}" \
        -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-cemu.cmake" -DPS4_PAID=0x3800000000000035
    ninja -C "$HOME/sb-ps4"
    stage="$HOME/sb-ps4/stage"
    rm -rf "$stage" && mkdir -p "$stage/sce_sys"
    cp "$HOME/sb-ps4/switch_bench_eboot/eboot.bin" "$stage/"
    cp "$HERE/sce_sys/icon0.png" "$stage/sce_sys/"
    mkdir -p "$HOME/sb-ps4/no-modules" "$HERE/../out"
    pkg="$(SFO_STYLE=plain NO_RIGHT_SPRX=1 DOLPHIN_PS4_MODULES_DIR="$HOME/sb-ps4/no-modules" \
        "$HERE/../scripts/make-pkg.sh" "$stage" SWBN00001 "Switch CPU Bench" "$(cat "$HERE/VERSION")" SWITCHCPUBENCH "$HOME/sb-ps4/out" |
        tail -1)"
    cp "$pkg" "$HERE/../out/SwitchCpuBench.pkg"
    echo "$HERE/../out/SwitchCpuBench.pkg"
    ;;
*)
    echo "usage: build.sh host|ps4" >&2
    exit 1
    ;;
esac
