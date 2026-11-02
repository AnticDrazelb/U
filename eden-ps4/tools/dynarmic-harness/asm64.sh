#!/bin/sh
# asm64.sh <in.s> <out.bin>: A64, linked at 0x10000
clang-21 --target=aarch64-none-elf -c "$1" -o "$2.o" && ld.lld-21 --image-base=0x10000 -Ttext=0x10000 -e 0x10000 "$2.o" -o "$2.elf" && llvm-objcopy-21 -O binary --only-section=.text "$2.elf" "$2"
