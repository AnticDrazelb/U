#!/bin/sh
# build64.sh [cflags...]: (04.22) game.c and zlib's bench for AArch64, for dynbench64's image mode
# (loaded and run at 0x10000000, x19 = frames/rounds): game64.bin, zbench64.bin (zlib from
# build-zlib.sh's sources, when there)
cd "$(dirname "$0")"
T="--target=aarch64-none-elf -mcpu=cortex-a57 ${*:--O2} -ffreestanding -fno-stack-protector"
clang-21 $T -c start64.s -o start64.o && clang-21 $T -c game.c -o game64.o &&
ld.lld-21 -Ttext=0x10000000 -e 0x10000000 start64.o game64.o -o game64.elf &&
llvm-objcopy-21 -O binary game64.elf game64.bin && ls -la game64.bin

# (04.27) unity.c: IL2CPP-shaped code (null and bounds checks to cold throws, virtual calls, write
# barriers, an Interlocked add): unity64.bin
clang-21 $T -mno-outline-atomics -c unity.c -o unity64.o &&
ld.lld-21 -Ttext=0x10000000 -e 0x10000000 start64.o unity64.o -o unity64.elf &&
llvm-objcopy-21 -O binary unity64.elf unity64.bin && ls -la unity64.bin
