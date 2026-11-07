// Xbox CPU Bench: can this machine run original Xbox (32-bit x86) code as it is, on its own CPU?
//
// The original Xbox has a 733 MHz Pentium III. The PS4's Jaguar is also x86, so an emulator there
// may not need a recompiler at all: the CPU can run 32-bit code in compatibility mode, through a
// 32-bit code segment, the way Cxbx-Reloaded runs Xbox code on Windows. This program finds out
// whether that works and what an emulator built around it would pay:
//
//  1. Segments: which code and data segments the system set up (LAR on every GDT selector).
//  2. CPU: seven kernels built for a Pentium III, linked at 0x00010000 like an XBE, run in 32-bit
//     mode from memory below 4 GiB, against the same kernels built natively. Checksums must match.
//  3. The emulator's costs: a call into Xbox code and back; a "kernel export" call from Xbox code to
//     64-bit code (a high-level kernel, as Cxbx-Reloaded and hakuX have); a hardware-register access
//     that faults and is done by the fault handler (the NV2A's registers, as Cxbx-Reloaded does it).
//  4. Three cores at once (the game's, and the emulator's GPU and audio threads beside it).
//  5. Memory: what is free below 4 GiB, the Xbox's 64 MiB at its own addresses, executable, and the
//     contiguous-memory alias at 0x80000000.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "guest_image.h"
#include "guest_image_high.h"
#include "platform.h"

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Native builds of guest/kernels.c (BENCH_PREFIX=native_).
extern "C" {
u32 native_xb_int(u32 n);
u32 native_xb_branchy(u32 n, void* ents, u32 count);
u32 native_xb_calls(u32 n);
u32 native_xb_memory(u32 n, void* buf, u32 words);
u32 native_xb_copy(u32 n, void* buf, u32 blocks);
u32 native_xb_sse(u32 n, void* a, void* b, u32 len);
u32 native_xb_x87(u32 n);
// src/trampoline.S
extern const u8 xbt_begin[], xbt_end[], xbt_enter64[], xbt_gate32[], xbt_back64[];
}

