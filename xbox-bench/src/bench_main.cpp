// Xbox CPU Bench: can this machine run original Xbox (32-bit x86) code as it is, on its own CPU?
//
// The original Xbox has a 733 MHz Pentium III. The PS4's Jaguar is also x86, so an emulator there
// doesn't need a recompiler at all if the CPU can be put into 32-bit compatibility mode - a far
// jump to a 32-bit code segment - the way Cxbx-Reloaded runs Xbox code natively on Windows. This
// program finds out whether that works, what it costs, and what an emulator around it would pay:
//
//  1. Segments: which code and data segments the system set up (LAR on every GDT selector).
//  2. CPU: seven kernels built for a Pentium III, linked at 0x00010000 like an XBE, run in 32-bit
//     mode out of memory below 4 GiB, against the same kernels built natively. Checksums must match.
//  3. The emulator's costs: a call into Xbox code and back, a "kernel export" call from Xbox code to
//     64-bit host code (a high-level kernel), and a hardware-register access that faults and is
//     emulated by the fault handler (the NV2A's registers, as Cxbx-Reloaded does it).
//  4. Three cores at once (one for the game, the others for the emulator's GPU and audio threads).
//  5. The guest's fs segment (Xbox code finds its thread through fs): an LDT entry.
//  6. Memory: what is free below 4 GiB, the Xbox's 64 MiB at its own addresses, executable, and the
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

// Where things go, as offsets from the layout's base (0 = the Xbox's own addresses; another
// base if the system has something there). All of it sits inside the Xbox's 64 MiB.
constexpr u64 kImageOffset = 0x00010000, kImageSize = 1 * MB; // guest/link.ld, an XBE's base
constexpr u64 kDataOffset = 0x01000000, kDataSize = 8 * MB;   // per core
constexpr u64 kStackOffset = 0x02800000, kStackSize = 1 * MB;  // per core
constexpr u64 kTrampOffset = 0x02F00000, kTrampStride = 64 * KB;
constexpr int kMaxCores = 3;
// The NV2A's registers (16 MiB at 0xFD000000), and the rest of the Xbox's devices up to 4 GiB:
// kept inaccessible, so every access faults.
constexpr u64 kMmioBase = 0xFD000000, kMmioSize = 0x03000000;

// The bench_table at the image's start: guest/kernels.c xb_table
enum GuestFn { kInt, kBranchy, kCalls, kMemory, kCopy, kSse, kX87, kEmpty, kMmio, kThunk, kFsRead, kFnCount };

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
    __asm__ volatile("lar %k2, %0\n\tsetz %1" : "=r"(rights), "=q"(ok) : "r"(u32(selector)) : "cc");
    __asm__ volatile("lsl %k2, %0\n\tsetz %1" : "=r"(limit), "=q"(ok_limit) : "r"(u32(selector)) : "cc");
    return {ok != 0, rights, ok_limit ? limit : 0};
}

bool IsCode(u32 r) { return (r & (1u << 12)) && (r & (1u << 11)); }
bool IsPresent(u32 r) { return r & (1u << 15); }
u32 Dpl(u32 r) { return (r >> 13) & 3; }
bool IsLong(u32 r) { return r & (1u << 21); }
bool IsBig(u32 r) { return r & (1u << 22); }

std::string Describe(const Descriptor& d)
{
    const u32 r = d.rights;
    std::string kind;
    if (!(r & (1u << 12)))
        kind = "system";
    else if (IsCode(r))
        kind = IsLong(r) ? "64-bit code" : IsBig(r) ? "32-bit code" : "16-bit code";
    else
        kind = fmt::format("{}-bit data{}", IsBig(r) ? 32 : 16, (r & (1u << 9)) ? " rw" : " ro");
    return fmt::format("{} dpl{} limit {:#x}{}", kind, Dpl(r), d.limit, IsPresent(r) ? "" : " not present");
}

u16 ReadCs() { u16 v; __asm__("mov %%cs, %0" : "=r"(v)); return v; }
u16 ReadSs() { u16 v; __asm__("mov %%ss, %0" : "=r"(v)); return v; }
u16 ReadDs() { u16 v; __asm__("mov %%ds, %0" : "=r"(v)); return v; }
u16 ReadFs() { u16 v; __asm__("mov %%fs, %0" : "=r"(v)); return v; }
u64 FsBase() { u64 v; __asm__("mov %%fs:0, %0" : "=r"(v)); return v; } // the TCB's self pointer

struct Segments
{
    u16 cs = 0, ss = 0, sel32 = 0;
    bool ok = false;
};

