// Guest memory: the regions of core.h mapped at their own addresses, and simple allocators in them.
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

#include <unistd.h>

#include "core.h"
#include "diagnostics.h"
#include "platform.h"

namespace xb {

int g_verbose = 1;
void (*g_log_sink)(const char* line) = nullptr;
std::atomic<u64> g_lines_dropped_in_handler{0};
// XB_STRICT_SIGNAL=1: a line logged inside the fault handler aborts the run (read before any handler)
const bool g_strict_signal = std::getenv("XB_STRICT_SIGNAL") != nullptr;

namespace {
void DroppedInHandler()
{
    if (g_strict_signal)
    {
        static const char m[] = "Log called inside the fault handler\n";
        (void)!write(2, m, sizeof(m) - 1);
        std::abort();
    }
    g_lines_dropped_in_handler++;
}
} // namespace

// Which threads are inside the fault handler, without thread-local storage: on the PS4 the handler
// runs with the interrupted fs base, a KPCR when guest code faulted, so anything fs-relative there
// (a thread_local, fs:0 as the TLS block's pointer) reads the guest's KPCR and ends the app. Each
// thread in the handler holds a slot with the bounds of the signal stack it runs on (the kernel's
// answer, so exact: no two threads' stacks overlap); a thread is in the handler when its stack
// pointer is within one.
namespace {
constexpr int kHandlerSlots = 64;
constexpr u64 kOwnStackSpan = 64 * 1024; // a handler on the thread's own stack: this much below its entry
std::atomic<u64> g_handler_hi[kHandlerSlots], g_handler_lo[kHandlerSlots];
std::atomic<int> g_handlers_in{0};
inline u64 StackPointer()
{
    u64 sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    return sp;
}
} // namespace

int EnterSignalHandler(u64 lo, u64 hi)
{
    if (!hi)
    {
        hi = StackPointer() + 4096; // not on a signal stack: the thread's own, below this frame
        lo = hi - kOwnStackSpan;
    }
    for (int i = 0; i < kHandlerSlots; i++)
    {
        u64 free = 0;
        if (g_handler_hi[i].compare_exchange_strong(free, ~0ull))
        {
            g_handler_lo[i].store(lo);
            g_handler_hi[i].store(hi);
            g_handlers_in.fetch_add(1);
            return i;
        }
    }
    return -1;
}

void LeaveSignalHandler(int slot)
{
    if (slot < 0)
        return;
    g_handlers_in.fetch_sub(1);
    g_handler_lo[slot].store(0);
    g_handler_hi[slot].store(0);
}

bool InSignalHandler()
{
    if (g_handlers_in.load(std::memory_order_acquire) == 0)
        return false;
    const u64 sp = StackPointer();
    for (int i = 0; i < kHandlerSlots; i++)
    {
        const u64 hi = g_handler_hi[i].load(std::memory_order_acquire);
        if (hi && hi != ~0ull && sp < hi && sp >= g_handler_lo[i].load(std::memory_order_relaxed))
            return true;
    }
    return false;
}

void Log(const char* fmt, ...)
{
    if (InSignalHandler())
    {
        DroppedInHandler();
        return;
    }
    static std::mutex m;
    // each line with the seconds since the start: where the time went
    char line[2048];
    const int stamp = std::snprintf(line, sizeof(line), "[%9.3f] ", diag::Seconds());
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line + stamp, sizeof(line) - size_t(stamp), fmt, ap);
    va_end(ap);
    std::lock_guard lock(m);
    std::fputs(line, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (g_log_sink)
        g_log_sink(line);
}

void (*g_log_sync)() = nullptr;
std::string g_selftest_file;
std::string g_fault_trail_file;

void LogSync(const char* fmt, ...)
{
    if (InSignalHandler())
    {
        DroppedInHandler(); // before any formatting: the C library's keeps per-thread state behind fs
        return;
    }
    char text[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    Log("%s", text);
    if (g_log_sync && !InSignalHandler())
        g_log_sync();
}

void (*g_fatal_hook)() = nullptr;

void Fatal(const char* fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Log("FATAL: %s", line);
    static std::atomic<bool> once{false};
    if (!once.exchange(true))
        diag::LogState("fatal error");
    if (g_fatal_hook)
        g_fatal_hook(); // the PS4: keeps the message on screen
    std::_Exit(2);
}

bool MapGuest(u32 base, u32 size, bool exec, std::string& error)
{
    error = platform::Map(base, size, platform::Read | platform::Write | (exec ? platform::Exec : 0));
    return error.empty();
}

namespace {

constexpr u32 kPage = 0x1000;
std::mutex g_mem;

// A first-fit allocator over [begin, end): free ranges by start.
struct RangeAllocator
{
    u32 begin = 0, end = 0;
    std::map<u32, u32> free;      // start -> size
    std::map<u32, u32> allocated; // start -> size

    void Init(u32 b, u32 e)
    {
        begin = b, end = e;
        free.clear(), allocated.clear();
        free[b] = e - b;
    }
    u32 Alloc(u32 size, u32 alignment, u32 lowest = 0, u32 highest = 0xffffffff)
    {
        size = (size + kPage - 1) & ~(kPage - 1);
        alignment = std::max(alignment, kPage);
        for (auto it = free.begin(); it != free.end(); ++it)
        {
            u64 start = std::max<u64>(it->first, lowest);
            start = (start + alignment - 1) & ~u64(alignment - 1);
            const u64 range_end = u64(it->first) + it->second;
            if (start + size > range_end || start + size - 1 > highest)
                continue;
            Take(it, u32(start), size);
            allocated[u32(start)] = size;
            return u32(start);
        }
        return 0;
    }
    bool AllocAt(u32 at, u32 size)
    {
        size = (size + kPage - 1) & ~(kPage - 1);
        auto it = free.upper_bound(at);
        if (it == free.begin())
            return false;
        --it;
        if (u64(it->first) + it->second < u64(at) + size)
            return false;
        Take(it, at, size);
        allocated[at] = size;
        return true;
    }
    bool Free(u32 at)
    {
        auto a = allocated.find(at);
        if (a == allocated.end())
            return false;
        u32 start = a->first, size = a->second;
        allocated.erase(a);
        auto next = free.lower_bound(start);
        if (next != free.end() && start + size == next->first)
            size += next->second, free.erase(next);
        auto prev = free.lower_bound(start);
        if (prev != free.begin())
        {
            --prev;
            if (prev->first + prev->second == start)
            {
                prev->second += size;
                return true;
            }
        }
        free[start] = size;
        return true;
    }
    // The allocation containing `at`: its start and size, or false.
    bool Find(u32 at, u32& start, u32& size) const
    {
        auto it = allocated.upper_bound(at);
        if (it == allocated.begin())
            return false;
        --it;
        if (at >= it->first + it->second)
            return false;
        start = it->first, size = it->second;
        return true;
    }
    u32 FreeBytes() const
    {
        u32 n = 0;
        for (auto& [s, z] : free)
            n += z;
        return n;
    }

private:
    void Take(std::map<u32, u32>::iterator it, u32 start, u32 size)
    {
        const u32 range_start = it->first, range_size = it->second;
        free.erase(it);
        if (start > range_start)
            free[range_start] = start - range_start;
        const u32 after = range_start + range_size - (start + size);
        if (after)
            free[start + size] = after;
    }
};

RangeAllocator g_virtual, g_contiguous, g_stacks;
u32 g_kernel_next = kKernelBase;

} // namespace

void InitMemory()
{
    struct Region
    {
        u32 base, size;
        bool exec;
        const char* what;
    } regions[] = {
        {kRamBase, kRamEnd - kRamBase, true, "RAM"},
        {kContiguousBase, kContiguousSize, false, "contiguous memory"},
        {kKernelBase, kKernelSize, true, "kernel area"},
        {kStackBase, kStackAreaSize, false, "stacks"},
    };
    // Whatever the system already has in a region (on the PS4 the app's own executable, at 0x400000)
    // stays where it is: the region is mapped around it, and the allocators never hand it out.
    std::vector<std::pair<u32, u32>> taken;
    const u32 page = std::max<u32>(platform::PageSize(), kPage);
    for (const Region& r : regions)
    {
        auto occupied = platform::Occupied(r.base, r.base + r.size);
        u32 at = r.base;
        occupied.push_back({r.base + r.size, r.base + r.size});
        for (auto [start, end] : occupied)
        {
            start &= ~(page - 1);
            end = (end + page - 1) & ~(page - 1);
            if (start > at)
            {
                Log("memory: mapping the %s at %#x (%#x bytes)", r.what, at, start - at);
                std::string e;
                if (!MapGuest(at, start - at, r.exec, e))
                    Fatal("%s at %#x (%#x bytes): %s", r.what, at, start - at, e.c_str());
            }
            if (end > start)
            {
                Log("memory: %#x-%#x in the %s is the system's; left out", start, end - 1, r.what);
                taken.push_back({start, end - start});
            }
            at = std::max(at, end);
        }
    }
    // the devices and the write-combined view of memory: reserved, inaccessible
    if (const std::string a = platform::Reserve(kAliasBase, kAliasSize); !a.empty())
        Fatal("memory view at %#x not reserved: %s", kAliasBase, a.c_str());
    const std::string e = platform::Reserve(kDeviceBase, 0x03000000);
    if (!e.empty())
        Fatal("device range at %#x not reserved: %s", kDeviceBase, e.c_str());
    g_virtual.Init(kRamBase, kRamEnd);
    g_contiguous.Init(kContiguousBase, kContiguousBase + kContiguousSize);
    g_stacks.Init(kStackBase, kStackBase + kStackAreaSize);
    for (auto [start, size] : taken)
        if (start >= kRamBase && start < kRamEnd)
            g_virtual.AllocAt(start, size);
    g_contiguous.AllocAt(kGpuInstanceMemory, 0x100000);
}

u32 AllocVirtual(u32 size, u32 at)
{
    std::lock_guard lock(g_mem);
    if (at)
        return g_virtual.AllocAt(at & ~(kPage - 1), size + (at & (kPage - 1))) ? (at & ~(kPage - 1)) : 0;
    const u32 p = g_virtual.Alloc(size, 0x10000);
    if (p)
        std::memset(Ptr(p), 0, size);
    return p;
}

bool FreeVirtual(u32 base)
{
    std::lock_guard lock(g_mem);
    return g_virtual.Free(base);
}

u32 AllocContiguous(u32 size, u32 alignment, u32 lowest, u32 highest)
{
    std::lock_guard lock(g_mem);
    // lowest/highest are physical addresses
    const u32 p = g_contiguous.Alloc(size, alignment ? alignment : kPage, kContiguousBase + lowest,
                                     highest >= kContiguousSize ? 0xffffffff : kContiguousBase + highest);
    if (p)
        std::memset(Ptr(p), 0, size);
    return p;
}

bool FreeContiguous(u32 base)
{
    std::lock_guard lock(g_mem);
    return g_contiguous.Free(base);
}

u32 AllocKernel(u32 size, u32 alignment)
{
    std::lock_guard lock(g_mem);
    const u32 p = (g_kernel_next + alignment - 1) & ~(alignment - 1);
    if (p + size > kKernelBase + kKernelSize)
        Fatal("kernel area full");
    g_kernel_next = p + size;
    std::memset(Ptr(p), 0, size);
    return p;
}

u32 AllocStack(u32 size)
{
    std::lock_guard lock(g_mem);
    // a guard page below each stack is left unmapped by not handing it out
    const u32 p = g_stacks.Alloc(size + kPage, 0x10000);
    if (!p)
        Fatal("out of stack space");
    platform::Protect(p, kPage, 0);
    return p + kPage + size;
}

u32 AllocSystem(u32 size)
{
    std::lock_guard lock(g_mem);
    const u32 p = g_stacks.Alloc(size, kPage);
    if (p)
    {
        const u32 bytes = (size + kPage - 1) & ~(kPage - 1);
        platform::Protect(p, bytes, platform::Read | platform::Write | platform::Exec);
        std::memset(Ptr(p), 0, bytes);
    }
    return p;
}

u32 FreeSystem(u32 base)
{
    std::lock_guard lock(g_mem);
    u32 start, size;
    if (!g_stacks.Find(base, start, size) || start != base)
        return 0;
    g_stacks.Free(base);
    return size / kPage;
}

u32 PhysicalAddress(u32 guest)
{
    if (guest >= kContiguousBase && guest < kContiguousBase + kContiguousSize)
        return guest - kContiguousBase;
    if (guest >= kRamBase && guest < kRamEnd)
        return kRamPhysical + guest;
    return guest;
}

bool QueryAllocation(u32 at, u32& start, u32& size)
{
    std::lock_guard lock(g_mem);
    return g_virtual.Find(at, start, size) || g_contiguous.Find(at, start, size) || g_stacks.Find(at, start, size);
}

u32 FreePages()
{
    std::lock_guard lock(g_mem);
    return (g_virtual.FreeBytes() + g_contiguous.FreeBytes()) / kPage;
}

} // namespace xb
