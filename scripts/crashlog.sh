#!/usr/bin/env bash
# Fetch /data/CemuPS4/{boot-trace,crash,hang}.log and Cemu's log.txt from the console and symbolize
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
for f in boot-trace.log crash.log hang.log log.txt; do
    curl -s --max-time 30 -o "$OUT/$f" "$HOST/data/CemuPS4/$f" || rm -f "$OUT/$f"
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
for f in $(curl -s --max-time 30 --list-only "$HOST/data/CemuPS4/screenshots/" | grep '\.png$'); do
    curl -s --max-time 60 -o "$OUT/screenshots/$f" "$HOST/data/CemuPS4/screenshots/$f"
done
echo "== screenshots: $(ls "$OUT/screenshots" | tr '\n' ' ')"
echo "== boot-trace.log"; cat "$OUT/boot-trace.log" 2>/dev/null || echo "(none)"
echo "== log.txt (last 60 lines)"; tail -60 "$OUT/log.txt" 2>/dev/null || echo "(none)"
# crash.log collects every run's crashes: only the last one, and only if this run crashed
if [ -s "$OUT/crash.log" ] && grep -q "crashed" "$OUT/boot-trace.log" 2>/dev/null; then
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
# hang.log: the watchdog's stack dumps of every named thread (PS4WindowSystem.cpp)
if [ -s "$OUT/hang.log" ]; then
    echo "== hang.log (last dump, symbolized; stacks may include stale return addresses)"
    awk '/^==== /{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$OUT/hang.log" | print_thread_dump
fi
