// What the PS4 build defines besides the benchmark: mprotect (below), and the emulator hooks
// Crema's runtime calls when it writes crash.log and hang.log (weak references in
// port/ps4_crashlog.cpp and port/ps4_sampler.cpp). create-fself turns an unresolved weak reference
// into an import it can't place, so this program defines them, doing nothing.
#include <cstddef>
#include <cstdint>

extern "C" {
int32_t sceKernelMprotect(const void* addr, size_t len, int32_t prot);

// xbyak's protect() refers to mprotect, which Crema never imports; the PS4 code allocator never
// calls it (dynarmic-ps4.patch maps code memory executable once). Defined here so every system
// function this program imports is one Crema imports and the consoles already run.
int mprotect(void* addr, size_t len, int prot)
{
    return sceKernelMprotect(addr, len, prot);
}

void ps4_crash_guest_info() {}
void ps4_crash_guest_regs(int) {}
void ps4_crash_jit_info(int, uint64_t, int) {}
void ps4_crash_note() {}
void ps4_crash_restart() {}
int ps4_jit_owner(uint64_t, uint32_t*, uint64_t*)
{
    return 0;
}
void ps4_jit_dump_function(int, uint32_t, uint64_t, uint32_t) {}
void ps4_recompiler_status(int) {}
void ps4_hang_guest_threads(int) {}
}
