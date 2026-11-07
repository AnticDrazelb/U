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
    # Variants (each its own app, so they install side by side):
    #   XB_IMAGE_BASE=0x40000000   link the program there (default: 0, placed at 0x400000)
    #   XB_ELF_TYPE=exec           make it a fixed-address program (tools/set-elf-type.py)
    #   XB_TITLE_ID, XB_TITLE, XB_NAME   its title ID, name on the home screen, and file name
    # shellcheck disable=SC1091
    source "$HERE/../scripts/ps4-env.sh"
    build="$HOME/xb-ps4${XB_TITLE_ID:+-$XB_TITLE_ID}"
    title_id="${XB_TITLE_ID:-XBBN00001}"
    name="${XB_NAME:-XboxCpuBench}"
    ps4_cmake -S "$HERE" -B "$build" "${common[@]}" \
        -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-cemu.cmake" -DPS4_PAID=0x3800000000000035 \
        -DXB_IMAGE_BASE="${XB_IMAGE_BASE:-}"
    ninja -C "$build"
    stage="$build/stage"
    rm -rf "$stage" && mkdir -p "$stage/sce_sys"
    cp "$build/xbox_bench_eboot/eboot.bin" "$stage/"
    [ -z "${XB_ELF_TYPE:-}" ] || python3 "$HERE/tools/set-elf-type.py" "$stage/eboot.bin" "$XB_ELF_TYPE"
    cp "$HERE/sce_sys/icon0.png" "$stage/sce_sys/"
    mkdir -p "$build/no-modules" "$HERE/../out"
    pkg="$(SFO_STYLE=plain NO_RIGHT_SPRX=1 DOLPHIN_PS4_MODULES_DIR="$build/no-modules" \
        "$HERE/../scripts/make-pkg.sh" "$stage" "$title_id" "${XB_TITLE:-Xbox CPU Bench}" "$(cat "$HERE/VERSION")" \
        "$(echo "$name" | tr -cd 'A-Za-z0-9')" "$build/out" | tail -1)"
    cp "$pkg" "$HERE/../out/$name.pkg"
    echo "$HERE/../out/$name.pkg"
    ;;
*)
    echo "usage: build.sh host|ps4" >&2
    exit 1
    ;;
esac
