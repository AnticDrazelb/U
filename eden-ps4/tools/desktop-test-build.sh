#!/usr/bin/env bash
# Desktop build of Eden with the PS4's kernel line-up code active (EDEN_TEST_ARENA_LINE_UP: aliases
# put on lined-up copies, allocation rests in one 16 KiB-aligned run; the arena's padding table only
# logs), in ~/eden-host: test.sh and game/stacktest.c run against it. Needs what build.sh host
# needs, and FFmpeg built for the desktop (FFMPEG_DIR, default ~/eden-host-ffmpeg: build-ffmpeg.sh's
# configure line with --cc=clang-21 and no --enable-cross-compile/--target-os/--cpu).
#   tools/desktop-test-build.sh            then  ./test.sh
#   EDEN_PS4_LOG="*:Warning Kernel:Info"   shows the line-up's log lines in a run
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
CMAKE="${CMAKE:-$(command -v cmake)}"
EDEN_DIR="${EDEN_DIR:-$HOME/eden}"
EDEN_DEPS="${EDEN_DEPS:-$HOME/eden-deps}"
FFMPEG_DIR="${FFMPEG_DIR:-$HOME/eden-host-ffmpeg}"
mapfile -t deps < <(python3 -I - "$EDEN_DIR/cpmfile.json" "$EDEN_DEPS" <<'PY'
import json, os, sys
deps = json.load(open(sys.argv[1]))
for name in sorted(os.listdir(sys.argv[2])):
    if name == "ffmpeg" or name not in deps or not os.path.isfile(os.path.join(sys.argv[2], name, ".eden-version")):
        continue
    pkg = deps[name].get("package", name)
    print(f"-D{pkg}_CUSTOM_DIR={os.path.join(sys.argv[2], name)}")
    print(f"-D{pkg}_FORCE_BUNDLED=ON")
PY
)
"$CMAKE" -S "$EDEN_DIR" -B "$HOME/eden-host" -G Ninja -DCMAKE_BUILD_TYPE=Release -DEDEN_EXTRA_FRONTEND_DIR="$HERE/src" \
    -DENABLE_QT=OFF -DENABLE_SDL3=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_UPDATE_CHECKER=OFF \
    -DENABLE_OPENGL=OFF -DENABLE_RESHADE=OFF -DENABLE_CUBEB=OFF -DENABLE_LIBUSB=OFF \
    -DENABLE_WERROR=OFF -DYUZU_ROOM=OFF -DYUZU_TESTS=OFF -DDYNARMIC_TESTS=OFF \
    -DYUZU_DISABLE_LLVM=ON -DUSE_DISCORD_PRESENCE=OFF -DBoost_FORCE_BUNDLED=ON "${deps[@]}" \
    -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
    -DCMAKE_CXX_FLAGS="-DEDEN_TEST_ARENA_LINE_UP=1" \
    -DYUZU_USE_EXTERNAL_FFMPEG=OFF -DYUZU_USE_BUNDLED_FFMPEG=OFF -DFFMPEG_DIR="$FFMPEG_DIR"
nice ninja -C "$HOME/eden-host" eden-ps4
