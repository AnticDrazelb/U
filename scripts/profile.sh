#!/usr/bin/env bash
# Fetches the profiler's samples.log from the console (Crema: Diagnostics > Profiler) and prints
# where each emulated CPU core and the GPU thread spent its time (scripts/profile.py), named from
# the newest packaged ELF (oelf/) unless one is given.
#   profile.sh [elf] [top]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
ELF="${1:-$(ls -t "$PS4_BUILD_ROOT"/oelf/v*.elf 2>/dev/null | head -1)}"
HOST="ftp://${PS4_HOST:-192.168.0.90}:${PS4_FTP_PORT:-2121}"
OUT="$PS4_BUILD_ROOT/logs"
mkdir -p "$OUT"
curl -s --max-time 60 -o "$OUT/samples.log" "$HOST/data/Crema/samples.log"
echo "== profile from $(basename "$ELF")"
python3 "$HERE/profile.py" "$OUT/samples.log" "$ELF" "${2:-25}"
