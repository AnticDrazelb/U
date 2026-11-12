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
SOURCES="memory dirty xbe cpu kernel kernel_rtl kernel_crypto files xiso devices nv2a render draw pixel usb audio apu nic xemu_glue diagnostics gpu_vulkan platform_freebsd main"
# xemu's devices (src/third_party; the NV2A not: no Vulkan here), C, with their shims
TP="$HERE/src/third_party"
XEMU_C="xemu/hw/xbox/mcpx/apu/apu.c xemu/hw/xbox/mcpx/apu/vp/vp.c xemu/hw/xbox/mcpx/apu/monitor.c xemu/hw/xbox/mcpx/apu/debug.c
    xemu/hw/xbox/mcpx/apu/dsp/gp_ep.c xemu/hw/xbox/mcpx/apu/dsp/dsp.c xemu/hw/xbox/mcpx/apu/dsp/dsp_c.c xemu/hw/xbox/mcpx/apu/dsp/dsp_dma.c
    xemu/hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.c libsamplerate/samplerate.c libsamplerate/src_sinc.c libsamplerate/src_linear.c
    libsamplerate/src_zoh.c xemu/hw/xbox/mcpx/nvnet/nvnet.c xemu/shim/xb_qemu_timer.c xemu/shim/xb_bql.c xemu/shim/xb_memory.c
    xemu/hw/usb/core.c xemu/hw/usb/desc.c xemu/hw/usb/hcd-ohci.c xemu/hw/usb/dev-hub.c xemu/hw/usb/desc-msos.c xemu/hw/usb/xb_bus.c
    xemu/hw/usb/xb_usb.c xemu/hw/xbox/xid.c xemu/hw/xbox/xid-gamepad.c"
XEMU_INC="-I$TP/xemu/shim -I$TP/xemu -I$TP/xemu/hw/xbox/mcpx/apu/dsp -I$TP/xemu/hw/xbox/mcpx/apu/dsp/interp -I$TP/libsamplerate -I$TP/xemu/hw/xbox/mcpx/nvnet -I$TP/xemu/hw/usb -I$TP/xxhash"
XEMU_FLAGS="-O2 -g -march=btver2 -w -DNDEBUG -DHAVE_STDBOOL_H -DPACKAGE=\"libsamplerate\" -DVERSION=\"0.2.2\" -DENABLE_SINC_FAST_CONVERTER -DCPU_CLIPS_POSITIVE=0 -DCPU_CLIPS_NEGATIVE=0 $XEMU_INC"
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
        (c++ -std=c++20 $FLAGS $XEMU_INC -c "$HERE/src/$s.cpp" -o "$o" && cp "$HERE/src/$s.cpp" "$OUT/obj/$s.cpp.built") &
        pids="$pids $!"
    fi
done
cc $FLAGS -c "$HERE/src/switch.S" -o "$OUT/obj/switch.o"
# xemu's C files too when any header under third_party changed (a structure's layout: an object built
# against the old one reads the wrong fields)
tp_headers_changed=0
find "$TP" -name '*.h' | sort | xargs cat | cmp -s - "$OUT/obj/tp-headers.built" || tp_headers_changed=1
for c in $XEMU_C; do
    o="$OUT/obj/x_$(echo "$c" | tr '/.' '__').o"
    if [ ! -f "$o" ] || ! cmp -s "$TP/$c" "$o.src" || [ "$tp_headers_changed" = 1 ]; then
        (cc -std=gnu11 $XEMU_FLAGS -c "$TP/$c" -o "$o" && cp "$TP/$c" "$o.src") &
        pids="$pids $!"
    fi
done
fail=0
for p in $pids; do wait "$p" || fail=1; done
[ "$fail" = 0 ] || { echo "compile failed" >&2; exit 1; }
cat "$HERE"/src/*.h "$HERE"/src/*.inc 2>/dev/null > "$OUT/obj/headers.built"
find "$TP" -name '*.h' | sort | xargs cat > "$OUT/obj/tp-headers.built"
c++ -no-pie -Wl,--image-base="${XB_IMAGE_BASE:-0x400000}" -o "$OUT/xbox-core" "$OUT"/obj/*.o -lpthread -lutil -lm
echo "$OUT/xbox-core"
