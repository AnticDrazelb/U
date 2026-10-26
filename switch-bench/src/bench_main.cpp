// Switch-CPU benchmark: how fast does this machine run Nintendo Switch (AArch64) code through
// Eden's dynarmic, configured the way Eden configures it when fastmem isn't available (as on the
// PS4): an absolute-offset page table, misalignment checks on page boundaries, cycle counting
// off (wall clock). Each kernel runs natively and recompiled (Eden's "Accurate" and "Auto" CPU
// accuracy), checksums are compared, and the same kernels run on three cores at once (Switch games
// use three). Then the platform's memory is probed for what an emulator would need.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <sys/mman.h>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/backend/x64/xbyak.h"

#include "guest_image.h"
#include "platform.h"

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Native builds of guest/bench.c (BENCH_PREFIX=native_).
extern "C" {
struct Entity;
struct Pair;
u64 native_bench_int(u64 n);
u64 native_bench_branchy(u64 n, void* ents, u64 count);
u64 native_bench_calls(u64 n);
u64 native_bench_memory(u64 n, void* buf, u64 words);
u64 native_bench_copy(u64 n, void* buf, u64 pairs);
u64 native_bench_float(u64 n, void* a, void* b, u64 len);
u64 native_bench_fma(u64 n);
}

namespace {

constexpr u64 kPageBits = 12;
constexpr u64 kPageSize = 1ull << kPageBits;
constexpr u32 kAddressBits = 32;          // guest addresses below 4 GiB (a 8 MiB page table)
constexpr u64 kImageBase = 0x80000000;   // guest/link.ld
constexpr u64 kImageSize = 256ull << 10;
constexpr u64 kStackBase = 0x90000000;
constexpr u64 kStackSize = 256ull << 10;
constexpr u64 kDataBase = 0xA0000000;    // the kernels' buffers (the memory kernel's 4 MiB is the most)
constexpr u64 kDataSize = 8ull << 20;
constexpr u64 kHaltAddr = 0x7FFF0000;    // "svc #0xff": where kernels return to
constexpr u32 kHaltSvc = 0xFF;
// Eden's PageTable entry layout (src/common/page_table.h)
constexpr u64 kAttributeMask = ((1ull << 44) - 1) << 12;

double Now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Test knobs (host builds; defaults are what the PS4 build uses):
//  BENCH_SIGN_BIT=n   page-table sign bit (Eden passes 57, which loses the pointer: see README.md)
//  BENCH_LOW_HOST=1   guest memory at low host addresses, so page-table entries (host - guest) are
//                     negative, as a Switch game's high addresses would make them on the PS4
u8 SignBit()
{
    const char* s = std::getenv("BENCH_SIGN_BIT");
    return s ? u8(std::atoi(s)) : u8(55); // 44 bits of pointer from bit 12: the top one is bit 55
}

// One emulated address space: a few regions of host memory behind a flat page table.
struct GuestMemory
{
    struct Region
    {
        u64 base, size;
        u8* host;
    };
    std::vector<Region> regions;
    std::vector<u64> table = std::vector<u64>(std::size_t(1) << (kAddressBits - kPageBits), 0);

    ~GuestMemory()
    {
        for (auto& r : regions)
            munmap(r.host, r.size);
    }
    u8* Map(u64 base, u64 size)
    {
        void* hint = nullptr;
        int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_FIXED_NOREPLACE
        static std::atomic<u64> low_next{0x10000000};
        if (std::getenv("BENCH_LOW_HOST"))
        {
            hint = reinterpret_cast<void*>(low_next.fetch_add(size + (1u << 20)));
            flags |= MAP_FIXED_NOREPLACE;
        }
#endif
        void* p = mmap(hint, size, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (p == MAP_FAILED)
            return nullptr;
        u8* host = static_cast<u8*>(p);
        regions.push_back({base, size, host});
        // absolute offset: host address = sign-extended (entry & mask) + guest address
        const u64 offset = reinterpret_cast<u64>(host) - base;
        for (u64 page = base >> kPageBits; page < (base + size) >> kPageBits; page++)
            table[page] = offset & kAttributeMask;
        return host;
    }
    u8* Host(u64 vaddr, u64 size)
    {
        for (auto& r : regions)
            if (vaddr >= r.base && vaddr + size <= r.base + r.size)
                return r.host + (vaddr - r.base);
        return nullptr;
    }
};

// Dynarmic's callbacks: memory the page table doesn't cover (it covers all of ours), the halt SVC.
class Env final : public Dynarmic::A64::UserCallbacks
{
public:
    GuestMemory& mem;
    Dynarmic::A64::Jit* jit = nullptr;
    u64 ticks = 0;
    u64 slow_accesses = 0; // loads and stores the page table didn't serve (callbacks)
    std::string fault;

