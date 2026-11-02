#!/bin/sh
# asm.sh <in.s> <out.bin>: assembled and linked at 0x10000 (the harness's code address)
T=--target=armv7a-none-eabi
clang-21 $T -mfloat-abi=hard -mfpu=neon -c "$1" -o "$2.o" && ld.lld-21 -Ttext=0x10000 -e 0x10000 "$2.o" -o "$2.elf" && llvm-objcopy-21 -O binary --only-section=.text "$2.elf" "$2"
