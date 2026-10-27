#!/usr/bin/env bash
# Builds hello.nro (in OUT, default this folder) with clang and lld: no libnx, no devkitPro.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE}"
CLANG="${CLANG:-clang-21}"
LLD="${LLD:-ld.lld-21}"
NM="${NM:-llvm-nm-21}"
READELF="${READELF:-llvm-readelf-21}"
OBJCOPY="${OBJCOPY:-llvm-objcopy-21}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

flags=(--target=aarch64-none-elf -mcpu=cortex-a57 -O2 -fPIE -ffreestanding -fno-builtin
    -fno-stack-protector -fno-asynchronous-unwind-tables -fvisibility=hidden -Wall -Wextra -Werror)
"$CLANG" "${flags[@]}" -c "$HERE/start.S" -o "$work/start.o"
"$CLANG" "${flags[@]}" -c "$HERE/hello.c" -o "$work/hello.o"
# The image runs where it is loaded, unrelocated: no absolute addresses, no GOT.
if "$READELF" -r "$work/start.o" "$work/hello.o" | grep -E 'R_AARCH64_(ABS|.*GOT)'; then
    echo "hello: absolute addresses in the code; it must only address memory relative to itself" >&2
    exit 1
fi
"$LLD" -T "$HERE/link.ld" -nostdlib --no-undefined -z max-page-size=0x1000 \
    "$work/start.o" "$work/hello.o" -o "$work/hello.elf"
"$OBJCOPY" -O binary "$work/hello.elf" "$work/hello.bin"
# The file runs to the end of the data segment (objcopy stops at the last byte it has).
end=$((16#$("$NM" "$work/hello.elf" | awk '$3 == "__end" { print $1 }')))
truncate -s "$end" "$work/hello.bin"
mkdir -p "$OUT"
cp "$work/hello.bin" "$OUT/hello.nro"
echo "$OUT/hello.nro ($end bytes)"