// Logs every present descriptor a user program may load (DPL 3) and picks the 32-bit code segment.
Segments SurveySegments()
{
    Segments s;
    s.cs = ReadCs();
    s.ss = ReadSs();
    Platform::Log(fmt::format("segments: cs {:#x}, ss {:#x}, ds {:#x}, fs {:#x}", s.cs, s.ss, ReadDs(), ReadFs()));
    for (u16 index = 1; index < 32; index++)
    {
        const u16 selector = u16(index << 3 | 3);
        const Descriptor d = Lar(selector);
        if (!d.valid)
            continue;
        Platform::Log(fmt::format("  {:#06x}: {}{}", selector, Describe(d), selector == s.cs ? " (cs)" : selector == s.ss ? " (ss)" : ""));
        if (!s.sel32 && IsCode(d.rights) && IsPresent(d.rights) && Dpl(d.rights) == 3 && !IsLong(d.rights) &&
            IsBig(d.rights))
            s.sel32 = selector;
    }
    const Descriptor ss = Lar(s.ss);
    const bool ss_flat = ss.valid && IsBig(ss.rights) && ss.limit == 0xFFFFFFFFu;
    if (!s.sel32)
        Platform::Log("  NO 32-bit user code segment: Xbox code can't run natively (a recompiler is needed)");
    else if (!ss_flat)
        Platform::Log("  ss isn't a flat 32-bit data segment: 32-bit code would have no usable stack");
    else
        Platform::Log(fmt::format("  32-bit code segment {:#x}, data segment {:#x} (flat): Xbox code can run natively",
                                  s.sel32, s.ss));
    s.ok = s.sel32 && ss_flat;
    return s;
}

// ---- the guest

struct Core;
Core* g_cores[kMaxCores] = {};
Segments g_seg;
u64 g_layout = 0;

// The NV2A stand-in: registers that keep what is written to them.
u32 g_mmio_regs[4096];
std::atomic<u64> g_mmio_accesses{0};

u32 ToU32(u64 v) { return static_cast<u32>(v); }

struct Core
{
    int index = 0;
    u64 tramp = 0, stack = 0, data = 0;
    std::string error;
    bool mapped_tramp = false, mapped_stack = false, mapped_data = false;

    u64 Slot(int offset) const { return tramp + u64(offset); }
    u64& SlotRef(int offset) const { return *reinterpret_cast<u64*>(Slot(offset)); }
    u64 Entry(const u8* symbol) const { return tramp + u64(symbol - xbt_begin); }

    explicit Core(int i) : index(i)
    {
        tramp = g_layout + kTrampOffset + u64(i) * kTrampStride;
        stack = g_layout + kStackOffset + u64(i) * kStackSize;
        data = g_layout + kDataOffset + u64(i) * kDataSize;
        const size_t tramp_size = std::max<size_t>(Platform::PageSize(), 4096);
        std::string e = Platform::MapFixed(tramp, tramp_size, Platform::Read | Platform::Write | Platform::Exec);
        if (!e.empty())
            error = fmt::format("trampoline at {:#x}: {}", tramp, e);
        else
            mapped_tramp = true;
        if (error.empty() && !(e = Platform::MapFixed(stack, kStackSize, Platform::Read | Platform::Write)).empty())
            error = fmt::format("stack at {:#x}: {}", stack, e);
        else if (error.empty())
            mapped_stack = true;
        if (error.empty() && !(e = Platform::MapFixed(data, kDataSize, Platform::Read | Platform::Write)).empty())
            error = fmt::format("data at {:#x}: {}", data, e);
        else if (error.empty())
            mapped_data = true;
        if (!error.empty())
            return;
        std::memcpy(reinterpret_cast<void*>(tramp), xbt_begin, size_t(xbt_end - xbt_begin));
        SlotRef(8) = g_seg.sel32;
        SlotRef(16) = g_seg.cs;
        SlotRef(32) = g_seg.ss;
        SlotRef(48) = 0;
        g_cores[i] = this;
    }
    ~Core()
    {
        if (g_cores[index] == this)
            g_cores[index] = nullptr;
        if (mapped_tramp)
            Platform::Unmap(tramp, std::max<size_t>(Platform::PageSize(), 4096));
        if (mapped_stack)
            Platform::Unmap(stack, kStackSize);
        if (mapped_data)
            Platform::Unmap(data, kDataSize);
    }

    void SetGate(u32 (*handler)(const u32*)) { SlotRef(24) = reinterpret_cast<u64>(handler); }

    // Calls guest function `fn` (cdecl) with up to six 32-bit arguments; returns its eax.
    u32 Call(u32 fn, std::initializer_list<u32> args)
    {
        u64 esp = (stack + kStackSize - 256) & ~u64(15);
        u32* a = reinterpret_cast<u32*>(esp);
        for (u32 v : args)
            *a++ = v;
        using Enter = u64 (*)(u32, u32);
        const auto enter = reinterpret_cast<Enter>(Entry(xbt_enter64));
        return ToU32(enter(fn, ToU32(esp)));
    }
    bool InStack(u64 esp) const { return esp >= stack && esp < stack + kStackSize;