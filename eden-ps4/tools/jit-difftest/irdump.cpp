// The 64-bit translator's IR for a few A64 instructions, with the IR passes (as the PS4's CPU Auto)
// and without: jit-irdump <hex instruction>...
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opt_passes.h"
#include "dynarmic/tests/A64/testenv.h"

using namespace Dynarmic;

int main(int argc, char** argv) {
    std::vector<u32> code;
    for (int i = 1; i < argc; i++)
        code.push_back(u32(std::strtoul(argv[i], nullptr, 16)));
    code.push_back(0x14000000);
    for (const bool passes : {true, false}) {
        A64TestEnv env;
        env.code_mem = code;
        env.code_mem_start_address = 0x1000;
        A64::UserConfig conf{};
        conf.callbacks = &env;
        conf.optimizations = all_safe_optimizations | OptimizationFlag::Unsafe_UnfuseFMA | OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        if (!passes)
            conf.optimizations &= ~(OptimizationFlag::GetSetElimination | OptimizationFlag::ConstProp | OptimizationFlag::MiscIROpt);
        conf.unsafe_optimizations = true;
        const A64::LocationDescriptor location{0x1000, {}};
        IR::Block block{location};
        A64::TranslationOptions options{};
        options.define_unpredictable_behaviour = true;
        A64::Translate(block, location, [&](u64 vaddr) { return env.MemoryReadCode(vaddr); }, options);
        Optimization::Optimize(block, conf, {});
        std::printf("===== passes %s\n%s\n", passes ? "ON" : "off", IR::DumpBlock(block).c_str());
    }
}
