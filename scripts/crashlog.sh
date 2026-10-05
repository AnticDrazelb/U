#!/usr/bin/env bash
# Fetch /data/CemuPS4/{boot-trace,crash}.log and Cemu's log.txt from the console and symbolize the
# crash addresses against the Cemu ELF (default: the newest packaged version's, from oelf/).
#   crashlog.sh [elf]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
ELF="${1:-$(ls -t "$PS4_BUILD_ROOT"/oelf/v*.elf 2>/dev/null | head -1)}"
HOST="ftp://${PS4_HOST:-192.168.0.90}:${PS4_FTP_PORT:-2121}"
OUT="$PS4_BUILD_ROOT/logs"
mkdir -p "$OUT"
for f in boot-trace.log crash.log log.txt; do
    curl -s --max-time 30 -o "$OUT/$f" "$HOST/data/CemuPS4/$f" || rm -f "$OUT/$f"
done
echo "== boot-trace.log"; cat "$OUT/boot-trace.log" 2>/dev/null || echo "(none)"
echo "== log.txt (last 60 lines)"; tail -60 "$OUT/log.txt" 2>/dev/null || echo "(none)"
if [ -s "$OUT/crash.log" ]; then
    echo "== crash.log (last crash)"
    last="$(awk '/^=== signal/{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$OUT/crash.log")"
    printf '%s\n' "$last" | grep -v "^  \[rsp"
    echo "== symbolized"
    printf '%s\n' "$last" | grep -oE "(rip 0x[0-9a-f]+ elf |^  \[rsp\+0x[0-9a-f]+\] )0x[0-9a-f]+" | grep -oE "0x[0-9a-f]+$" |
        while read -r addr; do
            printf '%s  ' "$addr"
            llvm-symbolizer-21 --obj="$ELF" --functions=linkage --demangle --no-inlines "$addr" | head -2 | paste -sd' '
        done
fi