    explicit Env(GuestMemory& m) : mem(m) {}

    std::optional<u32> MemoryReadCode(u64 vaddr) override
    {
        if (u8* p = mem.Host(vaddr, 4))
        {
            u32 v;
            std::memcpy(&v, p, 4);
            return v;
        }
        return std::nullopt;
    }
    u64 MemoryRead(u64 vaddr, std::size_t size) override
    {
        slow_accesses++;
        u64 v = 0;
        if (u8* p = mem.Host(vaddr, size))
            std::memcpy(&v, p, size);
        else
            Fault(fmt::format("read of {} bytes at {:#x}", size, vaddr));
        return v;
    }
    Dynarmic::A64::Vector MemoryRead128(u64 vaddr) override
    {
        return {MemoryRead(vaddr, 8), MemoryRead(vaddr + 8, 8)};
    }
    void MemoryWrite(u64 vaddr, u64 value, std::size_t size) override
    {
        slow_accesses++;
        if (u8* p = mem.Host(vaddr, size))
            std::memcpy(p, &value, size);
        else
            Fault(fmt::format("write of {} bytes at {:#x}", size, vaddr));
    }
    void MemoryWrite128(u64 vaddr, Dynarmic::A64::Vector value) override
    {
        MemoryWrite(vaddr, value[0], 8);
        MemoryWrite(vaddr + 8, value[1], 8);
    }
    void CallSVC(u32 swi) override
    {
        if (swi != kHaltSvc)
            Fault(fmt::format("svc #{:#x}", swi));
        jit->HaltExecution();
    }
    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception exception) override
    {
        Fault(fmt::format("exception {} at {:#x}", static_cast<int>(exception), pc));
    }
    void AddTicks(u64 t) override { ticks += t; }
    u64 GetTicksRemaining() override { return 1ull << 60; }
    u64 GetCNTPCT() override { return static_cast<u64>(Now() * 19200000.0); }

private:
    void Fault(std::string what)
    {
        if (fault.empty())
            fault = std::move(what);
        jit->HaltExecution();
    }
};

// Eden's CPU accuracy settings (src/core/arm/dynarmic/arm_dynarmic_64.cpp; Unsafe with its defaults)
enum class Accuracy { Accurate, Auto, Unsafe };

// A Switch CPU core: a dynarmic JIT over one address space with the benchmark image loaded.
struct Core
{
    GuestMemory mem;
    std::unique_ptr<Env> env;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    std::vector<u64> kernels;
    u8* data = nullptr;
    std::string error;

