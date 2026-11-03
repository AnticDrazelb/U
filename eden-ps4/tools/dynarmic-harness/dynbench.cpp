// Host harness for Eden-for-PS4's dynarmic: A32 code run with the PS4's page table configuration
// (absolute offsets, Eden's packed entries, sign extension from bit 55, misalignment checks on page
// boundaries, no fastmem, Unsafe optimisations).
//   dynbench bench <code.bin> <iterations> [options]   time a program
//   dynbench test  <code.bin> [arm|thumb] [options]     run once, print registers and a memory hash
// options: nosingle (a walk per access of LDM and the like), noexcl (exclusives through the monitor
// and callbacks)
// Guest map: code at 0x00010000, data 0x10000000-0x11000000 (16 MiB) in shuffled host pages, the
// page at 0x10800000 marked (writes take the slow path), 0x10900000 unmapped.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <random>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

#include "common/page_table.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/optimization_flags.h"

// (04.17) cachetest: allocations from an arena that can be unmapped, so that a saved code cache's
// pointer left unrelocated (into the first run's objects) faults in the second run instead of
// finding the same object at the same address
namespace {
struct Arena {
    uint8_t* base = nullptr;
    size_t size = 0, used = 0;
};
Arena* g_arena = nullptr;
Arena g_arenas[2];
void* ArenaAlloc(size_t size, size_t align) {
    if (!g_arena) {
        void* p = nullptr;
        if (posix_memalign(&p, std::max<size_t>(align, sizeof(void*)), size ? size : 1) != 0)
            throw std::bad_alloc{};
        return p;
    }
    size_t offset = (g_arena->used + align - 1) & ~(align - 1);
    if (offset + size > g_arena->size)
        throw std::bad_alloc{};
    g_arena->used = offset + size;
    return g_arena->base + offset;
}
// every arena ever used, unmapped ones too (what's freed after its arena went is not malloc's)
std::pair<uint8_t*, size_t> g_arena_ranges[8];
size_t g_arena_count = 0;
bool InArena(void* p) {
    for (size_t i = 0; i < g_arena_count; i++)
        if (static_cast<uint8_t*>(p) >= g_arena_ranges[i].first && static_cast<uint8_t*>(p) < g_arena_ranges[i].first + g_arena_ranges[i].second)
            return true;
    return false;
}
void NoteArena(const Arena& a) {
    if (g_arena_count < 8)
        g_arena_ranges[g_arena_count++] = {a.base, a.size};
}
}  // namespace
void* operator new(size_t size) { return ArenaAlloc(size, 16); }
void* operator new[](size_t size) { return ArenaAlloc(size, 16); }
void* operator new(size_t size, std::align_val_t align) { return ArenaAlloc(size, size_t(align)); }
void* operator new[](size_t size, std::align_val_t align) { return ArenaAlloc(size, size_t(align)); }
void operator delete(void* p) noexcept { if (p && !InArena(p)) free(p); }
void operator delete[](void* p) noexcept { if (p && !InArena(p)) free(p); }
void operator delete(void* p, size_t) noexcept { if (p && !InArena(p)) free(p); }
void operator delete[](void* p, size_t) noexcept { if (p && !InArena(p)) free(p); }
void operator delete(void* p, std::align_val_t) noexcept { if (p && !InArena(p)) free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { if (p && !InArena(p)) free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { if (p && !InArena(p)) free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { if (p && !InArena(p)) free(p); }

namespace {
constexpr uint32_t kCode = 0x00010000, kData = 0x10000000, kDataSize = 16u << 20;
constexpr uint32_t kMarked = 0x10800000, kUnmapped = 0x10900000;
// splitstack: the stack's pages (the 256 KiB below the initial sp and the page above) left out of the
// fastmem arena, as the PS4's thread stacks it can't map at their addresses: fastmem declined there
bool g_split_stack = false;
constexpr size_t kPages = size_t{1} << 20;

struct Env final : Dynarmic::A32::UserCallbacks {
    std::vector<uint8_t> code;
    int memfd = -1;                        // the data's memory, mappable twice (fastmem)
    uint8_t* data = nullptr;               // page-aligned
    std::vector<uint8_t*> host_page;       // guest data page -> host page
    std::vector<uint64_t> entries = std::vector<uint64_t>(kPages, 0);
    uint64_t slow_reads = 0, slow_writes = 0;
    Dynarmic::A32::Jit* jit = nullptr;
    void* fastmem_arena = nullptr;
    uint64_t cntpct_reads = 0;  // (04.17, TestCP15's CNTPCT)

    Env() {
        memfd = memfd_create("guest", 0);
        if (ftruncate(memfd, kDataSize) != 0)
            std::exit(2);
        data = static_cast<uint8_t*>(mmap(nullptr, kDataSize, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0));
        std::memset(data, 0, kDataSize);
        const size_t n = kDataSize / 4096;
        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; i++)
            order[i] = i;
        std::shuffle(order.begin(), order.end(), std::mt19937{1234});
        host_page.resize(n);
        for (size_t i = 0; i < n; i++) {
            host_page[i] = data + order[i] * 4096;
            // fill with a pattern so loads of untouched memory are checked too
            for (size_t b = 0; b < 4096; b++)
                host_page[i][b] = uint8_t(i * 131 + b * 7);
        }
        for (size_t i = 0; i < n; i++) {
            const uint32_t vaddr = kData + uint32_t(i * 4096);
            if (vaddr == kUnmapped)
                continue;
            auto& e = *reinterpret_cast<Common::PageTable::PageEntryData*>(&entries[vaddr >> 12]);
            e.Store(false, Common::PageType::Memory, 0, reinterpret_cast<uintptr_t>(host_page[i]) - vaddr);
            if (vaddr == kMarked)
                e.MarkRasterizerCached();
        }
    }
    uint8_t* Host(uint32_t vaddr) {
        if (vaddr < kData || vaddr >= kData + kDataSize || (vaddr & ~4095u) == kUnmapped)
            return nullptr;
        return host_page[(vaddr - kData) >> 12] + (vaddr & 4095);
    }
    std::optional<std::uint32_t> MemoryReadCode(uint32_t vaddr) override {
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
        return 0xE7F000F0u;  // udf
    }
    uint64_t MemoryRead(uint32_t vaddr, size_t size) override {
        slow_reads++;
        uint64_t v = 0;
        for (size_t i = 0; i < size; i++) {
            uint8_t* p = Host(vaddr + uint32_t(i));
            v |= uint64_t(p ? *p : 0) << (8 * i);
        }
        return v;
    }
    void MemoryWrite(uint32_t vaddr, uint64_t value, size_t size) override {
        slow_writes++;
        for (size_t i = 0; i < size; i++)
            if (uint8_t* p = Host(vaddr + uint32_t(i)))
                *p = uint8_t(value >> (8 * i));
    }
    template<typename T>
    bool WriteExclusive(uint32_t vaddr, T value, T expected) {
        slow_writes++;
        T current{};
        for (size_t i = 0; i < sizeof(T); i++) {
            uint8_t* p = Host(vaddr + uint32_t(i));
            current |= T(T(p ? *p : 0) << (8 * i));
        }
        if (current != expected)
            return false;
        for (size_t i = 0; i < sizeof(T); i++)
            if (uint8_t* p = Host(vaddr + uint32_t(i)))
                *p = uint8_t(value >> (8 * i));
        return true;
    }
    bool MemoryWriteExclusive8(uint32_t a, uint8_t v, uint8_t e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive16(uint32_t a, uint16_t v, uint16_t e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive32(uint32_t a, uint32_t v, uint32_t e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive64(uint32_t a, uint64_t v, uint64_t e) override { return WriteExclusive(a, v, e); }
    void CallSVC(uint32_t) override { jit->HaltExecution(); }
    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception e) override {
        std::fprintf(stderr, "exception %d at %08x\n", int(e), pc);
        std::exit(3);
    }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return ~uint64_t{0} >> 2; }
};

// (04.17) A coprocessor as Eden's CP15: TPIDRURW/TPIDRURO read and written through pointers to its
// fields, CNTPCT read through a callback whose argument is its parent (here the environment), a
// barrier through a callback without one
struct TestCP15 final : Dynarmic::A32::Coprocessor {
    using CoprocReg = Dynarmic::A32::CoprocReg;
    explicit TestCP15(Env& parent_) : parent{parent_} {}
    Env& parent;
    uint32_t uprw = 0x11223344, uro = 0x55667788;
    uint64_t ticks = 1000;
    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override { return std::nullopt; }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        if (!two && CRn == CoprocReg::C13 && opc1 == 0 && CRm == CoprocReg::C0 && opc2 == 2)
            return &uprw;
        if (!two && CRn == CoprocReg::C7 && opc1 == 0 && CRm == CoprocReg::C10 && opc2 == 5)
            return Callback{[](void*, std::uint32_t, std::uint32_t) -> std::uint64_t { return 0; }, std::nullopt};
        return {};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override { return {}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        if (!two && CRn == CoprocReg::C13 && opc1 == 0 && CRm == CoprocReg::C0)
            return opc2 == 2 ? CallbackOrAccessOneWord{&uprw} : opc2 == 3 ? CallbackOrAccessOneWord{&uro} : CallbackOrAccessOneWord{};
        return {};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) override;
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }
};
TestCP15::CallbackOrAccessTwoWords TestCP15::CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) {
    if (!two && opc == 0 && CRm == CoprocReg::C14) {
        return Callback{[](void* arg, std::uint32_t, std::uint32_t) -> std::uint64_t {
            Env& env = *static_cast<Env*>(arg);
            return 0x100000000ull * (env.cntpct_reads & 3) + 1000 + 7 * env.cntpct_reads++;
        }, &parent};
    }
    return {};
}

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

struct Options {
    bool thumb = false, single = true, excl = true, fastmem = false;
};
Options ParseOptions(int argc, char** argv, int first) {
    Options o;
    for (int i = first; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "thumb")
            o.thumb = true;
        else if (a == "nosingle")
            o.single = false;
        else if (a == "noexcl")
            o.excl = false;
        else if (a == "fastmem")
            o.fastmem = true;
        else if (a == "splitstack")
            o.fastmem = true, g_split_stack = true;
    }
    return o;
}

Dynarmic::ExclusiveMonitor g_monitor{1};

// fastmem: a 4 GiB arena with the data pages mapped at their guest addresses (but the marked and
// unmapped pages, which fault: the fallback, and the code recompiled without fastmem there)
void* FastmemArena(Env& env) {
    void*& arena = env.fastmem_arena;
    if (!arena) {
        // (4 GiB and 64 KiB past it reserved, as the PS4's 2^39 arena has room past 4 GiB: an address
        // folded into a displacement (fold_address_offsets) can pass 4 GiB there, and faults)
        arena = mmap(nullptr, (size_t(1) << 32) + 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        for (size_t i = 0; i < kDataSize / 4096; i++) {
            const uint32_t vaddr = kData + uint32_t(i * 4096);
            if (vaddr == kUnmapped || vaddr == kMarked)
                continue;
            const uint32_t sp = kData + kDataSize / 2;
            if (g_split_stack && vaddr >= sp - (256u << 10) && vaddr < sp + 4096)
                continue;
            if (mmap(static_cast<uint8_t*>(arena) + vaddr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, env.memfd,
                     env.host_page[i] - env.data) == MAP_FAILED)
                std::exit(4);
        }
    }
    return arena;
}

Dynarmic::A32::UserConfig MakeConfig(Env& env, const Options& o) {
    Dynarmic::A32::UserConfig config;
    config.global_monitor = &g_monitor;
    config.processor_id = 0;
    config.callbacks = &env;
    config.define_unpredictable_behaviour = true;
    config.page_table = reinterpret_cast<std::array<uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>*>(env.entries.data());
    config.page_table_pointer_mask = Common::PageTable::ATTRIBUTE_MASK;
    config.page_table_marked_bit = uint8_t(0);
    config.page_table_marked_bit_reads = false;
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
#ifdef HAVE_INLINE_COND
    config.inline_conditional_instructions = std::getenv("DYN_COND") != nullptr;
#endif
#ifdef HAVE_DECLINED_WALK
    config.fastmem_declined_shared_walk = std::getenv("DYN_NOSHARE") == nullptr;
#endif
#ifdef HAVE_DEAD_NZCV
    config.elide_dead_nzcv = std::getenv("DYN_NZCV") != nullptr;
#ifdef HAVE_DEAD_NZCV_ABI
    config.elide_dead_nzcv_abi = std::getenv("DYN_NZCV_ABI") != nullptr;
#endif
#ifdef HAVE_FOLD
    config.fold_address_offsets = std::getenv("DYN_FOLD") != nullptr;
    config.skip_redundant_extensions = std::getenv("DYN_EXT") != nullptr;
#endif
#ifdef HAVE_COLD
    config.split_cold_code = std::getenv("DYN_COLD") != nullptr;
#endif
#endif
#endif
    config.code_cache_size = 32u << 20;
    config.unsafe_optimizations = true;
    config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA | Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                            Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue | Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN |
                            Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
#ifdef HAVE_SINGLE_WALK
    config.page_table_single_walk_multiple = o.single;
#endif
#ifdef HAVE_INLINE_EXCLUSIVES
    config.page_table_inline_exclusives = o.excl;
#endif
    if (o.fastmem) {
        config.fastmem_pointer = reinterpret_cast<uintptr_t>(FastmemArena(env));
        config.fastmem_exclusive_access = true;
        config.recompile_on_fastmem_failure = true;
        config.recompile_on_exclusive_fastmem_failure = true;
    }
    return config;
}

void Setup(Dynarmic::A32::Jit& jit, bool thumb) {
    for (int i = 0; i < 16; i++)
        jit.Regs()[i] = 0x01010101u * uint32_t(i + 1);
    jit.Regs()[13] = kData + kDataSize / 2;  // sp: middle of the data
    jit.Regs()[15] = kCode | (thumb ? 1 : 0);
    jit.SetCpsr(thumb ? 0x000001F0 : 0x000001D0);
    jit.SetFpscr(0x03000000);
    for (size_t i = 0; i < 64; i++)
        jit.ExtRegs()[i] = 0x10000000u + uint32_t(i) * 0x01000193u;
    if (thumb)
        jit.Regs()[15] = kCode;
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
    // (a Thumb program's last halfword may end a word: pad with svc #0 in both encodings)
    while (env.code.size() % 4)
        env.code.push_back(env.code.size() % 2 ? 0xDF : 0x00);
    for (int i = 0; i < 2; i++)
        for (uint8_t b : {0x00, 0xDF, 0x00, 0xDF})
            env.code.push_back(b);
    if (mode == "image") {
        // image <program.bin> <frames> [arm|thumb] [options]: the program loaded at 0x10000000 and
        // run from there with r0 = frames; prints the time and the result (r0)
        const uint32_t frames = uint32_t(std::strtoul(argv[3], nullptr, 0));
        const Options o = ParseOptions(argc, argv, 4);
        for (size_t i = 0; i < env.code.size(); i++)
            *env.Host(kData + uint32_t(i)) = env.code[i];
        auto config = MakeConfig(env, o);
        Dynarmic::A32::Jit jit{config};
        env.jit = &jit;
        Setup(jit, o.thumb);
        jit.Regs()[15] = kData;
        jit.Regs()[0] = frames;
        const auto t0 = std::chrono::steady_clock::now();
        jit.Run();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        DumpJit();
        std::printf("%.1f ms, %.1f us a frame, result %08x (slow reads %llu, writes %llu)\n", s * 1e3, s * 1e6 / double(frames),
                    jit.Regs()[0], (unsigned long long)env.slow_reads, (unsigned long long)env.slow_writes);
        return 0;
    }
    if (mode == "bench") {
        const long iterations = argc > 3 ? std::atol(argv[3]) : 10000000;
        const Options o = ParseOptions(argc, argv, 4);
        auto config = MakeConfig(env, o);
        Dynarmic::A32::Jit jit{config};
        env.jit = &jit;
        Setup(jit, false);
        jit.Regs()[0] = uint32_t(iterations);
        jit.Run();  // warm (compile)
        Setup(jit, false);
        jit.Regs()[0] = uint32_t(iterations);
        env.slow_reads = env.slow_writes = 0;
        const auto t0 = std::chrono::steady_clock::now();
        jit.Run();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        DumpJit();
        std::printf("%s%s: %.3f s, %.1f ns an iteration (slow reads %llu, writes %llu)\n", o.single ? "single walk" : "per access",
                    o.excl ? ", inline exclusives" : ", monitor exclusives",
                    s, s * 1e9 / double(iterations), (unsigned long long)env.slow_reads, (unsigned long long)env.slow_writes);
        return 0;
    }
    if (mode == "cachetest") {
        // cachetest <code.bin> [arm|thumb] [options] [twice] [cp15]: (04.17) the program run and its
        // code cache saved, everything of that run then unmapped; the cache loaded in a second run with
        // all its objects elsewhere (the JIT, the callbacks, the monitor, the coprocessor), the
        // program run again (twice: that run's cache saved and loaded in a third). Prints what test
        // prints, for the last run. CACHETEST_B=<other.bin>: the last run runs that program instead
        // (its blocks not the saved ones: they must be found stale).
        const Options o = ParseOptions(argc, argv, 3);
        bool twice = false, cp15 = false;
        for (int i = 3; i < argc; i++) {
            twice |= std::string(argv[i]) == "twice";
            cp15 |= std::string(argv[i]) == "cp15";
        }
        const int runs = twice ? 3 : 2;
        for (Arena& a : g_arenas) {
            a.size = size_t(256) << 20;
            a.base = static_cast<uint8_t*>(mmap(nullptr, a.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
            NoteArena(a);
        }
        std::vector<uint8_t> program = env.code;
        std::vector<uint8_t> last_program = program;
        if (const char* other = std::getenv("CACHETEST_B")) {
            last_program = ReadFile(other);
            while (last_program.size() % 4)
                last_program.push_back(last_program.size() % 2 ? 0xDF : 0x00);
            for (int i = 0; i < 2; i++)
                for (uint8_t b : {0x00, 0xDF, 0x00, 0xDF})
                    last_program.push_back(b);
        }
        {
            // (one JIT made and gone first, the program run: statics it sets up (the code allocator's
            // map, the translator's decode tables) live outside the arenas)
            auto config = MakeConfig(env, o);
            if (cp15)
                config.coprocessors[15] = std::make_shared<TestCP15>(env);
            Dynarmic::A32::Jit warm{config};
            env.jit = &warm;
            Setup(warm, o.thumb);
            warm.Run();
            env.jit = nullptr;
        }
        std::vector<uint8_t> saved;
        saved.reserve(size_t(64) << 20);
        for (int run = 0; run < runs; run++) {
            const bool last = run == runs - 1;
            Arena& arena = g_arenas[run % 2];
            if (!arena.base) {
                arena.base = static_cast<uint8_t*>(mmap(nullptr, arena.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
                arena.used = 0;
                NoteArena(arena);
            }
            g_arena = &arena;
            void* spacer = ::operator new(size_t(1 + run) << 20);  // (each run's objects at other offsets)
            (void)spacer;
            Env* e = new Env;
            e->code = last ? last_program : program;
            auto* monitor = new Dynarmic::ExclusiveMonitor{1};
            auto config = MakeConfig(*e, o);
            config.global_monitor = monitor;
            std::shared_ptr<TestCP15> coprocessor;
            if (cp15) {
                coprocessor = std::make_shared<TestCP15>(*e);
                config.coprocessors[15] = coprocessor;
            }
            config.code_cache_objects = {{e, sizeof(Env)}};
            if (coprocessor)
                config.code_cache_objects.emplace_back(coprocessor.get(), sizeof(TestCP15));
            auto* jit = new Dynarmic::A32::Jit{config};
            e->jit = jit;
            if (run > 0) {
                size_t offset = 0;
                std::string why;
                const bool loaded = jit->LoadCodeCache([&](void* data, size_t size) {
                    if (offset + size > saved.size())
                        return false;
                    std::memcpy(data, saved.data() + offset, size);
                    offset += size;
                    return true;
                }, &why);
                if (!loaded) {
                    std::fprintf(stderr, "run %d: not loaded: %s\n", run, why.c_str());
                    return 6;
                }
            }
            Setup(*jit, o.thumb);
            jit->Run();
            const auto use = jit->GetCodeCacheUse();
            if (run > 0)
                std::fprintf(stderr, "run %d: cache %zu bytes, %zu blocks loaded, %zu used, %zu stale\n", run, saved.size(), use.loaded, use.used, use.stale);
            if (last) {
                for (int i = 0; i < 16; i++)
                    std::printf("r%d=%08x\n", i, jit->Regs()[i]);
                for (size_t i = 0; i < 64; i++)
                    std::printf("s%zu=%08x\n", i, jit->ExtRegs()[i]);
                uint64_t h = 1469598103934665603ull;
                for (size_t p = 0; p < kDataSize / 4096; p++)
                    for (size_t q = 0; q < 4096; q++)
                        h = (h ^ e->host_page[p][q]) * 1099511628211ull;
                std::printf("memory=%016llx cpsr=%08x\n", (unsigned long long)h, jit->Cpsr());
                return 0;
            }
            g_arena = nullptr;
            saved.clear();
            const bool ok = jit->SaveCodeCache([&](const void* data, size_t size) {
                saved.insert(saved.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
                return true;
            });
            if (!ok) {
                std::fprintf(stderr, "run %d: save failed\n", run);
                return 5;
            }
            g_arena = &arena;
            delete jit;  // (its code cache unmapped: the next run's lands at the same place)
            coprocessor.reset();  // (shared pointers into the arena let go before it goes)
            config.coprocessors = {};
            if (e->fastmem_arena)
                munmap(e->fastmem_arena, (size_t(1) << 32) + 0x10000);
            munmap(e->data, kDataSize);
            g_arena = nullptr;
            munmap(arena.base, arena.size);  // this run's objects: gone
            arena.base = nullptr;
        }
        return 0;
    }
    // test: run once, print registers, ext registers and a hash of the data
    const Options o = ParseOptions(argc, argv, 3);
    const bool thumb = o.thumb;
    auto config = MakeConfig(env, o);
    for (int i = 3; i < argc; i++)
        if (std::string(argv[i]) == "cp15")
            config.coprocessors[15] = std::make_shared<TestCP15>(env);
    Dynarmic::A32::Jit jit{config};
    env.jit = &jit;
    Setup(jit, thumb);
    jit.Run();
    DumpJit();
    for (int i = 0; i < 16; i++)
        std::printf("r%d=%08x\n", i, jit.Regs()[i]);
    for (size_t i = 0; i < 64; i++)
        std::printf("s%zu=%08x\n", i, jit.ExtRegs()[i]);
    uint64_t h = 1469598103934665603ull;
    for (size_t p = 0; p < kDataSize / 4096; p++)
        for (size_t b = 0; b < 4096; b++)
            h = (h ^ env.host_page[p][b]) * 1099511628211ull;
    std::printf("memory=%016llx cpsr=%08x\n", (unsigned long long)h, jit.Cpsr());
    std::fprintf(stderr, "slow reads %llu writes %llu\n", (unsigned long long)env.slow_reads, (unsigned long long)env.slow_writes);
    return 0;
}
