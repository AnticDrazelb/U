#!/usr/bin/env bash
# Builds xbox-core.
#   build.sh host   desktop build in ~/xc-host (run: ~/xc-host/xbox-core default.xbe --seconds 5 --png out.png)
#   build.sh ps4    eboot.bin in ~/xc-ps4, packaged with nxdk's samples as out/xbox-core.pkg
# The PS4 build needs Crema's toolchain and sysroot (docs/BUILDING.md); the samples come from an nxdk
# checkout with them built (NXDK_DIR, tests/run-nxdk-samples.sh builds them); the core's tests come
# from tests/kernel-test and tests/disc-test (make, then disc-test/make-image.sh).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
NXDK_DIR="${NXDK_DIR:-$HOME/emu/nxdk}"
SAMPLES=(hello hello++ sdl gamma winapi_filefind winapi_drivelist triangle xaudio usb sdl_gamecontroller mesh)
case "${1:-}" in
host)
    cmake -S "$HERE" -B "$HOME/xc-host" -G Ninja -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
    ninja -C "$HOME/xc-host"
    ;;
ps4)
    # Variants (each its own app, so they install side by side), as xbox-bench 01.04's:
    #   XB_IMAGE_BASE=0x1000000000   link the app there (default: 0, loaded at 0x400000)
    #   XB_ELF_TYPE=exec             make it a fixed-address program (../xbox-bench/tools/set-elf-type.py)
    #   XB_TITLE_ID, XB_NAME         its title ID and file name
    # shellcheck disable=SC1091
    source "$HERE/../scripts/ps4-env.sh"
    build="$HOME/xc-ps4${XB_TITLE_ID:+-$XB_TITLE_ID}"
    ps4_cmake -S "$HERE" -B "$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-cemu.cmake" -DPS4_PAID=0x3800000000000035 \
        -DXB_IMAGE_BASE="${XB_IMAGE_BASE:-}"
    ninja -C "$build"
    stage="$build/stage"
    rm -rf "$stage" && mkdir -p "$stage/sce_sys" "$stage/samples"
    cp "$build/xbox-core_eboot/eboot.bin" "$stage/"
    [ -z "${XB_ELF_TYPE:-}" ] || python3 "$HERE/../xbox-bench/tools/set-elf-type.py" "$stage/eboot.bin" "$XB_ELF_TYPE"
    cp "$HERE/../xbox-bench/sce_sys/icon0.png" "$stage/sce_sys/"
    for s in "${SAMPLES[@]}"; do
        xbe="$NXDK_DIR/samples/$s/bin/default.xbe"
        if [ -f "$xbe" ]; then
            # flat files: create-gp4 drops a folder whose name starts another's (hello, hello++)
            cp "$xbe" "$stage/samples/$s.xbe"
        else
            echo "sample $s not built ($xbe): left out" >&2
        fi
    done
    # the core's own tests: the kernel test as an XBE, the disc test as a disc image
    if [ -f "$HERE/tests/kernel-test/bin/default.xbe" ]; then
        cp "$HERE/tests/kernel-test/bin/default.xbe" "$stage/samples/test-kernel.xbe"
    fi
    if [ -f "$HERE/tests/disc-test/disc-test.iso" ]; then
        cp "$HERE/tests/disc-test/disc-test.iso" "$stage/samples/test-disc.iso"
    fi
    name="${XB_NAME:-xbox-core}"
    mkdir -p "$build/no-modules" "$HERE/../out"
    pkg="$(SFO_STYLE=plain NO_RIGHT_SPRX=1 DOLPHIN_PS4_MODULES_DIR="$build/no-modules" \
        "$HERE/../scripts/make-pkg.sh" "$stage" "${XB_TITLE_ID:-XBCR00001}" "$name" "$(cat "$HERE/VERSION")" \
        "$(echo "$name" | tr -cd 'A-Za-z0-9' | tr a-z A-Z)" "$build/out" | tail -1)"
    cp "$pkg" "$HERE/../out/$name.pkg"
    echo "$HERE/../out/$name.pkg"
    ;;
*)
    echo "usage: build.sh host|ps4" >&2
    exit 1
    ;;
esac