    Core(Accuracy accuracy, bool count_instructions, u8 core_index)
    {
        // The JIT first: its code cache is executable memory, the scarcest kind on the PS4.
        env = std::make_unique<Env>(mem);
        Dynarmic::A64::UserConfig config{};
        config.callbacks = env.get();
        // as Eden's ArmDynarmic64::MakeJit with a page table and no fastmem
        config.page_table = reinterpret_cast<void**>(mem.table.data());
        config.page_table_address_space_bits = kAddressBits;
        config.page_table_pointer_mask = kAttributeMask;
        config.page_table_marked_bit = u8(0);
        config.silently_mirror_page_table = false;
        config.absolute_offset_page_table = true;
        config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
        config.only_detect_misalignment_via_page_table_on_page_boundary = true;
        // as Eden does where host memory lies below 2^39 (FreeBSD, the PS4): entries can be negative
        config.page_table_sign_extension = SignBit();
        config.fastmem_pointer = std::nullopt;
        config.processor_id = core_index;
        config.dczid_el0 = 4;
        config.ctr_el0 = 0x8444c004;
        config.cntfrq_el0 = 19200000;
        config.define_unpredictable_behaviour = true;
        config.wall_clock_cntpct = !count_instructions;
        config.enable_cycle_counting = count_instructions;
        config.code_cache_size = 8u << 20; // Eden: 512 MiB; these kernels compile to well under 1 MiB
        if (accuracy != Accuracy::Accurate)
        {
            config.unsafe_optimizations = true;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        }
        if (accuracy == Accuracy::Unsafe)
        {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
        }
        try
        {
            jit = std::make_unique<Dynarmic::A64::Jit>(config);
        }
        catch (const std::exception& e)
        {
            error = fmt::format("no executable memory for the recompiler's code ({})", e.what());
            return;
        }
        env->jit = jit.get();

        if (!LoadImage())
            return;
        data = mem.Map(kDataBase, kDataSize);
        u8* halt = mem.Map(kHaltAddr, kPageSize);
        if (!data || !halt || !mem.Map(kStackBase, kStackSize))
        {
            error = "out of memory for the guest";
            return;
        }
        const u32 svc = 0xD4000001u | (kHaltSvc << 5);
        std::memcpy(halt, &svc, 4);
    }

    bool LoadImage()
    {
        // a little ELF64 loader: PT_LOAD segments into the image region
        const u8* elf = guest_image;
        if (guest_image_len < 64 || std::memcmp(elf, "\x7f" "ELF", 4) != 0)
        {
            error = "bad guest image";
            return false;
        }
        u8* image = mem.Map(kImageBase, kImageSize);
        if (!image)
        {
            error = "out of memory for the guest image";
            return false;
        }
        u64 phoff;
        std::uint16_t phentsize, phnum;
        std::memcpy(&phoff, elf + 32, 8);
        std::memcpy(&phentsize, elf + 54, 2);
        std::memcpy(&phnum, elf + 56, 2);
        for (unsigned i = 0; i < phnum; i++)
        {
            const u8* ph = elf + phoff + u64(i) * phentsize;
            u32 type;
            u64 offset, vaddr, filesz, memsz;
            std::memcpy(&type, ph, 4);
            std::memcpy(&offset, ph + 8, 8);
            std::memcpy(&vaddr, ph + 16, 8);
            std::memcpy(&filesz, ph + 32, 8);
            std::memcpy(&memsz, ph + 40, 8);
            if (type != 1)
                continue;
            if (vaddr < kImageBase || vaddr + memsz > kImageBase + kImageSize || offset + filesz > guest_image_len)
            {
                error = "guest segment outside the image region";
                return false;
            }
            std::memcpy(image + (vaddr - kImageBase), elf + offset, filesz);
        }
        for (int k = 0; k < 7; k++)
        {
            u64 entry;
            std::memcpy(&entry, image + k * 8, 8); // bench_table at the image start
            kernels.push_back(entry);
        }
        return true;
    }

    // Calls kernel k with up to four arguments; returns X0.
    u64 Call(int k, std::initializer_list<u64> args) { return CallAt(kernels[k], args); }
    // Runs only the halt stub: sets up dynarmic's dispatcher, so later first runs time compilation only.
    void WarmUp() { CallAt(kHaltAddr, {}); }

