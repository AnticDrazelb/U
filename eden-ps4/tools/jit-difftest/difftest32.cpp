// The 32-bit translator against itself (04.73): random blocks of ARM code (with VFP and ASIMD) run
// from the same state as the PS4's CPU Auto translates them with the 32-bit PS4 extras (on), without
// them (off: Auto as in 04.38-04.72) and with no optimisation at all (bare, the referee). Memory is
// a 4 GiB fastmem arena (cleared before each run; registers point into a 1 MiB window half the
// time); no cycle counting (the PS4's multicore), each run ending at an SVC (also any code outside
// the block). Registers, extension registers, the CPSR's flags and the window are compared.
//   jit-difftest32 [iterations] [seed]
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/translate/a32_translate.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/tests/fuzz_util.h"

using namespace Dynarmic;

namespace {
std::mt19937_64 rng;
u64 Rand(u64 lo, u64 hi) { return lo + rng() % (hi - lo + 1); }

constexpr u32 kWindow = 0x40000000, kWindowSize = 1 << 20;
constexpr u32 kSvc = 0xEF000000;  // svc #0
// (code nobody wrote: SVC in ARM state and, its first halfword, in Thumb too - a garbage address
// with bit 0 set would otherwise run Thumb through it for ever, never linking back)
constexpr u32 kSvcBoth = 0xEF00DF00;
std::vector<std::pair<u32, u32>> g_preset;  // words in the window each run starts with (DIFFTEST_PLT)

class Env final : public A32::UserCallbacks {
public:
    u8* arena = nullptr;  // 4 GiB, the fastmem view and the callbacks' memory both
    std::vector<u32> code;
    u32 code_start = 0;
    A32::Jit* jit = nullptr;
    bool raised = false;

