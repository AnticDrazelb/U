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
    ninja -C "$HOME/eden-host" eden-ps4
    ;;
*)
    echo "usage: build.sh host|ps4" >&2
    exit 1
    ;;
esac