    u64 CallAt(u64 pc, std::initializer_list<u64> args)
    {
        jit->Reset();
        int r = 0;
        for (u64 a : args)
            jit->SetRegister(r++, a);
        jit->SetRegister(30, kHaltAddr);
        jit->SetSP(kStackBase + kStackSize - 64);
        jit->SetPC(pc);
        env->fault.clear();
        jit->Run();
        if (!env->fault.empty() && error.empty())
            error = env->fault;
        return jit->GetRegister(0);
    }
};

// The kernels, their sizes and their native and guest calls.
struct Kernel
{
    const char* name;
    const char* what;
    u64 n;
    std::function<u64(u8*)> native;
    std::function<u64(Core&)> guest;
};

constexpr u64 kEntities = 512, kWords = 1u << 20, kPairs = 1u << 17, kFloats = 1u << 16;

std::vector<Kernel> MakeKernels(double scale)
{
    auto N = [&](u64 n) { return std::max<u64>(1, u64(double(n) * scale)); };
    // 0.05-0.2 s natively on a desktop, a few times that on the PS4; the slowest recompiled kernels
    // (float, calls) have the fewest iterations, to keep the whole run to a few minutes there
    const u64 n_int = N(60000000), n_br = N(50000), n_calls = N(8000), n_mem = N(24), n_copy = N(1000),
              n_float = N(2500), n_fma = N(8000000);
    const u64 d = kDataBase;
    return {
        {"int", "integer math", n_int, [=](u8*) { return native_bench_int(n_int); },
         [=](Core& c) { return c.Call(0, {n_int}); }},
        {"branchy", "game logic (state machines)", n_br,
         [=](u8* buf) { return native_bench_branchy(n_br, buf, kEntities); },
         [=](Core& c) { return c.Call(1, {n_br, d, kEntities}); }},
        {"calls", "calls and indirect calls", n_calls, [=](u8*) { return native_bench_calls(n_calls); },
         [=](Core& c) { return c.Call(2, {n_calls}); }},
        {"memory", "random and sequential memory", n_mem,
         [=](u8* buf) { return native_bench_memory(n_mem, buf, kWords); },
         [=](Core& c) { return c.Call(3, {n_mem, d, kWords}); }},
        {"copy", "16-byte block copies", n_copy, [=](u8* buf) { return native_bench_copy(n_copy, buf, kPairs); },
         [=](Core& c) { return c.Call(4, {n_copy, d, kPairs}); }},
        {"float", "vector float loops (NEON)", n_float,
         [=](u8* buf) { return native_bench_float(n_float, buf, buf + kFloats * 4, kFloats); },
         [=](Core& c) { return c.Call(5, {n_float, d, d + kFloats * 4, kFloats}); }},
        {"fma", "fused multiply-add math", n_fma, [=](u8*) { return native_bench_fma(n_fma); },
         [=](Core& c) { return c.Call(6, {n_fma}); }},
    };
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

} // namespace

std::string HostCpu()
{
    Xbyak::util::Cpu cpu;
    using C = Xbyak::util::Cpu;
    std::string f;
    auto feature = [&](bool has, const char* name) { f += fmt::format(" {}{}", has ? "+" : "-", name); };
    feature(cpu.has(C::tSSE41), "SSE4.1");
    feature(cpu.has(C::tAVX), "AVX");
    feature(cpu.has(C::tAVX2), "AVX2");
    feature(cpu.has(C::tFMA), "FMA");
    feature(cpu.has(C::tBMI2), "BMI2");
    feature(cpu.has(C::tF16C), "F16C");
    feature(cpu.has(C::tAESNI), "AES");
    return fmt::format("host CPU family {:#x} model {:#x}, {} hardware threads:{}", cpu.displayFamily,
                       cpu.displayModel, std::thread::hardware_concurrency(), f);
}

// What runs, for the progress line: "float (6/7): Auto, 1:23 so far".
class Progress
{
public:
    void Kernel(const char* name, size_t index, size_t count) { current = fmt::format("{} ({}/{})", name, index + 1, count); }
    void Step(const char* step)
    {
        const int elapsed = int(Now() - start);
        Platform::Progress(fmt::format("running {}: {}   {}:{:02} so far, about 3-5 minutes in all", current, step,
                                       elapsed / 60, elapsed % 60));
    }

private:
    double start = Now();
    std::string current;
};

int Run(Platform::Mode mode)
{
    // BENCH_SCALE (host builds): fewer iterations for quick checks
    double scale = 1.0;
    if (const char* s = std::getenv("BENCH_SCALE"))
        scale = std::atof(s);
    Progress progress;
    Platform::Log("Switch CPU Bench " BENCH_VERSION ": Eden's dynarmic (AArch64 -> x86-64), page table, no fastmem");
    Platform::Log(HostCpu());
    if (mode == Platform::Mode::MemoryOnly)
    {
        progress.Kernel("memory checks", 0, 1);
        Platform::MemoryChecks();
        Platform::Finish("Switch CPU Bench: memory checks done");
        return 0;
    }
    Platform::Notify("Switch CPU Bench started: about 3-5 minutes, results on screen");

    std::vector<Kernel> kernels = MakeKernels(scale);
    std::vector<u8> native_buf(kDataSize);
    progress.Kernel("setup", 0, 1);
    progress.Step("starting the recompilers");
    Core accurate(Accuracy::Accurate, false, 0), automatic(Accuracy::Auto, false, 0),
        unsafe(Accuracy::Unsafe, false, 0), counting(Accuracy::Auto, true, 0);
    for (Core* c : {&accurate, &automatic, &unsafe, &counting})
    {
        if (c->error.empty())
            c->WarmUp();
        if (!c->error.empty())
        {
            Platform::Log("setup failed: " + c->error);
            Platform::MemoryChecks();
            Platform::Finish("Switch CPU Bench failed to start: " + c->error);
            return 1;
        }
    }

    // Times are the best of two runs; "first run" is Accurate's first, which includes compiling.
    // Accurate must reproduce the native checksum exactly; Auto and Unsafe may differ only where they
    // are allowed to round differently (unfused multiply-adds on a CPU without FMA).
    Platform::Log("kernel   what                              native | Accurate        | Auto            | "
                  "Unsafe          | first run | Auto MIPS");
    struct Result
    {
        double native, automatic, unsafe, mips;
        u64 checksum;
    };
    std::vector<Result> results;
    int problems = 0;
    for (size_t ki = 0; ki < kernels.size(); ki++)
    {
        const Kernel& k = kernels[ki];
        progress.Kernel(k.name, ki, kernels.size());
        Result r{};
        u64 want = 0, got_acc = 0, got_auto = 0, got_unsafe = 0;
        progress.Step("native");
        r.native = BestOf(2, [&] { want = k.native(native_buf.data()); });
        r.checksum = want;
        progress.Step("recompiled, Accurate");
        const double t0 = Now();
        got_acc = k.guest(accurate);
        const double cold = Now() - t0;
        const double t_acc = BestOf(2, [&] { got_acc = k.guest(accurate); });
        progress.Step("recompiled, Auto");
        automatic.env->slow_accesses = 0;
        r.automatic = BestOf(2, [&] { got_auto = k.guest(automatic); });
        const u64 slow = automatic.env->slow_accesses / 2;
        progress.Step("recompiled, Unsafe");
        r.unsafe = BestOf(2, [&] { got_unsafe = k.guest(unsafe); });
        progress.Step("counting instructions");
        counting.env->ticks = 0;
        k.guest(counting);
        r.mips = counting.env->ticks / r.automatic / 1e6;

        std::string notes;
        if (slow)
            notes += fmt::format(" [{} slow-path memory accesses]", slow);
        const bool fma = std::string(k.name) == "fma";
        if (got_acc != want)
            notes += " [ACCURATE CHECKSUM WRONG]", problems++;
        if (got_auto != want)
            notes += fma ? " [Auto: unfused FMA, allowed]" : " [AUTO CHECKSUM WRONG]", problems += !fma;
        if (got_unsafe != want)
            notes += fma ? " [Unsafe: unfused FMA, allowed]" : " [UNSAFE CHECKSUM WRONG]", problems += !fma;
        for (Core* c : {&accurate, &automatic, &unsafe, &counting})
            if (!c->error.empty())
            {
                notes += " [guest fault: " + c->error + "]";
                c->error.clear();
                problems++;
            }
        Platform::Log(fmt::format("{:<8} {:<30} {:7.3f}s | {:6.3f}s x{:5.2f} | {:6.3f}s x{:5.2f} | {:6.3f}s x{:5.2f} | "
                                  "{:7.3f}s  | {:6.0f}M{}",
                                  k.name, k.what, r.native, t_acc, t_acc / r.native, r.automatic,
                                  r.automatic / r.native, r.unsafe, r.unsafe / r.native, cold, r.mips, notes));
        results.push_back(r);
    }
    auto geomean = [&](auto f) {
        double sum = 0;
        for (const Result& r : results)
            sum += std::log(f(r));
        return std::exp(sum / results.size());
    };
    const double slow_auto = geomean([](const Result& r) { return r.automatic / r.native; });
    const double slow_unsafe = geomean([](const Result& r) { return r.unsafe / r.native; });
    const double mips = geomean([](const Result& r) { return r.mips; });

    // Three cores at once (Switch games run on three): each with its own JIT and memory.
    Platform::Log("three cores at once (Auto):");
    double scaling_sum = 0;
    int scaling_count = 0;
    Core core1(Accuracy::Auto, false, 1), core2(Accuracy::Auto, false, 2);
    for (Core* c : {&core1, &core2})
        if (!c->error.empty())
        {
            Platform::Log("  not run: " + c->error);
            problems++;
        }
    Core* cores[3] = {&automatic, &core1, &core2};
    const int scaling_kernels[] = {0, 1, 3};
    for (size_t si = 0; si < 3 && core1.error.empty() && core2.error.empty(); si++)
    {
        const int ki = scaling_kernels[si];
        const Kernel& k = kernels[ki];
        progress.Kernel(fmt::format("three cores, {}", k.name).c_str(), si, 3);
        progress.Step("compiling");
        for (Core* c : cores)
            k.guest(*c);
        progress.Step("one core");
        const double single = BestOf(2, [&] { k.guest(automatic); });
        progress.Step("three cores");
        double times[3] = {}, worst = 1e30;
        u64 got[3] = {};
        for (int round = 0; round < 2; round++)
        {
            double t[3] = {};
            std::vector<std::thread> threads;
            for (int i = 0; i < 3; i++)
                threads.emplace_back([&, i] {
                    const double t0 = Now();
                    got[i] = k.guest(*cores[i]);
                    t[i] = Now() - t0;
                });
            for (auto& thread : threads)
                thread.join();
            const double w = *std::max_element(t, t + 3);
            if (w < worst)
                worst = w, std::copy(t, t + 3, times);
        }
        std::string notes;
        for (int i = 0; i < 3; i++)
            if (got[i] != results[ki].checksum || !cores[i]->error.empty())
            {
                notes += fmt::format(" [core {} CHECKSUM WRONG{}]", i,
                                     cores[i]->error.empty() ? "" : ": " + cores[i]->error);
                cores[i]->error.clear();
                problems++;
            }
        scaling_sum += single / worst;
        scaling_count++;
        Platform::Log(fmt::format("  {:<8} 1 core {:6.3f}s, 3 cores {:6.3f}/{:6.3f}/{:6.3f}s: each at {:3.0f}% of "
                                  "single-core speed{}",
                                  k.name, single, times[0], times[1], times[2], single / worst * 100.0, notes));
    }
    const double scaling = scaling_count ? scaling_sum / scaling_count : 0.0;

    const std::string summary = fmt::format(
        "Switch code runs {:.1f}x slower than native (Auto; Unsafe {:.1f}x), {:.0f}M guest instructions/s per "
        "core (a Switch core: 1020 MHz), 3 cores at once {:.0f}% each. Checksums: {}.",
        slow_auto, slow_unsafe, mips, scaling * 100.0,
        problems == 0 ? "all correct" : fmt::format("{} PROBLEMS", problems));
    Platform::Log("");
    Platform::Log(summary);
    if (mode == Platform::Mode::All)
        Platform::MemoryChecks();
    Platform::Finish(summary);
    return problems == 0 ? 0 : 2;
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
        Platform::Finish(fmt::format("Switch CPU Bench stopped by an error: {}", e.what()));
        g_exit_code = 3;
    }
    return nullptr;
}

int main()
{
    g_mode = Platform::Init();
    // On a thread with a known, generous stack: the PS4's main thread stack is small, and dynarmic
    // compiles on the thread that runs it.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attr, RunThread, nullptr) != 0)
        RunThread(nullptr);
    else
        pthread_join(thread, nullptr);
    pthread_attr_destroy(&attr);
    return g_exit_code;
}
