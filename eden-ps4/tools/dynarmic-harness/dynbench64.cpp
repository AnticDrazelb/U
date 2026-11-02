// A64 harness for Eden-for-PS4's dynarmic, as dynbench.cpp is for A32: Eden's page table
// configuration without fastmem. Exclusives inline through the page table vs through the monitor.
//   dynbench64 test  <code.bin> [noexcl]
//   dynbench64 bench <code.bin> <iterations> [noexcl]
// Guest map: code at 0x10000, data 0x10000000-0x11000000 in shuffled host pages, 0x10800000
// marked, 0x10900000 unmapped.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

#include "common/page_table.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/optimization_flags.h"

namespace {
constexpr uint64_t kCode = 0x00010000, kData = 0x10000000, kDataSize = 16u << 20;
constexpr uint64_t kMarked = 0x10800000, kUnmapped = 0x10900000;
constexpr size_t kPages = size_t{1} << 20;  // 32-bit address space

struct Env final : Dynarmic::A64::UserCallbacks {
    std::vector<uint8_t> code;
    int memfd = -1;          // the data's memory, mappable twice (fastmem)
    uint8_t* data = nullptr;
    std::vector<uint8_t*> host_page;
    std::vector<uint64_t> entries = std::vector<uint64_t>(kPages, 0);
    uint64_t slow_reads = 0, slow_writes = 0;
    Dynarmic::A64::Jit* jit = nullptr;

