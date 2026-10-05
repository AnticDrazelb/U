E=$HOME/cemups4-build/src/Cemu/bin/Cemu_release
llvm-nm-21 "$E" > /tmp/cemu.nm
grep -E ' (_exit|exit|kill|raise|abort|getpid|_Exit)$' /tmp/cemu.nm
echo "-- libkernel stub exports:"
llvm-nm-21 /opt/pacbrew/ps4/openorbis/lib/libkernel.so 2>/dev/null | grep -E ' (_exit|kill|raise|getpid|abort)$'
