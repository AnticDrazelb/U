// A dirty log for the contiguous memory (all of the RAM the GPU and the APU see): which pages were
// written since a reader (xemu's renderer: its surfaces, textures and vertex data) last looked.
//
// QEMU knows that from its CPU translator, or a hypervisor's dirty log; the CPU here runs natively.
// Without a log, xemu's every check (a surface and each texture of every draw) read the whole range
// to hash it: gigabytes a second at Halo's thousand draws. Here, as a hypervisor does it: a page a
// reader has looked at is write-protected; the first write to it after that, the guest's or the
// emulator's own, faults once, which marks the page written for every reader and lets it be written
// again (the fault handler: DirtyFault). A reader's check is then a look at the page's bits.
//
// Writes by system calls (a file read into the guest's memory) don't fault: they fail. Those paths
// call PrepareGuestWrite first. The guest's own protection requests are not applied anywhere here
// (every page stays read-write to it), so a write fault in this memory can only be the log's.
//
// Off (xemu's hashing instead) where the self-test in the CPU's start-up didn't pass.
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>

#include "core.h"
#include "platform.h"

namespace xb {
namespace {

std::atomic<bool> g_on{false};
u32 g_page = 0x1000; // the protection's unit (16 KiB on the PS4)
u32 g_pages = 0;
// per page: written since each reader last looked (a bit per reader); whether the log protects it;
// a lock held while either changes with the page's protection
std::unique_ptr<std::atomic<u32>[]> g_written;
std::unique_ptr<std::atomic<u8>[]> g_protected;
std::unique_ptr<std::atomic_flag[]> g_lock;
std::atomic<u64> g_faults{0}, g_protects{0};

struct PageLock
{
    std::atomic_flag& f;
    explicit PageLock(u32 p) : f(g_lock[p])
    {
        while (f.test_and_set(std::memory_order_acquire))
            __builtin_ia32_pause();
    }
    ~PageLock() { f.clear(std::memory_order_release); }
};

u32 PageAddress(u32 p) { return kContiguousBase + p * g_page; }

// the pages [first, end) of the offsets [offset, offset + size) within the contiguous memory
bool Pages(u64 offset, u64 size, u32& first, u32& end)
{
    if (offset >= kContiguousSize)
        return false;
    first = u32(offset / g_page);
    const u64 last = std::min<u64>(offset + (size ? size : 1), kContiguousSize);
    end = u32((last + g_page - 1) / g_page);
    return first < end;
}

} // namespace

bool DirtyLogOn() { return g_on.load(std::memory_order_relaxed); }

// From the fault handler (any thread, guest or emulator code): a write to a page the log protects.
// The page is marked written for every reader and made writable; the write is made again on return.
bool DirtyFault(u64 address)
{
    if (!g_on.load(std::memory_order_relaxed) || address < kContiguousBase ||
        address >= u64(kContiguousBase) + kContiguousSize)
        return false;
    const u32 p = u32((address - kContiguousBase) / g_page);
    PageLock lock(p);
    g_written[p].store(~0u, std::memory_order_release);
    if (g_protected[p].load(std::memory_order_relaxed))
    {
        platform::Protect(PageAddress(p), g_page, platform::Read | platform::Write);
        g_protected[p].store(0, std::memory_order_release);
    }
    g_faults.fetch_add(1, std::memory_order_relaxed);
    return true; // (a page another thread made writable already: the write just goes again)
}

// Before something that writes the guest's memory without faulting (a system call): its pages
// writable, and written for every reader.
void PrepareGuestWrite(u32 address, u32 size)
{
    u32 first, end;
    if (!g_on.load(std::memory_order_relaxed) || address < kContiguousBase ||
        !Pages(address - kContiguousBase, size, first, end))
        return;
    for (u32 p = first; p < end; p++)
    {
        PageLock lock(p);
        g_written[p].store(~0u, std::memory_order_release);
        if (g_protected[p].load(std::memory_order_relaxed))
        {
            platform::Protect(PageAddress(p), g_page, platform::Read | platform::Write);
            g_protected[p].store(0, std::memory_order_release);
        }
    }
}

std::string DirtyLogStats()
{
    if (!g_on)
        return "off (xemu's hashing)";
    return std::to_string(g_faults.exchange(0)) + " write faults, " + std::to_string(g_protects.exchange(0)) +
           " pages protected again";
}

// The start-up's: the page size, and the log on
void StartDirtyLog()
{
    g_page = std::max<u32>(platform::PageSize(), 0x1000);
    g_pages = kContiguousSize / g_page;
    g_written.reset(new std::atomic<u32>[g_pages]);
    g_protected.reset(new std::atomic<u8>[g_pages]);
    g_lock.reset(new std::atomic_flag[g_pages]);
    for (u32 p = 0; p < g_pages; p++)
    {
        g_written[p].store(~0u); // all written before anyone has looked
        g_protected[p].store(0);
        g_lock[p].clear();
    }
    g_on = true;
}

void StopDirtyLog()
{
    g_on = false;
    if (!g_pages)
        return;
    for (u32 p = 0; p < g_pages; p++)
        if (g_protected[p].exchange(0))
            platform::Protect(PageAddress(p), g_page, platform::Read | platform::Write);
}

// for the self-test: a page protected by the log as a reader's look leaves it
void DirtyProtectForTest(u32 address)
{
    const u32 p = (address - kContiguousBase) / g_page;
    PageLock lock(p);
    g_written[p].store(0);
    platform::Protect(PageAddress(p), g_page, platform::Read);
    g_protected[p].store(1);
}

bool DirtyWrittenForTest(u32 address)
{
    const u32 p = (address - kContiguousBase) / g_page;
    return g_written[p].load() != 0 && !g_protected[p].load();
}

} // namespace xb

