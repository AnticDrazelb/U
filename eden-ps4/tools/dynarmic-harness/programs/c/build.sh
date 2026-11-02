#!/bin/sh
# build.sh [cflags...]: game.c (default -O2) for the harness's image mode (loaded and run at 0x10000000): game.bin
cd "$(dirname "$0")"
T="--target=armv7a-none-eabihf -mcpu=cortex-a57 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard ${*:--O2} -ffreestanding -fno-stack-protector"
clang-21 $T -c start.s -o start.o && clang-21 $T -c game.c -o game.o &&
ld.lld-21 -Ttext=0x10000000 -e 0x10000000 start.o game.o -o game.elf &&
llvm-objcopy-21 -O binary game.elf game.bin && ls -la game.bin
