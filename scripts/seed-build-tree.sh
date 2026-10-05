#!/usr/bin/env bash
# One-time: create ~/cemups4-build from copies of DolphinPS4's sysroot (libc++ 21 + libc overlay)
# and Mesa source, so this port never writes into ~/dolphinps4-build.
set -euo pipefail
DPS="${DPS:-$HOME/dolphinps4-build}"
PSB="${PSB:-$HOME/cemups4-build}"
mkdir -p "$PSB/src" "$PSB/build" "$PSB/out" "$PSB/logs"
[ -d "$PSB/sysroot" ] || cp -a "$DPS/sysroot" "$PSB/sysroot"
[ -d "$PSB/src/mesa" ] || cp -a "$DPS/src/mesa" "$PSB/src/mesa"
[ -d "$PSB/src/libdrm" ] || cp -a "$DPS/src/libdrm" "$PSB/src/libdrm"
[ -d "$PSB/mesa-venv" ] || cp -a "$DPS/mesa-venv" "$PSB/mesa-venv"
[ -d "$PSB/host-tools" ] || cp -a "$DPS/host-tools" "$PSB/host-tools"
[ -d "$PSB/tools/go" ] || { mkdir -p "$PSB/tools"; cp -a "$DPS/tools/go" "$PSB/tools/go"; }
# Space-free path to this repo: toolchain flags (linker script paths) break on "PS4 Projects".
REPO="$(cd "$(dirname "$0")/.." && pwd)"
[ -e "$HOME/cemups4" ] || ln -s "$REPO" "$HOME/cemups4"
du -sh "$PSB"/*
git -C "$PSB/src/mesa" log -1 --oneline; git -C "$PSB/src/mesa" status --short | head