using namespace xb;

extern "C" {
// xemu's (shim/xb_memory.c), offsets within the contiguous memory. Whether [offset, offset + size)
// was written since `reader` last asked; the pages protected again, so that the next write shows.
bool xb_dirty_on(void) { return g_on.load(std::memory_order_relaxed); }

bool xb_dirty_test_and_clear(uint64_t offset, uint64_t size, unsigned reader)
{
    u32 first, end;
    if (!Pages(offset, size, first, end))
        return false;
    const u32 bit = 1u << (reader & 31);
    bool written = false;
    for (u32 p = first; p < end; p++)
    {
        // protected before its bit is read: a write after this faults and shows next time; one
        // before it is in the bit
        if (!g_protected[p].load(std::memory_order_acquire) ||
            (g_written[p].load(std::memory_order_relaxed) & bit))
        {
            PageLock lock(p);
            if (!g_protected[p].load(std::memory_order_relaxed))
            {
                platform::Protect(PageAddress(p), g_page, platform::Read);
                g_protected[p].store(1, std::memory_order_release);
                g_protects.fetch_add(1, std::memory_order_relaxed);
            }
            written |= (g_written[p].fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0;
        }
    }
    return written;
}

void xb_dirty_mark(uint64_t offset, uint64_t size, unsigned reader_mask)
{
    u32 first, end;
    if (!Pages(offset, size, first, end))
        return;
    for (u32 p = first; p < end; p++)
        g_written[p].fetch_or(reader_mask, std::memory_order_acq_rel);
}

// Pages [first, end) set to one protection in one system call (their locks held, in order, so the
// fault handler's one-page lock can't cross it)
static void SetRange(u32 first, u32 end, bool protect)
{
    for (u32 p = first; p < end; p++)
        while (g_lock[p].test_and_set(std::memory_order_acquire))
            __builtin_ia32_pause();
    platform::Protect(PageAddress(first), (end - first) * g_page,
                      protect ? platform::Read : platform::Read | platform::Write);
    for (u32 p = first; p < end; p++)
    {
        g_protected[p].store(protect ? 1 : 0, std::memory_order_release);
        g_lock[p].clear(std::memory_order_release);
    }
    if (protect)
        g_protects.fetch_add(end - first, std::memory_order_relaxed);
}

// The device about to write this range itself (a surface written back to RAM): writable without a
// fault a page, and marked for no reader (the caller marks the ones it means)
void xb_dirty_unprotect(uint64_t offset, uint64_t size)
{
    u32 first, end;
    if (g_on.load(std::memory_order_relaxed) && Pages(offset, size, first, end))
        SetRange(first, end, false);
}

// ... and done: protected again, so that a write after it (the guest's) shows to every reader
void xb_dirty_protect(uint64_t offset, uint64_t size)
{
    u32 first, end;
    if (g_on.load(std::memory_order_relaxed) && Pages(offset, size, first, end))
        SetRange(first, end, true);
}
}