    void Init() {
        arena = static_cast<u8*>(mmap(nullptr, (u64(1) << 32) + 0x10000, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
        if (arena == MAP_FAILED)
            std::abort();
        // past 4 GiB: no access, as the PS4's arena past a 32-bit process's space (a folded offset
        // past the top faults there and takes the slow path, which wraps)
        mprotect(arena + (u64(1) << 32), 0x10000, PROT_NONE);
    }
    void Reset() {
        madvise(arena, u64(1) << 32, MADV_DONTNEED);  // (all zeros again)
        for (u32 i = 0; i < kWindowSize; i++)
            arena[kWindow + i] = u8((kWindow + i) * 7 >> 3);
        for (const auto& [a, v] : g_preset)
            std::memcpy(arena + a, &v, sizeof(v));
        raised = false;
    }
    // (as the PS4's: memory as it is, no side effects; the guarded links' guess)
    std::optional<u32> PeekCode(u32 vaddr) override { return u32(MemoryRead(vaddr, 4)); }
    std::optional<u32> MemoryReadCode(u32 vaddr) override {
        if (vaddr >= code_start && vaddr - code_start < code.size() * 4)
            return code[(vaddr - code_start) / 4];
        return kSvcBoth;
    }
    u64 MemoryRead(u32 a, size_t size) override {
        u64 v = 0;
        for (size_t i = 0; i < size; i++)
            v |= u64(arena[u32(a + i)]) << (8 * i);  // (wrapping at 4 GiB, as the guest's addresses)
        return v;
    }
    void MemoryWrite(u32 a, u64 v, size_t size) override {
        for (size_t i = 0; i < size; i++)
            arena[u32(a + i)] = u8(v >> (8 * i));
    }
    bool MemoryWriteExclusive8(u32 a, u8 v, u8 e) override { return Cas(a, v, e, 1); }
    bool MemoryWriteExclusive16(u32 a, u16 v, u16 e) override { return Cas(a, v, e, 2); }
    bool MemoryWriteExclusive32(u32 a, u32 v, u32 e) override { return Cas(a, v, e, 4); }
    bool MemoryWriteExclusive64(u32 a, u64 v, u64 e) override { return Cas(a, v, e, 8); }
    bool Cas(u32 a, u64 v, u64 e, size_t size) {
        if (MemoryRead(a, size) != e)
            return false;
        MemoryWrite(a, v, size);
        return true;
    }
    void CallSVC(u32) override {
        raised = true;
        if (jit)
            jit->HaltExecution();
    }
    void ExceptionRaised(u32, A32::Exception) override {
        raised = true;
        if (jit)
            jit->HaltExecution();
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1000000; }
};

struct State {
    u32 start = 0;
    std::array<u32, 16> regs{};
    std::array<u32, 64> ext{};
    u32 cpsr = 0x000001d0, fpscr = 0;
};

struct Result {
    std::array<u32, 16> regs{};
    std::array<u32, 64> ext{};
    u32 cpsr = 0;
    std::vector<u8> window;
    bool raised = false;
    bool operator==(const Result&) const = default;
};

enum class Kind { On, Off, Bare };

struct Runner {
    Env env;
    std::optional<ExclusiveMonitor> monitor;
    std::optional<A32::Jit> jit;
    Kind kind;

    A32::UserConfig Config() {
        A32::UserConfig c{};
        c.callbacks = &env;
        c.global_monitor = &*monitor;
        c.processor_id = 0;
        c.enable_cycle_counting = false;
        c.define_unpredictable_behaviour = true;
        if (kind == Kind::Bare) {
            c.optimizations = no_optimizations;
            return c;
        }
        // as arm_dynarmic_32.cpp on the PS4 (CPU Auto), fastmem over the whole space
        c.optimizations = all_safe_optimizations | OptimizationFlag::Unsafe_UnfuseFMA |
                          OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
        c.unsafe_optimizations = true;
        c.fastmem_pointer = reinterpret_cast<uintptr_t>(env.arena);
        c.recompile_on_fastmem_failure = true;
        c.fastmem_exclusive_access = true;
        c.jit_frame_pointer = false;
        c.link_forward_without_halt_check = true;
        c.inline_rsb_pop = true;
        c.inline_fast_dispatch = true;
        if (kind == Kind::On) {
            // (DIFFTEST32_WITHOUT=<name>: that one left off, to tell which one a difference is)
            const char* w = std::getenv("DIFFTEST32_WITHOUT");
            const auto on = [&](const char* name) { return !w || std::strcmp(w, name) != 0; };
            c.inline_conditional_instructions = on("inline_conditional_instructions");
            c.elide_dead_nzcv = on("elide_dead_nzcv");
            c.elide_dead_nzcv_abi = on("elide_dead_nzcv") && on("elide_dead_nzcv_abi");
            c.fold_address_offsets = on("fold_address_offsets");
            c.skip_redundant_extensions = on("skip_redundant_extensions");
            c.split_cold_code = on("split_cold_code");
            c.loop_register_pins = on("loop_register_pins");
            c.guarded_indirect_links = on("guarded_indirect_links");
        }
        if (kind == Kind::Off)
            c.loop_register_pins = false;
        return c;
    }
    void Make(Kind k) {
        kind = k;
        env.Init();
        monitor.emplace(1);
        jit.emplace(Config());
        env.jit = &*jit;
    }
    Result Run(const std::vector<u32>& code, const State& s) {
        jit->ClearCache();
        env.Reset();
        env.code = code;
        env.code.push_back(kSvc);
        env.code_start = s.start;
        jit->ClearExclusiveState();
        jit->ClearHalt(HaltReason::UserDefined1);
        jit->Regs() = s.regs;
        jit->Regs()[15] = s.start;
        jit->ExtRegs() = s.ext;
        jit->SetCpsr(s.cpsr);
        jit->SetFpscr(s.fpscr);
        for (int again = 0; again < 16 && !env.raised; again++)
            jit->Run();
        Result r;
        r.regs = jit->Regs();
        r.ext = jit->ExtRegs();
        r.cpsr = jit->Cpsr() & 0xF80F0000;  // (NZCVQ and GE)
        r.window.assign(env.arena + kWindow, env.arena + kWindow + kWindowSize);
        r.raised = env.raised;
        return r;
    }
};

bool Testable(u32 instruction, u32 pc) {
    const A32::LocationDescriptor location = A32::LocationDescriptor{pc, {}, {}};
    IR::Block block{location};
    if (!A32::TranslateSingleInstruction(block, location, instruction))
        return false;  // (ends the block: only straight-line code)
    for (const auto& inst : block.instructions) {
        switch (inst.GetOpcode()) {
        case IR::Opcode::A32ExceptionRaised:
        case IR::Opcode::A32CallSupervisor:
        case IR::Opcode::A32CoprocInternalOperation:
        case IR::Opcode::A32CoprocSendOneWord:
        case IR::Opcode::A32CoprocSendTwoWords:
        case IR::Opcode::A32CoprocGetOneWord:
        case IR::Opcode::A32CoprocGetTwoWords:
        case IR::Opcode::A32CoprocLoadWords:
        case IR::Opcode::A32CoprocStoreWords:
            return false;
        default:
            break;
        }
    }
    return true;
}

std::vector<InstructionGenerator> MakeGenerators() {
    const std::vector<std::tuple<std::string, const char*>> list{
#define INST(fn, name, bitstring) {#fn, bitstring},
#include "dynarmic/frontend/A32/decoder/arm.inc"
#include "dynarmic/frontend/A32/decoder/asimd.inc"
#include "dynarmic/frontend/A32/decoder/vfp.inc"
#undef INST
    };
    static const char* const skip[] = {
        "arm_LDRBT", "arm_LDRHT", "arm_LDRSBT", "arm_LDRSHT", "arm_LDRT", "arm_STRBT", "arm_STRHT", "arm_STRT",
        "arm_LDM_eret", "arm_LDM_usr", "arm_STM_usr", "arm_BLX_reg", "arm_BLX_imm", "arm_BXJ", "arm_SETEND",
        "arm_CDP", "arm_LDC", "arm_MCR", "arm_MCRR", "arm_MRC", "arm_MRRC", "arm_STC", "arm_CPS", "arm_RFE",
        "arm_SRS", "arm_UDF", "vfp_VMRS", "vfp_VMSR", "arm_SWP", "arm_SWPB", "arm_WFE", "arm_WFI", "arm_SEV",
        "arm_YIELD", "arm_MSR_imm", "arm_MSR_reg", "arm_MRS",
        // exclusives (as dynarmic's own fuzzer: a misaligned one, which the guest can't do, spins)
        "arm_LDREXB", "arm_LDREXD", "arm_LDREXH", "arm_LDREX", "arm_LDAEXB", "arm_LDAEXD", "arm_LDAEXH", "arm_LDAEX",
        "arm_STREXB", "arm_STREXD", "arm_STREXH", "arm_STREX", "arm_STLEXB", "arm_STLEXD", "arm_STLEXH", "arm_STLEX",
    };
    std::vector<InstructionGenerator> g;
    for (const auto& [fn, bitstring] : list) {
        bool skipped = false;
        for (const char* s : skip)
            skipped = skipped || fn == s;
        if (!skipped)
            g.emplace_back(bitstring);
    }
    return g;
}

u32 RandomValue() {
    if (Rand(0, 1))
        return kWindow + u32(Rand(0, kWindowSize / 8 - 1)) * 8 + (Rand(0, 3) == 0 ? u32(Rand(0, 7)) : 0);
    switch (Rand(0, 5)) {
    case 0: return 0;
    case 1: return u32(Rand(0, 0xff));
    case 2: return ~u32(0) - u32(Rand(0, 0xff));
    case 3: return u32(1) << Rand(0, 31);
    default: return u32(rng());
    }
}
}  // namespace

int main(int argc, char** argv) {
    const u64 iterations = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10000;
    rng.seed(argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1);
    // loops: each block a loop (pins on in on, off in off); full: every run checked against bare
    const bool loops = argc > 3 && std::string(argv[3]) == "loops";
    const bool full = loops || std::getenv("DIFFTEST_FULL") != nullptr;
    const std::vector<InstructionGenerator> generators = MakeGenerators();
    Runner on, off, bare;
    on.Make(Kind::On);
    off.Make(Kind::Off);
    bare.Make(Kind::Bare);
    std::printf("%zu instruction kinds\n", generators.size());
    u64 on_wrong = 0, off_wrong = 0, unclear = 0;
    for (u64 it = 0; it < iterations; it++) {
        State s;
        s.start = u32(Rand(0x1000, 0x3fffff)) * 4;
        const size_t count = Rand(1, loops ? 12 : 20);
        std::vector<u32> code;
        while (code.size() < count) {
            const InstructionGenerator& g = generators[Rand(0, generators.size() - 1)];
            const u32 inst = g.Bits() | (u32(rng()) & ~g.Mask());  // (from the seed, not fuzz_util's own generator)
            if ((g.Mask() & 0xF0000000) == 0 && (inst & 0xF0000000) == 0xF0000000)
                continue;  // (the unconditional space, for an instruction with a condition field)
            if (loops) {
                // (r10, the counter, in none of its register fields nor a register list)
                bool uses10 = false;
                for (const int shift : {0, 8, 12, 16})
                    uses10 = uses10 || ((inst >> shift) & 15) == 10;
                if (uses10 || ((inst >> 25) & 7) == 4 && (inst & (1u << 10)))
                    continue;
            }
            if (Testable(inst, s.start + u32(code.size()) * 4))
                code.push_back(inst);
        }
        // (DIFFTEST_PLT=1, half of the blocks) two calls through a GOT slot in the window, each
        // through a PLT stub (BL stub: MOVW/MOVT IP, LDR PC, [IP, #off]!) or inline (MOVW/MOVT IP,
        // LDR R11, [IP, #off]!, BLX R11); the slot points at an ARM callee, which may point it at a
        // second one before its random code (the check failing), or at a Thumb one (BX LR), or at
        // garbage now and then. The callees clear R9, R11 and R12 first, their random code names no LR.
        g_preset.clear();
        if (!loops && std::getenv("DIFFTEST_PLT") && Rand(0, 1) == 0) {
            const u32 page = kWindow + u32(Rand(1, 3)) * 0x1000, off = u32(Rand(0, 1023)) * 4, slot = page + off;
            const auto movw_movt = [&](std::vector<u32>& out) {
                out.push_back(0xE300C000u | ((page & 0xffff) >> 12 << 16) | (page & 0xfff));
                out.push_back(0xE340C000u | ((page >> 16) >> 12 << 16) | ((page >> 16) & 0xfff));
            };
            // (no LR, which would make a return go anywhere, and no PC: a conditional load into it
            // passes Testable)
            const auto uses14 = [](u32 inst) {
                for (const int shift : {0, 8, 12, 16})
                    if (((inst >> shift) & 15) >= 14)
                        return true;
                return ((inst >> 25) & 7) == 4 && (inst & (3u << 14));  // (a register list with LR or PC)
            };
            const auto random_inst = [&](u32 pc, bool no_lr) {
                for (;;) {
                    const InstructionGenerator& g = generators[Rand(0, generators.size() - 1)];
                    const u32 inst = g.Bits() | (u32(rng()) & ~g.Mask());
                    if ((g.Mask() & 0xF0000000) == 0 && (inst & 0xF0000000) == 0xF0000000)
                        continue;
                    if (no_lr && uses14(inst))
                        continue;
                    if (Testable(inst, pc))
                        return inst;
                }
            };
            const bool stubbed[2] = {Rand(0, 1) == 0, Rand(0, 1) == 0};
            const size_t at1 = Rand(0, code.size()), at2 = Rand(at1, code.size());
            std::vector<u32> m;
            size_t call_index[2]{};
            for (size_t i = 0; i <= code.size(); i++) {
                for (int k = 0; k < 2; k++) {
                    if ((k == 0 ? at1 : at2) != i)
                        continue;
                    if (stubbed[k]) {
                        call_index[k] = m.size();
                        m.push_back(0u);  // (bl stub, below)
                    } else {
                        movw_movt(m);
                        m.push_back(0xE5BCB000u | off);  // ldr r11, [ip, #off]!
                        m.push_back(0xE12FFF3Bu);        // blx r11
                    }
                }
                if (i < code.size())
                    m.push_back(code[i]);
            }
            m.push_back(kSvc);
            const size_t stub_at = m.size();
            movw_movt(m);
            m.push_back(0xE5BCF000u | off);  // ldr pc, [ip, #off]!
            const size_t thumb_at = m.size();
            m.push_back(0x47704770u);  // (Thumb) bx lr; bx lr
            const size_t leaf_a = m.size();
            const u32 clear[3] = {0xE3A09000u, 0xE3A0B000u, 0xE3A0C000u};  // mov r9/r11/r12, #0
            std::vector<u32> a_code, b_code;
            for (size_t n = Rand(0, 5); a_code.size() < n;)
                a_code.push_back(random_inst(s.start + u32(leaf_a + 5 + a_code.size()) * 4, true));
            const bool rewrite = Rand(0, 1) == 0;
            const size_t leaf_b = leaf_a + (rewrite ? 2 : 0) + 3 + a_code.size() + 1;
            if (rewrite) {
                const u32 rel = u32(leaf_b - (leaf_a + 2)) * 4;  // add r9, pc, #rel (pc reads 8 ahead)
                m.push_back(0xE28F9000u | rel);
                m.push_back(0xE58C9000u);  // str r9, [ip]
            }
            m.insert(m.end(), clear, clear + 3);
            m.insert(m.end(), a_code.begin(), a_code.end());
            m.push_back(0xE12FFF1Eu);  // bx lr
            for (size_t n = Rand(0, 5); b_code.size() < n;)
                b_code.push_back(random_inst(s.start + u32(leaf_b + 3 + b_code.size()) * 4, true));
            m.insert(m.end(), clear, clear + 3);
            m.insert(m.end(), b_code.begin(), b_code.end());
            m.push_back(0xE12FFF1Eu);  // bx lr
            for (int k = 0; k < 2; k++)
                if (stubbed[k])
                    m[call_index[k]] = 0xEB000000u | (u32(s32(stub_at) - s32(call_index[k]) - 2) & 0xFFFFFF);
            code = m;
            const u32 pick = u32(Rand(0, 15));
            const u32 first = pick == 0 ? (RandomValue() & ~3u)
                            : pick < 5 ? (s.start + u32(thumb_at) * 4) | 1
                            : s.start + u32(leaf_a) * 4;
            g_preset.push_back({slot, first});
        }
        if (loops) {
            const u32 n = u32(code.size());
            code.push_back(0xE25AA001u);                                // subs r10, r10, #1
            code.push_back(0x1A000000u | (u32(-s32(n + 3)) & 0xFFFFFF));  // bne start
        }
        for (u32& r : s.regs)
            r = RandomValue();
        s.regs[13] = kWindow + kWindowSize / 2 + u32(Rand(0, kWindowSize / 4)) * 0 + u32(Rand(0, 1024)) * 8;
        for (u32& e : s.ext)
            e = u32(rng());
        s.cpsr = 0x000001d0 | (u32(Rand(0, 15)) << 28) | (u32(Rand(0, 15)) << 16);
        s.fpscr = u32(Rand(0, 3)) << 22;  // (the rounding mode; flush-to-zero and default NaN off)
        if (loops)
            s.regs[10] = u32(Rand(1, 4));
        // (DIFFTEST_SKIP=n: the first n iterations made but not run; DIFFTEST32_ONLY=on/off/bare:
        // only that run, no comparison: to tell whether a hang is one run's or the program's)
        static const u64 skip = std::getenv("DIFFTEST_SKIP") ? std::strtoull(std::getenv("DIFFTEST_SKIP"), nullptr, 10) : 0;
        if (it < skip)
            continue;
        if (const char* only = std::getenv("DIFFTEST32_ONLY")) {
            alarm(5);
            Runner& r = std::string_view{only} == "on" ? on : std::string_view{only} == "off" ? off : bare;
            const Result x = r.Run(code, s);
            std::printf("iteration %llu %s: finished, raised %d, r0 %08x\n", (unsigned long long)it, only, x.raised, x.regs[0]);
            continue;
        }
        alarm(20);
        static const bool trace = std::getenv("DIFFTEST_TRACE") != nullptr;
        if (trace) {
            std::fprintf(stderr, "it %llu:", (unsigned long long)it);
            for (const u32 inst : code)
                std::fprintf(stderr, " %08x", inst);
            std::fprintf(stderr, " r10=%u r2=%08x cpsr=%08x\n", s.regs[10], s.regs[2], s.cpsr);
        }
        if (trace) std::fprintf(stderr, "  on\n");
        const Result a = on.Run(code, s);
        if (trace) std::fprintf(stderr, "  off\n");
        const Result b = off.Run(code, s);
        if (a == b && !full)
            continue;
        if (trace) std::fprintf(stderr, "  bare\n");
        const Result c = bare.Run(code, s);
        if (a == c && b == c)
            continue;
        const bool on_bad = !(a == c) && b == c, off_bad = !(b == c) && a == c;
        (on_bad ? on_wrong : off_bad ? off_wrong : unclear)++;
        if (on_wrong + off_wrong + unclear <= 12) {
            std::printf("\n%s (iteration %llu, start %#x):", on_bad ? (loops ? "PINNED WRONG" : "EXTRAS WRONG") : off_bad ? "without extras wrong" : "unclear",
                        (unsigned long long)it, s.start);
            for (const u32 inst : code)
                std::printf(" %08x", inst);
            std::printf("\n");
            for (int r = 0; r < 16; r++)
                if (a.regs[r] != c.regs[r] || b.regs[r] != c.regs[r])
                    std::printf("   r%d: on %08x off %08x bare %08x\n", r, a.regs[r], b.regs[r], c.regs[r]);
            for (int e = 0; e < 64; e++)
                if (a.ext[e] != c.ext[e] || b.ext[e] != c.ext[e])
                    std::printf("   s%d: on %08x off %08x bare %08x\n", e, a.ext[e], b.ext[e], c.ext[e]);
            if (a.cpsr != c.cpsr || b.cpsr != c.cpsr)
                std::printf("   cpsr: on %08x off %08x bare %08x\n", a.cpsr, b.cpsr, c.cpsr);
            if (a.window != c.window || b.window != c.window)
                std::printf("   memory differs\n");
            if (a.raised != c.raised)
                std::printf("   raised: on %d bare %d\n", a.raised, c.raised);
        }
    }
    alarm(0);
    std::printf("\n%llu iterations: the extras wrong %llu, without them wrong %llu, unclear %llu\n",
                (unsigned long long)iterations, (unsigned long long)on_wrong, (unsigned long long)off_wrong,
                (unsigned long long)unclear);
    return int(on_wrong != 0);
}
