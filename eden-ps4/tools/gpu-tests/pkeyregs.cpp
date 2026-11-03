// (04.15) Which Maxwell 3D registers Vulkan::FixedPipelineState::Refresh reads (the pipeline key's):
// built against Eden's own fixed_pipeline_state.cpp by pkeyregs.sh. Two tests:
//  1. every bit of every register flipped from N random register files (zero, small, one-bit and
//     random values; random topology, engine state, device features, the squashed-blend setting):
//     a register any flip of which changed the key is in the set (the printed ranges, which go to
//     video_core/renderer_vulkan/pipeline_key_registers.h);
//  2. with the set's registers kept, every other register re-randomized 20 times from each of T new
//     register files: the key must never change. A third argument drops one register from the set,
//     which test 2 must then catch (it does: 0x1d1, 0x204, 0x3fa, 0x4d8, 0x700 tried).
// The engine is a zeroed Maxwell3D's storage (Refresh reads only its registers, draw topology,
// engine state and dirty flags; the dirty flags Refresh consumes are set before each call).
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
bool g_squashed_iterated_blend = false;
using Maxwell3D = Tegra::Engines::Maxwell3D;
using namespace Vulkan;
alignas(Maxwell3D) static unsigned char storage[sizeof(Maxwell3D)];
static Maxwell3D& M = *reinterpret_cast<Maxwell3D*>(storage);
static void SetAllDirty() {
    M.dirty.flags[Dirty::VertexInput] = true;
    M.dirty.flags[Dirty::ViewportSwizzles] = true;
    M.dirty.flags[Dirty::Blending] = true;
}
static void Key(FixedPipelineState& k, DynamicFeatures& f) {
    std::memset(&k, 0, sizeof k);
    SetAllDirty();
    k.Refresh(M, f);
}
int main(int argc, char** argv) {
    constexpr size_t N = Maxwell3D::Regs::NUM_REGS;
    const int bases = argc > 1 ? atoi(argv[1]) : 200;
    std::vector<unsigned> hits(N, 0);
    std::mt19937_64 rng(12345);
    auto rnd = [&](uint64_t n) { return rng() % n; };
    for (int b = 0; b < bases; b++) {
        DynamicFeatures f{};
        f.driver_id = 3; f.driver_version = 1;
        bool* flags[] = {&f.has_extended_dynamic_state, &f.has_extended_dynamic_state_2,
            &f.has_extended_dynamic_state_2_logic_op, &f.has_extended_dynamic_state_2_patch_control_points,
            &f.has_extended_dynamic_state_3_blend, &f.has_extended_dynamic_state_3_enables,
            &f.has_dynamic_state3_depth_clamp_enable, &f.has_dynamic_state3_logic_op_enable,
            &f.has_dynamic_state3_line_stipple_enable, &f.has_dynamic_vertex_input, &f.has_color_write_enable,
            &f.has_provoking_vertex, &f.has_provoking_vertex_first_mode, &f.has_provoking_vertex_last_mode,
            &f.has_provoking_vertex_tf_preserve};
        for (bool* p : flags) *p = rnd(2);
        g_squashed_iterated_blend = rnd(2);
        u32* regs = M.regs.reg_array.data();
        for (size_t r = 0; r < N; r++) {
            const uint64_t kind = rnd(10);
            regs[r] = kind < 4 ? 0 : kind < 7 ? u32(rnd(16)) : kind < 8 ? (1u << rnd(32)) : u32(rng());
        }
        M.draw_manager.draw_state.topology = static_cast<Maxwell3D::Regs::PrimitiveTopology>(rnd(15));
        M.engine_state = static_cast<Maxwell3D::EngineHint>(rnd(2));
        FixedPipelineState base, k;
        Key(base, f);
        for (size_t r = 0; r < N; r++) {
            const u32 saved = regs[r];
            for (int bit = 0; bit < 32; bit++) {
                regs[r] = saved ^ (1u << bit);
                Key(k, f);
                if (!(k == base)) { hits[r]++; break; }
            }
            regs[r] = saved;
        }
    }
    // (2) the others never do: keep the key's registers, give every other one a new random value
    // (zero, small, one bit, any), many times, from new register files: the key must not change
    if (argc > 3) {  // mutation: drop one register from the set (the test must catch it)
        hits[strtoul(argv[3], nullptr, 0)] = 0;
    }
    if (argc > 2) {
        const int trials = atoi(argv[2]);
        int changed = 0;
        for (int t = 0; t < trials; t++) {
            DynamicFeatures f{};
            bool* flags[] = {&f.has_extended_dynamic_state, &f.has_extended_dynamic_state_2,
                &f.has_extended_dynamic_state_2_logic_op, &f.has_extended_dynamic_state_2_patch_control_points,
                &f.has_extended_dynamic_state_3_blend, &f.has_extended_dynamic_state_3_enables,
                &f.has_dynamic_state3_depth_clamp_enable, &f.has_dynamic_state3_logic_op_enable,
                &f.has_dynamic_state3_line_stipple_enable, &f.has_dynamic_vertex_input, &f.has_color_write_enable,
                &f.has_provoking_vertex, &f.has_provoking_vertex_first_mode, &f.has_provoking_vertex_last_mode,
                &f.has_provoking_vertex_tf_preserve};
            for (bool* p : flags) *p = rnd(2);
            g_squashed_iterated_blend = rnd(2);
            u32* regs = M.regs.reg_array.data();
            auto fill = [&](bool key_too) {
                for (size_t r = 0; r < N; r++) {
                    if (hits[r] && !key_too) continue;
                    const uint64_t kind = rnd(10);
                    regs[r] = kind < 4 ? 0 : kind < 7 ? u32(rnd(16)) : kind < 8 ? (1u << rnd(32)) : u32(rng());
                }
            };
            fill(true);
            M.draw_manager.draw_state.topology = static_cast<Maxwell3D::Regs::PrimitiveTopology>(rnd(15));
            M.engine_state = static_cast<Maxwell3D::EngineHint>(rnd(2));
            FixedPipelineState base, k;
            Key(base, f);
            for (int i = 0; i < 20; i++) {
                fill(false);
                Key(k, f);
                if (!(k == base)) { changed++; break; }
            }
        }
        printf("other registers re-randomized: %d of %d register files changed the key\n", changed, trials);
    }
    // ranges of registers that ever changed the key
    size_t count = 0;
    for (size_t r = 0; r < N;) {
        if (!hits[r]) { r++; continue; }
        size_t e = r;
        while (e < N && hits[e]) e++;
        printf("0x%03zx-0x%03zx (%zu)\n", r, e - 1, e - r);
        count += e - r;
        r = e;
    }
    printf("registers: %zu\n", count);
}
