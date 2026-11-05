// The 64-bit translator against itself: random blocks of A64 code run twice, with the IR passes
// that broke Little Kitty, Big City on the PS4 (get/set elimination, constant propagation; 04.66)
// and without them, from the same state; any difference in the registers, flags or memory written
// is a mistranslation. Run it with DYNARMIC_HOST_FEATURES_MASK=16b00f for the code the PS4's
// Jaguar gets (no BMI2, AVX2, FMA, AVX-512).
//   jit-difftest [iterations] [seed] [int|all] [only-getset|only-constprop]

#include <algorithm>
#include <bit>
#include <chrono>
#include <csetjmp>
#include <exception>
#include <sys/mman.h>
#include <sys/time.h>
#include <ucontext.h>
#include <sys/wait.h>
#include <csignal>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <vector>

#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/tests/A64/testenv.h"
#include "dynarmic/tests/fuzz_util.h"
#include <fmt/format.h>

using namespace Dynarmic;

extern bool g_assert_throws, g_assert_hit;
std::vector<u32> g_current_block;  // (printed if the emitter terminates the test)
volatile int crash_stage = 0;  // (a forked state's runner: its exit code if the JIT aborts)
extern std::jmp_buf* g_assert_jump;

namespace {

std::mt19937_64 rng;

u64 Rand(u64 lo, u64 hi) {
    return std::uniform_int_distribution<u64>(lo, hi)(rng);
}

// The guest's memory as the PS4 has it: a window (a heap and a stack) the fastmem arena maps but
// for every fourth page (left out: an access there faults and is recompiled to walk the page
// table, as the PS4's declined pages are), the page table (the PS4's full-offset entries) mapping
// all of it, and the callbacks reading and writing the same memory; outside it, A64TestEnv's
constexpr u64 kWindow = 0x1180000000, kWindowSize = 2 << 20, kArenaBits = 37;
constexpr u64 kPage = 4096;

class Env final : public A64TestEnv {
public:
    bool raised = false;
    u8* backing = nullptr;  // the window's memory (a memfd, also mapped into the arena)
    u8* arena = nullptr;
    u64* page_table = nullptr;
    int fd = -1;

