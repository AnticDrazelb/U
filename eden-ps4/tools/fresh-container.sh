#!/usr/bin/env bash
# Sets up a fresh Ubuntu 24.04 container (root, 4 cores, ~25 GB free) to build Eden for PS4 and run
# the dynarmic harness, as docs/BUILDING.md and ../README.md describe, in about 45 minutes. GitHub's
# /archive/ downloads may be blocked: everything comes from git clones and release downloads.
# Each step leaves a stamp in ~/setup-stamps (rerun to continue after a failure).
#   eden-ps4/tools/fresh-container.sh            then: cd eden-ps4 && CMAKE=~/cmake331/bin/cmake ./build.sh ps4
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
export PSB=$HOME/cemups4-build PS4_BUILD_ROOT=$HOME/cemups4-build
ST=$HOME/setup-stamps
mkdir -p "$ST" "$PSB"
step() { [ -f "$ST/$1" ] && { echo "== $1 (done before)"; return; }; echo "== $1 $(date +%T)"; "$@"; touch "$ST/$1"; }
ln -sfn "$REPO" "$HOME/cemups4"
git -C "$REPO" config core.fileMode false
chmod +x "$REPO"/scripts/*.sh "$REPO"/toolchain/*.sh "$REPO"/toolchain/bin/* "$REPO"/eden-ps4/*.sh \
    "$REPO"/eden-ps4/tools/*.sh "$REPO"/eden-ps4/tools/dynarmic-harness/*.sh 2>/dev/null || true

packages() {
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg
    echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-21 main" \
        >/etc/apt/sources.list.d/llvm21.list
    apt-get update -qq
    # libboost-dev: the harness's dynarmic; texinfo: libnx's newlib; valgrind: instruction counts
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq clang-21 lld-21 llvm-21 libclang-rt-21-dev \
        ninja-build pkg-config glslang-tools python3-venv nasm zstd libxml2-dev libboost-dev texinfo valgrind
}
venvs() {
    python3 -m venv "$HOME/cmake331" && "$HOME/cmake331/bin/pip" -q install 'cmake==3.31.*'
    python3 -m venv "$PSB/mesa-venv" && "$PSB/mesa-venv/bin/pip" -q install meson mako pyyaml packaging
}
eden() {
    [ -d "$HOME/eden/.git" ] || git clone -q https://git.eden-emu.dev/eden-emu/eden.git "$HOME/eden"
    git -C "$HOME/eden" checkout -q -B ps4 eb88a40
    git -C "$HOME/eden" -c user.name=Claude -c user.email=noreply@anthropic.com am -q "$REPO"/eden-ps4/patches/*.patch
}
edendeps() { "$REPO/eden-ps4/fetch-deps.sh"; }
sdk() { "$REPO/toolchain/build-sdk.sh"; }
libcxx() { "$REPO/toolchain/build-libcxx.sh" && "$REPO/toolchain/build-runtime.sh" && "$REPO/scripts/install-overlay.sh"; }
deps() {
    # (libsmb2 and libnfs are GitHub /archive/ downloads in build-deps.sh: cloned instead. OpenSSL: Eden
    # finds it in the sysroot; without it, its CMake tries a blocked download)
    mkdir -p "$PSB/src/deps"
    [ -d "$PSB/src/deps/libsmb2" ] || git clone -q --depth 1 --branch v6.0.0 https://github.com/sahlberg/libsmb2.git "$PSB/src/deps/libsmb2"
    [ -d "$PSB/src/deps/libnfs" ] || git clone -q --depth 1 --branch libnfs-8.0.0 https://github.com/sahlberg/libnfs.git "$PSB/src/deps/libnfs"
    JOBS=$(nproc) "$REPO/scripts/build-deps.sh" zlib zstd libsmb2 libnfs openssl
}
mesa() {
    [ -d "$PSB/src/mesa" ] || git clone -q --depth 1 --branch mesa-26.0.8 https://gitlab.freedesktop.org/mesa/mesa.git "$PSB/src/mesa"
    if [ ! -f "$PSB/src/mesa/.ps4-patched" ]; then
        for p in mesa-ps4 mesa-ps4-aligned-vertex-fetch mesa-ps4-vertex-fetch-switch mesa-ps4-flush-all-draws \
                 mesa-ps4-gs-ring-probe mesa-ps4-cs-grow-oom; do
            git -C "$PSB/src/mesa" apply --exclude=src/amd/common/ac_ps4_drm.c "$REPO/patches/$p.patch"
        done
        touch "$PSB/src/mesa/.ps4-patched"
    fi
    [ -d "$PSB/src/libdrm" ] || git clone -q --depth 1 --branch libdrm-2.4.125 https://gitlab.freedesktop.org/mesa/drm.git "$PSB/src/libdrm"
    "$REPO/scripts/build-mesa.sh" setup
}
ffmpeg() { "$REPO/eden-ps4/build-ffmpeg.sh"; }
nx() { CMAKE="$HOME/cmake331/bin/cmake" "$REPO/eden-ps4/nx/build.sh"; }

step packages
step venvs
step eden
step edendeps
step sdk
step libcxx
step deps
step mesa
step ffmpeg
step nx
echo "ready: cd $REPO/eden-ps4 && CMAKE=~/cmake331/bin/cmake ./build.sh ps4"
