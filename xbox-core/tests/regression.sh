#!/usr/bin/env bash
# The regression set (HANDOFF §2) on the desktop build: the kernel (also with XB_DIRTY_STRESS), SEH, disc,
# report and APU trap tests (0 failures),
# nxdk's samples (each runs and draws: PNGs in the output folder to compare with tests/*.png) and
# NevolutionX (reaches its language menu), all with XB_STRICT_SIGNAL=1. Exit status 1 if any fails.
#   tests/regression.sh [build dir, default ~/xc-host]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CORE="${1:-$HOME/xc-host}/xbox-core"
NXDK_DIR="${NXDK_DIR:-$HOME/emu/nxdk}"
NEVX="${NEVX:-$HOME/emu/homebrew/NevolutionX/NevolutionX.iso}"
OUT="${OUT:-$HOME/emu/reg}"
mkdir -p "$OUT" && cd "$OUT" && rm -rf xbox-data
export XB_STRICT_SIGNAL=1
fail=0
run() { # name, file, seconds, extra args
    local name="$1" file="$2" secs="$3"
    shift 3
    if [ ! -f "$file" ]; then echo "SKIP $name ($file missing)"; return; fi
    timeout 300 "$CORE" "$file" --seconds "$secs" --png "$name.png" "$@" > "$name.log" 2>&1
    local rc=$?
    local r
    r=$(grep -oE "(kernel|SEH|disc|big image|report|APU trap) test: [0-9]+ failures" "$name.log" | tail -1)
    if [ $rc -ne 0 ] || { [ -n "$r" ] && [[ "$r" != *" 0 failures" ]]; } || grep -q "lines dropped in the fault handler: [1-9]" "$name.log"; then
        echo "FAIL $name (exit $rc) ${r:-}"; fail=1
    else
        echo "ok   $name ${r:-}"
    fi
}
run kernel "$HERE/kernel-test/bin/default.xbe" 6
# the kernel test again with every GPU-side page protected again all the time (XB_DIRTY_STRESS): file
# reads into textures' memory must land whole (Halo 2's map cache got "end of file" mid-file)
export XB_DIRTY_STRESS=1
run kernel-dirty-stress "$HERE/kernel-test/bin/default.xbe" 40
unset XB_DIRTY_STRESS
run seh "$HERE/seh-test/bin/default.xbe" 4
run bigimage "$HERE/bigimage-test/bin/default.xbe" 3
run disc-folder "$HERE/disc-test/bin/default.xbe" 4
run disc-image "$HERE/disc-test/disc-test.iso" 4
run report "$HERE/report-test/bin/default.xbe" 3
run apu-trap "$HERE/apu-trap-test/bin/default.xbe" 3
# the APU's sound as the console's device would play it: the four tones at their levels, nothing else
if [ -f "$HERE/apu-test/bin/default.xbe" ]; then
    if XB_WAV_DEVICE=1 timeout 120 "$CORE" "$HERE/apu-test/bin/default.xbe" --seconds 4 --wav apu.wav > apu.log 2>&1 &&
        grep -q "APU test: 0 failures" apu.log && python3 "$HERE/apu-test/check.py" apu.wav > apu-sound.txt; then
        echo "ok   apu-sound $(tail -1 apu-sound.txt)"
    else
        echo "FAIL apu-sound $(tail -1 apu-sound.txt 2>/dev/null)"; fail=1
    fi
fi
# Halo's text and screen-space draws replayed (its D3D's vertex program and state): each quad at its
# pixels, on this renderer and on xbox-core's own
for renderer in default own; do
    if [ -f "$HERE/halo-text-test/bin/default.xbe" ]; then
        if XB_NV2A=$([ $renderer = own ] && echo own || echo xemu) timeout 120 "$CORE" "$HERE/halo-text-test/bin/default.xbe" \
            --seconds 3 --png "halo-text-$renderer.png" > "halo-text-$renderer.log" 2>&1 &&
            python3 "$HERE/halo-text-test/check.py" "halo-text-$renderer.png" > "halo-text-$renderer.txt"; then
            echo "ok   halo-text ($renderer) $(tail -1 "halo-text-$renderer.txt")"
        else
            echo "FAIL halo-text ($renderer) $(tail -1 "halo-text-$renderer.txt" 2>/dev/null)"; fail=1
        fi
    fi
done
# frames drawn in pieces and flipped at vblank: the shown picture always whole (the probe, as a
# presenter reads it, never without the red square drawn last), and the frames keep coming
if [ -f "$HERE/flip-test/bin/default.xbe" ]; then
    XB_PICTURE_PROBE=600,460,ff0000 timeout 120 "$CORE" "$HERE/flip-test/bin/default.xbe" --seconds 8 > flip.log 2>&1
    probe=$(grep -a "picture probe" flip.log | tail -1)
    frames=$(grep -a -o "[0-9]* frames" flip.log | tail -1 | grep -o "[0-9]*")
    if [[ "$probe" == *" 0 not "* ]] && [ "${frames:-0}" -ge 20 ]; then
        echo "ok   flip ${frames} frames, ${probe#*picture probe: }"
    else
        echo "FAIL flip ${frames:-0} frames, ${probe#*picture probe: }"; fail=1
    fi
fi
for s in triangle mesh xaudio sdl usb sdl_gamecontroller; do
    run "$s" "$NXDK_DIR/samples/$s/bin/default.xbe" 5
done
run nevolutionx "$NEVX" 20
if [ -f nevolutionx.log ] && ! grep -q "F:\\\\Apps" nevolutionx.log; then echo "FAIL nevolutionx: didn't scan its menu"; fail=1; fi
exit $fail