    Env() {
        memfd = memfd_create("guest", 0);
        if (ftruncate(memfd, kDataSize) != 0)
            std::exit(2);
        data = static_cast<uint8_t*>(mmap(nullptr, kDataSize, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0));
        const size_t n = kDataSize / 4096;
        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; i++)
            order[i] = i;
        std::shuffle(order.begin(), order.end(), std::mt19937{1234});
        host_page.resize(n);
        for (size_t i = 0; i < n; i++) {
            host_page[i] = data + order[i] * 4096;
            for (size_t b = 0; b < 4096; b++)
                host_page[i][b] = uint8_t(i * 131 + b * 7);
        }
        for (size_t i = 0; i < n; i++) {
            const uint64_t vaddr = kData + i * 4096;
            if (vaddr == kUnmapped)
                continue;
            auto& e = *reinterpret_cast<Common::PageTable::PageEntryData*>(&entries[vaddr >> 12]);
            e.Store(false, Common::PageType::Memory, uint16_t(600 + i), reinterpret_cast<uintptr_t>(host_page[i]) - vaddr);
            if (vaddr == kMarked)
                e.MarkRasterizerCached();
        }
    }
    uint8_t* Host(uint64_t vaddr) {
        if (vaddr < kData || vaddr >= kData + kDataSize || (vaddr & ~uint64_t{4095}) == kUnmapped)
            return nullptr;
        return host_page[(vaddr - kData) >> 12] + (vaddr & 4095);
    }
    std::optional<std::uint32_t> MemoryReadCode(uint64_t vaddr) override {
        if (uint8_t* p = vaddr >= kData ? Host(vaddr) : nullptr) {
            uint32_t w;
            std::memcpy(&w, p, 4);
            return w;
        }
        if (vaddr >= kCode && vaddr - kCode + 4 <= code.size()) {
            uint32_t w;
            std::memcpy(&w, &code[vaddr - kCode], 4);
            return w;
        }
        return 0xD4000001u;  // svc #0
    }
    uint64_t MemoryRead(uint64_t vaddr, size_t size) override {
        slow_reads++;
        uint64_t v = 0;
        for (size_t i = 0; i < size; i++) {
            uint8_t* p = Host(vaddr + i);
            v |= uint64_t(p ? *p : 0) << (8 * i);
        }
        return v;
    }
    Dynarmic::A64::Vector MemoryRead128(uint64_t vaddr) override { return {MemoryRead(vaddr, 8), MemoryRead(vaddr + 8, 8)}; }
    void MemoryWrite(uint64_t vaddr, uint64_t value, size_t size) override {
        slow_writes++;
        for (size_t i = 0; i < size; i++)
            if (uint8_t* p = Host(vaddr + i))
                *p = uint8_t(value >> (8 * i));
    }
    void MemoryWrite128(uint64_t vaddr, Dynarmic::A64::Vector value) override {
        MemoryWrite(vaddr, value[0], 8);
        MemoryWrite(vaddr + 8, value[1], 8);
    }
    template<typename T>
    bool WriteExclusive(uint64_t vaddr, T value, T expected) {
        slow_writes++;
        T current{};
        for (size_t i = 0; i < sizeof(T); i++) {
            uint8_t* p = Host(vaddr + i);
            current |= T(T(p ? *p : 0) << (8 * i));
        }
        if (current != expected)
            return false;
        for (size_t i = 0; i < sizeof(T); i++)
            if (uint8_t* p = Host(vaddr + i))
                *p = uint8_t(value >> (8 * i));
        return true;
    }
    bool MemoryWriteExclusive8(uint64_t a, uint8_t v, uint8_t e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive16(uint64_t a, uint16_t v, uint16_t e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive32(uint64_t a, uint32_t v, uint32_t e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive64(uint64_t a, uint64_t v, uint64_t e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive128(uint64_t a, Dynarmic::A64::Vector v, Dynarmic::A64::Vector e) override {
        if (MemoryRead(a, 8) != e[0] || MemoryRead(a + 8, 8) != e[1])
            return false;
        MemoryWrite128(a, v);
        return true;
    }
    void CallSVC(uint32_t) override { jit->HaltExecution(); }
    void ExceptionRaised(uint64_t pc, Dynarmic::A64::Exception e) override {
        std::fprintf(stderr, "exception %d at %llx\n", int(e), (unsigned long long)pc);
        std::exit(3);
    }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return ~uint64_t{0} >> 2; }
    uint64_t GetCNTPCT() override { return 0; }
};

std::vector<uint8_t> ReadFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::perror(path);
        std::exit(1);
    }
    std::vector<uint8_t> v;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        v.insert(v.end(), buf, buf + n);
    std::fclose(f);
    return v;
}

Dynarmic::ExclusiveMonitor g_monitor{1};

// fastmem: an arena of 2^28 bytes, none of it mapped: every data access (0x10000000 up) is past it,
// and takes the out-of-arena path (the page table walk, then the fallback). fastmem2: an arena of
// 2^32 with the data pages mapped at their guest addresses (but the marked and unmapped pages):
// fastmem proper, the faults sent to the fallback and the code recompiled without it.
bool g_fastmem = false, g_fastmem_mapped = false;
void* FastmemArena(Env& env) {
    static void* arena = nullptr;
    if (!arena) {
        const size_t size = size_t(1) << (g_fastmem_mapped ? 32 : 28);
        arena = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (g_fastmem_mapped) {
            for (size_t i = 0; i < kDataSize / 4096; i++) {
                const uint64_t vaddr = kData + i * 4096;
                if (vaddr == kUnmapped || vaddr == kMarked)
                    continue;
                const off_t offset = env.host_page[i] - env.data;
                if (mmap(static_cast<uint8_t*>(arena) + vaddr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                         env.memfd, offset) == MAP_FAILED)
                    std::exit(4);
            }
        }
    }
    return arena;
}

Dynarmic::A64::UserConfig MakeConfig(Env& env, bool excl, bool walk) {
    Dynarmic::A64::UserConfig config;
    config.callbacks = &env;
    config.global_monitor = &g_monitor;
    config.processor_id = 0;
    config.define_unpredictable_behaviour = true;
    config.page_table = reinterpret_cast<void**>(env.entries.data());
    config.page_table_address_space_bits = 32;
    config.page_table_pointer_mask = Common::PageTable::ATTRIBUTE_MASK;
    config.page_table_marked_bit = uint8_t(0);
    config.page_table_marked_bit_reads = false;
    config.silently_mirror_page_table = false;
    config.absolute_offset_page_table = true;
    config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
    config.only_detect_misalignment_via_page_table_on_page_boundary = true;
    if (!Common::PageTable::FULL_OFFSET)
        config.page_table_sign_extension = uint8_t(Common::PageTable::SIGN_BIT);
    config.wall_clock_cntpct = true;
    config.enable_cycle_counting = false;
#ifdef HAVE_NEW_OPTS
    config.jit_frame_pointer = !std::getenv("DYN_NOFRAME");
    config.link_forward_without_halt_check = std::getenv("DYN_FWD") != nullptr;
    config.inline_rsb_pop = std::getenv("DYN_RSB") != nullptr;
#ifdef HAVE_INLINE_DISPATCH
    config.inline_fast_dispatch = std::getenv("DYN_DISP") != nullptr;
#endif
#endif
    config.code_cache_size = 32u << 20;
    config.unsafe_optimizations = true;
    config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA | Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                            Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue | Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN |
                            Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    config.page_table_inline_exclusives = excl;
    config.page_table_shared_walk = walk;
    if (g_fastmem) {
        config.fastmem_pointer = reinterpret_cast<uintptr_t>(FastmemArena(env));
        config.fastmem_address_space_bits = g_fastmem_mapped ? 32 : 28;
        config.silently_mirror_fastmem = false;
        config.fastmem_exclusive_access = true;
        config.recompile_on_fastmem_failure = true;
        config.recompile_on_exclusive_fastmem_failure = true;
    }
    return config;
}

void Setup(Dynarmic::A64::Jit& jit) {
    for (size_t i = 0; i < 31; i++)
        jit.SetRegister(i, 0x0101010101010101ull * (i + 1));
    std::array<Dynarmic::A64::Vector, 32> v{};
    for (size_t i = 0; i < 32; i++)
        v[i] = {0x1111111111111111ull * (i + 1), 0x0f0f0f0f0f0f0f0full ^ (i * 0x9e3779b97f4a7c15ull)};
    jit.SetVectors(v);
    jit.SetSP(kData + kDataSize / 2);
    jit.SetPC(kCode);
    jit.SetPstate(0);
    jit.SetFpcr(0);
}
}  // namespace


