#!/usr/bin/env bash
# Pre-flight checks of the linked ELF (lessons from DolphinPS4/PSChrome): libraries must be imported
# by name (a full path gives CE-41839-5), the runtime's wraps/overrides must be defined (T) rather
# than imported, and there must be exactly one RW PT_LOAD.
set -uo pipefail
ELF="${1:-$HOME/cemups4-build/src/Cemu/bin/Cemu_release}"
echo "== imported libraries"; llvm-readelf-21 -d "$ELF" | grep NEEDED
if llvm-readelf-21 -d "$ELF" | grep NEEDED | grep -q '/'; then echo "ERROR: library imported by path"; fi
echo "== runtime symbols"
for s in __wrap_malloc __wrap_free __wrap_mmap __wrap_clock_gettime __wrap_pthread_once getentropy \
         cemu_ps4_fiber_switch ac_ps4_submit_flip vk_icdGetInstanceProcAddr; do
    printf '%-28s %s\n' "$s" "$(llvm-nm-21 "$ELF" | awk -v s="$s" '$NF==s{print $(NF-1)}' | head -1)"
done
echo "== program headers"; llvm-readelf-21 -lW "$ELF" | grep -E 'LOAD|TLS'