namespace {

constexpr u64 KB = 1024, MB = 1024 * KB;

// Where things go, as offsets from the layout's base: 0 (the Xbox's own addresses) or, if the
// system has something there, 0x40000000. All of it lies inside the Xbox's 64 MiB.
constexpr u64 kLayouts[] = {0x00000000, 0x40000000}; // guest/link.ld is built for each
constexpr u64 kImageOffset = 0x00010000, kImageSize = 1 * MB; // an XBE's base
constexpr u64 kDataOffset = 0x01000000, kDataSize = 8 * MB;   // per core
constexpr u64 kStackOffset = 0x02800000, kStackSize = 1 * MB;  // per core
constexpr u64 kTrampOffset = 0x02F00000, kTrampStride = 64 * KB;
constexpr int kMaxCores = 3;
// The NV2A's registers start at 0xFD000000; the Xbox's other devices and its flash follow, up
// to 4 GiB. Kept inaccessible, so every access there faults.
constexpr u64 kMmioBase = 0xFD000000, kMmioSize = 0x03000000;
// Trampoline slots (src/trampoline.S)
constexpr int kSlotSel32 = 8, kSlotSel64 = 16, kSlotHandler = 24, kSlotGuestDs = 32, kSlotBase = 48;

// The bench_table at the image's start (guest/kernels.c xb_table)
enum GuestFn { kInt, kBranchy, kCalls, kMemory, kCopy, kSse, kX87, kEmpty, kMmio, kThunk, kFnCount };

double Now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename F>
double BestOf(int runs, F&& f)
{
    double best = 1e30;
    for (int i = 0; i < runs; i++)
    {
        const double t0 = Now();
        f();
        best = std::min(best, Now() - t0);
    }
    return best;
}

std::string Mib(u64 bytes)
{
    return bytes % MB ? fmt::format("{} KiB", bytes / KB) : fmt::format("{} MiB", bytes / MB);
}

// ---- segments

struct Descriptor
{
    bool valid;
    u32 rights, limit;
};

Descriptor Lar(u16 selector)
{
    u32 rights = 0, limit = 0;
    u8 ok = 0, ok_limit = 0;
    __asm__ volatile("lar %2, %0\n\tsetz %1" : "=r"(rights), "=q"(ok) : "r"(u32(selector)) : "cc");
    __asm__ volatile("lsl %2, %0\n\tsetz %1" : "=r"(limit), "=q"(ok_limit) : "r"(u32(selector)) : "cc");
    return {ok != 0, rights, ok_limit ? limit : 0};
}

bool IsSegment(u32 r) { return r & (1u << 12); }
bool IsCode(u32 r) { return IsSegment(r) && (r & (1u << 11)); }
bool IsPresent(u32 r) { return r & (1u << 15); }
u32 Dpl(u32 r) { return (r >> 13) & 3; }
bool IsLong(u32 r) { return r & (1u << 21); }
bool IsBig(u32 r) { return r & (1u << 22); }

std::string Describe(const Descriptor& d)
{
    const u32 r = d.rights;
    std::string kind;
    if (!IsSegment(r))
        kind = fmt::format("system type {}", (r >> 8) & 15);
    else if (IsCode(r))
        kind = IsLong(r) ? "64-bit code" : IsBig(r) ? "32-bit code" : "16-bit code";
    else
        kind = fmt::format("{}-bit data{}", IsBig(r) ? 32 : 16, (r & (1u << 9)) ? " rw" : " ro");
    return fmt::format("{}, dpl {}, limit {:#x}{}", kind, Dpl(r), d.limit, IsPresent(r) ? "" : ", not present");
}

u16 ReadCs()
{
    u16 v;
    __asm__("mov %%cs, %0" : "=r"(v));
    return v;
}
u16 ReadSs()
{
    u16 v;
    __asm__("mov %%ss, %0" : "=r"(v));
    return v;
}
u16 ReadDs()
{
    u16 v;
    __asm__("mov %%ds, %0" : "=r"(v));
    return v;
}

struct Segments
{
    u16 cs = 0, ss = 0, sel32 = 0;
    bool ok = false;
};

// Logs the descriptors a user program can see and picks the 32-bit code segment.
Segments SurveySegments()
{
    Segments s;
    s.cs = ReadCs();
    s.ss = ReadSs();
    Platform::Log(fmt::format("segments: cs {:#x}, ss {:#x}, ds {:#x}", s.cs, s.ss, ReadDs()));
    for (u16 index = 1; index < 32; index++)
    {
        const u16 selector = u16(index << 3 | 3);
        const Descriptor d = Lar(selector);
        if (!d.valid)
            continue;
        Platform::Log(fmt::format("  {:#06x}: {}{}", selector, Describe(d),
                                  selector == s.cs ? " (cs)" : selector == s.ss ? " (ss)" : ""));
        if (!s.sel32 && IsCode(d.rights) && IsPresent(d.rights) && Dpl(d.rights) == 3 && !IsLong(d.rights) &&
            IsBig(d.rights))
            s.sel32 = selector;
    }
    const Descriptor ss = Lar(s.ss);
    const bool ss_flat = ss.valid && IsBig(ss.rights) && ss.limit == 0xFFFFFFFFu;
    if (!s.sel32)
        Platform::Log("  NO 32-bit user code segment: Xbox code can't run natively here (a recompiler is needed)");
    else if (!ss_flat)
        Platform::Log("  ss isn't a flat 32-bit data segment: 32-bit code would have no usable stack");
    else
        Platform::Log(fmt::format("  32-bit code segment {:#x} with flat data segment {:#x}: Xbox code can run "
                                  "natively",
                                  s.sel32, s.ss));
    s.ok = s.sel32 && ss_flat;
    return s;
}

// ---- the guest

struct Core;
Core* g_cores[kMaxCores] = {};
Segments g_seg;
u64 g_layout = 0;

// The guest's address space: its code and data selectors, and their base (guest address a is
// linear address base + a, mod 4 GiB). Base 0 with the system's own 32-bit segments is the flat
// space; an LDT entry can give it another base.
struct GuestSpace
{
    u64 base = 0;
    u16 cs32 = 0, ds32 = 0;
};
GuestSpace g_space;
u64 Linear(u64 guest)
{
    return (g_space.base + guest) & 0xFFFFFFFFu;
}
u32 Guest(u64 linear)
{
    return u32(linear - g_space.base);
}

// The NV2A stand-in: registers that keep what is written to them.
u32 g_mmio_regs[4096];
std::atomic<u64> g_mmio_accesses{0};

size_t TrampSize()
{
    return std::max<size_t>(Platform::PageSize(), 4096);
}

// One emulated CPU: its own trampoline (the switch keeps the emulator's stack pointer in it), its
// own stack and data, inside the shared Xbox address space. Addresses here are linear.
struct Core
{
    int index = 0;
    u64 tramp = 0, stack = 0, data = 0;
    u32 GuestData() const { return Guest(data); }
    std::string error;
    std::vector<std::pair<u64, size_t>> mapped;

    u64& Slot(int offset) const { return *reinterpret_cast<u64*>(tramp + u64(offset)); }
    u64 Entry(const u8* symbol) const { return tramp + u64(symbol - xbt_begin); }
    bool InTrampoline(u64 rip) const { return rip >= tramp && rip < tramp + u64(xbt_end - xbt_begin); }
    bool InStack(u32 guest_esp) const { return Linear(guest_esp) >= stack && Linear(guest_esp) < stack + kStackSize; }

    bool Map(u64 addr, size_t size, int access, const char* what)
    {
        const std::string e = Platform::MapFixed(addr, size, access);
        if (!e.empty())
        {
            error = fmt::format("{} at {:#x} ({}): {}", what, addr, Platform::PoolName(access), e);
            return false;
        }
        mapped.push_back({addr, size});
        return true;
    }

