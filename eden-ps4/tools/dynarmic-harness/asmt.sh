#!/bin/sh
# asmt.sh <in.s> <out.bin>: Thumb, linked at 0x10000
clang-21 --target=thumbv7a-none-eabi -mfloat-abi=hard -mfpu=neon -c "$1" -o "$2.o" 2>/dev/null && ld.lld-21 -Ttext=0x10000 -e 0x10000 "$2.o" -o "$2.elf" && llvm-objcopy-21 -O binary --only-section=.text "$2.elf" "$2"
