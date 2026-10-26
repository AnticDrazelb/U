// The emulator hooks Crema's runtime calls when it writes crash.log and hang.log (weak references
// in port/ps4_crashlog.cpp and port/ps4_sampler.cpp). create-fself turns an unresolved weak
// reference into an import it can't place, so this program defines them, doing nothing.
#include <cstdint>

extern "C" {
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
