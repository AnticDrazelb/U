#!/usr/bin/env bash
# Builds Eden's core with the PS4 launcher (src/) as an out-of-tree frontend.
#   build.sh host   desktop build in ~/eden-host (run: ~/eden-host/bin/eden-ps4 <program.nro>)
#   build.sh ps4    eboot.bin in ~/eden-ps4-build, packaged as out/EdenPS4.pkg in this repo
# Needs Eden at EDEN_DIR (eb88a40 with patches/ applied), its dependencies from fetch-deps.sh in
# EDEN_DEPS, CMake 3.31 or newer (CMAKE=...), and for the PS4 build Crema's PS4 sysroot and
# toolchain (docs/BUILDING.md) and FFmpeg from build-ffmpeg.sh.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EDEN_DIR="${EDEN_DIR:-$HOME/eden}"
EDEN_DEPS="${EDEN_DEPS:-$HOME/eden-deps}"
CMAKE="${CMAKE:-$(command -v cmake)}"

# <package>_CUSTOM_DIR for each dependency fetch-deps.sh cloned (FFmpeg is built separately).
dep_args() {
    python3 -I - "$EDEN_DIR/cpmfile.json" "$EDEN_DEPS" <<'PY'
import json, os, sys
deps = json.load(open(sys.argv[1]))
for name in sorted(os.listdir(sys.argv[2])):
    if name == "ffmpeg" or name not in deps or not os.path.isfile(os.path.join(sys.argv[2], name, ".eden-version")):
        continue
    pkg = deps[name].get("package", name)
    print(f"-D{pkg}_CUSTOM_DIR={os.path.join(sys.argv[2], name)}")
    print(f"-D{pkg}_FORCE_BUNDLED=ON")
PY
}
mapfile -t deps < <(dep_args)

# Eden's core without its frontends, network services, OpenGL, SDL3 and tests.
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DEDEN_EXTRA_FRONTEND_DIR="$HERE/src"
    -DENABLE_QT=OFF -DENABLE_SDL3=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_UPDATE_CHECKER=OFF
    -DENABLE_OPENGL=OFF -DENABLE_RESHADE=OFF -DENABLE_CUBEB=OFF -DENABLE_LIBUSB=OFF
    -DENABLE_WERROR=OFF -DYUZU_ROOM=OFF -DYUZU_TESTS=OFF -DDYNARMIC_TESTS=OFF
    -DYUZU_DISABLE_LLVM=ON -DUSE_DISCORD_PRESENCE=OFF -DBoost_FORCE_BUNDLED=ON "${deps[@]}")

case "${1:-}" in
host)
    "$CMAKE" -S "$EDEN_DIR" -B "$HOME/eden-host" "${common[@]}" \
        -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21
    ninja -C "$HOME/eden-host" ${NINJA_FLAGS:-} eden-ps4
    ;;
ps4)
    # shellcheck disable=SC1091
    source "$HERE/../scripts/ps4-env.sh"
    PATH="$(dirname "$CMAKE"):$PATH" # ps4_cmake runs the cmake on PATH
    build="$HOME/eden-ps4-build"
    # -march goes in the Release flags: CMAKE_<LANG>_FLAGS would replace the toolchain's own
    jaguar="-O2 -DNDEBUG -march=btver2"
    ps4_cmake -S "$EDEN_DIR" -B "$build" "${common[@]}" \
        -DCMAKE_TOOLCHAIN_FILE="$HERE/ps4-eden.cmake" -DPS4_PAID=0x3800000000000035 \
        -DCMAKE_C_FLAGS_RELEASE="$jaguar" -DCMAKE_CXX_FLAGS_RELEASE="$jaguar" \
        -DYUZU_USE_EXTERNAL_FFMPEG=OFF -DYUZU_USE_BUNDLED_FFMPEG=OFF \
        -DFFMPEG_DIR="${FFMPEG_DIR:-$HOME/eden-ps4-ffmpeg}"
    ninja -C "$build" ${NINJA_FLAGS:-} eden-ps4_eboot
    elf="$build/extra_frontend/eden-ps4"

    # Every system function it imports must be one Crema imports (the consoles run those), as
    # switch-bench checks: see its README.
    if [ -n "${CREMA_ELF:-}" ]; then
        missing="$(comm -13 <(llvm-nm-21 -u "$CREMA_ELF" | awk '{print $2}' | sort -u) \
            <(llvm-nm-21 -u "$elf" | awk '{print $2}' | sort -u))"
        [ -z "$missing" ] || { echo "imports Crema doesn't make:"; echo "$missing"; exit 1; }
    fi

    # The ELF of every build is kept, to symbolize the stack addresses in a crash report.
    version="$(cat "$HERE/VERSION")"
    mkdir -p "$HOME/eden-ps4-elves"
    cp "$elf" "$HOME/eden-ps4-elves/eden-ps4-$version-$(date +%Y%m%d-%H%M%S).elf"

    stage="$build/stage"
    rm -rf "$stage" && mkdir -p "$stage/sce_sys"
    cp "$build/extra_frontend/eden-ps4_eboot/eboot.bin" "$stage/"
    cp "$HERE/sce_sys/icon0.png" "$stage/sce_sys/"
    OUT="$stage" "$HERE/hello/build.sh" >/dev/null
    mkdir -p "$build/no-modules" "$HERE/../out"
    pkg="$(SFO_STYLE=plain NO_RIGHT_SPRX=1 DOLPHIN_PS4_MODULES_DIR="$build/no-modules" \
        "$HERE/../scripts/make-pkg.sh" "$stage" EDNP00001 "Eden for PS4" "$version" EDENPS4 "$build/out" |
        tail -1)"
    cp "$pkg" "$HERE/../out/EdenPS4.pkg"
    echo "$HERE/../out/EdenPS4.pkg"
    ;;
*)
    echo "usage: build.sh host|ps4" >&2
    exit 1
    ;;
esac
