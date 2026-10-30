#!/usr/bin/env bash
# Builds a toolchain for Switch homebrew from source, with clang 21 and lld: devkitPro's newlib,
# libnx, compiler-rt's builtins, and switchbrew's elf2nro and nacptool. devkitPro's packages
# (devkitA64's gcc) aren't needed: their servers can be blocked where git clones aren't.
#   NX_SRC     sources and build folders (default ~/nx)
#   NX_PREFIX  the toolchain (default $NX_SRC/devkitpro), laid out as devkitPro's
#   EDEN_DEPS  Eden's dependencies (fetch-deps.sh), for lz4, which elf2nro uses
#   LLVM_SRC   LLVM 12's source, which ../../toolchain/build-sdk.sh unpacks, for compiler-rt
#              (default: that one, else downloaded into NX_SRC)
# Then: $NX_PREFIX/libnx-clang/nx-build <out.nro> "<title>" "<author>" "<version>" <sources...>
# (../game/build.sh uses it). Needs texinfo (makeinfo) for newlib's build.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
NX_SRC="${NX_SRC:-$HOME/nx}"
NX_PREFIX="${NX_PREFIX:-$NX_SRC/devkitpro}"
EDEN_DEPS="${EDEN_DEPS:-$HOME/eden-deps}"
LLVM_SRC="${LLVM_SRC:-$HOME/ps4-sdk-build/llvm-project-12.0.1.src}"
CMAKE="${CMAKE:-$(command -v cmake)}"
JOBS="${JOBS:-$(nproc)}"
ARCH_FLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57"

# clone <url> <commit> <dir>: that commit alone, local changes discarded
clone() {
    [ -d "$3/.git" ] || { git init -q "$3" && git -C "$3" remote add origin "$1"; }
    git -C "$3" fetch -q --depth 1 origin "$2"
    git -C "$3" checkout -q -f FETCH_HEAD
}
mkdir -p "$NX_SRC"
clone https://github.com/devkitPro/newlib.git 57bd8e1ade3cf46d2e61a72516ac8bfaf8fdfd62 "$NX_SRC/newlib"
clone https://github.com/switchbrew/libnx.git b156b4774fb2df7b93402169509266da7547fc4c "$NX_SRC/libnx"
clone https://github.com/devkitPro/devkita64-rules.git b037940200a5a9a87888743fb83d7a87e348c8e7 "$NX_SRC/devkita64-rules"
clone https://github.com/switchbrew/switch-tools.git 22756068dd0ed6ff9734c59cb4f99ebd3f62555b "$NX_SRC/switch-tools"
clone https://github.com/devkitPro/general-tools.git 46086605cdc63fb02ba0ed08cdc00801ba00c6f0 "$NX_SRC/general-tools"
if [ ! -d "$LLVM_SRC/compiler-rt" ]; then
    LLVM_SRC="$NX_SRC/llvm-project-12.0.1.src"
    [ -d "$LLVM_SRC" ] || curl -fsSL \
        https://github.com/llvm/llvm-project/releases/download/llvmorg-12.0.1/llvm-project-12.0.1.src.tar.xz |
        tar -xJ -C "$NX_SRC"
fi

# devkitA64 stand-ins: clang and LLVM's tools under devkitA64's names, so that libnx's and
# newlib's makefiles work unchanged.
DKA64="$NX_PREFIX/devkitA64"
mkdir -p "$DKA64/bin" "$NX_PREFIX/tools/bin" "$NX_PREFIX/libnx-clang"
cp "$NX_SRC/devkita64-rules/base_rules" "$NX_SRC/devkita64-rules/base_tools" "$DKA64/"
cat > "$DKA64/bin/aarch64-none-elf-gcc" <<WRAP
#!/bin/sh
# devkitA64 stand-in: clang 21 for the Switch's CPU (AArch64, Cortex-A57), with devkitPro's newlib.
# clang errors on some old-C constructs newlib and libnx still have, where gcc only warns; and its
# warnings differ from gcc's, which libnx builds with -Werror.
DKA64="\$(cd "\$(dirname "\$0")/.." && pwd)"
exec clang-21 --target=aarch64-none-elf $ARCH_FLAGS -fPIC \\
    -isystem "\$DKA64/aarch64-none-elf/include" \\
    -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types \\
    -Wno-error=int-conversion -Wno-unused-command-line-argument "\$@" -Wno-error
WRAP
cat > "$DKA64/bin/aarch64-none-elf-g++" <<WRAP
#!/bin/sh
DKA64="\$(cd "\$(dirname "\$0")/.." && pwd)"
exec clang++-21 --target=aarch64-none-elf $ARCH_FLAGS -fPIC \\
    -isystem "\$DKA64/aarch64-none-elf/include" -Wno-unused-command-line-argument "\$@" -Wno-error
WRAP
cat > "$DKA64/bin/aarch64-none-elf-as" <<WRAP
#!/bin/sh
exec clang-21 --target=aarch64-none-elf -march=armv8-a+crc+crypto -c -x assembler "\$@"
WRAP
chmod +x "$DKA64/bin/aarch64-none-elf-gcc" "$DKA64/bin/aarch64-none-elf-g++" "$DKA64/bin/aarch64-none-elf-as"
ln -sf aarch64-none-elf-gcc "$DKA64/bin/aarch64-none-elf-cc"
ln -sf aarch64-none-elf-g++ "$DKA64/bin/aarch64-none-elf-c++"
for tool in ar nm objcopy objdump ranlib readelf size strip; do
    ln -sf "$(command -v llvm-$tool-21)" "$DKA64/bin/aarch64-none-elf-$tool"
done
for tool in ar nm ranlib; do
    ln -sf "$(command -v llvm-$tool-21)" "$DKA64/bin/aarch64-none-elf-gcc-$tool"
