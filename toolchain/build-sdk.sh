#!/usr/bin/env bash
# Builds the PS4 SDK in /opt/pacbrew/ps4/openorbis from public sources, as docs/BUILDING.md
# section 2 describes: PacBrew's ps4-openorbis recipes at their pinned versions, with clang 21.
#
#   toolchain/build-sdk.sh          build every piece not built yet
#   toolchain/build-sdk.sh lld      (re)build one piece
#
# Pieces: lld musl headers libs builtins libunwind fself gp4 pkgtool vars links. Each leaves a
# stamp in $SDK_WORK/<piece>.done. Needs clang-21, lld-21, llvm-21, cmake, ninja, pkg-config.
set -euo pipefail
OO=/opt/pacbrew/ps4/openorbis
SDK_WORK="${SDK_WORK:-$HOME/ps4-sdk-build}"
JOBS="${JOBS:-$(nproc)}"
LLVM12=llvm-project-12.0.1.src
PACBREW_REV=16f27e3   # github.com/PacBrew/ps4-openorbis
mkdir -p "$SDK_WORK" "$OO/bin" "$OO/lib"
cd "$SDK_WORK"

clone() {  # clone <github owner/repo> <commit> <dir>: GitHub /archive/ downloads are blocked in some build environments
    [ -d "$3/.git" ] || git clone -q "https://github.com/$1.git" "$3"
    git -C "$3" checkout -q "$2"
}

get() {  # get <url> [file]: download once into $SDK_WORK
    local f="${2:-$(basename "$1")}"
    [ -f "$f" ] || curl -fsSL --retry 5 --retry-all-errors -o "$f.part" "$1" && { [ -f "$f" ] || mv "$f.part" "$f"; }
}

pacbrew() {
    clone PacBrew/ps4-openorbis "$PACBREW_REV" ps4-openorbis
}

llvm12() {
    get "https://github.com/llvm/llvm-project/releases/download/llvmorg-12.0.1/$LLVM12.tar.xz"
    if [ ! -d "$LLVM12" ]; then
        tar -xJf "$LLVM12.tar.xz"
        pacbrew
        patch -d "$LLVM12" -Np1 -i "$SDK_WORK/ps4-openorbis/clang/001-ps4.patch"
    fi
}

go117() {
    get https://go.dev/dl/go1.17.5.linux-amd64.tar.gz
    [ -d go ] || tar -xzf go1.17.5.linux-amd64.tar.gz
}

# LLD 12.0.1 (only lld; the compiler is clang 21).
build_lld() {
    llvm12
    cmake -G Ninja -S "$LLVM12/llvm" -B build-lld \
        -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
        "-DCMAKE_CXX_FLAGS=-include cstdint -include limits" \
        -DCMAKE_BUILD_TYPE=MinSizeRel -DLLVM_ENABLE_PROJECTS=lld -DLLVM_TARGETS_TO_BUILD=X86 \
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
        -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_ZLIB=OFF -DCMAKE_INSTALL_PREFIX="$OO"
    ninja -C build-lld -j"$JOBS" lld
    install -m755 build-lld/bin/lld "$OO/bin/lld"
    ln -sf lld "$OO/bin/ld.lld"
}

# PacBrew's musl (C library, crt1/crti/crtn, C headers).
build_musl() {
    links_clang
    clone PacBrew/ps4-openorbis-musl 140a653299ffcb1457bc63a5ab1f507d7bb37c54 musl
    git -C musl clean -qfdx
    (cd musl && ./configure --target=x86_64-scei-ps4 --prefix="$OO" --disable-shared \
        CC="$OO/bin/clang" CFLAGS="-fPIC -DPS4 -D_LIBUNWIND_IS_BAREMETAL=1 -DPS4_FLEXIBLE_MEMORY=1" &&
        make --quiet -j"$JOBS" && make install)
}

# OpenOrbis headers, samples, link.x and crtlib.o.
build_headers() {
    links_clang
    clone OpenOrbis/OpenOrbis-PS4-Toolchain cd480230b946239f758333ef0dff40d67c5f0c20 oo-toolchain
    (cd oo-toolchain/src/crt && "$OO/bin/clang" -c -o crtlib.o crtlib.c)
    install -m755 oo-toolchain/src/crt/crtlib.o "$OO/lib/"
    install -m755 oo-toolchain/link.x "$OO/"
    cp -r oo-toolchain/include oo-toolchain/samples "$OO/"
}

