#!/bin/sh
# Builds xbox-core on FreeBSD (no cmake needed), as the PS4 build is set up where it matters: Jaguar's
# instruction set (-march=btver2), the executable at 0x400000, src/platform_freebsd.cpp.
#   tests/freebsd-vm/build.sh [out dir]      (in the VM: run.sh copies the sources in and calls it)
set -eu
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$HOME/xc-bsd}"
mkdir -p "$OUT/obj"
VERSION="$(head -1 "$HERE/VERSION")"
FLAGS="-O2 -g -march=btver2 -Wall -Wextra -Wno-unused-parameter -I$HERE/src -DXB_VERSION=\"$VERSION\""
SOURCES="memory xbe cpu kernel kernel_rtl kernel_crypto files xiso devices nv2a render draw pixel usb audio apu diagnostics platform_freebsd main"
pids=""
for s in $SOURCES; do
    o="$OUT/obj/$s.o"
    if [ ! -f "$o" ] || [ "$HERE/src/$s.cpp" -nt "$o" ] || [ -n "$(find "$HERE/src" -name '*.h' -newer "$o" | head -1)" ]; then
        c++ -std=c++20 $FLAGS -c "$HERE/src/$s.cpp" -o "$o" &
        pids="$pids $!"
    fi
done
cc $FLAGS -c "$HERE/src/switch.S" -o "$OUT/obj/switch.o"
fail=0
for p in $pids; do wait "$p" || fail=1; done
[ "$fail" = 0 ] || { echo "compile failed" >&2; exit 1; }
c++ -no-pie -Wl,--image-base=0x400000 -o "$OUT/xbox-core" "$OUT"/obj/*.o -lpthread -lutil -lm
echo "$OUT/xbox-core"
