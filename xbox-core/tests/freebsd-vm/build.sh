#!/bin/sh
# Builds xbox-core on FreeBSD (no cmake needed), as the PS4 build is set up where it matters: Jaguar's
# instruction set (-march=btver2), the executable at 0x400000, src/platform_freebsd.cpp.
#   tests/freebsd-vm/build.sh [out dir]      (in the VM: run.sh copies the sources in and calls it)
#   XB_IMAGE_BASE=0x40000000 build.sh ...    linked there instead (the PS4 variant that leaves the Xbox's addresses)
set -eu
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$HOME/xc-bsd}"
mkdir -p "$OUT/obj"
VERSION="$(head -1 "$HERE/VERSION")"
FLAGS="-O2 -g -march=btver2 -Wall -Wextra -Wno-unused-parameter -I$HERE/src -DXB_NO_VULKAN -DXB_VERSION=\"$VERSION\""
SOURCES="memory xbe cpu kernel kernel_rtl kernel_crypto files xiso devices nv2a render draw pixel usb audio apu diagnostics gpu_vulkan platform_freebsd main"
# this build's identity (as cmake/build-id.cmake writes it for the other builds)
printf 'namespace xb { extern const char* const kBuildId; const char* const kBuildId = "%s %s"; }\n' \
    "$(date -u '+%Y-%m-%d %H:%M:%S UTC')" "$(od -An -N3 -tx1 /dev/urandom | tr -d ' \n')" > "$OUT/obj/build_id.cpp"
c++ -std=c++20 -c "$OUT/obj/build_id.cpp" -o "$OUT/obj/build_id.o"
pids=""
headers_changed=0
cat "$HERE"/src/*.h "$HERE"/src/*.inc 2>/dev/null | cmp -s - "$OUT/obj/headers.built" || headers_changed=1
for s in $SOURCES; do
    o="$OUT/obj/$s.o"
    # rebuilt when its source or any header differs from what it was built from: by content, not
    # by time (the sources come in by tar with the host's times; the VM's clock is its own)
    if [ ! -f "$o" ] || ! cmp -s "$HERE/src/$s.cpp" "$OUT/obj/$s.cpp.built" || [ "$headers_changed" = 1 ]; then
        (c++ -std=c++20 $FLAGS -c "$HERE/src/$s.cpp" -o "$o" && cp "$HERE/src/$s.cpp" "$OUT/obj/$s.cpp.built") &
        pids="$pids $!"
    fi
done
cc $FLAGS -c "$HERE/src/switch.S" -o "$OUT/obj/switch.o"
fail=0
for p in $pids; do wait "$p" || fail=1; done
[ "$fail" = 0 ] || { echo "compile failed" >&2; exit 1; }
cat "$HERE"/src/*.h "$HERE"/src/*.inc 2>/dev/null > "$OUT/obj/headers.built"
c++ -no-pie -Wl,--image-base="${XB_IMAGE_BASE:-0x400000}" -o "$OUT/xbox-core" "$OUT"/obj/*.o -lpthread -lutil -lm
echo "$OUT/xbox-core"