done
ln -sf "$(command -v ld.lld-21)" "$DKA64/bin/aarch64-none-elf-ld"
export PATH="$DKA64/bin:$PATH"

# newlib, with devkitPro's libsysbase (the system calls libnx implements)
rm -rf "$NX_SRC/build-newlib" && mkdir -p "$NX_SRC/build-newlib"
(
    cd "$NX_SRC/build-newlib"
    CFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections -DCUSTOM_MALLOC_LOCK" \
        "$NX_SRC/newlib/configure" --disable-newlib-supplied-syscalls --enable-newlib-mb \
        --disable-newlib-wide-orient --enable-newlib-register-fini \
        --target=aarch64-none-elf --prefix="$DKA64" --disable-multilib >/dev/null
    make -j"$JOBS" >/dev/null
    make install -j1 >/dev/null
)

# compiler-rt's builtins (what libgcc gives gcc)
rm -rf "$NX_SRC/build-builtins"
"$CMAKE" -G Ninja -S "$LLVM_SRC/compiler-rt" -B "$NX_SRC/build-builtins" \
    -DCMAKE_C_COMPILER="$DKA64/bin/aarch64-none-elf-gcc" -DCMAKE_CXX_COMPILER="$DKA64/bin/aarch64-none-elf-g++" \
    -DCMAKE_AR="$(command -v llvm-ar-21)" -DCMAKE_RANLIB="$(command -v llvm-ranlib-21)" \
    -DCMAKE_NM="$(command -v llvm-nm-21)" \
    -DCMAKE_C_COMPILER_TARGET=aarch64-none-elf -DCMAKE_ASM_COMPILER_TARGET=aarch64-none-elf \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY "-DCMAKE_C_FLAGS=-fPIC -O2" "-DCMAKE_ASM_FLAGS=-fPIC" \
    -DLLVM_PATH="$LLVM_SRC/llvm" -DLLVM_CONFIG_PATH=LLVM_CONFIG_PATH-NOTFOUND \
    -DCOMPILER_RT_DEFAULT_TARGET_TRIPLE=aarch64-none-elf -DCOMPILER_RT_BAREMETAL_BUILD=ON \
    -DCOMPILER_RT_BUILD_BUILTINS=ON -DCOMPILER_RT_BUILD_CRT=OFF -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
    -DCOMPILER_RT_BUILD_XRAY=OFF -DCOMPILER_RT_BUILD_LIBFUZZER=OFF -DCOMPILER_RT_BUILD_PROFILE=OFF \
    -DCOMPILER_RT_BUILD_MEMPROF=OFF -DCOMPILER_RT_OS_DIR=baremetal >/dev/null
ninja -C "$NX_SRC/build-builtins" >/dev/null
cp "$NX_SRC/build-builtins/lib/baremetal/libclang_rt.builtins-aarch64.a" "$DKA64/aarch64-none-elf/lib/"

# elf2nro, nacptool, bin2s (host tools): before libnx, whose build runs bin2s
T="$NX_PREFIX/tools/bin"
mkdir -p "$T"
(cd "$NX_SRC/switch-tools/src" &&
    cc -O2 -DPACKAGE_STRING='"switch-tools"' -I"$EDEN_DEPS/lz4/lib" -o "$T/elf2nro" \
        elf2nro.c romfs.c filepath.c "$EDEN_DEPS/lz4/lib/lz4.c" &&
    cc -O2 -DPACKAGE_STRING='"switch-tools"' -o "$T/nacptool" nacptool.c)
cc -O2 -DPACKAGE_STRING='"bin2s (general-tools)"' -o "$T/bin2s" "$NX_SRC/general-tools/bin2s.c"
export PATH="$T:$PATH"

# libnx, with clang's changes (libnx-clang.patch: thread pointer, atomics, arrays in structures,
# a weak symbol)
cp "$HERE/clang_atomics.h" "$HERE/crt_clang.c" "$HERE/nx-build" "$NX_PREFIX/libnx-clang/"
git -C "$NX_SRC/libnx" apply "$HERE/libnx-clang.patch"
make -C "$NX_SRC/libnx/nx" DEVKITPRO="$NX_PREFIX" -j"$JOBS" lib/libnx.a \
    ARCH="$ARCH_FLAGS -fPIC -ftls-model=local-exec -include $NX_PREFIX/libnx-clang/clang_atomics.h" >/dev/null
mkdir -p "$NX_PREFIX/libnx"
(cd "$NX_SRC/libnx/nx" &&
    cp default_icon.jpg switch_rules switch.ld switch.specs "$NX_PREFIX/libnx/" &&
    cp -r include lib "$NX_PREFIX/libnx/" && cp -r external/bsd/include "$NX_PREFIX/libnx/")
# switch.ld with a TLS program header, which lld needs for TLS symbols (GNU ld doesn't)
python3 - "$NX_PREFIX/libnx/switch.ld" "$NX_PREFIX/libnx-clang/switch_clang.ld" <<'PY'
import sys
s = open(sys.argv[1]).read()
s = s.replace('\tdyn    PT_DYNAMIC;\n}', '\tdyn    PT_DYNAMIC;\n\ttls    PT_TLS; /* for lld */\n}', 1)
for sec in ('.tdata', '.tbss'):
    i = s.index('\t' + sec + ' :')
    j = s.index('} :data', i)
    s = s[:j] + '} :data :tls' + s[j + len('} :data'):]
open(sys.argv[2], 'w').write(s)
PY
"$DKA64/bin/aarch64-none-elf-gcc" -O2 -c "$NX_PREFIX/libnx-clang/crt_clang.c" -o "$NX_PREFIX/libnx-clang/crt_clang.o"

echo "$NX_PREFIX/libnx-clang/nx-build"
