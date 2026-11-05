// The 64-bit translator against itself: random blocks of A64 code run twice, with the IR passes
// that broke Little Kitty, Big City on the PS4 (get/set elimination, constant propagation; 04.66)
// and without them, from the same state; any difference in the registers, flags or memory written
// is a mistranslation. Run it with DYNARMIC_HOST_FEATURES_MASK=16b00f for the code the PS4's
// Jaguar gets (no BMI2, AVX2, FMA, AVX-512).
//   jit-difftest [iterations] [seed] [int|all] [only-getset|only-constprop]

#include <algorithm>
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
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/tests/A64/testenv.h"
#include "dynarmic/tests/fuzz_util.h"

using namespace Dynarmic;

namespace {

std::mt19937_64 rng;

u64 Rand(u64 lo, u64 hi) {
    return std::uniform_int_distribution<u64>(lo, hi)(rng);
}

class Env final : public A64TestEnv {
public:
    bool raised = false;
    void CallSVC(std::uint32_t) override { raised = true; ticks_left = 0; }
    void ExceptionRaised(u64, Dynarmic::A64::Exception) override { raised = true; ticks_left = 0; }
};

bool Testable(u32 instruction, u64 pc, bool last) {
    const A64::LocationDescriptor location{pc, {}};
    IR::Block block{location};
    const bool goes_on = A64::TranslateSingleInstruction(block, location, instruction);
    if (!goes_on && !last)
        return false;
    for (const auto& inst : block.instructions) {
        switch (inst.GetOpcode()) {
        case IR::Opcode::A64ExceptionRaised:
        case IR::Opcode::A64CallSupervisor:
        case IR::Opcode::A64DataCacheOperationRaised:
        case IR::Opcode::A64InstructionCacheOperationRaised:
        case IR::Opcode::A64GetCNTPCT:
        case IR::Opcode::A64ClearExclusive:
        case IR::Opcode::A64ExclusiveReadMemory8:
        case IR::Opcode::A64ExclusiveReadMemory16:
        case IR::Opcode::A64ExclusiveReadMemory32:
        case IR::Opcode::A64ExclusiveReadMemory64:
        case IR::Opcode::A64ExclusiveReadMemory128:
        case IR::Opcode::A64ExclusiveWriteMemory8:
        case IR::Opcode::A64ExclusiveWriteMemory16:
        case IR::Opcode::A64ExclusiveWriteMemory32:
        case IR::Opcode::A64ExclusiveWriteMemory64:
        case IR::Opcode::A64ExclusiveWriteMemory128:
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
        "SXTH_2|SXTW|RMIF|SETF8|SETF16|CRC32|LDAR|STLR|LDTR|STTR|B_uncond|BL|BR|BLR|RET)$");
    static const std::vector<std::string> never{"UnallocatedEncoding", "STLLR", "LDLAR", "STXR", "STLXR", "STXP",
                                                "STLXP", "LDXR", "LDAXR", "LDXP", "LDAXP", "MSR_reg", "MSR_imm",
                                                "MRS", "SVC", "BRK", "HLT", "WFI", "WFE", "YIELD", "SEV", "SEVL"};
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
A64::UserConfig Config(Env& env, OptimizationFlag remove) {
    A64::UserConfig c{};
    c.callbacks = &env;
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
    return c;
}

u64 RandomValue() {
    switch (Rand(0, 7)) {
    case 0: return 0;
    case 1: return Rand(0, 0xff);
    case 2: return ~u64(0) - Rand(0, 0xff);
    case 3: return Rand(0, 0xffffffff);
    case 4: return 0x1180000000 + Rand(0, 0x800000) * 8; // a heap pointer
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
    std::optional<A64::Jit> jit;
    Result Run(const std::vector<u32>& code, const State& s) {
        env.code_mem = code;
        env.code_mem.push_back(0x14000000); // B .
        env.code_mem_start_address = s.start;
        env.modified_memory.clear();
        env.raised = false;
        env.ticks_left = code.size();
        jit->ClearCache();
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
    const Generators generators = MakeGenerators(integer_only);
    std::printf("%zu instruction kinds; host features mask %s\n", generators.list.size(),
                std::getenv("DYNARMIC_HOST_FEATURES_MASK") ? std::getenv("DYNARMIC_HOST_FEATURES_MASK") : "(all)");

    // on: as the PS4's Auto; off: without the passes (Little Kitty's profile); bare: no optimisation
    // at all (CPU accuracy Paranoid), the referee
    Runner on, off, bare;
    on.jit.emplace(Config(on.env, OptimizationFlag{}));
    off.jit.emplace(Config(off.env, remove));
    {
        A64::UserConfig c = Config(bare.env, OptimizationFlag{});
        c.optimizations = no_optimizations;
        c.unsafe_optimizations = false;
        bare.jit.emplace(c);
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
        s.sp = (0x3000000000 + Rand(0, 0xfffffff)) & ~u64(15);
        s.pstate = u32(Rand(0, 15)) << 28;

        const Result a = on.Run(code, s), b = off.Run(code, s);
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
    std::printf("\n%llu iterations: the passes wrong %llu, without them wrong %llu, unclear %llu; %zu kinds\n",
                (unsigned long long)iterations, (unsigned long long)on_wrong, (unsigned long long)off_wrong,
                (unsigned long long)other, kinds.size());
    return 0;
}
