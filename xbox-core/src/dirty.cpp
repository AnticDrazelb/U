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
//
// Held pages (xemu's CPU access callbacks, which its translator gave it): a surface drawn on the GPU
// and not written back to RAM yet has its pages made inaccessible instead of written back at once
// (which cost xemu's renderer half its time on the PS4: ten whole surfaces a frame, each waiting
// for the GPU). Guest code touching one faults; outside the signal handler (FaultResumed) the
// surfaces there are written back (the renderer's own callback) and the access is made again. The
// emulator's own access (the presenter, a device) releases the page without a write-back, as
// xemu's devices read RAM on a PC. Off (every surface written back as its drawing ends, as
// before) unless the self-test's two steps pass; XB_EAGER_SURFACES=1 keeps it off.
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "core.h"
#include "platform.h"

namespace xb {
namespace {

std::atomic<bool> g_on{false};
u32 g_page = 0x1000; // the protection's unit (16 KiB on the PS4)
u32 g_pages = 0;
// per page: written since each reader last looked (a bit per reader); whether the log protects it;
// a lock held while either changes with the page's protection
// written-since bits per 4 KiB unit (QEMU's page, which xemu's checks assume): the protection is per
// system page (16 KiB on the PS4), so a write fault marks all of its units, but a reader's check
// clears only the units its range covers. (Per 16 KiB, a check of one vertex buffer cleared the
// mark of another sharing its page, which then went unnoticed: stale vertices, warped models.)
constexpr u32 kUnit = 0x1000;
u32 g_units = 1; // units a page
std::unique_ptr<std::atomic<u32>[]> g_written;
void MarkPage(u32 p, u32 mask)
{
    for (u32 u = p * g_units; u < (p + 1) * g_units; u++)
        g_written[u].fetch_or(mask, std::memory_order_acq_rel);
}
std::unique_ptr<std::atomic<u8>[]> g_protected;
std::unique_ptr<std::atomic_flag[]> g_lock;
std::unique_ptr<std::atomic<u8>[]> g_held; // no access until written back (a surface on the GPU)
std::atomic<bool> g_holds{false};
std::atomic<u64> g_faults{0}, g_protects{0}, g_held_pages{0}, g_hold_released{0}, g_hold_resolved{0};
void (*g_surface_access)(uint64_t offset, uint64_t size) = nullptr; // xemu's renderer's (surface.c)
// where the emulator's own code touched held pages (rip), the latest 256: the stats name the most
constexpr u32 kReleaseRing = 256;
std::atomic<u64> g_release_rip[kReleaseRing];
std::atomic<u32> g_release_next{0};


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
void DirtyNoteRelease(u64 rip)
{
    g_release_rip[g_release_next.fetch_add(1, std::memory_order_relaxed) % kReleaseRing].store(rip, std::memory_order_relaxed);
}

int DirtyFault(u64 address, bool can_defer)
{
    if (!g_on.load(std::memory_order_relaxed) || address < kContiguousBase ||
        address >= u64(kContiguousBase) + kContiguousSize)
        return 0;
    const u32 p = u32((address - kContiguousBase) / g_page);
    PageLock lock(p);
    if (g_held[p].load(std::memory_order_relaxed))
    {
        // a held page: guest code's access waits for the surfaces' write-back (outside the handler:
        // DirtyResolveHeld); the emulator's own releases it as it is (read-only: a write faults
        // again and is marked as any write is)
        if (can_defer)
            return 2;
        g_held[p].store(0, std::memory_order_relaxed);
        g_protected[p].store(1, std::memory_order_relaxed);
        platform::Protect(PageAddress(p), g_page, platform::Read);
        g_hold_released.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    MarkPage(p, ~0u);
    if (g_protected[p].load(std::memory_order_relaxed))
    {
        platform::Protect(PageAddress(p), g_page, platform::Read | platform::Write);
        g_protected[p].store(0, std::memory_order_release);
    }
    g_faults.fetch_add(1, std::memory_order_relaxed);
    return 1; // (a page another thread made writable already: the write just goes again)
}

bool DirtyHeld(u64 address)
{
    if (!g_held || !g_on.load(std::memory_order_relaxed) || address < kContiguousBase ||
        address >= u64(kContiguousBase) + kContiguousSize)
        return false;
    return g_held[u32((address - kContiguousBase) / g_page)].load(std::memory_order_acquire) != 0;
}

// Guest code's access to a held page, outside the signal handler: the surfaces drawn there written
// back (xemu's renderer waits for its GPU), then the page released if that didn't already
void DirtyResolveHeld(u64 address)
{
    const u32 p = u32((address - kContiguousBase) / g_page);
    if (g_surface_access)
        g_surface_access(u64(p) * g_page, g_page);
    PageLock lock(p);
    if (g_held[p].exchange(0, std::memory_order_acq_rel))
    {
        g_protected[p].store(1, std::memory_order_relaxed);
        platform::Protect(PageAddress(p), g_page, platform::Read);
    }
    g_hold_resolved.fetch_add(1, std::memory_order_relaxed);
}

bool LazySurfaces() { return g_holds.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed); }
void SetLazySurfaces(bool on) { g_holds = on && g_on.load() && !std::getenv("XB_EAGER_SURFACES"); }

// for the self-test: a page held as a surface's are
void DirtyHoldForTest(u32 address)
{
    const u32 p = (address - kContiguousBase) / g_page;
    PageLock lock(p);
    g_held[p].store(1);
    platform::Protect(PageAddress(p), g_page, 0);
}

// Held pages in [first, end) written back (the surfaces drawn there) and released: before a system
// call reads or writes them (it would fail on them), or the emulator's code rewrites them (a later
// write-back of the surface would otherwise land over what it wrote). Not from xemu's renderer
// thread (it would wait for itself): the guest's threads, in kernel calls.
static void ResolveRange(u32 first, u32 end)
{
    if (!g_holds.load(std::memory_order_relaxed))
        return;
    bool any = false;
    for (u32 p = first; p < end && !any; p++)
        any = g_held[p].load(std::memory_order_acquire) != 0;
    if (!any)
        return;
    if (g_surface_access)
        g_surface_access(u64(first) * g_page, u64(end - first) * g_page);
    for (u32 p = first; p < end; p++)
    {
        PageLock lock(p);
        if (g_held[p].exchange(0, std::memory_order_acq_rel))
        {
            g_protected[p].store(1, std::memory_order_relaxed);
            platform::Protect(PageAddress(p), g_page, platform::Read);
        }
    }
    g_hold_resolved.fetch_add(1, std::memory_order_relaxed);
}

// Before a system call reads the guest's memory (a file written from it)
void PrepareGuestRead(u32 address, u32 size)
{
    u32 first, end;
    if (g_on.load(std::memory_order_relaxed) && address >= kContiguousBase &&
        Pages(address - kContiguousBase, size, first, end))
        ResolveRange(first, end);
}

// Before something that writes the guest's memory without faulting (a system call): its pages
// writable, and written for every reader.
void PrepareGuestWrite(u32 address, u32 size)
{
    u32 first, end;
    if (!g_on.load(std::memory_order_relaxed) || address < kContiguousBase ||
        !Pages(address - kContiguousBase, size, first, end))
        return;
    ResolveRange(first, end);
    for (u32 p = first; p < end; p++)
    {
        PageLock lock(p);
        MarkPage(p, ~0u);
        const bool held = g_held[p].exchange(0, std::memory_order_relaxed);
        if (g_protected[p].load(std::memory_order_relaxed) || held)
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
    std::string s = std::to_string(g_faults.exchange(0)) + " write faults, " + std::to_string(g_protects.exchange(0)) +
                    " pages protected again";
    if (g_holds)
    {
        const u64 released = g_hold_released.exchange(0);
        s += "; surfaces held on the GPU: " + std::to_string(g_held_pages.exchange(0)) + " pages held, " +
             std::to_string(g_hold_resolved.exchange(0)) + " written back on the game's access, " +
             std::to_string(released) + " released by the emulator";
        if (released)
        {
            // where (the most frequent of the latest 256; elf: the app's code, named in the log bundle)
            std::map<u64, u32> counts;
            for (u32 i = 0; i < kReleaseRing; i++)
                if (const u64 r = g_release_rip[i].exchange(0, std::memory_order_relaxed))
                    counts[r]++;
            std::vector<std::pair<u32, u64>> top;
            for (auto& [r, c] : counts)
                top.push_back({c, r});
            std::sort(top.rbegin(), top.rend());
            const u64 base = platform::CodeBase();
            for (size_t i = 0; i < top.size() && i < 3; i++)
            {
                char b[160];
                const u64 r = top[i].second;
                const std::string module = r >= base && r - base < 0x40000000ull ? "" : platform::ModuleName(r);
                if (module.empty())
                    std::snprintf(b, sizeof(b), "%s%u at elf 0x%llx", i ? ", " : " (", top[i].first,
                                  (unsigned long long)(r - base));
                else
                    std::snprintf(b, sizeof(b), "%s%u in %s at %#llx", i ? ", " : " (", top[i].first, module.c_str(),
                                  (unsigned long long)r);
                s += b;
            }
            s += ")";
        }
    }
    else
        s += "; surfaces written back as their drawing ends";
    return s;
}

extern "C" bool xb_dirty_test_and_clear(uint64_t offset, uint64_t size, unsigned reader);

// The start-up's: the page size, and the log on
void StartDirtyLog()
{
    g_page = std::max<u32>(platform::PageSize(), 0x1000);
    // XB_DIRTY_PAGE=16384: the PS4's page size on a desktop (its granularity tested here)
    if (const char* e = std::getenv("XB_DIRTY_PAGE"))
        if (const u32 v = u32(std::strtoul(e, nullptr, 0)); v >= g_page && (v & (v - 1)) == 0)
            g_page = v;
    g_pages = kContiguousSize / g_page;
    g_units = std::max<u32>(g_page / kUnit, 1);
    g_written.reset(new std::atomic<u32>[size_t(g_pages) * g_units]);
    for (u32 u = 0; u < g_pages * g_units; u++)
        g_written[u].store(~0u); // all written before anyone has looked
    g_protected.reset(new std::atomic<u8>[g_pages]);
    g_lock.reset(new std::atomic_flag[g_pages]);
    g_held.reset(new std::atomic<u8>[g_pages]);
    for (u32 p = 0; p < g_pages; p++)
    {
        g_protected[p].store(0);
        g_held[p].store(0);
        g_lock[p].clear();
    }
    g_on = true;
    // XB_DIRTY_STRESS=1: every page protected again all the time (a reader of its own asking about
    // all of them, as xemu's renderer does about textures' and surfaces' memory in a busy game): what
    // writes the guest's memory without faulting through the emulator must still get through
    if (std::getenv("XB_DIRTY_STRESS"))
        std::thread([] {
            for (;;)
            {
                xb_dirty_test_and_clear(0, kContiguousSize, 31);
                std::this_thread::yield();
            }
        }).detach();
}

void StopDirtyLog()
{
    g_on = false;
    g_holds = false;
    if (!g_pages)
        return;
    for (u32 p = 0; p < g_pages; p++)
        if ((g_protected[p].exchange(0) | g_held[p].exchange(0)) != 0)
            platform::Protect(PageAddress(p), g_page, platform::Read | platform::Write);
}

// for the self-test: a page protected by the log as a reader's look leaves it
void DirtyProtectForTest(u32 address)
{
    const u32 p = (address - kContiguousBase) / g_page;
    PageLock lock(p);
    for (u32 u = p * g_units; u < (p + 1) * g_units; u++)
        g_written[u].store(0);
    platform::Protect(PageAddress(p), g_page, platform::Read);
    g_protected[p].store(1);
}

bool DirtyWrittenForTest(u32 address)
{
    const u32 p = (address - kContiguousBase) / g_page;
    return g_written[p * g_units].load() != 0 && !g_protected[p].load();
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
    // the 4 KiB units the range covers (the bits cleared: those only)
    const u32 ufirst = u32(offset / kUnit);
    const u32 uend = u32((std::min<u64>(offset + (size ? size : 1), kContiguousSize) + kUnit - 1) / kUnit);
    const u32 bit = 1u << (reader & 31);
    bool written = false;
    for (u32 p = first; p < end; p++)
    {
        const u32 a = std::max(ufirst, p * g_units), b = std::min(uend, (p + 1) * g_units);
        bool any = false;
        for (u32 u = a; u < b && !any; u++)
            any = (g_written[u].load(std::memory_order_relaxed) & bit) != 0;
        // protected before its bits are read: a write after this faults and shows next time; one
        // before it is in the bits
        if (!g_protected[p].load(std::memory_order_acquire) || any)
        {
            PageLock lock(p);
            if (!g_protected[p].load(std::memory_order_relaxed))
            {
                g_protected[p].store(1, std::memory_order_release);
                if (!g_held[p].load(std::memory_order_relaxed)) // (a held page stays inaccessible)
                    platform::Protect(PageAddress(p), g_page, platform::Read);
                g_protects.fetch_add(1, std::memory_order_relaxed);
            }
            for (u32 u = a; u < b; u++)
                written |= (g_written[u].fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0;
        }
    }
    return written;
}

void xb_dirty_mark(uint64_t offset, uint64_t size, unsigned reader_mask)
{
    u32 first, end;
    if (!Pages(offset, size, first, end))
        return;
    const u32 ufirst = u32(offset / kUnit);
    const u32 uend = u32((std::min<u64>(offset + (size ? size : 1), kContiguousSize) + kUnit - 1) / kUnit);
    for (u32 u = ufirst; u < uend; u++)
        g_written[u].fetch_or(reader_mask, std::memory_order_acq_rel);
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
        g_held[p].store(0, std::memory_order_release);
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

// xemu's renderer: a surface drawn and left on the GPU (not written back): its pages held, so that
// guest code touching them gets the write-back first (DirtyFault, DirtyResolveHeld)
bool xb_lazy_surfaces(void) { return LazySurfaces(); }

void xb_dirty_hold(uint64_t offset, uint64_t size)
{
    u32 first, end;
    if (!LazySurfaces() || !Pages(offset, size, first, end))
        return;
    static const bool debug = std::getenv("XB_DEBUG_HOLDS") != nullptr;
    static std::atomic<int> logged{0};
    if (debug && logged++ < 40)
        Log("hold: %#llx, %#llx bytes", (unsigned long long)offset, (unsigned long long)size);
    for (u32 p = first; p < end; p++)
        while (g_lock[p].test_and_set(std::memory_order_acquire))
            __builtin_ia32_pause();
    platform::Protect(PageAddress(first), (end - first) * g_page, 0);
    for (u32 p = first; p < end; p++)
    {
        if (!g_held[p].exchange(1, std::memory_order_acq_rel))
            g_held_pages.fetch_add(1, std::memory_order_relaxed);
        g_lock[p].clear(std::memory_order_release);
    }
}

// the renderer's callback for guest code's access to held pages (offset, size within the memory)
void xb_set_surface_access_hook(void (*hook)(uint64_t offset, uint64_t size)) { g_surface_access = hook; }
}
