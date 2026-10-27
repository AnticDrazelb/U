#!/usr/bin/env bash
# Builds FFmpeg for the PS4 (static libraries in FFMPEG_DIR, default ~/eden-ps4-ffmpeg) from the
# copy fetch-deps.sh cloned at the commit Eden pins, with what Eden uses: the H.264, VP8 and VP9
# decoders, the yadif and scale filters, swscale (as Eden's own YUZU_USE_EXTERNAL_FFMPEG build).
# Compiled through Crema's PS4 compiler wrapper (toolchain/bin/ps4-cc).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TC="$HERE/../toolchain"
SRC="${EDEN_DEPS:-$HOME/eden-deps}/ffmpeg"
PREFIX="${FFMPEG_DIR:-$HOME/eden-ps4-ffmpeg}"
BUILD="${FFMPEG_BUILD:-$HOME/eden-ps4-ffmpeg-build}"
# shellcheck disable=SC1091
source "$TC/env.sh"

rm -rf "$BUILD" && mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" --prefix="$PREFIX" \
    --enable-cross-compile --arch=x86_64 --cpu=btver2 --target-os=none \
    --cc="$TC/bin/ps4-cc" --cxx="$TC/bin/ps4-c++" --ar="$PS4_AR" --ranlib="$PS4_RANLIB" \
    --nm="llvm-nm-$CLANG_VER" --strip="llvm-strip-$CLANG_VER" \
    --enable-static --disable-shared --enable-pic --enable-pthreads \
    --disable-programs --disable-doc --disable-everything --disable-network \
    --disable-avdevice --disable-avformat --disable-swresample \
    --disable-vaapi --disable-vdpau --disable-iconv \
    --enable-decoder=h264 --enable-decoder=vp8 --enable-decoder=vp9 --enable-filter=yadif,scale

# The PS4's C library (musl) links these, so configure finds them, but they are Linux system calls
# that the PS4's FreeBSD kernel doesn't have.
for feature in SCHED_GETAFFINITY PRCTL PTHREAD_SETNAME_NP; do
    sed -i "s/^#define HAVE_${feature} 1$/#define HAVE_${feature} 0/" config.h
    grep -q "^#define HAVE_${feature} 0$" config.h || { echo "config.h: HAVE_${feature} not found" >&2; exit 1; }
done
grep -q '^#define HAVE_PTHREADS 1$' config.h || { echo "configure found no pthreads" >&2; exit 1; }

make -j"$JOBS"
make install
ls -la "$PREFIX/lib"