// DUMP_JIT=<dir> (with PERF_BUILDID_DIR set): each recompiled block's code into <dir>/<name>.bin
// at the end of image mode, for objdump
void DumpJit() {
    const char* dir = std::getenv("DUMP_JIT");
    const char* perf = std::getenv("PERF_BUILDID_DIR");
    if (!dir || !perf)
        return;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/perf-%d.map", perf, int(getpid()));
    FILE* map = std::fopen(path, "r");
    if (!map)
        return;
    unsigned long long start, size;
    char name[256];
    while (std::fscanf(map, "%llx %llx %255s", &start, &size, name) == 3) {
        std::snprintf(path, sizeof(path), "%s/%s.bin", dir, name);
        if (FILE* out = std::fopen(path, "wb")) {
            std::fwrite(reinterpret_cast<const void*>(start), 1, size, out);
            std::fclose(out);
        }
    }
    std::fclose(map);
}

int main(int argc, char** argv) {
    if (argc < 3)
        return 1;
    const std::string mode = argv[1];
    Env env;
    env.code = ReadFile(argv[2]);
    bool excl = true, walk = true;
    for (int i = 3; i < argc; i++) {
        if (std::string(argv[i]) == "noexcl")
            excl = false;
        if (std::string(argv[i]) == "nowalk")
            walk = false;
        if (std::string(argv[i]) == "fastmem")
            g_fastmem = true;
        if (std::string(argv[i]) == "fastmem2")
            g_fastmem = g_fastmem_mapped = true;
    }
    auto config = MakeConfig(env, excl, walk);
    Dynarmic::A64::Jit jit{config};
    env.jit = &jit;
    if (mode == "image") {
        // image <program.bin> <frames> [options]: the program loaded at 0x10000000 and run from
        // there with x19 = frames; prints the time and the result (x0)
        const uint64_t frames = std::strtoull(argv[3], nullptr, 0);
        for (size_t i = 0; i < env.code.size(); i++)
            *env.Host(kData + i) = env.code[i];
        Setup(jit);
        jit.SetPC(kData);
        jit.SetRegister(19, frames);
        const auto t0 = std::chrono::steady_clock::now();
        jit.Run();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        DumpJit();
        std::printf("%.1f ms, %.1f us a frame, result %08llx (slow reads %llu, writes %llu)\n", s * 1e3, s * 1e6 / double(frames),
                    (unsigned long long)jit.GetRegisters()[0], (unsigned long long)env.slow_reads, (unsigned long long)env.slow_writes);
        return 0;
    }
    if (mode == "bench") {
        const long iterations = std::atol(argv[3]);
        Setup(jit);
        jit.SetRegister(19, uint64_t(iterations));
        jit.Run();
        Setup(jit);
        jit.SetRegister(19, uint64_t(iterations));
        env.slow_reads = env.slow_writes = 0;
        const auto t0 = std::chrono::steady_clock::now();
        jit.Run();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s, %s: %.1f ns an iteration (slow reads %llu, writes %llu)\n", excl ? "inline exclusives" : "monitor exclusives",
                    walk ? "shared walk" : "walk per access",
                    s * 1e9 / double(iterations), (unsigned long long)env.slow_reads, (unsigned long long)env.slow_writes);
        return 0;
    }
    Setup(jit);
    jit.Run();
    const auto regs = jit.GetRegisters();
    for (size_t i = 0; i < 31; i++)
        std::printf("x%zu=%016llx\n", i, (unsigned long long)regs[i]);
    const auto vecs = jit.GetVectors();
    for (size_t i = 0; i < 32; i++)
        std::printf("q%zu=%016llx%016llx\n", i, (unsigned long long)vecs[i][1], (unsigned long long)vecs[i][0]);
    std::printf("sp=%016llx\n", (unsigned long long)jit.GetSP());
    uint64_t h = 1469598103934665603ull;
    for (size_t p = 0; p < kDataSize / 4096; p++)
        for (size_t b = 0; b < 4096; b++)
            h = (h ^ env.host_page[p][b]) * 1099511628211ull;
    std::printf("memory=%016llx pstate=%08x pc=%llx\n", (unsigned long long)h, jit.GetPstate(), (unsigned long long)jit.GetPC());
    std::fprintf(stderr, "slow reads %llu writes %llu\n", (unsigned long long)env.slow_reads, (unsigned long long)env.slow_writes);
    return 0;
}
