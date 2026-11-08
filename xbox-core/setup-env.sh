#!/usr/bin/env bash
# Sets up a fresh Ubuntu 24.04 container (a cloud session) to build and test xbox-core: what
# HANDOFF.md §2 says isn't in the repo. Each step skips itself when its result exists. ~15 min.
#   xbox-core/setup-env.sh            the PS4 toolchain, sysroot, nxdk, its samples, the tests, NevolutionX
#   xbox-core/setup-env.sh shadps4    also shadPS4 (~30 min more)
# Then: xbox-core/build.sh host && xbox-core/build.sh ps4.
#
# Differences from docs/BUILDING.md, for a container whose network allows GitHub and the package
# registries but not apt.llvm.org, go.dev or Microsoft's .NET downloads:
#   - clang/llvm 21.1.8 is LLVM's GitHub release tarball, in /opt/llvm21 (clang-21 etc. linked into
#     /usr/local/bin); the SDK's bin/clang and llvm-* point there
#   - ld.lld 12.0.1 is LLVM's prebuilt 12.0.1 release (PacBrew's clang patch is only a compile fix)
#   - create-fself and create-gp4 are built with Ubuntu's Go 1.22, PkgTool.Core with Ubuntu's .NET 8
#     (retargeted from net6.0)
#   - libsmb2 and libnfs are cloned at their tags (GitHub /archive/ tarballs are refused)
#   - libturbojpeg and the Crema-only parts (Mesa, Cemu, the other deps) aren't built: xbox-core
#     doesn't use them
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
O=/opt/pacbrew/ps4/openorbis
SRC="$HOME/src"
DL="$HOME/dl"
LOG="$DL/logs"
mkdir -p "$SRC" "$DL" "$LOG" "$HOME/emu"
step() { echo "=== $*"; }
clone() { # clone <github owner/repo> <dir> [rev]
    [ -d "$2/.git" ] || git clone -q "https://github.com/$1" "$2"
    [ -z "${3:-}" ] || git -C "$2" checkout -q "$3"
}

step "apt packages"
sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq >/dev/null
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq ninja-build cmake pkg-config python3-venv \
    python3-pil nasm golang-go dotnet-sdk-8.0 xvfb flex bison libxml2-dev >/dev/null

step "clang 21 (/opt/llvm21)"
if [ ! -x /opt/llvm21/bin/clang ]; then
    curl -sSL -o "$DL/llvm21.tar.xz" \
        https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.8/LLVM-21.1.8-Linux-X64.tar.xz
    sudo mkdir -p /opt/llvm21
    sudo tar -xJf "$DL/llvm21.tar.xz" -C /opt/llvm21 --strip-components=1
    rm "$DL/llvm21.tar.xz"
fi
sudo ln -sf /opt/llvm21/bin/clang /usr/local/bin/clang-21
sudo ln -sf /opt/llvm21/bin/clang++ /usr/local/bin/clang++-21
for t in llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-strip ld.lld; do
    sudo ln -sf "/opt/llvm21/bin/$t" "/usr/local/bin/$t-21"
done

step "OpenOrbis SDK ($O)"
sudo mkdir -p /opt/pacbrew && sudo chown -R "$(id -u):$(id -g)" /opt/pacbrew
mkdir -p "$O/bin/linux" "$O/lib" "$O/cmake" "$O/usr/bin" "$O/usr/include" "$O/usr/lib/pkgconfig"
clone PacBrew/ps4-openorbis "$SRC/ps4-openorbis"
P="$SRC/ps4-openorbis"
cp "$P/vars/ps4vars.sh" "$O/"
cp "$P/vars/ps4.cmake" "$O/cmake/"
cp "$P/vars/openorbis-cmake" "$O/usr/bin/"
cp "$P/pkg-config/pkg-config.in" "$O/usr/bin/openorbis-pkg-config"
chmod +x "$O/usr/bin/"*
for t in clang clang++ llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-strip llvm-objdump llvm-readelf; do
    ln -sf "/opt/llvm21/bin/$t" "$O/bin/$t"
