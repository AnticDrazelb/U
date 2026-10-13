#!/usr/bin/env bash
# Build Cemu's third-party libraries for the PS4 (static) into $PS4_SYSROOT. Recipes for zlib,
# libpng, OpenSSL and curl come from PSChrome's toolchain/build-deps.sh (verified on hardware).
#
#   scripts/build-deps.sh            build everything not built yet
#   scripts/build-deps.sh curl       (re)build one library
#
# Run it through the space-free link ~/cemups4 (scripts/seed-build-tree.sh): toolchain flags such
# as the linker script path break on the space in "PS4 Projects".
#
# Each library leaves a stamp in $PSB/build/deps/<name>.done.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TC="$HERE/../toolchain"
source "$TC/env.sh"
need cmake ninja perl python3

SRC="$PSB/src/deps"
BLD="$PSB/build/deps"
mkdir -p "$SRC" "$BLD"
JOBS="${JOBS:-8}"

export PKG_CONFIG_LIBDIR="$PS4_SYSROOT/lib/pkgconfig:$PS4_SYSROOT/share/pkgconfig"
unset PKG_CONFIG_PATH CFLAGS CXXFLAGS LDFLAGS CPPFLAGS

fetch() {  # fetch <dir name> <url>
    local dir="$1" url="$2" file
    file="$SRC/$(basename "$url")"
    [ -d "$SRC/$dir" ] && return
    [ -f "$file" ] || curl -fsSL --retry 5 --retry-all-errors --connect-timeout 30 -o "$file" "$url"
    mkdir -p "$SRC/$dir"
    tar -xf "$file" -C "$SRC/$dir" --strip-components=1
}

cmake_build() {  # cmake_build <name> <source dir> [cmake args...]
    local name="$1" src="$2"; shift 2
    rm -rf "$BLD/$name"
    cmake -G Ninja -S "$src" -B "$BLD/$name" \
        -DCMAKE_TOOLCHAIN_FILE="$TC/ps4-deps.cmake" \
        -DPS4_TRY_LINK=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PS4_SYSROOT" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_PREFIX_PATH="$PS4_SYSROOT" \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        "$@"
    ninja -C "$BLD/$name" -j"$JOBS"
    ninja -C "$BLD/$name" install
}

# musl's libc.a links these, so configure checks find them, but they are Linux syscalls (or
# netlink/xattr) that the PS4's FreeBSD kernel lacks.
MUSL_LINUX_ONLY=(-DHAVE_ACCEPT4=0 -DHAVE_EVENTFD=0 -DHAVE_SYS_EVENTFD_H=0 -DHAVE_PIPE2=0
    -DHAVE_SENDMMSG=0 -DHAVE_FSETXATTR=0 -DHAVE_FSETXATTR_5=0 -DHAVE_GETIFADDRS=0
    -DHAVE_CLOCK_GETTIME_MONOTONIC_RAW=0 -DHAVE_STROPTS_H=0)

ZLIB_ARGS=(-DZLIB_LIBRARY="$PS4_SYSROOT/lib/libz.a" -DZLIB_INCLUDE_DIR="$PS4_SYSROOT/include")

# --- libraries, in dependency order -------------------------------------

