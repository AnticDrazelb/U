#!/usr/bin/env bash
# Desktop regression tests for the launcher and the test programs: each program runs in the
# desktop build (build.sh host) with scripted controller input, and what it printed, drew and
# played is checked. Builds the programs first (hello/build.sh, game/build.sh).
#   EDEN_PS4_BIN  the desktop build (default ~/eden-host/bin/eden-ps4)
# Exit code: the number of failed checks.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${EDEN_PS4_BIN:-$HOME/eden-host/bin/eden-ps4}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
failed=0

check() { # check <description> <command...>
    local what="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        echo "  ok    $what"
    else
        echo "  FAIL  $what"
        failed=$((failed + 1))
    fi
}

# run <name> <program> <timeout> <input script> [extra env...]: output in $work/<name>.log, frames
# in $work/<name>-frames, sound in $work/<name>.wav, exit code in $work/<name>.code
run() {
    local name="$1" program="$2" timeout="$3" input="$4"
    shift 4
    env EDEN_PS4_DATA="$work/data" EDEN_PS4_FRAMES="$work/$name-frames" EDEN_PS4_AUDIO="$work/$name.wav" \
        EDEN_PS4_INPUT="$input" EDEN_PS4_TIMEOUT="$timeout" "$@" "$BIN" "$program" >"$work/$name.log" 2>&1
    echo $? >"$work/$name.code"
}
code() { [ "$(cat "$work/$1.code")" = "$2" ]; }
says() { grep -q "$2" "$work/$1.log"; }
drew() { [ -s "$work/$1-frames/last.ppm" ] && head -c 20 "$work/$1-frames/last.ppm" | grep -q "$2"; }
# loud <name> <seconds>: the WAV has at least that many seconds of sound above a whisper
loud() {
    python3 - "$work/$1.wav" "$2" <<'PY'
import struct, sys, wave
w = wave.open(sys.argv[1])
data = w.readframes(w.getnframes())
samples = struct.unpack('<%dh' % (len(data) // 2), data)[0::w.getnchannels()]
window = 4800
loud = sum(1 for i in range(0, len(samples), window) if max(map(abs, samples[i:i + window]), default=0) > 300)
sys.exit(0 if loud * window / 48000 >= float(sys.argv[2]) else 1)
PY
}

echo "building the programs"
OUT="$work" "$HERE/hello/build.sh" >/dev/null || { echo "hello/build.sh failed"; exit 1; }
OUT="$work" "$HERE/game/build.sh" >/dev/null || { echo "game/build.sh failed"; exit 1; }

echo "hello.nro: stage 2's checks"
run hello "$work/hello.nro" 30 ""
check "exits" code hello 0
check "all checks pass" says hello "hello: all checks passed"

echo "breakout.nro: picture, controller, sound"
run breakout "$work/breakout.nro" 20 "2:A,2.2:,3:Right,4:,7:Plus,7.3:"
check "exits on +" code breakout 0
check "A launches the ball" says breakout "breakout: ball launched"
check "draws 640x360 frames" drew breakout "640 360"
check "beeps (audout)" loud breakout 0.2

echo "blocks.nro: picture, sound, the SD card"
run blocks "$work/blocks.nro" 20 "1:Up,1.1:,1.6:Up,1.7:,2.2:Up,2.3:,5:Plus,5.3:"
check "exits on +" code blocks 0
check "draws 640x360 frames" drew blocks "640 360"
check "plays its tune" loud blocks 2
check "saves its best score" test -s "$work/data/user/sdmc/blocks-best.txt"
best="$(cat "$work/data/user/sdmc/blocks-best.txt" 2>/dev/null)"
run blocks2 "$work/blocks.nro" 20 "1:Plus,1.3:"
check "reads it back" says blocks2 "best score $best\$"
check "doesn't save a lower score" bash -c "! grep -q 'saved' '$work/blocks2.log'"

echo "padtest.nro: the controller test"
run padtest "$work/padtest.nro" 20 "1:A+Left,2:,3:Plus+Minus,3.3:"
check "exits on + and -" code padtest 0
check "draws 1280x720 frames (libnx's console)" drew padtest "1280 720"

echo "gputest.nro: drawn with the GPU (clears, done in software by the null renderer)"
run gputest "$work/gputest.nro" 20 "5:Plus,5.5:"
check "exits on +" code gputest 0
check "submits GPU work and waits on its fences" says gputest "gputest: [0-9]* fps"
check "its frames have the picture" python3 - "$work/gputest-frames/last.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
pixels = data[data.index(b'255\n') + 4:]
sys.exit(0 if sum(pixels) / len(pixels) > 40 else 1)
PY
# the first frame's triangle (drawn with shaders): red near its right corner, the red vertex
check "its shader triangle is drawn (software shaders)" python3 - "$work/gputest-frames/frame-0001.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
r, g, b = pixels[(180 * width + 395) * 3:(180 * width + 395) * 3 + 3]
sys.exit(0 if r > g + 60 and r > b + 60 else 1)
PY
# its textured square (the sprite sampled by the texture unit): the sprite's orange at its centre
check "its textured square is drawn (software texture sampling)" python3 - "$work/gputest-frames/frame-0001.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
r, g, b = pixels[(261 * width + 144) * 3:(261 * width + 144) * 3 + 3]
sys.exit(0 if r > 200 and 120 < g < 240 and b < 140 else 1)
PY
# its backdrop of tiles (a texture the DMA engine made block linear, filtered bilinearly, blended):
# along a row, colours vary where the bands alone are flat
check "its tiled backdrop is drawn (block-linear texture, bilinear, blended)" python3 - "$work/gputest-frames/frame-0001.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
row = {pixels[(20 * width + x) * 3:(20 * width + x) * 3 + 3] for x in range(width)}
sys.exit(0 if len(row) > 20 else 1)
PY
# its depth test: two bars across each other in depth, red in front on the left, green on the right
check "its depth test works (red bar in front on the left, green on the right)" python3 - "$work/gputest-frames/frame-0001.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
at = lambda x, y: pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]
(lr, lg, lb), (rr, rg, rb) = at(480, 54), at(608, 54)
sys.exit(0 if lr > 200 and lg < 100 and rg > 200 and rr < 100 else 1)
PY
# its square coloured from a storage buffer (a global load the shader recompiler tracks to one),
# drawn with its indices in the command stream
check "its storage buffer is read (a cyan square, top left, drawn with inline indices)" python3 - "$work/gputest-frames/frame-0001.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
r, g, b = pixels[(22 * width + 48) * 3:(22 * width + 48) * 3 + 3]
sys.exit(0 if r < 60 and g > 200 and b > 200 else 1)
PY
# its lines (a yellow rectangle, 3 pixels wide) and point (white, 6 pixels), bottom right
check "its lines and point are drawn" python3 - "$work/gputest-frames/frame-0001.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
at = lambda x, y: pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]
(lr, lg, lb), (pr, pg, pb) = at(566, 305), at(566, 325)
sys.exit(0 if lr > 220 and lg > 200 and lb < 80 and min(pr, pg, pb) > 230 else 1)
PY
# its stencil test: an orange triangle (a square drawn where a triangle set the stencil), left
check "its stencil test works (an orange triangle, left)" python3 - "$work/gputest-frames/frame-0001.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
at = lambda x, y: pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]
(ir, ig, ib), (orr, og, ob) = at(56, 225), at(20, 190)
sys.exit(0 if ir > 230 and 100 < ig < 180 and ib < 60 and not (orr > 230 and ob < 60) else 1)
PY
# a texture the CPU writes to: from frame 30 the sprite's middle is blue, and the turning square
# (which samples the sprite) shows it in the last frame
check "a texture the CPU rewrites is drawn as rewritten" python3 - "$work/gputest-frames/last.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
r, g, b = pixels[(261 * width + 144) * 3:(261 * width + 144) * 3 + 3]
sys.exit(0 if b > 200 and r < 80 and g < 120 else 1)
PY
# its bouncing squares, cleared by a GPU macro (MME, run by Eden's macro interpreter): the first
# is grey, top left
check "its squares are drawn by a GPU macro" python3 - "$work/gputest-frames/frame-0001.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
r, g, b = pixels[(74 * width + 53) * 3:(74 * width + 53) * 3 + 3]
sys.exit(0 if all(abs(c - 77) < 6 for c in (r, g, b)) else 1)
PY
check "its macro is interpreted (no macro JIT)" bash -c "! grep -q 'Macro JIT' '$work/gputest.log'"
# its floor in perspective, mipmapped (a colour a level), striped by a branch in its shader: near,
# level 0's grey checkerboard blended toward level 1's red; halfway, orange (levels 1 and 2); far,
# blue (level 5); a darkened stripe between two bright ones
check "its floor picks its texture's levels from derivatives, and its shader branches per pixel" python3 - "$work/gputest-frames/frame-0001.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
at = lambda x, y: pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]
near, middle, far = at(384, 350), at(384, 315), at(384, 285)
bright, dark = at(384, 333), at(384, 339)
ok = near[0] > 200 and near[1] > 150 and near[2] > 150
ok &= middle[0] > 200 and middle[1] < 150 and middle[2] < 100
ok &= far[2] > 200 and far[0] < 130 and far[1] < 130
ok &= dark[0] < bright[0] * 0.75 and dark[1] < bright[1] * 0.75
sys.exit(0 if ok else 1)
PY
# its squares whose fragment programs write their own depths (otherwise the same: the green one,
# drawn second, would cover the red), top right under the bars: red in front on the left, green on
# the right; and the tile through an sRGB header below them, its colours decoded (the tile's green,
# 120 and up, averages under 110 decoded)
check "its fragment programs' own depths are tested, and sRGB textures decoded" python3 - "$work/gputest-frames/frame-0001.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
at = lambda x, y: pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]
(lr, lg, lb), (rr, rg, rb) = at(480, 112), at(608, 112)
ok = lr > 200 and lg < 100 and rg > 200 and rr < 100
square = [at(x, y) for x in range(468, 525) for y in range(139, 168)]
green = sum(p[1] for p in square) / len(square)
ok &= 40 < green < 110
sys.exit(0 if ok else 1)
PY
# its compute program: 4x4 workgroups of 8x8 invocations, each putting a colour from where it is in
# shared memory and, after a barrier, writing its mirror image's (in its workgroup) to a texture
# shown on a square at the top: every texel as worked out here
check "its compute program runs (shared memory, a barrier, a storage buffer)" python3 - "$work/gputest-frames/frame-0001.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
at = lambda x, y: tuple(pixels[(y * width + x) * 3:(y * width + x) * 3 + 3])
bad = 0
for py in range(11, 52):
    for px in range(322, 382):
        u, v = (px + 0.5 - 320) / 64 * 32, (py + 0.5 - 9) / 45 * 32
        if abs(v - round(v)) < 0.05:
            continue  # on a texel's edge
        x, y = int(u), int(v)
        tx, ty = x % 8, y % 8
        expected = ((x // 8 * 8 + 7 - tx) * 8, (y // 8 * 8 + 7 - ty) * 8, (63 - (ty * 8 + tx)) * 4)
        bad += at(px, py) != expected
sys.exit(0 if bad == 0 else 1)
PY
# its storage image: a compute program storing (formatted stores) each texel's colour, read from a
# texel buffer of 8 by (x + y) / 4 % 8, shown on the square below: every texel as worked out here
check "its storage image is written (formatted stores) from a texel buffer" python3 - "$work/gputest-frames/frame-0001.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
at = lambda x, y: tuple(pixels[(y * width + x) * 3:(y * width + x) * 3 + 3])
palette = [(0.9, 0.2, 0.2), (0.95, 0.6, 0.1), (0.95, 0.9, 0.2), (0.3, 0.85, 0.3),
           (0.2, 0.8, 0.85), (0.25, 0.4, 0.95), (0.6, 0.3, 0.9), (0.95, 0.95, 0.95)]
bad = checked = 0
for py in range(65, 106):
    for px in range(322, 382):
        u, v = (px + 0.5 - 320) / 64 * 32, (py + 0.5 - 63) / 45 * 32
        if abs(v - round(v)) < 0.05:
            continue  # on a texel's edge
        expected = tuple(int(c * 255 + 0.5) for c in palette[(int(u) + int(v)) // 4 % 8])
        bad += max(abs(a - b) for a, b in zip(at(px, py), expected)) > 1
        checked += 1
sys.exit(0 if bad == 0 and checked > 2000 else 1)
PY
# on the right: an indexed strip of two magenta quads with a restart index between them (nothing
# drawn between), and a point of 32 pixels coloured by its coordinates (black top left, red top
# right, green bottom left, yellow bottom right)
check "its primitive restart and point coordinates work" python3 - "$work/gputest-frames/frame-0001.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
width = int(data.split(b'\n')[1].split()[0])
pixels = data[data.index(b'255\n') + 4:]
at = lambda x, y: tuple(pixels[(y * width + x) * 3:(y * width + x) * 3 + 3])
magenta = lambda p: p[0] > 200 and p[1] < 80 and p[2] > 180
ok = magenta(at(582, 186)) and magenta(at(618, 186)) and not magenta(at(600, 186))
tl, tr, bl, br = at(586, 208), at(614, 208), at(586, 236), at(614, 236)
ok &= max(tl[:2]) < 60 and tr[0] > 200 and tr[1] < 60 and bl[1] > 200 and bl[0] < 60 and min(br[:2]) > 200
sys.exit(0 if ok else 1)
PY

echo "gputest.nro's stress test: 400 more sprites, a draw each (queued, rasterized together)"
run gputest_stress "$work/gputest.nro" 20 "2:A,2.3:,7:Plus,7.5:"
check "exits on +" code gputest_stress 0
check "draws its sprites" says gputest_stress "fps, [0-9]* command words a frame (stress test)"

echo "gputest.nro's 3D stress test: 12 large lit, textured, depth-tested quads"
run gputest_3d "$work/gputest.nro" 20 "2:B,2.3:,7:Plus,7.5:"
check "exits on +" code gputest_3d 0
check "draws them" says gputest_3d "fps, [0-9]* command words a frame (3D stress test)"

echo "the launcher: stopping a program"
run stop "$work/breakout.nro" 20 "2:Stop"
check "Stop ends it (exit code 3)" code stop 3
check "says so" says stop "the program was stopped"

echo "the launcher: a program's full-memory heap (libnx's default) is quick to clear"
cat >"$work/fullheap.c" <<'C'
#include <stdio.h>
#include <switch.h>
int main(void) {
    consoleDebugInit(debugDevice_SVC);
    fprintf(stderr, "fullheap: started\n");
    return 0;
}
C
"${NX_BUILD:-$HOME/nx/devkitpro/libnx-clang/nx-build}" "$work/fullheap.nro" fullheap test 1.0 "$work/fullheap.c" >/dev/null 2>&1
start=$(date +%s)
run fullheap "$work/fullheap.nro" 60 ""
check "starts and exits in under 15 s" bash -c "grep -q 'fullheap: started' '$work/fullheap.log' && [ \$(( \$(date +%s) - $start )) -lt 15 ]"

echo "$failed failed"
exit "$failed"