done
if [ ! -x "$O/bin/lld" ]; then
    curl -sSL -o "$DL/llvm12.tar.xz" \
        https://github.com/llvm/llvm-project/releases/download/llvmorg-12.0.1/clang+llvm-12.0.1-x86_64-linux-gnu-ubuntu-16.04.tar.xz
    tar -xJf "$DL/llvm12.tar.xz" -C "$DL" --wildcards '*/bin/lld'
    cp "$DL"/clang+llvm-12.0.1-*/bin/lld "$O/bin/lld"
    rm -rf "$DL/llvm12.tar.xz" "$DL"/clang+llvm-12.0.1-*
fi
ln -sf lld "$O/bin/ld.lld"

clone OpenOrbis/OpenOrbis-PS4-Toolchain "$SRC/OpenOrbis-PS4-Toolchain" cd48023
cp -r "$SRC/OpenOrbis-PS4-Toolchain/include" "$SRC/OpenOrbis-PS4-Toolchain/samples" "$O/"
cp "$SRC/OpenOrbis-PS4-Toolchain/link.x" "$O/"
if [ ! -f "$O/lib/libkernel.so" ]; then
    curl -sSL -o "$DL/orbis-lib.tar.gz" https://github.com/OpenOrbis/orbis-lib-gen/releases/download/v1.3/lib.tar.gz
    mkdir -p "$DL/orbis-lib" && tar -xzf "$DL/orbis-lib.tar.gz" -C "$DL/orbis-lib"
    cp "$DL/orbis-lib/lib/"* "$O/lib/"
fi
cp "$P/orbis-lib-gen/libSceLibcInternal.so" "$O/lib/"

if [ ! -f "$O/lib/libc.a" ]; then
    clone PacBrew/ps4-openorbis-musl "$SRC/ps4-openorbis-musl" 140a653
    (cd "$SRC/ps4-openorbis-musl" && ./configure --target=x86_64-scei-ps4 --prefix="$O" --disable-shared \
        CC="$O/bin/clang" CFLAGS="-fPIC -DPS4 -D_LIBUNWIND_IS_BAREMETAL=1 -DPS4_FLEXIBLE_MEMORY=1" &&
        make --quiet -j"$(nproc)" && make install) > "$LOG/musl.log" 2>&1
fi
"$O/bin/clang" -c -o "$O/lib/crtlib.o" "$SRC/OpenOrbis-PS4-Toolchain/src/crt/crtlib.c" 2>/dev/null