    void Init() {
        fd = memfd_create("window", 0);
        if (fd < 0 || ftruncate(fd, kWindowSize) != 0)
            std::abort();
        backing = static_cast<u8*>(mmap(nullptr, kWindowSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        arena = static_cast<u8*>(mmap(nullptr, u64(1) << kArenaBits, PROT_NONE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
        page_table = static_cast<u64*>(mmap(nullptr, (u64(1) << (kArenaBits - 12)) * sizeof(u64),
                                            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                            -1, 0));
        if (backing == MAP_FAILED || arena == MAP_FAILED || page_table == MAP_FAILED)
            std::abort();
        for (u64 i = 0; i < kWindowSize / kPage; i++) {
            const u64 va = kWindow + i * kPage;
            if (i % 4 != 3 &&
                mmap(arena + va, kPage, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, off_t(i * kPage)) ==
                    MAP_FAILED)
                std::abort();
            // (absolute offset: host = entry + va; the low 12 bits the attributes, 0 here)
            page_table[va >> 12] = u64(reinterpret_cast<uintptr_t>(backing + i * kPage)) - va;
        }
    }
    static bool InWindow(u64 va) { return va >= kWindow && va < kWindow + kWindowSize; }
    void Reset() {
        for (u64 i = 0; i < kWindowSize; i++)
            backing[i] = u8((kWindow + i) * 7 >> 3);
        modified_memory.clear();
        raised = false;
    }

    u64 window_callbacks = 0;  // accesses to the window through the callbacks (the fallbacks)
    bool trace = false;        // (DIFFTEST_TRACE: each callback access printed)
    u64 MemoryRead(u64 vaddr, size_t size) override {
        if (trace && size > 1)
            std::printf("      read%zu %#llx\n", size * 8, (unsigned long long)vaddr);
        if (InWindow(vaddr) && size > 1)
            window_callbacks++;
        if (size == 1)
            return InWindow(vaddr) ? backing[vaddr - kWindow] : A64TestEnv::MemoryRead(vaddr, 1);
        u64 value = 0;
        for (size_t i = 0; i < size; i++)
            value |= MemoryRead(vaddr + i, 1) << (8 * i);
        return value;
    }
    void MemoryWrite(u64 vaddr, u64 value, size_t size) override {
        if (trace && size > 1)
            std::printf("      write%zu %#llx = %#llx\n", size * 8, (unsigned long long)vaddr, (unsigned long long)value);
        if (InWindow(vaddr) && size > 1)
            window_callbacks++;
        for (size_t i = 0; i < size; i++) {
            const u64 a = vaddr + i;
            const u8 b = u8(value >> (8 * i));
            if (InWindow(a))
                backing[a - kWindow] = b;
            else
                A64TestEnv::MemoryWrite(a, b, 1);
        }
    }
    Vector MemoryRead128(u64 a) override { return {MemoryRead(a, 8), MemoryRead(a + 8, 8)}; }
    void MemoryWrite128(u64 a, Vector v) override { MemoryWrite(a, v[0], 8); MemoryWrite(a + 8, v[1], 8); }
    // (an exclusive store succeeds only if the memory still holds what was read, as Eden's)
    bool MemoryWriteExclusive8(u64 a, std::uint8_t v, std::uint8_t e) override { return Cas(a, v, e, 1); }
    bool MemoryWriteExclusive16(u64 a, std::uint16_t v, std::uint16_t e) override { return Cas(a, v, e, 2); }
    bool MemoryWriteExclusive32(u64 a, std::uint32_t v, std::uint32_t e) override { return Cas(a, v, e, 4); }
    bool MemoryWriteExclusive64(u64 a, std::uint64_t v, std::uint64_t e) override { return Cas(a, v, e, 8); }
    bool MemoryWriteExclusive128(u64 a, Vector v, Vector e) override {
        if (MemoryRead(a, 8) != e[0] || MemoryRead(a + 8, 8) != e[1])
            return false;
        MemoryWrite128(a, v);
        return true;
    }
    bool Cas(u64 a, u64 v, u64 e, size_t size) {
        if (MemoryRead(a, size) != e)
            return false;
        MemoryWrite(a, v, size);
        return true;
    }
    void CallSVC(std::uint32_t) override { raised = true; ticks_left = 0; }
    void ExceptionRaised(u64, Dynarmic::A64::Exception) override { raised = true; ticks_left = 0; }
};

bool Testable(u32 instruction, u64 pc, bool last) {
    const A64::LocationDescriptor location{pc, {}};
    IR::Block block{location};
    bool goes_on = false;
    // (an instruction the translator asserts on, as ARMv8.2's FP16 forms the Switch's A57 lacks: skipped)
    std::jmp_buf jump;
    g_assert_hit = false;
    g_assert_jump = &jump;
    g_assert_throws = true;
    if (setjmp(jump) != 0) {
        g_assert_throws = false;
        return false;
    }
    goes_on = A64::TranslateSingleInstruction(block, location, instruction);
    g_assert_throws = false;
    if (g_assert_hit)
        return false;
    if (!goes_on && !last)
        return false;
    for (const auto& inst : block.instructions) {
        switch (inst.GetOpcode()) {
        case IR::Opcode::A64ExceptionRaised:
        case IR::Opcode::A64CallSupervisor:
        case IR::Opcode::A64DataCacheOperationRaised:
        case IR::Opcode::A64InstructionCacheOperationRaised:
        case IR::Opcode::A64GetCNTPCT:
            return false;
        default:
            break;
        }
    }
    return true;
}

struct Generators {
    std::vector<InstructionGenerator> list;
    std::vector<std::string> names;
};

Generators MakeGenerators(bool integer_only) {
    const std::vector<std::pair<std::string, const char*>> all{
#define INST(fn, name, bitstring) {#fn, bitstring},
#include "dynarmic/frontend/A64/decoder/a64.inc"
#undef INST
    };
    // the integer, pointer and memory instructions (what a heap's code is made of), branches
    static const std::regex integer(
        "^(REV32_int|RBIT_int|REV16_int|CLZ_int|CLS_int|REV|SMULH|UMULH|UDIV|SDIV|LSLV|LSRV|ASRV|RORV|ADC|ADCS|"
        "SBC|SBCS|CCMN_reg|CCMP_reg|CCMN_imm|CCMP_imm|CSEL|CSINC|CSINV|CSNEG|SMADDL|SMSUBL|UMADDL|UMSUBL|MADD|"
        "MSUB|ADD_ext|ADDS_ext|SUB_ext|SUBS_ext|EXTR|B_cond|AND_imm|ORR_imm|EOR_imm|ANDS_imm|MOVN|MOVZ|MOVK|SBFM|"
        "BFM|UBFM|STNP_LDNP_gen|AND_shift|BIC_shift|ORR_shift|ORN_shift|EOR_shift|EON|ANDS_shift|BICS|ADD_shift|"
        "ADDS_shift|SUB_shift|SUBS_shift|ADD_imm|ADDS_imm|SUB_imm|SUBS_imm|CBZ|CBNZ|TBZ|TBNZ|ADR|ADRP|STP_LDP_gen|"
        "STRx_reg|LDRx_reg|STURx_LDURx|STRx_LDRx_imm_1|STRx_LDRx_imm_2|LDR_lit_gen|LDRSW_lit|SXTB_1|SXTB_2|SXTH_1|"
        "SXTH_2|SXTW|RMIF|SETF8|SETF16|CRC32|LDAR|STLR|LDTR|STTR|B_uncond|BL|BR|BLR|RET|"
        "LDXR|STXR|LDAXR|STLXR|LDXP|STXP|LDAXP|STLXP|CLREX|"
        // (the vector loads, stores and moves memcpy and memset are made of)
        "STP_LDP_fpsimd|STR_imm_fpsimd_1|LDR_imm_fpsimd_1|STR_imm_fpsimd_2|LDR_imm_fpsimd_2|STUR_fpsimd|"
        "LDUR_fpsimd|STR_reg_fpsimd|LDR_reg_fpsimd|LDR_lit_fpsimd|STNP_LDNP_fpsimd|FMOV_float_gen|UMOV|SMOV|"
        "INS_gen|INS_elt|DUP_gen|DUP_elt_1|DUP_elt_2|MOVI|ORR_asimd_reg|AND_asimd|EOR_asimd|BIC_asimd_reg|"
        "ORN_asimd|EXT|ADD_vector|SUB_vector|CMEQ_reg_2|CMEQ_zero_2|UMAXV|UMINV|ADDV|ADDP_vec|XTN|UZP1|UZP2|"
        "ZIP1|ZIP2|TRN1|TRN2|LD1R_1|LD1R_2|LD1_sngl_1|ST1_sngl_1|LD1_sngl_2|ST1_sngl_2|CMEQ_reg_1|ADDP_pair|"
        "STx_mult_1|STx_mult_2|LDx_mult_1|LDx_mult_2)$");
    static const std::vector<std::string> never{"UnallocatedEncoding", "STLLR", "LDLAR", "MSR_reg", "MSR_imm",
                                                "MRS", "SVC", "BRK", "HLT", "WFI", "WFE", "YIELD", "SEV", "SEVL",
                                                // (ARMv8.2's crypto, which the Switch's A57 lacks)
                                                "SM3SS1", "SM3TT1A", "SM3TT1B", "SM3TT2A", "SM3TT2B",
                                                "SM3PARTW1", "SM3PARTW2", "SM4E", "SM4EKEY", "SHA512H",
                                                "SHA512H2", "SHA512SU0", "SHA512SU1", "EOR3", "BCAX", "RAX1",
                                                "XAR"};
    Generators g;
    for (const auto& [fn, bits] : all) {
        if (std::find(never.begin(), never.end(), fn) != never.end())
            continue;
        if (integer_only && !std::regex_match(fn, integer))
            continue;
        g.list.emplace_back(bits);
        g.names.push_back(fn);
    }
    return g;
}

u32 RandomInstruction(const Generators& g, u64 pc, bool last) {
    while (true) {
        const u32 inst = g.list[Rand(0, g.list.size() - 1)].Generate();
        if (Testable(inst, pc, last))
            return inst;
    }
}

// as the PS4's CPU Auto (arm_dynarmic_64.cpp): the safe optimisations, Auto's two unsafe ones, the
// PS4's dispatch extras
A64::UserConfig Config(Env& env, ExclusiveMonitor& monitor, OptimizationFlag remove) {
    A64::UserConfig c{};
    c.callbacks = &env;
    // memory as arm_dynarmic_64.cpp sets it up on the PS4
    c.page_table = reinterpret_cast<void**>(env.page_table);
    c.page_table_address_space_bits = kArenaBits;
    c.page_table_pointer_mask = ~u64{0xFFF};
    c.page_table_marked_bit = u8(0);
    c.page_table_marked_bit_reads = false;
    c.silently_mirror_page_table = false;
    c.absolute_offset_page_table = true;
    c.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
    c.only_detect_misalignment_via_page_table_on_page_boundary = true;
    c.fastmem_pointer = reinterpret_cast<uintptr_t>(env.arena);
    c.fastmem_address_space_bits = kArenaBits;
    c.silently_mirror_fastmem = false;
    c.fastmem_exclusive_access = true;
    c.recompile_on_exclusive_fastmem_failure = true;
    c.global_monitor = &monitor;
    c.processor_id = 0;
    c.optimizations = all_safe_optimizations | OptimizationFlag::Unsafe_UnfuseFMA |
                      OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    c.optimizations &= ~remove;
    c.unsafe_optimizations = true;
    c.define_unpredictable_behaviour = true;
    c.dczid_el0 = 4;
    c.ctr_el0 = 0x8444c004;
    c.cntfrq_el0 = 19200000;
    c.jit_frame_pointer = false;
    c.link_forward_without_halt_check = true;
    c.inline_rsb_pop = true;
    c.inline_fast_dispatch = true;
    c.code_cache_size = 64 * 1024 * 1024;
    if (std::getenv("DIFFTEST_DUMP")) {  // each block's IR, as translated and as emitted (to stderr)
        c.dump_pc_lo = 0;
        c.dump_pc_hi = ~u64{0};
    }
    return c;
}

u64 RandomValue() {
    // (half of them into the window, where the accesses go through fastmem or the page table)
    if (Rand(0, 1))
        return kWindow + Rand(0, kWindowSize / 8 - 1) * 8 + (Rand(0, 3) == 0 ? Rand(0, 7) : 0);
    switch (Rand(0, 7)) {
    case 0: return 0;
    case 1: return Rand(0, 0xff);
    case 2: return ~u64(0) - Rand(0, 0xff);
    case 3: return Rand(0, 0xffffffff);
    case 4: return kWindow + Rand(0, kWindowSize / 8 - 1) * 8; // into the heap and stack window
    case 5: return u64(1) << Rand(0, 63);
    default: return rng();
    }
}


struct State {
    u64 start = 0, sp = 0;
    u32 pstate = 0;
    std::array<u64, 31> regs{};
    std::array<Vector, 32> vecs{};
};

struct Result {
    std::array<u64, 31> regs{};
    std::array<Vector, 32> vecs{};
    u64 sp = 0, pc = 0;
    u32 nzcv = 0, fpsr = 0;
    bool raised = false;
    std::vector<std::pair<u64, u8>> memory;
    bool operator==(const Result&) const = default;
};

struct Runner {
    Env env;
    std::optional<ExclusiveMonitor> monitor;
    std::optional<A64::Jit> jit;
    std::vector<u8> window_after;
    // Twice from the same state: an access that faulted in the arena the first time (a page left
    // out) is recompiled to walk the page table (the PS4's declined pages, its inline and shared
    // walks), so the second run is what the PS4 runs from then on; its result is the one compared
    Result Run(const std::vector<u32>& code, const State& s) {
        jit->ClearCache();
        const u64 before = env.window_callbacks;
        RunOnce(code, s);
        const u64 middle = env.window_callbacks;
        Result r = RunOnce(code, s);
        first_run_callbacks += middle - before;
        second_run_callbacks += env.window_callbacks - middle;
        return r;
    }
    u64 first_run_callbacks = 0, second_run_callbacks = 0;
    Result RunOnce(const std::vector<u32>& code, const State& s) {
        env.Reset();
        env.code_mem = code;
        env.code_mem.push_back(0x14000000); // B .
        env.code_mem_start_address = s.start;
        env.ticks_left = code.size();
        jit->ClearExclusiveState();
        jit->SetRegisters(s.regs);
        jit->SetVectors(s.vecs);
        jit->SetPC(s.start);
        jit->SetSP(s.sp);
        jit->SetFpcr(0);
        jit->SetFpsr(0);
        jit->SetPstate(s.pstate);
        jit->Run();
        Result r;
        r.regs = jit->GetRegisters();
        r.vecs = jit->GetVectors();
        r.sp = jit->GetSP();
        r.pc = jit->GetPC();
        r.nzcv = jit->GetPstate() >> 28;
        r.fpsr = jit->GetFpsr();
        r.raised = env.raised;
        r.memory.assign(env.modified_memory.begin(), env.modified_memory.end());
        std::sort(r.memory.begin(), r.memory.end());
        // the window's bytes that changed
        for (u64 i = 0; i < kWindowSize; i++)
            if (env.backing[i] != u8((kWindow + i) * 7 >> 3))
                r.memory.emplace_back(kWindow + i, env.backing[i]);
        return r;
    }
};

std::string Describe(u32 inst) {
    char command[256];
    std::snprintf(command, sizeof(command),
                  "echo '0x%02x 0x%02x 0x%02x 0x%02x' | llvm-mc --disassemble -triple=aarch64 "
                  "-mattr=+v8.4a,+crc,+rcpc,+flagm 2>/dev/null | grep -v '\\.text'",
                  inst & 0xff, (inst >> 8) & 0xff, (inst >> 16) & 0xff, inst >> 24);
    std::string out;
    if (FILE* p = popen(command, "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), p))
            out += line;
        pclose(p);
    }
    out.erase(std::remove(out.begin(), out.end(), '\n'), out.end());
    std::replace(out.begin(), out.end(), '\t', ' ');
    return out;
}

}  // namespace

// (compile, DIFFTEST_PROFILE=1) where the translator's time goes: the program counter sampled every
// 50 us of CPU time, then the functions named (addr2line) with their share
static std::vector<uintptr_t> g_samples;
static void ProfileTick(int, siginfo_t*, void* context) {
    if (g_samples.size() < g_samples.capacity())
        g_samples.push_back(uintptr_t(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]));
}
static void ProfileReport() {
    std::map<uintptr_t, size_t> counts;
    for (uintptr_t pc : g_samples)
        counts[pc]++;
    // the executable's own addresses (position independent: its load address off)
    uintptr_t base = 0;
    if (std::FILE* maps = std::fopen("/proc/self/maps", "r")) {
        char line[512];
        if (std::fgets(line, sizeof(line), maps))
            base = std::strtoull(line, nullptr, 16);
        std::fclose(maps);
    }
    const std::string list = "/tmp/difftest-pcs.txt";
    if (std::FILE* out = std::fopen(list.c_str(), "w")) {
        for (const auto& [pc, n] : counts)
            std::fprintf(out, "%#llx\n", (unsigned long long)(pc - base));
        std::fclose(out);
    }
    const std::string cmd = "addr2line -f -C -e /proc/" + std::to_string(getpid()) + "/exe < " + list;
    std::map<std::string, size_t> by_function;
    if (std::FILE* p = popen(cmd.c_str(), "r")) {
        char fn[4096], loc[4096];
        auto it = counts.begin();
        while (it != counts.end() && std::fgets(fn, sizeof(fn), p) && std::fgets(loc, sizeof(loc), p)) {
            std::string name = fn;
            name.erase(name.find_last_not_of("\n") + 1);
            if (name.size() > 150)
                name = name.substr(0, 150);
            by_function[name] += it->second;
            ++it;
        }
        pclose(p);
    }
    std::vector<std::pair<size_t, std::string>> sorted;
    for (const auto& [name, n] : by_function)
        sorted.push_back({n, name});
    std::sort(sorted.rbegin(), sorted.rend());
    std::printf("%zu samples:\n", g_samples.size());
    for (size_t k = 0; k < sorted.size() && k < 40; k++)
        std::printf("  %5.1f%%  %s\n", 100.0 * double(sorted[k].first) / double(g_samples.size()), sorted[k].second.c_str());
}

int main(int argc, char** argv) {
    const u64 iterations = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000;
    const u64 seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
    const bool integer_only = !(argc > 3 && std::string(argv[3]) == "all");
    OptimizationFlag remove = OptimizationFlag::GetSetElimination | OptimizationFlag::ConstProp |
                              OptimizationFlag::MiscIROpt;
    if (argc > 4 && std::string(argv[4]) == "only-getset")
        remove = OptimizationFlag::GetSetElimination;
    if (argc > 4 && std::string(argv[4]) == "only-constprop")
        remove = OptimizationFlag::ConstProp;
    rng.seed(seed);
    // (a forked state whose JIT aborts (dynarmic's segfault report, then abort): its runner as the exit)
    std::signal(SIGABRT, [](int) {
        if (crash_stage)
            _exit(crash_stage);
        std::_Exit(134);
    });
    std::set_terminate([] {
        std::fprintf(stderr, "terminated in the block:\n");
        for (const u32 inst : g_current_block)
            std::fprintf(stderr, "  %08x %s\n", inst, Describe(inst).c_str());
        std::abort();
    });
    const Generators generators = MakeGenerators(integer_only);
    std::printf("%zu instruction kinds; host features mask %s\n", generators.list.size(),
                std::getenv("DYNARMIC_HOST_FEATURES_MASK") ? std::getenv("DYNARMIC_HOST_FEATURES_MASK") : "(all)");

    // on: as the PS4's Auto; off: without the passes (Little Kitty's profile); bare: no optimisation
    // at all (CPU accuracy Paranoid), the referee
    Runner on, off, bare;
    for (Runner* r : {&on, &off, &bare}) {
        r->env.Init();
        r->monitor.emplace(1);
    }
    on.jit.emplace(Config(on.env, *on.monitor, OptimizationFlag{}));
    off.jit.emplace(Config(off.env, *off.monitor, remove));
    {
        A64::UserConfig c = Config(bare.env, *bare.monitor, OptimizationFlag{});
        c.optimizations = no_optimizations;
        c.unsafe_optimizations = false;
        bare.jit.emplace(c);
    }

    // (compile) jit-difftest <blocks> <stride> compile <nro> [passes off: 1]: the translator's
    // speed: blocks starting every <stride> bytes of an NRO's code (from its start, at the game's
    // 0x80000000), each translated (and emitted) and not run, timed; as the PS4's CPU Auto
    if (argc > 4 && std::string(argv[3]) == "compile") {
        std::FILE* f = std::fopen(argv[4], "rb");
        if (!f)
            return 2;
        std::vector<u8> file;
        u8 buffer[65536];
        for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;)
            file.insert(file.end(), buffer, buffer + n);
        std::fclose(f);
        u32 text_off = 0, text_size = 0;
        if (file.size() >= 0x30 && std::memcmp(file.data() + 0x10, "NRO0", 4) == 0) {
            std::memcpy(&text_off, file.data() + 0x20, 4);
            std::memcpy(&text_size, file.data() + 0x24, 4);
        } else {
            text_size = u32(file.size());  // (raw code)
        }
        Runner& r = argc > 5 && std::string(argv[5]) == "1" ? off : on;
        r.env.Reset();
        r.env.code_mem.assign(text_size / 4, 0);
        std::memcpy(r.env.code_mem.data(), file.data() + text_off, text_size & ~3u);
        r.env.code_mem_start_address = 0x80000000;
        const u64 blocks = std::strtoull(argv[1], nullptr, 0), stride = std::max<u64>(4, std::strtoull(argv[2], nullptr, 0)) & ~3ull;
        u64 done = 0;
        const bool profile = std::getenv("DIFFTEST_PROFILE") != nullptr;
        if (profile) {
            g_samples.reserve(4 << 20);
            struct sigaction sa{};
            sa.sa_sigaction = ProfileTick;
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sigaction(SIGPROF, &sa, nullptr);
            const itimerval every{{0, 50}, {0, 50}};
            setitimer(ITIMER_PROF, &every, nullptr);
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (u64 at = 0; done < blocks && at + 4 <= text_size; at += stride, done++) {
            r.jit->SetPC(0x80000000 + at);
            r.jit->SetPstate(0);
            r.jit->SetFpcr(0);
            r.jit->CompileCurrentBlock();
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (profile) {
            const itimerval never{};
            setitimer(ITIMER_PROF, &never, nullptr);
            ProfileReport();
        }
        std::printf("%llu blocks of %u KiB of code: %.1f ms, %.1f us a block\n", (unsigned long long)done,
                    text_size / 1024, s * 1000, s * 1e6 / double(std::max<u64>(done, 1)));
        if (std::getenv("DIFFTEST_HASH")) {
            // what was emitted, as the persistent cache saves it (run with setarch -R: the code
            // buffer's addresses, in the code, then match from run to run)
            u64 hash = 1469598103934665603ull, bytes = 0;
            r.jit->SaveCodeCache([&](const void* data, size_t size) {
                const auto* p = static_cast<const u8*>(data);
                for (size_t k = 0; k < size; k++)
                    hash = (hash ^ p[k]) * 1099511628211ull;
                bytes += size;
                return true;
            });
            std::printf("emitted: %llu bytes saved, hash %016llx\n", (unsigned long long)bytes, (unsigned long long)hash);
        }
        return 0;
    }

    // (directed) jit-difftest 0 0 via: STR X0, [X0, #8]; STR X0, [X0, #16] with X0 on a page the
    // arena leaves out (a walk), the two through one base (a shared walk) whose span crosses into
    // the next page (the shared pointer fails: the slow path, which adds the offset to the base's
    // register, the stored value's too)
    if (argc > 3 && std::string(argv[3]) == "via") {
        // str x0, [x0]; str x0, [x0, #8] (the group's start is the base itself), and with #8, #16
        const std::vector<u32> code = argc > 4 ? std::vector<u32>{0xf9000400, 0xf9000800}
                                               : std::vector<u32>{0xf9000000, 0xf9000400};
        int failures = 0;
        for (const u64 in_page : {u64(0x100), u64(0xff0), u64(0xfe8), u64(0xff8)}) {
            State st;
            st.start = 0x10000;
            st.sp = kWindow + kWindowSize / 2;
            st.regs[0] = kWindow + 3 * kPage + in_page;  // (page 3: left out of the arena)
            const Result x = on.Run(code, st), y = off.Run(code, st), z = bare.Run(code, st);
            const auto stored = [&](const Result& r, u64 at) {
                u64 v = 0;
                for (const auto& [addr, byte] : r.memory)
                    if (addr >= at && addr < at + 8)
                        v |= u64(byte) << (8 * (addr - at));
                return v;
            };
            const u64 a = st.regs[0] - (argc > 4 ? 0 : 8);  // (labels: [a+8] is the first store's)
            std::printf("x0 = %#llx: [x0+8] on %#llx off %#llx bare %#llx; [x0+16] on %#llx off %#llx bare %#llx%s\n",
                        (unsigned long long)a, (unsigned long long)stored(x, a + 8), (unsigned long long)stored(y, a + 8),
                        (unsigned long long)stored(z, a + 8), (unsigned long long)stored(x, a + 16),
                        (unsigned long long)stored(y, a + 16), (unsigned long long)stored(z, a + 16),
                        x == z ? "" : "   <<< passes on wrong");
            failures += !(x == z);
        }
        return failures;
    }

    // (blocks) jit-difftest <states> <seed> blocks <file>: real blocks (each line its start address
    // and its instructions, hex: the console's 'jit dump'), each run from <states> random states
    if (argc > 4 && std::string(argv[3]) == "blocks") {
        std::FILE* f = std::fopen(argv[4], "r");
        if (!f)
            return 2;
        char line[8192];
        unsigned failing = 0;
        while (std::fgets(line, sizeof(line), f)) {
            char* p = line;
            const u64 start = std::strtoull(p, &p, 16);
            std::vector<u32> code;
            while (true) {
                char* q = p;
                const unsigned long w = std::strtoul(p, &q, 16);
                if (q == p)
                    break;
                code.push_back(u32(w));
                p = q;
            }
            if (code.empty())
                continue;
            unsigned on_bad = 0, off_bad = 0, unclear = 0, crashes[4] = {};
            std::string first;
            for (u64 i = 0; i < iterations; i++) {
                State st;
                st.start = start;
                for (auto& r : st.regs)
                    r = RandomValue();
                for (auto& v : st.vecs)
                    v = Rand(0, 1) ? Vector{rng(), rng()} : Vector{u64(std::bit_cast<u64>(double(Rand(0, 1000)) / 8)), 0};
                st.sp = (kWindow + kWindowSize / 2 + Rand(0, kWindowSize / 4)) & ~u64(15);
                st.pstate = u32(Rand(0, 15)) << 28;
                if (std::getenv("DIFFTEST_FORK")) {
                    // (each state in a child: a crash of one of the three is counted, not fatal; which
                    // one crashed, by the child's exit: 10 + the runner)
                    std::fflush(stdout);
                    const pid_t child = fork();
                    if (child == 0) {
                        crash_stage = 10;
                        const Result x = on.Run(code, st);
                        crash_stage = 11;
                        const Result y = off.Run(code, st);
                        crash_stage = 12;
                        const Result z = bare.Run(code, st);
                        _exit(x == y && y == z ? 0 : !(x == z) && y == z ? 1 : !(y == z) && x == z ? 2 : 3);
                    }
                    int status = 0;
                    waitpid(child, &status, 0);
                    const int code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                    if (code_ == 1) on_bad++;
                    else if (code_ == 2) off_bad++;
                    else if (code_ == 3) unclear++;
                    else if (code_ >= 10) crashes[code_ - 10]++;
                    else if (code_ == -1) crashes[3]++;
                    continue;
                }
                const Result x = on.Run(code, st), y = off.Run(code, st);
                if (x == y)
                    continue;
                const Result z = bare.Run(code, st);
                const bool xb = !(x == z) && y == z, yb = !(y == z) && x == z;
                (xb ? on_bad : yb ? off_bad : unclear)++;
                if (first.empty()) {
                    for (size_t r = 0; r < 31; r++)
                        if (x.regs[r] != y.regs[r] || x.regs[r] != z.regs[r])
                            first += fmt::format(" x{} start {:#x}: on {:#x} off {:#x} bare {:#x};", r, st.regs[r],
                                                 x.regs[r], y.regs[r], z.regs[r]);
                    for (size_t r = 0; r < 32; r++)
                        if (x.vecs[r] != y.vecs[r] || x.vecs[r] != z.vecs[r])
                            first += fmt::format(" v{} on {:#x}{:016x} off {:#x}{:016x} bare {:#x}{:016x};", r,
                                                 x.vecs[r][1], x.vecs[r][0], y.vecs[r][1], y.vecs[r][0], z.vecs[r][1],
                                                 z.vecs[r][0]);
                    if (x.nzcv != y.nzcv || x.nzcv != z.nzcv)
                        first += fmt::format(" nzcv on {:x} off {:x} bare {:x};", x.nzcv, y.nzcv, z.nzcv);
                    if (!(x.memory == y.memory) || !(x.memory == z.memory))
                        first += " memory differs;";
                    if (x.pc != y.pc || x.pc != z.pc)
                        first += fmt::format(" pc on {:#x} off {:#x} bare {:#x};", x.pc, y.pc, z.pc);
                }
            }
            // (shrink: the first failing state, the fewest instructions that still fail with it)
            if (on_bad != 0 && std::getenv("DIFFTEST_SHRINK")) {
                rng.seed(seed);
                for (u64 i = 0; i < iterations; i++) {
                    State st;
                    st.start = start;
                    for (auto& r : st.regs)
                        r = RandomValue();
                    for (auto& v : st.vecs)
                        v = Rand(0, 1) ? Vector{rng(), rng()} : Vector{u64(std::bit_cast<u64>(double(Rand(0, 1000)) / 8)), 0};
                    st.sp = (kWindow + kWindowSize / 2 + Rand(0, kWindowSize / 4)) & ~u64(15);
                    st.pstate = u32(Rand(0, 15)) << 28;
                    // (each candidate in a child process: one that crashes the JIT is just not it)
                    const auto bad = [&](const std::vector<u32>& c) {
                        std::fflush(stdout);
                        const pid_t child = fork();
                        if (child == 0) {
                            const Result x = on.Run(c, st), z = bare.Run(c, st);
                            _exit(!(x == z) && off.Run(c, st) == z ? 1 : 0);
                        }
                        int status = 0;
                        waitpid(child, &status, 0);
                        return WIFEXITED(status) && WEXITSTATUS(status) == 1;
                    };
                    if (!bad(code))
                        continue;
                    std::vector<u32> small = code;
                    for (bool shrunk = true; shrunk && small.size() > 1;) {
                        shrunk = false;
                        for (size_t k = 0; k < small.size(); k++) {
                            std::vector<u32> c = small;
                            c.erase(c.begin() + k);
                            if (bad(c)) {
                                small = c;
                                shrunk = true;
                                break;
                            }
                        }
                    }
                    std::fflush(stdout);
                    if (fork() != 0) {
                        int status = 0;
                        wait(&status);
                        break;
                    }
                    on.env.trace = bare.env.trace = std::getenv("DIFFTEST_TRACE") != nullptr;
                    std::printf("  the passes' run (first, then second), its callback accesses:\n");
                    const Result x = on.Run(small, st);
                    std::printf("  bare's:\n");
                    const Result z = bare.Run(small, st);
                    on.env.trace = bare.env.trace = false;
                    std::printf("  smallest failing (%zu instructions), sp %#llx:\n", small.size(), (unsigned long long)st.sp);
                    for (const u32 inst : small)
                        std::printf("     %08x %s\n", inst, Describe(inst).c_str());
                    for (size_t r = 0; r < 31; r++)
                        std::printf("     x%-2zu %#018llx%s\n", r, (unsigned long long)st.regs[r],
                                    x.regs[r] != z.regs[r] ? "  <<< differs after" : "");
                    std::map<u64, std::pair<int, int>> mem;
                    for (const auto& [a, b] : x.memory) mem[a].first = b + 1;
                    for (const auto& [a, b] : z.memory) mem[a].second = b + 1;
                    for (const auto& [a, v] : mem)
                        if (v.first != v.second)
                            std::printf("     memory %#llx: on %02x, bare %02x\n", (unsigned long long)a, v.first - 1,
                                        v.second - 1);
                    std::fflush(stdout);
                    _exit(0);
                }
            }
            if (crashes[0] + crashes[1] + crashes[2] + crashes[3])
                std::printf("  crashes: with the passes %u, without %u, bare %u, unknown %u\n", crashes[0], crashes[1],
                            crashes[2], crashes[3]);
            std::printf("%#llx (%zu instructions): %s", (unsigned long long)start, code.size(),
                        on_bad + off_bad + unclear == 0 ? "same\n" : "");
            if (on_bad + off_bad + unclear != 0) {
                failing++;
                std::printf("DIFFERENT: the pass wrong %u, without it wrong %u, unclear %u of %llu\n   %s\n", on_bad,
                            off_bad, unclear, (unsigned long long)iterations, first.c_str());
                for (const u32 inst : code)
                    std::printf("     %08x %s\n", inst, Describe(inst).c_str());
            }
        }
        std::fclose(f);
        return int(failing);
    }

    u64 on_wrong = 0, off_wrong = 0, other = 0;
    std::map<std::string, u64> kinds;
    for (u64 it = 0; it < iterations; it++) {
        State s;
        s.start = Rand(0x1000, 0x7fffffff) * 4;
        const size_t count = Rand(1, 24);
        std::vector<u32> code;
        for (size_t i = 0; i < count; i++)
            code.push_back(RandomInstruction(generators, s.start + i * 4, i + 1 == count));
        for (auto& r : s.regs)
            r = RandomValue();
        for (auto& v : s.vecs)
            v = {rng(), rng()};
        s.sp = (kWindow + kWindowSize / 2 + Rand(0, kWindowSize / 4)) & ~u64(15);
        s.pstate = u32(Rand(0, 15)) << 28;

        g_current_block = code;
        Result a, b;
        try {
            a = on.Run(code, s);
            b = off.Run(code, s);
        } catch (const std::exception& e) {
            // (the emitter refused the block: what, once per first instruction kind; the JITs made
            // again, as one left mid-compile can't be trusted)
            std::string key = std::string("EMITTER ERROR: ") + e.what() + ":";
            for (const u32 inst : code)
                key += " | " + Describe(inst);
            if (kinds[key]++ == 0)
                std::printf("\n%s (iteration %llu, start %#llx)\n", key.c_str(), (unsigned long long)it,
                            (unsigned long long)s.start);
            on.jit.reset();
            off.jit.reset();
            on.jit.emplace(Config(on.env, *on.monitor, OptimizationFlag{}));
            off.jit.emplace(Config(off.env, *off.monitor, remove));
            continue;
        }
        if (a == b)
            continue;
        const Result c = bare.Run(code, s);
        const bool on_bad = !(a == c) && b == c;
        const bool off_bad = !(b == c) && a == c;
        (on_bad ? on_wrong : off_bad ? off_wrong : other)++;
        // the fewest instructions that still differ the same way (each dropped in turn)
        const auto still = [&](const std::vector<u32>& candidate) {
            const Result x = on.Run(candidate, s), y = off.Run(candidate, s);
            if (x == y)
                return false;
            const Result z = bare.Run(candidate, s);
            return on_bad ? (!(x == z) && y == z) : off_bad ? (!(y == z) && x == z) : true;
        };
        std::vector<u32> small = code;
        for (bool shrunk = true; shrunk && small.size() > 1;) {
            shrunk = false;
            for (size_t i = 0; i < small.size(); i++) {
                std::vector<u32> candidate = small;
                candidate.erase(candidate.begin() + i);
                if (!candidate.empty() && still(candidate)) {
                    small = candidate;
                    shrunk = true;
                    break;
                }
            }
        }
        std::string key = on_bad ? "ON WRONG:" : off_bad ? "off wrong:" : "unclear:";
        for (const u32 inst : small)
            key += " | " + Describe(inst);
        if (kinds[key]++ != 0)
            continue;
        const Result x = on.Run(small, s), y = off.Run(small, s), z = bare.Run(small, s);
        std::printf("\n%s (iteration %llu)\n", key.c_str(), (unsigned long long)it);
        for (size_t i = 0; i < 31; i++)
            if (x.regs[i] != y.regs[i] || x.regs[i] != z.regs[i])
                std::printf("  x%-2zu start %016llx: on %016llx off %016llx bare %016llx\n", i,
                            (unsigned long long)s.regs[i], (unsigned long long)x.regs[i],
                            (unsigned long long)y.regs[i], (unsigned long long)z.regs[i]);
        if (x.nzcv != y.nzcv || x.nzcv != z.nzcv)
            std::printf("  nzcv start %x: on %x off %x bare %x\n", s.pstate >> 28, x.nzcv, y.nzcv, z.nzcv);
        if (!(x.memory == y.memory) || !(x.memory == z.memory))
            std::printf("  memory differs (%zu/%zu/%zu bytes)\n", x.memory.size(), y.memory.size(), z.memory.size());
        if (x.pc != y.pc || x.pc != z.pc)
            std::printf("  pc on %llx off %llx bare %llx\n", (unsigned long long)x.pc, (unsigned long long)y.pc,
                        (unsigned long long)z.pc);
    }
    std::printf("window accesses through the callbacks (passes on): first runs %llu, second runs %llu\n",
                (unsigned long long)on.first_run_callbacks, (unsigned long long)on.second_run_callbacks);
    std::printf("\n%llu iterations: the passes wrong %llu, without them wrong %llu, unclear %llu; %zu kinds\n",
                (unsigned long long)iterations, (unsigned long long)on_wrong, (unsigned long long)off_wrong,
                (unsigned long long)other, kinds.size());
    return 0;
}