# The system library stubs (orbis-lib-gen) and PacBrew's stripped libSceLibcInternal.so.
build_libs() {
    pacbrew
    get https://github.com/OpenOrbis/orbis-lib-gen/releases/download/v1.3/lib.tar.gz
    rm -rf libgen && mkdir libgen && tar -xzf lib.tar.gz -C libgen
    cp libgen/lib/* "$OO/lib/"
    cp ps4-openorbis/orbis-lib-gen/libSceLibcInternal.so "$OO/lib/"
}

# compiler-rt builtins and libunwind 12.0.1, with PacBrew's flags.
runtime_flags=(-DCMAKE_C_COMPILER="$OO/bin/clang" -DCMAKE_CXX_COMPILER="$OO/bin/clang++"
    "-DCMAKE_C_FLAGS=-fPIC -D__PS4__ -D_LIBUNWIND_IS_BAREMETAL=1"
    "-DCMAKE_CXX_FLAGS=-fPIC -D__PS4__ -D_LIBUNWIND_IS_BAREMETAL=1"
    -DLLVM_PATH="$SDK_WORK/$LLVM12/llvm" -DLLVM_CONFIG_PATH=LLVM_CONFIG_PATH-NOTFOUND
    -DCMAKE_INSTALL_PREFIX="$SDK_WORK/runtime-install")

build_builtins() {
    llvm12; links_clang
    cmake -G Ninja -S "$LLVM12/compiler-rt" -B build-builtins "${runtime_flags[@]}" \
        -DCOMPILER_RT_DEFAULT_TARGET_TRIPLE=x86_64-scei-ps4 -DCOMPILER_RT_BAREMETAL_BUILD=YES \
        -DCOMPILER_RT_BUILD_BUILTINS=ON -DCOMPILER_RT_BUILD_CRT=OFF \
        -DCOMPILER_RT_BUILD_SANITIZERS=OFF -DCOMPILER_RT_BUILD_XRAY=OFF \
        -DCOMPILER_RT_BUILD_LIBFUZZER=OFF -DCOMPILER_RT_BUILD_PROFILE=OFF \
        -DCOMPILER_RT_BUILD_MEMPROF=OFF
    ninja -C build-builtins -j"$JOBS"
    find build-builtins -name 'libclang_rt.builtins-x86_64.a' -exec install -m644 {} "$OO/lib/" \;
    [ -f "$OO/lib/libclang_rt.builtins-x86_64.a" ]
}

build_libunwind() {
    llvm12; links_clang
    cmake -G Ninja -S "$LLVM12/libunwind" -B build-libunwind "${runtime_flags[@]}" \
        -DLIBUNWIND_USE_COMPILER_RT=YES -DLIBUNWIND_BUILD_32_BITS=NO \
        -DLIBUNWIND_ENABLE_STATIC=ON -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBUNWIND_SYSROOT="$OO" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
    ninja -C build-libunwind -j"$JOBS"
    install -m644 build-libunwind/lib/libunwind.a "$OO/lib/"
}

build_fself() {
    go117
    clone OpenOrbis/create-fself 30d2e10507f55124cf21e85e19825482e4f93c87 create-fself
    (cd create-fself/cmd/create-fself && GOFLAGS=-mod=mod "$SDK_WORK/go/bin/go" build -o create-fself -modfile=go-linux.mod)
    install -d "$OO/bin/linux"
    install -m755 create-fself/cmd/create-fself/create-fself "$OO/bin/create-fself"
    ln -sf ../create-fself "$OO/bin/linux/create-fself"
}

build_gp4() {
    go117
    clone OpenOrbis/create-gp4 bf8e66e9e9bdd92e41e1697893bc8098a151fa24 create-gp4
    (cd create-gp4/cmd/create-gp4 && GOFLAGS=-mod=mod "$SDK_WORK/go/bin/go" build -o create-gp4)
    install -d "$OO/bin/linux"
    install -m755 create-gp4/cmd/create-gp4/create-gp4 "$OO/bin/create-gp4"
    ln -sf ../create-gp4 "$OO/bin/linux/create-gp4"
}

# LibOrbisPkg's PkgTool.Core, published with the .NET 6 SDK.
build_pkgtool() {
    pacbrew
    get https://builds.dotnet.microsoft.com/dotnet/Sdk/6.0.428/dotnet-sdk-6.0.428-linux-x64.tar.gz dotnet6.tar.gz
    [ -d dotnet ] || { mkdir dotnet && tar -xzf dotnet6.tar.gz -C dotnet; }
    clone OpenOrbis/LibOrbisPkg 594021fdc435409f755a6ae0781b65ec6cec846c liborbispkg
    (cd liborbispkg && DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 \
        "$SDK_WORK/dotnet/dotnet" publish -r linux-x64 -c Release LibOrbisPkg.Core.sln)
    install -d "$OO/bin/linux/pkgtool"
    install -m755 ps4-openorbis/pkgtools/PkgTool.Core "$OO/bin/linux/"
    cp -R liborbispkg/PkgTool.Core/bin/Release/net6.0/linux-x64/publish/. "$OO/bin/linux/pkgtool/"
}

build_vars() {
    pacbrew
    install -m644 ps4-openorbis/vars/ps4vars.sh "$OO/"
    install -d "$OO/cmake" "$OO/usr/bin"
    install -m644 ps4-openorbis/vars/ps4.cmake "$OO/cmake/"
    install -m755 ps4-openorbis/vars/openorbis-cmake "$OO/usr/bin/"
    install -m755 ps4-openorbis/pkg-config/pkg-config.in "$OO/usr/bin/openorbis-pkg-config"
}

links_clang() {
    ln -sf "$(command -v clang-21)" "$OO/bin/clang"
    ln -sf "$(command -v clang++-21)" "$OO/bin/clang++"
}

# bin/clang and bin/llvm-*: the clang/llvm 21 tools.
build_links() {
    links_clang
    for t in ar ranlib nm objcopy objdump strip readelf symbolizer; do
        ln -sf "$(command -v llvm-$t-21)" "$OO/bin/llvm-$t"
    done
}

all=(links lld musl headers libs builtins libunwind fself gp4 pkgtool vars)
for p in "${@:-${all[@]}}"; do
    if [ $# -eq 0 ] && [ -f "$SDK_WORK/$p.done" ]; then continue; fi
    echo "=== $p"
    "build_$p"
    touch "$SDK_WORK/$p.done"
done
echo "PS4 SDK in $OO"
