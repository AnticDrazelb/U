#!/usr/bin/env bash
# Builds xbox-core.
#   build.sh host   desktop build in ~/xc-host (run: ~/xc-host/xbox-core default.xbe --seconds 5 --png out.png)
#   build.sh ps4    eboot.bin in ~/xc-ps4, packaged with nxdk's samples as out/xbox-core.pkg
# The PS4 build needs Crema's toolchain and sysroot (docs/BUILDING.md); the samples come from an nxdk
# checkout with them built (NXDK_DIR, tests/run-nxdk-samples.sh builds them).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
NXDK_DIR="${NXDK_DIR:-$HOME/emu/nxdk}"
SAMPLES=(hello hello++ sdl gamma winapi_filefind winapi_drivelist triangle xaudio usb sdl_gamecontroller)
case "${1:-}" in
host)
    cmake -S "$HERE" -B "$HOME/xc-host" -G Ninja -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
    ninja -C "$HOME/xc-host"
    ;;
ps4)
    # shellcheck disable=SC1091
    source "$HERE/../scripts/ps4-env.sh"
    ps4_cmake -S "$HERE" -B "$HOME/xc-ps4" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-cemu.cmake" -DPS4_PAID=0x3800000000000035
    ninja -C "$HOME/xc-ps4"
    stage="$HOME/xc-ps4/stage"
    rm -rf "$stage" && mkdir -p "$stage/sce_sys" "$stage/samples"
    cp "$HOME/xc-ps4/xbox-core_eboot/eboot.bin" "$stage/"
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
    mkdir -p "$HOME/xc-ps4/no-modules" "$HERE/../out"
    pkg="$(SFO_STYLE=plain NO_RIGHT_SPRX=1 DOLPHIN_PS4_MODULES_DIR="$HOME/xc-ps4/no-modules" \
        "$HERE/../scripts/make-pkg.sh" "$stage" XBCR00001 "xbox-core" "$(cat "$HERE/VERSION")" XBOXCORE "$HOME/xc-ps4/out" |
        tail -1)"
    cp "$pkg" "$HERE/../out/xbox-core.pkg"
    echo "$HERE/../out/xbox-core.pkg"
    ;;
*)
    echo "usage: build.sh host|ps4" >&2
    exit 1
    ;;
esac
