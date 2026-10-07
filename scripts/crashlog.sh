#!/usr/bin/env bash
# Fetch /data/Crema/{boot-trace,crash,hang}.log and Cemu's log.txt from the console and symbolize
# the crash and hang addresses against the Cemu ELF (default: the newest packaged version's, from
# oelf/).
#   crashlog.sh [elf]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
ELF="${1:-$(ls -t "$PS4_BUILD_ROOT"/oelf/v*.elf 2>/dev/null | head -1)}"
HOST="ftp://${PS4_HOST:-192.168.0.90}:${PS4_FTP_PORT:-2121}"
OUT="$PS4_BUILD_ROOT/logs"
mkdir -p "$OUT"
for f in boot-trace.log boot-trace-game.log crash.log hang.log log.txt; do
    curl -s --max-time 30 -o "$OUT/$f" "$HOST/data/Crema/$f" || rm -f "$OUT/$f"
done
symbolize() {
    llvm-symbolizer-21 --obj="$ELF" --functions=linkage --demangle --no-inlines "$1" | head -2 | paste -sd' '
}
# A thread stack dump (ps4_dump_thread_stacks: hang.log, the end of a crash) from stdin, symbolized.
print_thread_dump() {
    while IFS= read -r line; do
        case "$line" in
        "-- "*)
            echo "$line"
            rip="$(echo "$line" | grep -oE "rip elf 0x[0-9a-f]+" | grep -oE "0x[0-9a-f]+$" || true)"
            [ -n "$rip" ] && echo "     rip  $(symbolize "$rip")"
            ;;
        *"stack:"*)
            for addr in $(echo "$line" | grep -oE "0x[0-9a-f]+" | head -14); do
                echo "     $addr  $(symbolize "$addr")"
            done
            ;;
        *) echo "$line" ;;
        esac
    done
}
# TV screenshots the frontend takes during a run (PS4WindowSystem.cpp)
rm -rf "$OUT/screenshots"; mkdir -p "$OUT/screenshots"
for f in $(curl -s --max-time 30 --list-only "$HOST/data/Crema/screenshots/" | grep '\.png$'); do
    curl -s --max-time 60 -o "$OUT/screenshots/$f" "$HOST/data/Crema/screenshots/$f"
done
echo "== screenshots: $(ls "$OUT/screenshots" | tr '\n' ' ')"
echo "== boot-trace.log"; cat "$OUT/boot-trace.log" 2>/dev/null || echo "(none)"
# a game that stopped and went back to the launcher: its own trace (the launcher's is above)
if [ -s "$OUT/boot-trace-game.log" ] && grep -q "game stopped" "$OUT/boot-trace.log" 2>/dev/null; then
    echo "== boot-trace-game.log (the game that stopped)"; grep -v "mmap:" "$OUT/boot-trace-game.log" | tail -40
fi
echo "== log.txt (last 60 lines)"; tail -60 "$OUT/log.txt" 2>/dev/null || echo "(none)"
# crash.log collects every run's crashes: only the last one, and only if this run crashed
if [ -s "$OUT/crash.log" ] && grep -q "crashed\|game stopped: The game crashed" "$OUT/boot-trace.log" "$OUT/boot-trace-game.log" 2>/dev/null; then
    echo "== crash.log (last crash)"
    last="$(awk '/^=== /{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$OUT/crash.log")"
    # the crashing thread (before the other threads' dump)
    crashed="$(printf '%s\n' "$last" | sed '/^==== /,$d')"
    printf '%s\n' "$crashed" | grep -v "^  \[rsp"
    echo "== symbolized"
    printf '%s\n' "$crashed" | grep -oE "(rip 0x[0-9a-f]+ elf |^  \[rsp\+0x[0-9a-f]+\] )0x[0-9a-f]+" | grep -oE "0x[0-9a-f]+$" |
        while read -r addr; do
            printf '%s  ' "$addr"
            symbolize "$addr"
        done
    # the game's code around the crash (guest code lines), disassembled
    if printf '%s
' "$crashed" | grep -q "^guest code "; then
        echo "== the game's code around the crash (LR, then the callers)"
        printf '%s
' "$crashed" > "$OUT/crash-block.txt"
        python3 "$HERE/guest-code.py" "$OUT/crash-block.txt"
    fi
    # every other thread at the crash (ps4_dump_thread_stacks), symbolized like hang.log
    if printf '%s\n' "$last" | grep -q "^==== "; then
        echo "== the other threads at the crash"
        printf '%s\n' "$last" | sed -n '/^==== /,$p' | print_thread_dump
    fi
    # the emulated side (ps4_crash_guest_info: guest CPU registers, stack trace, threads)
    if grep -q "Active PPC instance" "$OUT/log.txt" 2>/dev/null; then
        echo "== guest state at the crash (log.txt)"
        sed -n '/Game info/,$p' "$OUT/log.txt" | grep -v "^-*$"
    fi
else
    echo "== crash.log: no crash in this run"
fi
# the GPU driver's files (port/mesa/ac_ps4_drm.c, in data_dir/gpu): its log, a GPU
# fault report, and with Diagnostics > GPU Fault Recorder the flight recorder (progress.log: the
# last submission and per-draw trace id the GPU reached; ib-N.bin: the last 8 command streams)
G="$HOST/data/Crema/gpu"
rm -rf "$OUT/gpu"; mkdir -p "$OUT/gpu"
for f in radv-ps4.log gpu-fault.txt; do
    curl -s --max-time 30 -o "$OUT/gpu/$f" "$G/$f" || rm -f "$OUT/gpu/$f"
done
for f in $(curl -s --max-time 30 --list-only "$G/flight/" | grep -E '\.(log|bin)$'); do
    curl -s --max-time 60 -o "$OUT/gpu/$f" "$G/flight/$f"
done
[ -s "$OUT/gpu/gpu-fault.txt" ] && { echo "== gpu-fault.txt"; head -40 "$OUT/gpu/gpu-fault.txt"; }
if [ -s "$OUT/gpu/progress.log" ]; then
    echo "== GPU flight recorder (last lines of progress.log; command streams in $OUT/gpu)"
    tail -15 "$OUT/gpu/progress.log"
fi
# hang.log: the watchdog's stack dumps of every named thread (PS4WindowSystem.cpp)
if [ -s "$OUT/hang.log" ]; then
    echo "== hang.log (last dump, symbolized; stacks may include stale return addresses)"
    awk '/^==== /{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$OUT/hang.log" | print_thread_dump
fi
