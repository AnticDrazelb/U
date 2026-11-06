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
#include <string_view>
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
std::vector<std::pair<u64, u64>> g_preset;  // 8-byte words in the window each run starts with

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
        // (04.73: 16 MiB of no access before and after it, as the PS4's arena)
        arena = static_cast<u8*>(mmap(nullptr, (u64(1) << kArenaBits) + (32 << 20), PROT_NONE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
        if (arena != MAP_FAILED)
            arena += 16 << 20;
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
        for (const auto& [va, value] : g_preset)  // (DIFFTEST_PLT: the GOT slot)
            std::memcpy(backing + (va - kWindow), &value, sizeof(value));
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
    A64::Jit* halt_jit = nullptr;  // (loops: no cycle counting, an SVC halts the run)
    bool svc_outside = false;      // (DIFFTEST_NO_TICKS: code outside the block reads as SVC #0)
    std::optional<std::uint32_t> MemoryReadCode(u64 vaddr) override {
        if (!IsInCodeMem(vaddr))
            return svc_outside ? 0xD4000001u : 0x14000000u;
        return code_mem[(vaddr - code_mem_start_address) / 4];
    }
    // (as the PS4's: memory as it is now, no side effects; the guarded links' guess)
    std::optional<std::uint32_t> PeekCode(u64 vaddr) override {
        u32 word = 0;
        for (u64 i = 0; i < 4; i++) {
            const u64 a = vaddr + i;
            word |= u32(InWindow(a) ? backing[a - kWindow] : u8(A64TestEnv::MemoryRead(a, 1))) << (8 * i);
        }
        return word;
    }
    void CallSVC(std::uint32_t) override {
        raised = true;
        ticks_left = 0;
        if (halt_jit)
            halt_jit->HaltExecution();
    }
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
        const InstructionGenerator& pick = g.list[Rand(0, g.list.size() - 1)];
        const u32 inst = pick.Bits() | (u32(rng()) & ~pick.Mask());  // (reproducible from the seed)
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
    if (std::getenv("DIFFTEST_SAFE")) {  // (Auto's unsafe float shortcuts off: exact against bare)
        c.optimizations &= ~(OptimizationFlag::Unsafe_UnfuseFMA | OptimizationFlag::Unsafe_IgnoreGlobalMonitor);
        c.unsafe_optimizations = false;
    }
    c.define_unpredictable_behaviour = true;
    c.dczid_el0 = 4;
    c.ctr_el0 = 0x8444c004;
    c.cntfrq_el0 = 19200000;
    c.jit_frame_pointer = false;
    c.link_forward_without_halt_check = true;
    c.inline_rsb_pop = true;
    c.inline_fast_dispatch = true;
    c.code_cache_size = 64 * 1024 * 1024;
    c.loop_register_pins = std::getenv("DIFFTEST_NO_PINS") == nullptr;
    c.fastmem_range_check_reuse = std::getenv("DIFFTEST_NO_REUSE") == nullptr;
    c.guarded_indirect_links = std::getenv("DIFFTEST_NO_GUARD") == nullptr;
    // (04.74) BLs to short leaf functions followed, and their RETs back (DIFFTEST_NO_LEAF=1: not)
    c.follow_leaf_calls = std::getenv("DIFFTEST_NO_LEAF") == nullptr;
    // (DIFFTEST_FOLLOW=1) BL and forward B translated through, and known returns followed
    c.follow_direct_branches = c.follow_known_returns = std::getenv("DIFFTEST_FOLLOW") != nullptr;
    // (04.73) the 64-bit translator's PS4 extras (04.22-04.30), as Eden sets them (on under Auto
    // again from 04.73): DIFFTEST_NO_EXTRAS=1 leaves them off (Auto as in 04.35-04.72)
    if (std::getenv("DIFFTEST_NO_EXTRAS") == nullptr) {
        c.fold_address_offsets = true;
        c.skip_redundant_extensions = true;
        c.split_cold_code = true;
        c.elide_dead_nzcv = true;
        c.elide_dead_nzcv_abi = true;
        c.return_stack_pc_only = true;
        c.plain_acquire_loads = true;
        c.elide_load_store_only_barriers = true;
        c.superblocks = true;
        c.superblocks_cond = true;
        c.bitfield_shifts = true;
        // (DIFFTEST_WITHOUT=<name>: that extra left off, to tell which one a difference is)
        if (const char* w = std::getenv("DIFFTEST_WITHOUT")) {
            const std::string_view n{w};
            if (n == "fold_address_offsets") c.fold_address_offsets = false;
            if (n == "skip_redundant_extensions") c.skip_redundant_extensions = false;
            if (n == "split_cold_code") c.split_cold_code = false;
            if (n == "elide_dead_nzcv") c.elide_dead_nzcv = c.elide_dead_nzcv_abi = false;
            if (n == "elide_dead_nzcv_abi") c.elide_dead_nzcv_abi = false;
            if (n == "return_stack_pc_only") c.return_stack_pc_only = false;
            if (n == "plain_acquire_loads") c.plain_acquire_loads = false;
            if (n == "elide_load_store_only_barriers") c.elide_load_store_only_barriers = false;
            if (n == "superblocks") c.superblocks = c.superblocks_cond = false;
            if (n == "superblocks_cond") c.superblocks_cond = false;
            if (n == "bitfield_shifts") c.bitfield_shifts = false;
        }
        // (DIFFTEST_NO_ABI=1: no reliance on the procedure call standard's dead flags at calls and
        // returns, which random code breaks: for checking calls)
        if (std::getenv("DIFFTEST_NO_ABI"))
            c.elide_dead_nzcv_abi = false;
    }
    if (std::getenv("DIFFTEST_DUMP")) {  // each block's IR, as translated and as emitted (to stderr)
        c.dump_pc_lo = 0;
        c.dump_pc_hi = ~u64{0};
    }
    return c;
}

// the referee: no optimisation at all, none of the PS4 extras either
void MakeBare(A64::UserConfig& c) {
    c.optimizations = no_optimizations;
    c.unsafe_optimizations = false;
    c.fold_address_offsets = c.skip_redundant_extensions = c.split_cold_code = false;
    c.elide_dead_nzcv = c.elide_dead_nzcv_abi = c.return_stack_pc_only = false;
    c.plain_acquire_loads = c.elide_load_store_only_barriers = false;
    c.superblocks = c.superblocks_cond = c.bitfield_shifts = false;
    c.loop_register_pins = false;
    c.follow_known_returns = false;
    c.fastmem_range_check_reuse = false;
    c.guarded_indirect_links = false;
    c.follow_leaf_calls = false;
    c.link_forward_without_halt_check = c.inline_rsb_pop = c.inline_fast_dispatch = false;
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
    bool ignore_fpsr = false;
    Result RunOnce(const std::vector<u32>& code, const State& s) {
        env.Reset();
        env.code_mem = code;
        env.code_mem.push_back(env.svc_outside ? 0xD4000001u : 0x14000000u); // SVC #0 (no ticks), else B .
        env.code_mem_start_address = s.start;
        env.ticks_left = code.size();
        jit->ClearExclusiveState();
        jit->ClearHalt(HaltReason::UserDefined1);  // (an SVC's halt from the run before)
        jit->SetRegisters(s.regs);
        jit->SetVectors(s.vecs);
        jit->SetPC(s.start);
        jit->SetSP(s.sp);
        jit->SetFpcr(0);
        jit->SetFpsr(0);
        jit->SetPstate(s.pstate);
        jit->Run();
        // (no ticks: a run that stopped before its SVC, for a recompile its fault asked for, goes on)
        for (int again = 0; env.svc_outside && !env.raised && again < 16; again++)
            jit->Run();
        // (no ticks: FPSR's sticky flags left out of the comparison: they carry over from the run
        // before in some orders of runs, the same in every configuration, 04.72's too)
        ignore_fpsr = env.svc_outside;
        Result r;
        r.regs = jit->GetRegisters();
        r.vecs = jit->GetVectors();
        r.sp = jit->GetSP();
        r.pc = jit->GetPC();
        r.nzcv = jit->GetPstate() >> 28;
        r.fpsr = ignore_fpsr ? 0 : jit->GetFpsr();
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
        if (std::exception_ptr e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& x) {
                std::fprintf(stderr, "terminate: exception %s\n", x.what());
            } catch (...) {
                std::fprintf(stderr, "terminate: an exception of another type\n");
            }
        }
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
    // (DIFFTEST_NO_TICKS=1) without cycle counting, as the PS4's multicore: superblocks and links
    // without the halt check then apply; a run ends at an SVC (any code outside the block)
    const bool no_ticks = std::getenv("DIFFTEST_NO_TICKS") != nullptr;
    {
        A64::UserConfig c = Config(on.env, *on.monitor, OptimizationFlag{});
        c.enable_cycle_counting = !no_ticks;
        on.jit.emplace(c);
        c = Config(off.env, *off.monitor, remove);
        c.enable_cycle_counting = !no_ticks;
        off.jit.emplace(c);
        c = Config(bare.env, *bare.monitor, OptimizationFlag{});
        MakeBare(c);
        c.enable_cycle_counting = !no_ticks;
        bare.jit.emplace(c);
    }
    for (Runner* r : {&on, &off, &bare}) {
        r->env.svc_outside = no_ticks;
        r->env.halt_jit = no_ticks ? &*r->jit : nullptr;
    }

    // (loopbench) jit-difftest 0 0 loopbench: a few loops run 20 million times, timed with the
    // loop's registers pinned and without (no cycle counting, as the PS4's multicore)
    if (argc > 3 && std::string(argv[3]) == "loopbench") {
        struct Case { const char* name; std::vector<u32> code; };
        const std::vector<Case> cases = {
            // add x1, x1, x2; eor x3, x3, x1; subs x28, x28, #1; b.ne -3
            {"arithmetic", {0x8B020021, 0xCA010063, 0xF100079C, 0x54FFFFA1, 0xD4000001}},
            // ldr x1, [x9]; add x1, x1, #1; str x1, [x9]; subs x28, x28, #1; b.ne -4
            {"load add store", {0xF9400121, 0x91000421, 0xF9000121, 0xF100079C, 0x54FFFF81, 0xD4000001}},
            // memcpy's 32 bytes a round, the same 32 each time (x9 -> x8, then both back):
            // ldp x10, x11, [x9]; ldp x12, x13, [x9, #16]; stp x10, x11, [x8]; stp x12, x13, [x8, #16];
            // add x8, x8, #32; add x9, x9, #32; sub x8, x8, #32; sub x9, x9, #32; subs x28.. ; b.ne
            {"memcpy round", {0xA9402D2A, 0xA941352C, 0xA9002D0A, 0xA901350C, 0x91008108, 0x91008129, 0xD1008108,
                              0xD1008129, 0xF100079C, 0x54FFFEC1, 0xD4000001}},
        };
        for (const bool pins : {false, true}) {
            for (Runner* r : {&on}) {
                A64::UserConfig c = Config(r->env, *r->monitor, OptimizationFlag{});
                c.enable_cycle_counting = false;
                c.loop_register_pins = pins;
                r->jit.reset();
                r->jit.emplace(c);
                r->env.halt_jit = &*r->jit;
            }
            for (const Case& k : cases) {
                State st;
                st.start = 0x10000;
                st.regs[28] = 20000000;
                st.regs[8] = kWindow + 0x1000;
                st.regs[9] = kWindow + 0x2000;  // (pages 1 and 2: in the arena)
                st.sp = kWindow + kWindowSize / 2;
                const auto t0 = std::chrono::steady_clock::now();
                on.RunOnce(k.code, st);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                std::printf("%-16s pins %-3s %7.1f ms (%.2f ns a round)\n", k.name, pins ? "on" : "off", ms, ms * 1e6 / 20e6);
            }
        }
        return 0;
    }

    // (pltbench) jit-difftest 0 0 pltbench: a call through a PLT stub and one through the GOT
    // inline (BLR), 20 million times each, with the guarded links and without
    if (argc > 3 && std::string(argv[3]) == "pltbench") {
        const u64 page = kWindow + kPage, off = 0x10, base = 0x10000;
        const auto x16 = [&](std::vector<u32>& out) {
            out.push_back(0xD2800000u | (u32(page & 0xffff) << 5) | 16);
            out.push_back(0xF2800000u | (1u << 21) | (u32((page >> 16) & 0xffff) << 5) | 16);
            out.push_back(0xF2800000u | (2u << 21) | (u32((page >> 32) & 0xffff) << 5) | 16);
            out.push_back(0xF9400000u | u32(off / 8) << 10 | (16 << 5) | 17);
            out.push_back(0x91000000u | u32(off) << 10 | (16 << 5) | 16);
        };
        struct Case { const char* name; std::vector<u32> code; u64 leaf; };
        std::vector<Case> cases;
        {
            std::vector<u32> c = {0x94000005u, 0xF100079Cu, 0x54FFFFC1u, 0xD4000001u, 0xD503201Fu};
            x16(c);
            c.push_back(0xD61F0220u);  // br x17
            const u64 leaf = base + 4 * c.size();
            c.push_back(0x91000421u);  // add x1, x1, #1
            c.push_back(0xD65F03C0u);  // ret
            cases.push_back({"plt stub", c, leaf});
        }
        {
            std::vector<u32> c;
            x16(c);
            c.push_back(0xD63F0220u);  // blr x17
            c.push_back(0xF100079Cu);  // subs x28, x28, #1
            c.push_back(0x54000001u | (u32(-7 & 0x7ffff) << 5));  // b.ne the start
            c.push_back(0xD4000001u);
            const u64 leaf = base + 4 * c.size();
            c.push_back(0x91000421u);
            c.push_back(0xD65F03C0u);
            cases.push_back({"got blr", c, leaf});
        }
        const u64 rounds = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 20000000;
        for (const bool guard : {false, true}) {
            if (const char* only = std::getenv("PLTBENCH_GUARD"); only && (only[0] == '1') != guard)
                continue;  // (one of the two: for callgrind)
            A64::UserConfig c = Config(on.env, *on.monitor, OptimizationFlag{});
            c.enable_cycle_counting = false;
            c.guarded_indirect_links = guard;
            on.jit.reset();
            on.jit.emplace(c);
            on.env.halt_jit = &*on.jit;
            for (const Case& k : cases) {
                g_preset = {{page + off, k.leaf}};
                State st;
                st.start = base;
                st.regs[28] = rounds;
                st.sp = kWindow + kWindowSize / 2;
                const auto t0 = std::chrono::steady_clock::now();
                const Result r = on.RunOnce(k.code, st);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                std::printf("%-10s guard %-3s %8.1f ms (%.2f ns a call) x1 %llx\n", k.name, guard ? "on" : "off", ms,
                            ms * 1e6 / double(rounds), (unsigned long long)r.regs[1]);
            }
        }
        return 0;
    }

    // (loops) jit-difftest <iterations> <seed> loops [all]: random loop bodies (x28, the counter,
    // in none of their register fields) closed by SUBS X28/W28 + B.NE or SUB + CBNZ back to the
    // start, then an SVC that halts: run without cycle counting (the PS4's multicore), with the
    // loop's registers pinned (on), without (off), and with no optimisation (bare, the referee)
    if (argc > 3 && std::string(argv[3]) == "loops") {
        const Generators loop_generators = MakeGenerators(!(argc > 4 && std::string(argv[4]) == "all"));
        for (Runner* r : {&on, &off, &bare}) {
            A64::UserConfig c = Config(r->env, *r->monitor, OptimizationFlag{});
            c.enable_cycle_counting = false;
            c.loop_register_pins = r == &on;
            if (r == &bare)
                MakeBare(c);
            r->jit.reset();
            r->jit.emplace(c);
            r->env.halt_jit = &*r->jit;
        }
        const auto uses28 = [](u32 inst) {
            for (const int shift : {0, 5, 10, 16})
                if (((inst >> shift) & 31) == 28)
                    return true;
            return false;
        };
        u64 wrong = 0, unclear = 0;
        for (u64 it = 0; it < iterations; it++) {
            State s;
            s.start = Rand(0x1000, 0x7fffffff) * 4;
            const size_t body = Rand(1, 14);
            std::vector<u32> code;
            while (code.size() < body) {
                const u32 inst = RandomInstruction(loop_generators, s.start + code.size() * 4, false);
                if (!uses28(inst))
                    code.push_back(inst);
            }
            const s32 back = -s32(body + 1);
            switch (Rand(0, 2)) {
            case 0:  // subs x28, x28, #1; b.ne start
                code.push_back(0xF100079Cu);
                code.push_back(0x54000001u | ((u32(back) & 0x7ffff) << 5));
                break;
            case 1:  // subs w28, w28, #1; b.ne start
                code.push_back(0x7100079Cu);
                code.push_back(0x54000001u | ((u32(back) & 0x7ffff) << 5));
                break;
            default:  // sub x28, x28, #1; cbnz x28, start
                code.push_back(0xD100079Cu);
                code.push_back(0xB500001Cu | ((u32(back) & 0x7ffff) << 5));
                break;
            }
            code.push_back(0xD4000001u);  // svc #0
            for (auto& r : s.regs)
                r = RandomValue();
            for (auto& v : s.vecs)
                v = {rng(), rng()};
            s.regs[28] = Rand(1, 4);
            s.sp = (kWindow + kWindowSize / 2 + Rand(0, kWindowSize / 4)) & ~u64(15);
            s.pstate = u32(Rand(0, 15)) << 28;
            g_current_block = code;
            alarm(20);  // (a loop that never ends: the process ends, the block on stderr)
            Result a, b, c;
            try {
                a = on.Run(code, s);
                b = off.Run(code, s);
            } catch (const std::exception& e) {
                std::printf("emitter error: %s\n", e.what());
                continue;
            }
            if (a == b)
                continue;
            c = bare.Run(code, s);
            const bool on_bad = !(a == c) && b == c;
            (on_bad ? wrong : unclear)++;
            if (wrong + unclear <= 5) {
                std::printf("\n%s (iteration %llu, x28 %llu):\n", on_bad ? "PINNED WRONG" : "unclear",
                            (unsigned long long)it, (unsigned long long)s.regs[28]);
                for (const u32 inst : code)
                    std::printf("     %08x  %s\n", inst, Describe(inst).c_str());
                for (size_t r = 0; r < 31; r++)
                    if (a.regs[r] != c.regs[r] || b.regs[r] != c.regs[r])
                        std::printf("   x%zu: pinned %#llx, not %#llx, bare %#llx\n", r, (unsigned long long)a.regs[r],
                                    (unsigned long long)b.regs[r], (unsigned long long)c.regs[r]);
                if (a.sp != c.sp)
                    std::printf("   sp: pinned %#llx, bare %#llx\n", (unsigned long long)a.sp, (unsigned long long)c.sp);
                if (a.memory != c.memory)
                    std::printf("   memory differs\n");
            }
        }
        alarm(0);
        std::printf("%llu loops: pinned wrong %llu, unclear %llu\n", (unsigned long long)iterations,
                    (unsigned long long)wrong, (unsigned long long)unclear);
        return int(wrong != 0);
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
                        // (an exception from a runner: its message, then counted as its crash)
                        const auto guarded = [&](Runner& r, int stage) {
                            crash_stage = stage;
                            alarm(std::getenv("DIFFTEST_CHILD_ALARM") ? unsigned(std::atoi(std::getenv("DIFFTEST_CHILD_ALARM"))) : 30u);  // (a loop in real code with a random counter: ends the child)
                            try {
                                return r.Run(code, st);
                            } catch (const std::exception& e) {
                                std::fprintf(stderr, "runner %d exception: %s\n", stage - 10, e.what());
                                _exit(stage);
                            }
                        };
                        const Result x = guarded(on, 10);
                        const Result y = guarded(off, 11);
                        const Result z = guarded(bare, 12);
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
            code.push_back(RandomInstruction(generators, s.start + i * 4, !no_ticks && i + 1 == count));
        if (no_ticks) {
            // forward branches (superblocks' side exits, links forward) to a later instruction or
            // the SVC after the block: CBZ/CBNZ, TBZ/TBNZ, B.cond, B
            for (size_t i = 0; i + 1 < count + 1 && i < count; i++) {
                if (Rand(0, 4) != 0)
                    continue;
                const u32 k = u32(Rand(1, count - i));  // (i + k <= count: the SVC at most)
                const u32 rt = u32(Rand(0, 30));
                switch (Rand(0, 3)) {
                case 0: code[i] = (Rand(0, 1) ? 0xB4000000u : 0x34000000u) | (Rand(0, 1) << 24) | ((k & 0x7ffff) << 5) | rt; break;
                case 1: code[i] = 0x36000000u | (Rand(0, 1) << 24) | (u32(Rand(0, 1)) << 31) | (u32(Rand(0, 31)) << 19) | ((k & 0x3fff) << 5) | rt; break;
                case 2: code[i] = 0x54000000u | ((k & 0x7ffff) << 5) | u32(Rand(0, 13)); break;
                default: code[i] = 0x14000000u | (k & 0x3ffffff); break;
                }
            }
            // a call (DIFFTEST_CALLS=1, a third of the blocks): the block, BL leaf, more of it, SVC,
            // then the leaf (random, then RET) after it
            bool called = false;  // (DIFFTEST_PLT's calls only in blocks without this one's)
            if (std::getenv("DIFFTEST_CALLS") && Rand(0, 2) == 0) {
                called = true;
                const size_t at = Rand(0, code.size());  // (the BL goes in here)
                std::vector<u32> leaf;
                for (size_t n = Rand(0, 6); leaf.size() < n;)
                    leaf.push_back(RandomInstruction(generators, s.start + 4 * (code.size() + 2 + leaf.size()), false));
                code.insert(code.begin() + at, 0u);
                code.push_back(0xD4000001u);  // svc #0 (the end, the RET's way back passed)
                const size_t leaf_at = code.size();
                code.insert(code.end(), leaf.begin(), leaf.end());
                code.push_back(0xD65F03C0u);  // ret
                code[at] = 0x94000000u | (u32(leaf_at - at) & 0x3ffffff);  // bl leaf
            }
            // (DIFFTEST_PLT=1, half of the blocks) two calls through a GOT slot in the window, each
            // through a PLT stub (bl stub: movz/movk x16, ldr x17, [x16, #off], add x16, br x17) or
            // inline (the same with blr x17); the first callee may point the slot at a second one
            // before its random code and RET (the guarded link's check failing); a garbage slot now
            // and then
            g_preset.clear();
            if (std::getenv("DIFFTEST_PLT") && !called && Rand(0, 1) == 0) {
                const u64 page = kWindow + (Rand(0, 1) ? 1 : 3) * kPage;  // (in the arena or not)
                const u32 off = u32(Rand(0, 511)) * 8;
                const u64 slot = page + off;
                const auto x16_sequence = [&](std::vector<u32>& out) {
                    out.push_back(0xD2800000u | (u32(page & 0xffff) << 5) | 16);
                    out.push_back(0xF2800000u | (1u << 21) | (u32((page >> 16) & 0xffff) << 5) | 16);
                    out.push_back(0xF2800000u | (2u << 21) | (u32((page >> 32) & 0xffff) << 5) | 16);
                    out.push_back(0xF9400000u | ((off / 8) << 10) | (16 << 5) | 17);  // ldr x17, [x16, #off]
                    out.push_back(0x91000000u | ((off & 0xfff) << 10) | (16 << 5) | 16);  // add x16, x16, #off
                };
                const bool stubbed[2] = {Rand(0, 1) == 0, Rand(0, 1) == 0};
                std::vector<size_t> sites;  // (where each call's first instruction goes)
                const size_t at1 = Rand(0, code.size()), at2 = Rand(at1, code.size());
                // build: main with the call sites, svc, stub, leaf A, leaf B
                std::vector<u32> main_code;
                size_t call_index[2]{};
                for (size_t i = 0; i <= code.size(); i++) {
                    for (int k = 0; k < 2; k++) {
                        if ((k == 0 ? at1 : at2) != i)
                            continue;
                        call_index[k] = main_code.size();
                        if (stubbed[k]) {
                            main_code.push_back(0u);  // (bl stub, below)
                        } else {
                            x16_sequence(main_code);
                            main_code.push_back(0xD63F0220u);  // blr x17
                        }
                    }
                    if (i < code.size())
                        main_code.push_back(code[i]);
                }
                // (the forward branches the block had: their targets moved by the calls put in before them)
                main_code.push_back(0xD4000001u);  // svc #0
                const size_t stub_at = main_code.size();
                x16_sequence(main_code);
                main_code.push_back(0xD61F0220u);  // br x17
                const size_t leaf_a = main_code.size();
                const bool rewrite = Rand(0, 1) == 0;
                std::vector<u32> a_code, b_code;
                // (the callees' random code names no X30 - one making it a code address, from X17 = its
                // own, X9 = the other, would RET back into itself for ever - and starts by clearing X9,
                // X16 and X17)
                const auto uses30 = [](u32 inst) {
                    for (const int shift : {0, 5, 10, 16})
                        if (((inst >> shift) & 31) == 30)
                            return true;
                    return false;
                };
                const auto leaf_instruction = [&](u64 pc) {
                    for (;;)
                        if (const u32 inst = RandomInstruction(generators, pc, false); !uses30(inst))
                            return inst;
                };
                const u32 clear[3] = {0xD2800009u, 0xD2800010u, 0xD2800011u};  // movz x9/x16/x17, #0
                for (size_t n = Rand(0, 5); a_code.size() < n;)
                    a_code.push_back(leaf_instruction(s.start + 4 * (leaf_a + 5 + a_code.size())));
                // (each callee clears x16 first, which a stub leaves at the slot: random stores through it
                // would make the slot point anywhere, back into the caller too, for ever)
                const size_t leaf_b = leaf_a + (rewrite ? 2 : 0) + 3 + a_code.size() + 1;
                if (rewrite) {
                    const s64 rel = s64(leaf_b - leaf_a) * 4;  // adr x9, leaf B
                    main_code.push_back(0x10000000u | (u32(rel & 3) << 29) | (u32((rel >> 2) & 0x7ffff) << 5) | 9);
                    main_code.push_back(0xF9000209u);  // str x9, [x16]
                }
                main_code.insert(main_code.end(), clear, clear + 3);
                main_code.insert(main_code.end(), a_code.begin(), a_code.end());
                main_code.push_back(0xD65F03C0u);  // ret
                for (size_t n = Rand(0, 5); b_code.size() < n;)
                    b_code.push_back(leaf_instruction(s.start + 4 * (leaf_b + 3 + b_code.size())));
                main_code.insert(main_code.end(), clear, clear + 3);
                main_code.insert(main_code.end(), b_code.begin(), b_code.end());
                main_code.push_back(0xD65F03C0u);  // ret
                for (int k = 0; k < 2; k++)
                    if (stubbed[k])
                        main_code[call_index[k]] = 0x94000000u | (u32(stub_at - call_index[k]) & 0x3ffffff);
                code = main_code;
                const u64 first = Rand(0, 15) == 0 ? (RandomValue() & ~u64(3)) : s.start + 4 * leaf_a;
                g_preset.push_back({slot, first});
            }
            alarm(20);
        }
        for (auto& r : s.regs)
            r = RandomValue();
        for (auto& v : s.vecs)
            v = {rng(), rng()};
        s.sp = (kWindow + kWindowSize / 2 + Rand(0, kWindowSize / 4)) & ~u64(15);
        s.pstate = u32(Rand(0, 15)) << 28;

        g_current_block = code;
        Result a, b;
        static const u64 skip = std::getenv("DIFFTEST_SKIP") ? std::strtoull(std::getenv("DIFFTEST_SKIP"), nullptr, 10) : 0;
        if (it < skip)  // (DIFFTEST_SKIP=n: the first n iterations made but not run, to get to one)
            continue;
        if (const char* only = std::getenv("DIFFTEST_ONLY")) {  // (on/off/bare: that run alone)
            Runner& r = std::string_view{only} == "on" ? on : std::string_view{only} == "off" ? off : bare;
            const Result x = r.Run(code, s);
            std::printf("iteration %llu %s: finished, raised %d, x0 %llx\n", (unsigned long long)it, only, int(x.raised),
                        (unsigned long long)x.regs[0]);
            continue;
        }
        static const bool progress = std::getenv("DIFFTEST_PROGRESS") != nullptr;
        if (progress) {  // (each iteration's code before it runs, and which run: to find a hang)
            std::fprintf(stderr, "iteration %llu start %#llx:", (unsigned long long)it, (unsigned long long)s.start);
            for (const u32 inst : code)
                std::fprintf(stderr, " %08x", inst);
            std::fprintf(stderr, "\n");
        }
        try {
            if (progress) std::fprintf(stderr, "  on\n");
            a = on.Run(code, s);
            if (progress) std::fprintf(stderr, "  off\n");
            b = off.Run(code, s);
            if (progress) std::fprintf(stderr, "  done\n");
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
        // (DIFFTEST_FULL=1: every run checked against bare, so that what on and off share - the PS4
        // extras, loop pins, followed branches - is checked too, not only the IR passes)
        static const bool full = std::getenv("DIFFTEST_FULL") != nullptr;
        if (a == b && !full)
            continue;
        const Result c = bare.Run(code, s);
        if (a == c && b == c)
            continue;
        const bool on_bad = !(a == c) && b == c;
        const bool off_bad = !(b == c) && a == c;
        (on_bad ? on_wrong : off_bad ? off_wrong : other)++;
        if (std::getenv("DIFFTEST_REPEAT") && on_wrong + off_wrong + other <= 40) {
            std::printf("\n  first comparison, iteration %llu:", (unsigned long long)it);
            for (const u32 inst : code)
                std::printf(" %08x", inst);
            std::printf("\n");
            for (size_t i = 0; i < 31; i++)
                if (a.regs[i] != c.regs[i] || b.regs[i] != c.regs[i])
                    std::printf("    x%zu: on %llx off %llx bare %llx\n", i, (unsigned long long)a.regs[i],
                                (unsigned long long)b.regs[i], (unsigned long long)c.regs[i]);
            if (a.nzcv != c.nzcv) std::printf("    nzcv on %x off %x bare %x\n", a.nzcv, b.nzcv, c.nzcv);
            if (a.pc != c.pc) std::printf("    pc on %llx off %llx bare %llx\n", (unsigned long long)a.pc, (unsigned long long)b.pc, (unsigned long long)c.pc);
            if (!(a.memory == c.memory)) std::printf("    memory on %zu off %zu bare %zu entries\n", a.memory.size(), b.memory.size(), c.memory.size());
            if (a.raised != c.raised) std::printf("    raised on %d off %d bare %d\n", a.raised, b.raised, c.raised);
            if (!(a.vecs == c.vecs)) std::printf("    vectors differ\n");
            if (a.fpsr != c.fpsr) std::printf("    fpsr on %x off %x bare %x\n", a.fpsr, b.fpsr, c.fpsr);
            if (a.sp != c.sp) std::printf("    sp on %llx bare %llx\n", (unsigned long long)a.sp, (unsigned long long)c.sp);
            const Result a2 = on.Run(code, s);
            std::printf("    on again: %s\n", a2 == a ? "same as its first" : a2 == c ? "now as bare" : "different again");
        }
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
        if (std::getenv("DIFFTEST_REPEAT")) {
            std::printf("  words:");
            for (const u32 inst : small)
                std::printf(" %08x", inst);
            std::printf("\n  repeats: on %d%d%d off %d%d%d bare %d%d%d (1 = same as its first run)\n",
                        on.Run(small, s) == x, on.Run(small, s) == x, on.Run(small, s) == x,
                        off.Run(small, s) == y, off.Run(small, s) == y, off.Run(small, s) == y,
                        bare.Run(small, s) == z, bare.Run(small, s) == z, bare.Run(small, s) == z);
            std::printf("  raised: on %d off %d bare %d; pc on %llx off %llx bare %llx\n", x.raised, y.raised, z.raised,
                        (unsigned long long)x.pc, (unsigned long long)y.pc, (unsigned long long)z.pc);
        }
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