    explicit Core(int i) : index(i)
    {
        tramp = Linear(g_layout + kTrampOffset + u64(i) * kTrampStride);
        stack = Linear(g_layout + kStackOffset + u64(i) * kStackSize);
        data = Linear(g_layout + kDataOffset + u64(i) * kDataSize);
        if (!Map(tramp, TrampSize(), Platform::Read | Platform::Write | Platform::Exec, "trampoline") ||
            !Map(stack, kStackSize, Platform::Read | Platform::Write, "stack") ||
            !Map(data, kDataSize, Platform::Read | Platform::Write, "data"))
            return;
        std::memcpy(reinterpret_cast<void*>(tramp), xbt_begin, size_t(xbt_end - xbt_begin));
        Slot(kSlotSel32) = g_space.cs32;
        Slot(kSlotSel64) = g_seg.cs;
        Slot(kSlotGuestDs) = g_space.ds32;
        Slot(kSlotBase) = g_space.base;
        g_cores[i] = this;
    }
    ~Core()
    {
        if (g_cores[index] == this)
            g_cores[index] = nullptr;
        for (auto& [addr, size] : mapped)
            Platform::Unmap(addr, size);
    }
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    void SetGate(u32 (*handler)(const u32*)) { Slot(kSlotHandler) = reinterpret_cast<u64>(handler); }
    u32 Gate() const { return Guest(Entry(xbt_gate32)); }

    // Calls guest function `fn` (cdecl) with 32-bit arguments; returns its eax.
    u32 Call(u32 fn, std::initializer_list<u32> args)
    {
        Platform::PrepareThread();
        const u64 esp = (stack + kStackSize - 4096) & ~u64(15);
        u32* a = reinterpret_cast<u32*>(esp);
        for (u32 v : args)
            *a++ = v;
        using Enter = u64 (*)(u32, u32);
        const auto enter = reinterpret_cast<Enter>(Entry(xbt_enter64));
        return u32(enter(fn, Guest(esp)));
    }
};

// The guest image, loaded once for all cores.
struct Image
{
    u64 base = 0;
    u32 fn[kFnCount] = {};
    std::string error;
    bool mapped = false;