build_zlib() {
    fetch zlib https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.xz
    local s="$SRC/zlib" o="$BLD/zlib"
    rm -rf "$o"; mkdir -p "$o"
    for f in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil; do
        "$TC/bin/ps4-cc" -O2 -march=btver2 -DHAVE_UNISTD_H -DHAVE_STDARG_H -c "$s/$f.c" -o "$o/$f.o"
    done
    rm -f "$PS4_SYSROOT/lib/libz.a"
    "$PS4_AR" rcs "$PS4_SYSROOT/lib/libz.a" "$o"/*.o
    mkdir -p "$PS4_SYSROOT/include" "$PS4_SYSROOT/lib/pkgconfig"
    cp "$s/zlib.h" "$s/zconf.h" "$PS4_SYSROOT/include/"
    cat > "$PS4_SYSROOT/lib/pkgconfig/zlib.pc" <<EOF
prefix=$PS4_SYSROOT
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: zlib
Description: zlib compression library
Version: 1.3.1
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
EOF
}

build_libpng() {
    fetch libpng https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.47.tar.gz
    cmake_build libpng "$SRC/libpng" "${ZLIB_ARGS[@]}" \
        -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF
}

build_zstd() {
    fetch zstd https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    cmake_build zstd "$SRC/zstd/build/cmake" \
        -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF \
        -DZSTD_BUILD_TESTS=OFF -DZSTD_MULTITHREAD_SUPPORT=ON -DZSTD_LEGACY_SUPPORT=OFF \
        -DZSTD_BUILD_DICTBUILDER=OFF "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -DZSTD_TRACE=0"
    # dictBuilder needs qsort_r (missing from the OpenOrbis libc); the trace hooks are weak
    # undefined symbols, which create-fself rejects ("missing module for symbol")
}

build_libzip() {
    fetch libzip https://github.com/nih-at/libzip/releases/download/v1.11.4/libzip-1.11.4.tar.xz
    cmake_build libzip "$SRC/libzip" "${ZLIB_ARGS[@]}" \
        -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF \
        -DENABLE_WINDOWS_CRYPTO=OFF -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF \
        -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF
}

build_pugixml() {
    fetch pugixml https://github.com/zeux/pugixml/releases/download/v1.15/pugixml-1.15.tar.gz
    cmake_build pugixml "$SRC/pugixml" -DPUGIXML_BUILD_TESTS=OFF
}

build_fmt() {
    fetch fmt https://github.com/fmtlib/fmt/archive/refs/tags/12.1.0.tar.gz
    cmake_build fmt "$SRC/fmt" -DFMT_TEST=OFF -DFMT_DOC=OFF -DFMT_INSTALL=ON
}

build_glm() {
    fetch glm https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
    cmake_build glm "$SRC/glm" -DGLM_BUILD_TESTS=OFF -DGLM_BUILD_LIBRARY=OFF -DGLM_BUILD_INSTALL=ON
}

build_rapidjson() {
    # The last release (1.1.0, 2016) doesn't build with modern compilers; vcpkg uses master too.
    fetch rapidjson https://github.com/Tencent/rapidjson/archive/24b5e7a8b27f42fa16b96fc70aade9106cf7102f.tar.gz
    cmake_build rapidjson "$SRC/rapidjson" -DRAPIDJSON_BUILD_DOC=OFF -DRAPIDJSON_BUILD_EXAMPLES=OFF \
        -DRAPIDJSON_BUILD_TESTS=OFF -DRAPIDJSON_BUILD_THIRDPARTY_GTEST=OFF
}

build_glslang() {
    fetch glslang https://github.com/KhronosGroup/glslang/archive/refs/tags/15.1.0.tar.gz
    cmake_build glslang "$SRC/glslang" -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF \
        -DENABLE_GLSLANG_BINARIES=OFF -DENABLE_SPVREMAPPER=OFF -DGLSLANG_ENABLE_INSTALL=ON \
        -DENABLE_HLSL=OFF -DBUILD_EXTERNAL=OFF
}

build_boost() {
    fetch boost https://github.com/boostorg/boost/releases/download/boost-1.89.0/boost-1.89.0-cmake.tar.xz
    cmake_build boost "$SRC/boost" -DBOOST_ENABLE_PYTHON=OFF -DBOOST_ENABLE_MPI=OFF \
        -DBOOST_INSTALL_LAYOUT=system -DBUILD_TESTING=OFF \
        "-DBOOST_INCLUDE_LIBRARIES=program_options;filesystem;nowide;tokenizer;container;algorithm;functional;optional;signals2;asio;ptr_container;property_tree;static_string;random;predef;crc;circular_buffer;scope_exit;range;endian"
}

build_openssl() {
    fetch openssl https://github.com/openssl/openssl/releases/download/openssl-3.5.0/openssl-3.5.0.tar.gz
    local o="$BLD/openssl"
    rm -rf "$o"; mkdir -p "$o"
    # _GNU_SOURCE doesn't mean glibc here: musl's strerror_r returns int.
    sed -i 's/^#elif defined(_GNU_SOURCE)$/#elif defined(_GNU_SOURCE) \&\& !defined(__PS4__)/' \
        "$SRC/openssl/crypto/o_str.c"
    # Linux target for musl, minus everything that needs Linux-only kernel interfaces (AF_ALG,
    # kTLS, secure-heap mlock/madvise, makecontext). Entropy comes from /dev/urandom (musl's
    # getrandom() would issue a Linux syscall number to the FreeBSD kernel).
    (cd "$o" && CC="$TC/bin/ps4-cc" AR="$PS4_AR" RANLIB="$PS4_RANLIB" \
        "$SRC/openssl/Configure" linux-x86_64 -march=btver2 \
        --prefix="$PS4_SYSROOT" --libdir=lib --openssldir=/app0/ssl \
        --with-rand-seed=devrandom \
        no-shared no-module no-dso no-engine no-async no-afalgeng no-ktls no-secure-memory \
        no-tests no-docs no-apps no-uplink)
    make -C "$o" -j"$JOBS" build_libs
    make -C "$o" install_dev
    # The Linux target records "-ldl -pthread" as libcrypto's dependencies; the PS4 link calls
    # ld.lld directly (no -pthread driver flag), pthreads come from libkernel and there's no DSO.
    sed -i 's/ -ldl -pthread//; s/-ldl -pthread//' "$PS4_SYSROOT/lib/cmake/OpenSSL/OpenSSLConfig.cmake" \
        "$PS4_SYSROOT/lib/pkgconfig/libcrypto.pc"
}

build_curl() {
    fetch curl https://curl.se/download/curl-8.14.1.tar.xz
    cmake_build curl "$SRC/curl" "${ZLIB_ARGS[@]}" \
        -DBUILD_CURL_EXE=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_TESTING=OFF \
        -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
        -DCURL_USE_OPENSSL=ON -DOPENSSL_ROOT_DIR="$PS4_SYSROOT" -DOPENSSL_USE_STATIC_LIBS=ON \
        -DUSE_NGHTTP2=OFF -DCURL_USE_LIBPSL=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
        -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF -DCURL_USE_GSSAPI=OFF \
        -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DENABLE_THREADED_RESOLVER=ON \
        -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none -DCURL_CA_FALLBACK=OFF "${MUSL_LINUX_ONLY[@]}"
}

# Network game library (SMB and NFS shares), both by Ronnie Sahlberg, LGPL-2.1.
build_libsmb2() {
    fetch libsmb2 https://github.com/sahlberg/libsmb2/archive/refs/tags/v6.0.0.tar.gz
    # its PS4 branch replaces CMAKE_C_FLAGS (our sysroot and target flags) with -funwind-tables
    sed -i '/elseif(PS4)/,/elseif(EE)/{/CMAKE_C_FLAGS\|CMAKE_CXX_FLAGS\|add_compile_options/d}' "$SRC/libsmb2/CMakeLists.txt"
    # OpenOrbis' endian.h has __bswapNN as functions (not macros): don't rename them to __bswap_NN
    sed -i 's/^#ifndef __bswap\(16\|32\|64\)$/#if !defined(__bswap\1) \&\& !defined(PS4_PLATFORM)/' \
        "$SRC/libsmb2/include/portable-endian.h"
    # its AES functions get their own names: Cemu's AES128_ECB_encrypt is a function pointer
    # variable (util/crypto/aes128.h), and libsmb2's signing called into it - a jump into data
    # right after a successful login (SMB3 signing, required by Windows 11)
    grep -q smb2_AES128_ECB_encrypt "$SRC/libsmb2/lib/aes.h" || sed -i '1i\
#define AES128_ECB_encrypt smb2_AES128_ECB_encrypt\
#define AES128_ECB_decrypt smb2_AES128_ECB_decrypt\
#define AES128_CBC_encrypt_buffer smb2_AES128_CBC_encrypt_buffer\
#define AES128_CBC_decrypt_buffer smb2_AES128_CBC_decrypt_buffer' "$SRC/libsmb2/lib/aes.h"
    cmake_build libsmb2 "$SRC/libsmb2" -DENABLE_EXAMPLES=OFF "${MUSL_LINUX_ONLY[@]}"
}

build_libnfs() {
    fetch libnfs https://github.com/sahlberg/libnfs/archive/refs/tags/libnfs-8.0.0.tar.gz
    cmake_build libnfs "$SRC/libnfs" -DENABLE_TESTS=OFF -DENABLE_DOCUMENTATION=OFF -DENABLE_UTILS=OFF \
        -DENABLE_EXAMPLES=OFF -DENABLE_MULTITHREADING=OFF -DENABLE_TLS=OFF "${MUSL_LINUX_ONLY[@]}"
}

ALL=(zlib libpng zstd libzip pugixml fmt glm rapidjson glslang boost openssl curl libsmb2 libnfs)
targets=("$@")
[ ${#targets[@]} -gt 0 ] || targets=("${ALL[@]}")
for t in "${targets[@]}"; do
    if [ $# -eq 0 ] && [ -f "$BLD/$t.done" ]; then continue; fi
    echo "=== $t"
    "build_$t" > "$BLD/$t.log" 2>&1 || { echo "FAILED: $t (see $BLD/$t.log)"; tail -30 "$BLD/$t.log"; exit 1; }
    touch "$BLD/$t.done"
done
echo "deps done"
