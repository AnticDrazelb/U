// Guest memory: the regions of core.h mapped at their own addresses, and simple allocators in them.
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

#include <sys/mman.h>

#include "core.h"

namespace xb {

int g_verbose = 1;

void Log(const char* fmt, ...)
{
    static std::mutex m;
    std::lock_guard lock(m);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void Fatal(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stdout, "FATAL: ");
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    std::_Exit(2);
}

bool MapGuest(u32 base, u32 size, bool exec, std::string& error)
{
    const int prot = PROT_READ | PROT_WRITE | (exec ? PROT_EXEC : 0);
    void* p = mmap(Ptr(base), size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED || Guest(p) != base)
    {
        error = std::string("mmap: ") + std::strerror(errno);
        if (p != MAP_FAILED)
            munmap(p, size);
        return false;
    }
    return true;
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
        {kContiguousBase, kContiguousSize, true, "contiguous memory"},
        {kKernelBase, kKernelSize, true, "kernel area"},
        {kStackBase, kStackAreaSize, false, "stacks"},
    };
    for (const Region& r : regions)
    {
        std::string e;
        if (!MapGuest(r.base, r.size, r.exec, e))
            Fatal("%s at %#x: %s", r.what, r.base, e.c_str());
    }
    // the devices: reserved, inaccessible
    void* p = mmap(Ptr(kDeviceBase), 0x03000000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != Ptr(kDeviceBase))
        Fatal("device range at %#x not reserved", kDeviceBase);
    g_virtual.Init(kRamBase, kRamEnd);
    g_contiguous.Init(kContiguousBase, kContiguousBase + kContiguousSize);
    g_stacks.Init(kStackBase, kStackBase + kStackAreaSize);
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
    mprotect(Ptr(p), kPage, PROT_NONE);
    return p + kPage + size;
}

u32 PhysicalAddress(u32 guest)
{
    if (guest >= kContiguousBase && guest < kContiguousBase + kContiguousSize)
        return guest - kContiguousBase;
    return guest; // RAM: its "physical" address is its address (the GPU translates back)
}

bool QueryAllocation(u32 at, u32& start, u32& size)
{
    std::lock_guard lock(g_mem);
    return g_virtual.Find(at, start, size) || g_contiguous.Find(at, start, size);
}

u32 FreePages()
{
    std::lock_guard lock(g_mem);
    return (g_virtual.FreeBytes() + g_contiguous.FreeBytes()) / kPage;
}

} // namespace xb
