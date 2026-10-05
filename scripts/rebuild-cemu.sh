#!/usr/bin/env bash
# Fresh configure + build (after toolchain changes). Logs: ~/cemups4-build/logs/{configure,build}.log
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
[ "${1:-}" = clean ] && rm -rf "$HOME/cemups4-build/build/cemu"
bash "$HERE/logrun.sh" configure bash "$HERE/configure-cemu.sh" || { tail -20 "$HOME/cemups4-build/logs/configure.log"; exit 1; }
bash "$HERE/logrun.sh" build bash "$HERE/build-cemu.sh" -k 0
L="$HOME/cemups4-build/logs/build.log"
tail -1 "$L"; echo "errors: $(grep -c 'error:' "$L")"
grep "error:" "$L" | sed 's/.*src\/Cemu\///' | sort | uniq -c | sort -rn | head -${TOP:-80}
