// What the PS4 build defines besides the launcher: mprotect (below), the emulator hooks Crema's
// runtime calls when it writes crash.log and hang.log (weak references in port/ps4_crashlog.cpp
// and port/ps4_sampler.cpp), and zstd's trace hooks. create-fself turns an unresolved weak
// reference into an import it can't place, so this program defines them, doing nothing.
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

// Without Mesa's RADV linked (built by scripts/build-mesa.sh, linked when it's there: see
// CMakeLists.txt), its entry points say there's no Vulkan: the launcher keeps the null renderer.
#ifndef EDEN_PS4_RADV
void* vk_icdGetInstanceProcAddr(void*, const char*)
{
    return nullptr;
}
void* vk_common_GetDeviceProcAddr(void*, const char*)
{
    return nullptr;
}
int ac_ps4_submit_flip(int, unsigned, unsigned, int64_t)
{
    return -1;
}
uint64_t ac_ps4_gpu_bytes()
{
    return 0;
}
void ac_ps4_get_gpu_stats(uint64_t* busy_ticks, uint64_t* span_ticks, uint64_t* submits, uint64_t* waits,
                          uint64_t* wait_ns)
{
    *busy_ticks = *span_ticks = *submits = *waits = *wait_ns = 0;
}
#endif

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

// zstd's trace hooks (weak references in Eden's zstd and zbic builds): 0 is "not tracing", so the
// _end hooks are never called.
unsigned long long ZSTD_trace_compress_begin(const void*)
{
    return 0;
}
void ZSTD_trace_compress_end(unsigned long long, const void*) {}
unsigned long long ZSTD_trace_decompress_begin(const void*)
{
    return 0;
}
void ZSTD_trace_decompress_end(unsigned long long, const void*) {}
}
