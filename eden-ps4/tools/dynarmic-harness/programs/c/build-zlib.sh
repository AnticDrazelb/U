#!/bin/sh
# build-zlib.sh [cflags...]: zbench.c with zlib (~/eden-deps/zlib) for the harness's image mode: zbench.bin
cd "$(dirname "$0")"
Z=${ZLIB_SRC:-$HOME/eden-deps/zlib}
T="--target=armv7a-none-eabihf -mcpu=cortex-a57 -marm -mfpu=neon-fp-armv8 -ffunction-sections -fdata-sections -mfloat-abi=hard -ffreestanding -fno-stack-protector -DZ_SOLO -I$Z ${*:--O2}"
objs=""
for f in adler32 crc32 deflate inflate inftrees inffast trees zutil; do
    clang-21 $T -w -c "$Z/$f.c" -o "z_$f.o" || exit 1
    objs="$objs z_$f.o"
done
clang-21 $T -c zbench.c -o zbench.o && clang-21 $T -c start.s -o start.o &&
ld.lld-21 --gc-sections -Ttext=0x10000000 -e _start start.o zbench.o $objs -o zbench.elf &&
llvm-objcopy-21 -O binary zbench.elf zbench.bin && ls -la zbench.bin
