#!/usr/bin/env bash
# Host tests for Direct Vertex Data (no console needed), with AddressSanitizer:
#   MESA=<mesa-26.0.8 tree with patches/mesa-ps4*.patch applied> CEMU_SRC=<Cemu tree with
#   patches/cemu-ps4.patch applied> tests/host/run.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mktemp -d)"
: "${MESA:?set MESA to the patched Mesa 26.0.8 tree}" "${CEMU_SRC:?set CEMU_SRC to the patched Cemu tree}"

# 1. port/mesa/ac_ps4_drm.c: user-memory buffers, in the order RADV's bo_from_ptr/destroy call them
cp "$HERE/../../port/mesa/ac_ps4_drm.c" "$MESA/src/amd/common/ac_ps4_drm.c"
( cd "$MESA" && gcc -std=gnu11 -O1 -g -w -fsanitize=address,undefined -D__PS4__ -D__ORBIS__ -D_GNU_SOURCE \
    -DHAVE_PTHREAD -DHAVE_ENDIAN_H -DHAVE_STRUCT_TIMESPEC -DHAVE_TIMESPEC_GET \
    -Isrc/amd/common -Isrc/amd -Isrc -Iinclude -Isrc/amd/addrlib -I/usr/include/libdrm \
    "$HERE/winsys-user-memory.c" src/util/vma.c src/util/u_call_once.c src/util/simple_mtx.c \
    src/util/os_time.c src/c11/impl/threads_posix.c src/c11/impl/time.c -o "$OUT/winsys" )
echo -n "winsys user memory: "; ASAN_OPTIONS=detect_leaks=0 "$OUT/winsys" 2>/dev/null

# 2. LatteBufferCache.cpp: which vertex buffers may be read directly
python3 - "$CEMU_SRC/src/Cafe/HW/Latte/Core/LatteBufferCache.cpp" "$OUT/direct_vertex_block.inc" <<'PY'
import sys
s = open(sys.argv[1]).read()
a = s.index('// Crema setting direct_vertex (Experimental): vertex buffers the GPU reads straight')
open(sys.argv[2], 'w').write(s[a:s.index('#endif', a)])
PY
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$OUT" "$HERE/direct-vertex-eligibility.cpp" -o "$OUT/eligibility"
echo -n "direct vertex eligibility: "; "$OUT/eligibility"