    bool Load(const u8* elf, size_t len)
    {
        base = Linear(g_layout + kImageOffset);
        const std::string e =
            Platform::MapFixed(base, kImageSize, Platform::Read | Platform::Write | Platform::Exec);
        if (!e.empty())
        {
            error = fmt::format("image at {:#x} ({}): {}", base, Platform::PoolName(Platform::Exec), e);
            return false;
        }
        mapped = true;
        // a little ELF32 loader: PT_LOAD segments into the image region
        if (len < 52 || std::memcmp(elf, "\x7f" "ELF", 4) != 0 || elf[4] != 1)
        {
            error = "bad guest image";
            return false;
        }
        u32 phoff;
        u16 phentsize, phnum;
        std::memcpy(&phoff, elf + 28, 4);
        std::memcpy(&phentsize, elf + 42, 2);
        std::memcpy(&phnum, elf + 44, 2);
        for (unsigned i = 0; i < phnum; i++)
        {
            const u8* ph = elf + phoff + u64(i) * phentsize;
            u32 type, offset, vaddr, filesz, memsz;
            std::memcpy(&type, ph, 4);
            std::memcpy(&offset, ph + 4, 4);
            std::memcpy(&vaddr, ph + 8, 4);
            std::memcpy(&filesz, ph + 16, 4);
            std::memcpy(&memsz, ph + 20, 4);
            if (type != 1)
                continue;
            const u64 guest_base = g_layout + kImageOffset;
            if (vaddr < guest_base || u64(vaddr) + memsz > guest_base + kImageSize || u64(offset) + filesz > len)
            {
                error = "guest segment outside the image region";
                return false;
            }
            std::memcpy(reinterpret_cast<void*>(Linear(vaddr)), elf + offset, filesz);
        }
        std::memcpy(fn, reinterpret_cast<const void*>(base), sizeof(fn)); // xb_table
        return true;
    }
    ~Image()
    {
        if (mapped)
            Platform::Unmap(base, kImageSize);
    }
};

// Faults in guest code. Hardware-register accesses are done here: the two MOV forms are decoded
// (a real emulator decodes every instruction that can touch memory). Any other fault in guest code,
// or in the trampoline, ends that guest call: execution goes back to the emulator with an error.
std::string g_fault[kMaxCores];

bool OnFault(Platform::FaultContext& c)
{
    const u64 rip = *c.rip;
    Core* core = nullptr;
    for (Core* k : g_cores)
        if (k && ((c.cs == g_space.cs32 && k->InStack(u32(*c.gpr[4]))) || k->InTrampoline(rip)))
            core = k;
    if (!core)
        return false;
    // a fault in back64 itself can't be recovered from here: let the crash log have it
    if (rip >= core->Entry(xbt_back64) && rip < core->Entry(xbt_gate32))
        return false;
    const u64 guest_address = Guest(c.address);
    if (c.cs == g_space.cs32 && guest_address >= kMmioBase && guest_address < kMmioBase + kMmioSize)
    {
        const u8* code = reinterpret_cast<const u8*>(Linear(u32(rip)));
        const u8 op = code[0], modrm = code[1];
        const int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
        if ((op == 0x89 || op == 0x8B) && mod != 3)
        {
            size_t length = 2 + (rm == 4 ? 1 : 0) + (mod == 1 ? 1 : mod == 2 ? 4 : 0) + (mod == 0 && rm == 5 ? 4 : 0);
            u32& r = g_mmio_regs[((guest_address - kMmioBase) >> 2) % 4096];
            if (op == 0x89)
                r = u32(*c.gpr[reg]);
            else
                *c.gpr[reg] = r;
            *c.rip = rip + length;
            g_mmio_accesses.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    g_fault[core->index] = fmt::format("signal {} at rip {:#x} (cs {:#x}), address {:#x}", c.signal, rip, c.cs,
                                       c.address);
    *c.gpr[0] = 0;
    *c.gpr[2] = 0;
    *c.rip = core->Entry(xbt_back64);
    Platform::SetCs(c, g_seg.cs);
    return true;
}

// A stand-in for a kernel export answered on the host (KeGetTickCount, KeSetEvent...).
u32 KernelExport(const u32* args)
{
    return args[0] * 5u + (args[1] ^ 0x5bd1e995u);
}

// ---- the CPU tests

struct Kernel
{
    const char* name;
    const char* what;
    std::function<u32(u8*)> native;
    std::function<u32(Core&, const Image&)> guest;
};

constexpr u32 kEntities = 512, kWords = 1u << 20, kBlocks = 1u << 15, kFloats = 1u << 16;

std::vector<Kernel> MakeKernels(double scale)
{
    auto N = [&](u64 n) { return u32(std::max<u64>(1, u64(double(n) * scale))); };
    const u32 n_int = N(30000000), n_br = N(20000), n_calls = N(800), n_mem = N(12), n_copy = N(600),
              n_sse = N(2000), n_x87 = N(30000000);
    return {
        {"int", "integer math", [=](u8*) { return native_xb_int(n_int); },
         [=](Core& c, const Image& im) { return c.Call(im.fn[kInt], {n_int}); }},
        {"branchy", "game logic (state machines)", [=](u8* buf) { return native_xb_branchy(n_br, buf, kEntities); },
         [=](Core& c, const Image& im) { return c.Call(im.fn[kBranchy], {n_br, c.GuestData(), kEntities}); }},
        {"calls", "calls and indirect calls", [=](u8*) { return native_xb_calls(n_calls); },
         [=](Core& c, const Image& im) { return c.Call(im.fn[kCalls], {n_calls}); }},
        {"memory", "random and sequential memory", [=](u8* buf) { return native_xb_memory(n_mem, buf, kWords); },
         [=](Core& c, const Image& im) { return c.Call(im.fn[kMemory], {n_mem, c.GuestData(), kWords}); }},
        {"copy", "64-byte block copies", [=](u8* buf) { return native_xb_copy(n_copy, buf, kBlocks); },
         [=](Core& c, const Image& im) { return c.Call(im.fn[kCopy], {n_copy, c.GuestData(), kBlocks}); }},
        {"sse", "4-wide float math (SSE)",
         [=](u8* buf) { return native_xb_sse(n_sse, buf, buf + kFloats * 4, kFloats); },
         [=](Core& c, const Image& im) {
             return c.Call(im.fn[kSse], {n_sse, c.GuestData(), c.GuestData() + kFloats * 4, kFloats});
         }},
        {"x87", "double math (x87 in the guest)", [=](u8*) { return native_xb_x87(n_x87); },
         [=](Core& c, const Image& im) { return c.Call(im.fn[kX87], {n_x87}); }},
    };
}

class Progress
{
public:
    void Step(const std::string& step)
    {
        const int elapsed = int(Now() - start);
        Platform::Progress(fmt::format("{}   {}:{:02} so far", step, elapsed / 60, elapsed % 60));
    }

private:
    double start = Now();
};

// Picks the first layout whose image, trampolines, stacks and data all map.
bool ChooseLayout(std::unique_ptr<Image>& image, std::vector<std::unique_ptr<Core>>& cores)
{
    const u8* images[] = {guest_image, guest_image_high};
    const size_t lengths[] = {guest_image_len, guest_image_high_len};
    // with a segment base, guest addresses can be anywhere: only the Xbox's own
    const size_t layouts = g_space.base ? 1 : std::size(kLayouts);
    for (size_t l = 0; l < layouts; l++)
    {
        g_layout = kLayouts[l];
        Platform::Trace(fmt::format("layout {:#x}: image", g_layout));
        image = std::make_unique<Image>();
        std::string error;
        if (!image->Load(images[l], lengths[l]))
            error = image->error;
        cores.clear();
        for (int i = 0; i < kMaxCores && error.empty(); i++)
        {
            Platform::Trace(fmt::format("layout {:#x}: core {}", g_layout, i));
            cores.push_back(std::make_unique<Core>(i));
            if (!cores.back()->error.empty())
                error = cores.back()->error;
        }
        if (error.empty())
        {
            Platform::Log(fmt::format("guest memory: image at {:#x}, data, stacks and trampolines from {:#x}{}{}",
                                      g_layout + kImageOffset, g_layout + kDataOffset,
                                      g_layout ? " (not the Xbox's own addresses: those weren't free)" : " (the Xbox's own addresses)",
                                      g_space.base ? fmt::format(", at linear {:#x} and up", image->base) : ""));
            return true;
        }
        Platform::Log(fmt::format("guest memory at {:#x}: {}", g_layout, error));
        cores.clear();
        image.reset();
    }
    return false;
}

int RunCpu(double scale, Progress& progress, const std::string& title)
{
    Platform::Log("");
    Platform::Log(title);
    std::unique_ptr<Image> image;
    std::vector<std::unique_ptr<Core>> cores;
    if (!ChooseLayout(image, cores))
    {
        Platform::Log("  not run: no layout of guest memory mapped");
        return 1;
    }
    const std::string reserve = Platform::ReserveFixed(Linear(kMmioBase), kMmioSize);
    Platform::Log(reserve.empty() ? fmt::format("  hardware registers {:#x}-{:#x} kept unmapped (they fault)", kMmioBase,
                                                kMmioBase + kMmioSize - 1)
                                  : fmt::format("  hardware register range not reserved ({}): MMIO test skipped", reserve));
    Platform::InstallFaultHandler(OnFault);
    Core& core = *cores[0];
    int problems = 0;

    // The first switch into 32-bit code: if anything is wrong, this is where it shows.
    progress.Step("first call into 32-bit code");
    Platform::Trace("first call into 32-bit code");
    const u32 echo = core.Call(image->fn[kEmpty], {0x12345678u});
    if (echo != 0x12345678u || !g_fault[0].empty())
    {
        Platform::Log(fmt::format("  the first call into 32-bit code FAILED: returned {:#x}{}", echo,
                                  g_fault[0].empty() ? "" : ", " + g_fault[0]));
        Platform::RemoveFaultHandler();
        return 1;
    }
    Platform::Log("  calling into 32-bit code works");

    std::vector<Kernel> kernels = MakeKernels(scale);
    std::vector<u8> native_buf(kDataSize);
    Platform::Log("kernel   what                              native | Xbox code       | first run");
    std::vector<double> ratios;
    std::vector<u32> checksums;
    for (size_t ki = 0; ki < kernels.size(); ki++)
    {
        const Kernel& k = kernels[ki];
        progress.Step(fmt::format("{} ({}/{}): native", k.name, ki + 1, kernels.size()));
        u32 want = 0, got = 0;
        const double native = BestOf(2, [&] { want = k.native(native_buf.data()); });
        progress.Step(fmt::format("{} ({}/{}): Xbox code", k.name, ki + 1, kernels.size()));
        Platform::Trace(fmt::format("kernel {}", k.name));
        const double t0 = Now();
        got = k.guest(core, *image);
        const double first = Now() - t0;
        const double guest = BestOf(2, [&] { got = k.guest(core, *image); });
        std::string notes;
        if (!g_fault[0].empty())
            notes += " [guest fault: " + g_fault[0] + "]", g_fault[0].clear(), problems++;
        else if (got != want)
            notes += fmt::format(" [CHECKSUM WRONG: {:#x}, native {:#x}]", got, want), problems++;
        ratios.push_back(guest / native);
        checksums.push_back(want);
        Platform::Log(fmt::format("{:<8} {:<30} {:7.3f}s | {:6.3f}s x{:5.2f} | {:7.3f}s{}", k.name, k.what, native, guest,
                                  guest / native, first, notes));
    }
    double log_sum = 0;
    for (double r : ratios)
        log_sum += std::log(r);
    const double slowdown = std::exp(log_sum / double(ratios.size()));

    // The emulator's own costs
    Platform::Log("what an emulator around it pays:");
    const u32 n_calls = u32(std::max(1.0, 2000000 * scale));
    progress.Step("calls into Xbox code");
    const double t_enter = BestOf(2, [&] {
        for (u32 i = 0; i < n_calls; i++)
            core.Call(image->fn[kEmpty], {i});
    });
    Platform::Log(fmt::format("  a call into Xbox code and back: {:.0f} ns", t_enter / n_calls * 1e9));

    progress.Step("kernel calls from Xbox code");
    Platform::Trace("kernel calls (gate)");
    core.SetGate(KernelExport);
    const u32 n_thunk = u32(std::max(1.0, 2000000 * scale));
    u32 thunk_result = 0;
    const double t_thunk = BestOf(2, [&] { thunk_result = core.Call(image->fn[kThunk], {n_thunk, core.Gate()}); });
    u32 thunk_want = 7;
    for (u32 i = 0; i < n_thunk; i++)
    {
        const u32 args[2] = {thunk_want, i};
        thunk_want = KernelExport(args);
    }
    std::string thunk_notes;
    if (!g_fault[0].empty())
        thunk_notes = " [guest fault: " + g_fault[0] + "]", g_fault[0].clear(), problems++;
    else if (thunk_result != thunk_want)
        thunk_notes = " [RESULT WRONG]", problems++;
    Platform::Log(fmt::format("  a kernel call from Xbox code to the host and back: {:.0f} ns{}", t_thunk / n_thunk * 1e9,
                              thunk_notes));

    if (reserve.empty())
    {
        progress.Step("hardware register accesses");
        Platform::Trace("hardware register accesses (faults)");
        const u32 n_mmio = u32(std::max(1.0, 100000 * scale));
        g_mmio_accesses = 0;
        u32 mmio_result = 0;
        const double t_mmio = BestOf(2, [&] { mmio_result = core.Call(image->fn[kMmio], {n_mmio, u32(kMmioBase + 0x100)}); });
        u32 mmio_want = 0;
        for (u32 i = 0; i < n_mmio; i++)
            mmio_want += i * 3u + 1u;
        std::string notes;
        if (!g_fault[0].empty())
            notes = " [guest fault: " + g_fault[0] + "]", g_fault[0].clear(), problems++;
        else if (mmio_result != mmio_want || g_mmio_accesses != u64(n_mmio) * 4)
            notes = fmt::format(" [WRONG: {} accesses handled]", g_mmio_accesses.load()), problems++;
        Platform::Log(fmt::format("  a hardware register access (fault, decode, emulate): {:.2f} us{}",
                                  t_mmio / (2.0 * n_mmio) * 1e6, notes));
    }

    // Three cores at once
    Platform::Log("three cores at once:");
    double scaling_sum = 0;
    int scaling_count = 0;
    for (int ki : {kInt, kBranchy, kMemory})
    {
        const Kernel& k = kernels[size_t(ki)];
        progress.Step(fmt::format("three cores: {}", k.name));
        Platform::Trace(fmt::format("three cores: {}", k.name));
        const double single = BestOf(2, [&] { k.guest(core, *image); });
        double worst = 1e30;
        u32 got[kMaxCores] = {};
        for (int round = 0; round < 2; round++)
        {
            double t[kMaxCores] = {};
            std::vector<std::thread> threads;
            for (int i = 0; i < kMaxCores; i++)
                threads.emplace_back([&, i] {
                    const double s = Now();
                    got[i] = k.guest(*cores[size_t(i)], *image);
                    t[i] = Now() - s;
                });
            for (auto& thread : threads)
                thread.join();
            worst = std::min(worst, *std::max_element(t, t + kMaxCores));
        }
        std::string notes;
        for (int i = 0; i < kMaxCores; i++)
            if (got[i] != checksums[size_t(ki)] || !g_fault[i].empty())
            {
                notes += fmt::format(" [core {} WRONG{}]", i, g_fault[i].empty() ? "" : ": " + g_fault[i]);
                g_fault[i].clear();
                problems++;
            }
        scaling_sum += single / worst;
        scaling_count++;
        Platform::Log(fmt::format("  {:<8} each core at {:3.0f}% of one core alone{}", k.name, single / worst * 100.0,
                                  notes));
    }
    Platform::RemoveFaultHandler();
    if (reserve.empty())
        Platform::Unmap(Linear(kMmioBase), kMmioSize);

    Platform::Log("");
    Platform::Log(fmt::format("Xbox code runs at x{:.2f} the time of native code (1.00 = the same speed), 3 cores "
                              "at once {:.0f}% each; a call in {:.0f} ns, a kernel call {:.0f} ns. Checksums: {}.",
                              slowdown, scaling_sum / scaling_count * 100.0, t_enter / n_calls * 1e9,
                              t_thunk / n_thunk * 1e9,
                              problems == 0 ? "all correct" : fmt::format("{} PROBLEMS", problems)));
    return problems == 0 ? 0 : 2;
}

// ---- memory: what an Xbox emulator needs below 4 GiB

void RunMemory(Progress& progress)
{
    Platform::Log("");
    Platform::Log(fmt::format("memory (pages of {}):", Mib(Platform::PageSize())));

    // free ranges below 4 GiB, in 16 MiB steps
    progress.Step("memory: free ranges below 4 GiB");
    Platform::Trace("memory: free ranges below 4 GiB");
    constexpr u64 kStep = 16 * MB;
    std::string ranges;
    u64 run_start = 0, free_total = 0;
    bool in_run = false;
    for (u64 a = 0; a <= 0x100000000ull; a += kStep)
    {
        bool free = false;
        if (a < 0x100000000ull)
        {
            const u64 at = a ? a : 0x10000; // the first 64 KiB never maps
            free = Platform::ReserveFixed(at, kStep - (at - a)).empty();
            if (free)
                Platform::Unmap(at, kStep - (at - a));
        }
        if (free && !in_run)
            run_start = a, in_run = true;
        if (!free && in_run)
        {
            ranges += fmt::format(" {:#x}-{:#x}", run_start, a - 1);
            free_total += a - run_start;
            in_run = false;
        }
    }
    Platform::Log(fmt::format("  free below 4 GiB: {} in all:{}", Mib(free_total), ranges.empty() ? " none" : ranges));

    // what is mapped below 4 GiB, and what it is (01.00 found something in the Xbox's RAM range)
    progress.Step("memory: mappings below 4 GiB");
    Platform::Trace("memory: mappings below 4 GiB");
    const std::vector<Platform::Mapping> mappings = Platform::Mappings(0x100000000ull);
    Platform::Log(fmt::format("  mapped below 4 GiB: {} ranges", mappings.size()));
    for (size_t i = 0; i < mappings.size() && i < 24; i++)
        Platform::Log(fmt::format("    {:#010x}-{:#010x} {:>9} {}", mappings[i].start, mappings[i].end - 1,
                                  Mib(mappings[i].end - mappings[i].start), mappings[i].what));
    if (mappings.size() > 24)
        Platform::Log(fmt::format("    ... {} more", mappings.size() - 24));

    auto check = [&](const std::string& name, const std::function<std::string()>& f) {
        progress.Step(fmt::format("memory: {}", name));
        Platform::Trace(fmt::format("memory: {}", name));
        const std::string e = f();
        Platform::Log(fmt::format("  {}: {}", name, e.empty() ? "yes" : "NO (" + e + ")"));
        return e.empty();
    };
    auto map_and_free = [&](u64 at, u64 size, int access) {
        std::string e = Platform::MapFixed(at, size, access);
        if (e.empty())
            Platform::Unmap(at, size);
        return e;
    };
    constexpr u64 kRam = 64 * MB - 64 * KB; // the Xbox's 64 MiB above its first 64 KiB
    const int rw = Platform::Read | Platform::Write, rwx = rw | Platform::Exec;
    check("the Xbox's RAM at its own addresses (0x10000-0x3ffffff)", [&] { return map_and_free(0x10000, kRam, rw); });
    // The same questions at an address that is free, so the answers are about the memory, not the
    // address: 64 MiB at 0x04000000, its alias at 0x84000000.
    constexpr u64 kFreeBase = 0x04000000;
    check(fmt::format("64 MiB at {:#x}", kFreeBase), [&] { return map_and_free(kFreeBase, 64 * MB, rw); });
    check(fmt::format("the same, executable ({})", Platform::PoolName(Platform::Exec)),
          [&] { return map_and_free(kFreeBase, 64 * MB, rwx); });
    // how much executable memory there is at low addresses, 16 MiB at a time (up to 256 MiB)
    {
        progress.Step("memory: executable memory below 4 GiB");
        Platform::Trace("memory: executable memory below 4 GiB");
        u64 got = 0;
        while (got < 256 * MB && Platform::MapFixed(kFreeBase + got, 16 * MB, rwx).empty())
            got += 16 * MB;
        if (got)
            Platform::Unmap(kFreeBase, got);
        Platform::Log(fmt::format("  executable memory at {:#x}, 16 MiB at a time: {}", kFreeBase, Mib(got)));
    }
    auto alias = [&](u64 a, u64 b, u64 size, int access) {
        std::string e = Platform::MapAlias(a, b, size, access);
        if (!e.empty())
            return e;
        volatile u32* pa = reinterpret_cast<volatile u32*>(a + size - 4);
        volatile u32* pb = reinterpret_cast<volatile u32*>(b + size - 4);
        *pa = 0xC0FFEE01u;
        const bool ok = *pb == 0xC0FFEE01u;
        Platform::Unmap(a, size);
        Platform::Unmap(b, size);
        return ok ? std::string() : std::string("maps, but the views don't share memory");
    };
    check(fmt::format("64 MiB at {:#x} again at {:#x} (contiguous memory, an alias)", kFreeBase, kFreeBase + 0x80000000),
          [&] { return alias(kFreeBase, kFreeBase + 0x80000000, 64 * MB, rw); });
    check("the same alias, executable", [&] { return alias(kFreeBase, kFreeBase + 0x80000000, 64 * MB, rwx); });
    check(fmt::format("an alias of one {} page", Mib(Platform::PageSize())),
          [&] { return alias(kFreeBase, kFreeBase + 0x80000000, Platform::PageSize(), rw); });
    check("an alias of one 4 KiB page (the Xbox's page size)",
          [&] { return alias(kFreeBase, kFreeBase + 0x80000000, 4 * KB, rw); });
    check("the hardware range 0xfd000000-0xffffffff kept unmapped", [&] {
        std::string e = Platform::ReserveFixed(kMmioBase, kMmioSize);
        if (e.empty())
            Platform::Unmap(kMmioBase, kMmioSize);
        return e;
    });
}

// ---- segments with a base: the Xbox's addresses anywhere free

// What the system exports that an emulator could use here, looked up at run time.
void ReportSystemFunctions()
{
    Platform::Log("");
    Platform::Log("system functions (looked up, not imported):");
    for (const char* name : {"i386_set_ldt", "i386_get_ldt", "sysarch", "amd64_set_fsbase",
                             "sceKernelJitCreateSharedMemory", "sceKernelJitCreateAliasOfSharedMemory",
                             "sceKernelJitMapSharedMemory"})
    {
        Platform::Trace(fmt::format("system function {}", name));
        std::string where;
        const void* p = Platform::SystemFunction(name, where);
        Platform::Log(fmt::format("  {:<38} {}", name, p ? fmt::format("yes, in {}", where) : "no"));
    }
}

// On the PS4 the Xbox's low addresses hold the app's own executable (0x400000-0x977fff in 01.01).
// A code and a data segment with base B put guest address a at linear B + a: the whole 4 GiB guest
// space moves past it. Their LDT entries also give the guest an fs of its own later.
constexpr u64 kSegmentBase = 0x10000000;

int RunBased(double scale, Progress& progress)
{
    ReportSystemFunctions();
    Platform::Log("");
    Platform::Log(fmt::format("segments with a base: guest address 0 at linear {:#x} (LDT entries)", kSegmentBase));
    Platform::Trace("LDT: code segment");
    std::string error;
    const u16 code = Platform::SetLdtEntry(1, u32(kSegmentBase), true, error);
    if (!code)
    {
        Platform::Log("  code segment: NO (" + error + ")");
        return 0;
    }
    Platform::Trace("LDT: data segment");
    const u16 data = Platform::SetLdtEntry(2, u32(kSegmentBase), false, error);
    if (!data)
    {
        Platform::Log(fmt::format("  code segment {:#x}; data segment: NO ({})", code, error));
        return 0;
    }
    const Descriptor dc = Lar(code), dd = Lar(data);
    Platform::Log(fmt::format("  code segment {:#x}: {}", code, dc.valid ? Describe(dc) : "NOT VISIBLE (lar)"));
    Platform::Log(fmt::format("  data segment {:#x}: {}", data, dd.valid ? Describe(dd) : "NOT VISIBLE (lar)"));
    if (!dc.valid || !dd.valid)
        return 0;
    g_space = {kSegmentBase, code, data};
    const int result = RunCpu(scale, progress, "cpu, through the based segments:");
    g_space = {0, g_seg.sel32, g_seg.ss};
    return result;
}

int Run(Platform::Mode mode)
{
    double scale = 1.0;
    if (const char* s = std::getenv("BENCH_SCALE"))
        scale = std::atof(s);
    Progress progress;
    Platform::Log("Xbox CPU Bench " BENCH_VERSION ": original Xbox code (32-bit x86) run natively, no recompiler");
    Platform::Trace("segments");
    g_seg = SurveySegments();
    int result = 0;
    if (mode != Platform::Mode::MemoryOnly)
    {
        if (g_seg.ok)
        {
            g_space = {0, g_seg.sel32, g_seg.ss};
            result = RunCpu(scale, progress,
                            "cpu: Xbox code (Pentium III build) run by this CPU in 32-bit mode, no recompiler");
        }
        else
            Platform::Log("cpu: not run (no 32-bit code segment)"), result = 1;
        if (g_seg.ok)
            result = std::max(result, RunBased(scale, progress));
    }
    if (mode != Platform::Mode::CpuOnly)
        RunMemory(progress);
    Platform::Finish("Xbox CPU Bench done");
    return result;
}

int g_exit_code = 0;
Platform::Mode g_mode = Platform::Mode::All;

void* RunThread(void*)
{
    try
    {
        g_exit_code = Run(g_mode);
    }
    catch (const std::exception& e)
    {
        Platform::Log(fmt::format("stopped by an error: {}", e.what()));
        Platform::Finish("Xbox CPU Bench stopped by an error");
        g_exit_code = 3;
    }
    return nullptr;
}

} // namespace

int main()
{
    g_mode = Platform::Init();
    // On a thread with a known, generous stack (the PS4's main thread stack is small).
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attr, RunThread, nullptr) != 0)
        RunThread(nullptr);
    else
        pthread_join(thread, nullptr);
    pthread_attr_destroy(&attr);
    return g_exit_code;
}
