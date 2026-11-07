#!/usr/bin/env bash
# Builds nxdk's samples and runs each in xbox-core for a few seconds, saving its screen as a PNG.
#   NXDK_DIR=~/emu/nxdk tests/run-nxdk-samples.sh [sample ...]     (default: hello)
# Needs nxdk (git clone --recursive https://github.com/XboxDev/nxdk) with its prerequisites (clang,
# lld, bison, flex), and xbox-core built in ~/xc-host.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
NXDK_DIR="${NXDK_DIR:-$HOME/emu/nxdk}"
CORE="${CORE:-$HOME/xc-host/xbox-core}"
OUT="${OUT:-$HERE/out}"
export NXDK_DIR PATH="$NXDK_DIR/bin:/opt/llvm21/bin:$PATH"
mkdir -p "$OUT"
samples=("$@")
[ ${#samples[@]} -gt 0 ] || samples=(hello)
for s in "${samples[@]}"; do
    echo "=== $s"
    make -s -C "$NXDK_DIR/samples/$s" > "$OUT/$s-build.log" 2>&1 || { echo "build failed: $OUT/$s-build.log"; continue; }
    (cd "$NXDK_DIR/samples/$s/bin" && timeout 60 "$CORE" default.xbe --seconds "${SECONDS_PER_SAMPLE:-3}" \
        --png "$OUT/$s.png" > "$OUT/$s.log" 2>&1) || true
    grep -E "guest fault|halted|isn't implemented|FATAL|framebuffer" "$OUT/$s.log" || true
done
