#!/usr/bin/env bash
# Builds the Xbox CPU benchmark.
#   build.sh host   desktop build in ~/xb-host (run: ~/xb-host/xbox_bench; BENCH_SCALE=0.1 for a quick run)
#   build.sh ps4    eboot.bin in ~/xb-ps4, packaged as out/XboxCpuBench.pkg in this repo
# Needs the fmt headers (HEADERS_DIR, by default the Crema PS4 sysroot's; docs/BUILDING.md), and for
# the PS4 build Crema's toolchain. clang and ld.lld build the 32-bit guest.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SYSROOT="${PS4_SYSROOT:-$HOME/cemups4-build/sysroot}"
HEADERS_DIR="${HEADERS_DIR:-$SYSROOT/include}"
common=(-G Ninja -DHEADERS_DIR="$HEADERS_DIR")

case "${1:-}" in
host)
    cmake -S "$HERE" -B "$HOME/xb-host" "${common[@]}" -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
    ninja -C "$HOME/xb-host"
    ;;
ps4)
    # shellcheck disable=SC1091
    source "$HERE/../scripts/ps4-env.sh"
    ps4_cmake -S "$HERE" -B "$HOME/xb-ps4" "${common[@]}" \
        -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-cemu.cmake" -DPS4_PAID=0x3800000000000035
    ninja -C "$HOME/xb-ps4"
    stage="$HOME/xb-ps4/stage"
    rm -rf "$stage" && mkdir -p "$stage/sce_sys"
    cp "$HOME/xb-ps4/xbox_bench_eboot/eboot.bin" "$stage/"
    cp "$HERE/sce_sys/icon0.png" "$stage/sce_sys/"
    mkdir -p "$HOME/xb-ps4/no-modules" "$HERE/../out"
    pkg="$(SFO_STYLE=plain NO_RIGHT_SPRX=1 DOLPHIN_PS4_MODULES_DIR="$HOME/xb-ps4/no-modules" \
        "$HERE/../scripts/make-pkg.sh" "$stage" XBBN00001 "Xbox CPU Bench" "$(cat "$HERE/VERSION")" XBOXCPUBENCH "$HOME/xb-ps4/out" |
        tail -1)"
    cp "$pkg" "$HERE/../out/XboxCpuBench.pkg"
    echo "$HERE/../out/XboxCpuBench.pkg"
    ;;
*)
    echo "usage: build.sh host|ps4" >&2
    exit 1
    ;;
esac