if [ ! -f "$O/lib/libunwind.a" ] || [ ! -f "$O/lib/libclang_rt.builtins-x86_64.a" ]; then
    S="$DL/llvm-project-12.0.1.src"
    if [ ! -d "$S/libunwind" ]; then
        curl -sSL -o "$DL/llvm12-src.tar.xz" \
            https://github.com/llvm/llvm-project/releases/download/llvmorg-12.0.1/llvm-project-12.0.1.src.tar.xz
        tar -xJf "$DL/llvm12-src.tar.xz" -C "$DL" llvm-project-12.0.1.src/{compiler-rt,libunwind,llvm/cmake,llvm/utils,libcxx/include,libcxx/cmake,libcxx/utils}
        rm "$DL/llvm12-src.tar.xz"
    fi
    F="-fPIC -D__PS4__ -D_LIBUNWIND_IS_BAREMETAL=1"
    common=(-G Ninja -DCMAKE_C_COMPILER="$O/bin/clang" -DCMAKE_CXX_COMPILER="$O/bin/clang++"
        -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" -DLLVM_PATH=../llvm -DLLVM_CONFIG_PATH=LLVM_CONFIG_PATH-NOTFOUND)
    (cd "$S/compiler-rt" && cmake -S . -B build "${common[@]}" -DCOMPILER_RT_DEFAULT_TARGET_TRIPLE=x86_64-scei-ps4 \
        -DCOMPILER_RT_BAREMETAL_BUILD=YES -DCOMPILER_RT_BUILD_BUILTINS=ON -DCOMPILER_RT_BUILD_CRT=OFF \
        -DCOMPILER_RT_BUILD_SANITIZERS=OFF -DCOMPILER_RT_BUILD_XRAY=OFF -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
        -DCOMPILER_RT_BUILD_PROFILE=OFF -DCOMPILER_RT_BUILD_MEMPROF=OFF -DCMAKE_INSTALL_PREFIX="$S/rt-install" &&
        ninja -C build install) > "$LOG/compiler-rt.log" 2>&1
    cp "$S/rt-install/lib/linux/libclang_rt.builtins-x86_64.a" "$O/lib/"
    (cd "$S/libunwind" && cmake -S . -B build "${common[@]}" -DLIBUNWIND_USE_COMPILER_RT=YES \
        -DLIBUNWIND_BUILD_32_BITS=NO -DLIBUNWIND_ENABLE_STATIC=ON -DLIBUNWIND_ENABLE_SHARED=OFF \
        -DLIBUNWIND_SYSROOT="$O" -DCMAKE_INSTALL_PREFIX="$S/uw-install" &&
        ninja -C build install) > "$LOG/libunwind.log" 2>&1
    cp "$S/uw-install/lib/libunwind.a" "$O/lib/"
fi

export GOTOOLCHAIN=local
if [ ! -x "$O/bin/create-fself" ]; then
    clone OpenOrbis/create-fself "$SRC/create-fself" 30d2e10
    (cd "$SRC/create-fself/cmd/create-fself" && go build -o "$O/bin/create-fself" -modfile=go-linux.mod)
fi
if [ ! -x "$O/bin/create-gp4" ]; then
    clone OpenOrbis/create-gp4 "$SRC/create-gp4" bf8e66e
    (cd "$SRC/create-gp4/cmd/create-gp4" && go build -o "$O/bin/create-gp4")
fi
ln -sf ../create-fself "$O/bin/linux/create-fself"
ln -sf ../create-gp4 "$O/bin/linux/create-gp4"

if [ ! -x "$O/bin/linux/pkgtool/PkgTool.Core" ]; then
    clone OpenOrbis/LibOrbisPkg "$SRC/LibOrbisPkg" 594021f
    (cd "$SRC/LibOrbisPkg" &&
        sed -i 's#<TargetFramework>net6.0</TargetFramework>#<TargetFramework>net8.0</TargetFramework>#' \
            PkgTool.Core/PkgTool.Core.csproj LibOrbisPkg.Core/LibOrbisPkg.Core.csproj &&
        DOTNET_CLI_TELEMETRY_OPTOUT=1 dotnet publish -r linux-x64 -c Release --self-contained LibOrbisPkg.Core.sln) \
        > "$LOG/pkgtool.log" 2>&1
    mkdir -p "$O/bin/linux/pkgtool"
    cp -R "$SRC/LibOrbisPkg/PkgTool.Core/bin/Release/net8.0/linux-x64/publish/." "$O/bin/linux/pkgtool/"
fi
cp "$P/pkgtools/PkgTool.Core" "$O/bin/linux/" && chmod +x "$O/bin/linux/PkgTool.Core"

step "sysroot (~/cemups4-build/sysroot): libc++ 21, runtime, overlay, libsmb2, libnfs"
git -C "$REPO" config core.fileMode false
find "$REPO" -name '*.sh' -not -path '*/.git/*' -exec chmod +x {} +
chmod +x "$REPO"/toolchain/bin/*
SYS="$HOME/cemups4-build/sysroot"
[ -f "$SYS/lib/libc++.a" ] || "$REPO/toolchain/build-libcxx.sh" > "$LOG/libcxx.log" 2>&1
[ -f "$SYS/lib/libdolphinps4_rt.a" ] || "$REPO/toolchain/build-runtime.sh" > "$LOG/runtime.log" 2>&1
"$REPO/scripts/install-overlay.sh" > /dev/null
D="$HOME/cemups4-build/src/deps"
mkdir -p "$D"
[ -d "$D/libsmb2" ] || git clone -q --depth 1 -b v6.0.0 https://github.com/sahlberg/libsmb2 "$D/libsmb2"
[ -d "$D/libnfs" ] || git clone -q --depth 1 -b libnfs-8.0.0 https://github.com/sahlberg/libnfs "$D/libnfs"
[ -f "$SYS/lib/libsmb2.a" ] && [ -f "$SYS/lib/libnfs.a" ] || "$REPO/scripts/build-deps.sh" libsmb2 libnfs

step "nxdk (~/emu/nxdk), its samples, the core's tests, NevolutionX"
export NXDK_DIR="$HOME/emu/nxdk" PATH="$HOME/emu/nxdk/bin:/opt/llvm21/bin:$PATH"
[ -d "$NXDK_DIR/.git" ] || git clone -q --recursive https://github.com/XboxDev/nxdk.git "$NXDK_DIR"
for s in hello hello++ sdl gamma winapi_filefind winapi_drivelist triangle xaudio usb sdl_gamecontroller mesh; do
    [ -f "$NXDK_DIR/samples/$s/bin/default.xbe" ] || make -C "$NXDK_DIR/samples/$s" -j"$(nproc)" > "$LOG/nxdk-$s.log" 2>&1 ||
        echo "sample $s failed: $LOG/nxdk-$s.log"
done
for t in "$HERE"/tests/*-test; do
    make -C "$t" -j"$(nproc)" > "$LOG/test-$(basename "$t").log" 2>&1 || echo "$(basename "$t") failed: $LOG/test-$(basename "$t").log"
done
"$HERE/tests/disc-test/make-image.sh"
# the test builds rewrite stray tracked files (titles with spaces split into words): keep them out of git
git -C "$REPO" checkout -- xbox-core/tests/rt-test xbox-core/tests/tex-test 2>/dev/null || true
NX="$HOME/emu/homebrew/NevolutionX"
if [ ! -f "$NX/NevolutionX.iso" ]; then
    [ -d "$NX/.git" ] || git clone -q --recursive https://github.com/dracc/NevolutionX "$NX"
    make -C "$NX" -j"$(nproc)" > "$LOG/nevolutionx.log" 2>&1 || echo "NevolutionX failed: $LOG/nevolutionx.log"
fi

CC="$HOME/emu/games/ClassiCube"
if [ ! -f "$CC/ClassiCube-xbox.iso" ]; then
    [ -d "$CC/.git" ] || git clone -q --depth 1 https://github.com/ClassiCube/ClassiCube "$CC"
    make -C "$CC" xbox -j"$(nproc)" > "$LOG/classicube.log" 2>&1 || echo "ClassiCube failed: $LOG/classicube.log"
fi

if [ "${1:-}" = shadps4 ]; then
    step "shadPS4 (~/emu/shadPS4/build/shadps4)"
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq libx11-dev libxext-dev libxrandr-dev \
        libxcursor-dev libxi-dev libxss-dev libxtst-dev libwayland-dev libxkbcommon-dev libdecor-0-dev \
        libasound2-dev libpulse-dev libudev-dev libssl-dev zlib1g-dev libvulkan-dev libgl-dev libedit-dev \
        libevdev-dev libjack-dev libsndio-dev libpng-dev libusb-1.0-0-dev mesa-vulkan-drivers >/dev/null
    SH="$HOME/emu/shadPS4"
    [ -d "$SH/.git" ] || git clone -q https://github.com/shadps4-emu/shadPS4 "$SH"
    # fdk-aac's own home (android.googlesource.com) may be unreachable: its GitHub mirror instead
    git -C "$SH" submodule init -q
    git -C "$SH" config submodule.externals/aacdec/fdk-aac.url https://github.com/mstorsjo/fdk-aac
    git -C "$SH" submodule update -q --init --recursive
    # spdlog downloads fmt as a GitHub /archive/ tarball, which may be refused: a clone of the tag
    [ -d "$HOME/emu/fmt-12.1.0" ] || git clone -q --depth 1 -b 12.1.0 https://github.com/fmtlib/fmt "$HOME/emu/fmt-12.1.0"
    # LLVM's libc++: clang 21 can't compile shadPS4's std::ranges::to with Ubuntu's libstdc++ 13 or 14
    cmake -S "$SH" -B "$SH/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_FMT="$HOME/emu/fmt-12.1.0" \
        -DCMAKE_C_COMPILER=/opt/llvm21/bin/clang -DCMAKE_CXX_COMPILER=/opt/llvm21/bin/clang++ \
        -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
        -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++ -Wl,-rpath,/opt/llvm21/lib/x86_64-unknown-linux-gnu" > "$LOG/shadps4.log" 2>&1
    ninja -C "$HOME/emu/shadPS4/build" >> "$LOG/shadps4.log" 2>&1
    # a user/ folder beside where it runs keeps its files there (HANDOFF's paths), not in ~/.local/share
    mkdir -p "$HOME/emu/run/user"
fi
echo "setup done. Logs in $LOG. Next: xbox-core/build.sh host && xbox-core/build.sh ps4"
