// The NV2A's drawing on the host's GPU, through Vulkan (gpu_vulkan.cpp): RADV on the PS4, any driver
// (lavapipe for tests) on the desktop. The CPU still does what is per vertex (vertex programs, fog,
// clipping, culling, flat and two-sided colours); the GPU rasterizes, runs the texture stages and the
// register combiners (one shader that reads the NV2A's state from a uniform buffer: SPIR-V compiled at
// build time, nothing compiled at run time), tests depth and blends. Surfaces live in GPU images and are
// written back to the guest's memory where the guest or the CPU side may look: each vblank (the
// picture), before the CPU draws or clears or blits, and before a texture reads one. Whatever the GPU
// path doesn't do yet is drawn by the software renderer, after the surfaces are written back.
#pragma once

#include <cstdint>
#include <string>

#include "core.h"

namespace xb::gpu {

// A vertex as the rasterizer gets it: screen position (x, y in surface pixels, z in the depth
// buffer's units, w), the colours (after flat and two-sided shading), the fog factor (x) and the four
// texture coordinate sets.
struct Vertex
{
    float pos[4];
    float diffuse[4];
    float specular[4];
    float fog[4];
    float tex[4][4];
};

// The fragment shader's uniform block (std140: everything in 16-byte rows), filled from the NV2A's
// registers by pixel.cpp (GpuPixelSetup) as it decodes them for the software path.
struct Uniforms
{
    u32 stage_words[8][4]; // icw, ocw, aicw, aocw
    float stage_c0[8][4], stage_c1[8][4];
    u32 control[4];     // stages, cw0, cw1, flags (1 uses ef, 2 uses sum, 4 alpha test)
    u32 stage_flags[4]; // a byte a stage: bit 0 rgb writes, bit 1 alpha writes
    u32 alpha_test[4];  // function, reference
    float fog_color[4], final_c0[4], final_c1[4];
    u32 program[4], enabled[4], stage_input[4], kind[4];
    float tex_scale[4][4], bump[4][4], bump_lum[4][4];
    u32 clip_mode[4];
    float depth[4]; // 1 / the depth's largest value, polygon offset bias, factor, 1 with the w-buffer
    u32 key[4];      // each stage's SET_COLOR_KEY_COLOR
    u32 key_mode[4]; // each stage's colour key mode (bits 0-1), alpha kill (bit 2), the key without alpha (bit 3)
    u32 dot_map[4];  // each stage's SET_DOT_RGBMAPPING conversion
    float eye[4];    // SET_EYE_VECTOR (dot reflect specular const)
};
// A texture stage's texture as the GPU path needs it
struct TextureInfo
{
    bool used = false;
    u32 address = 0, bytes = 0, palette = 0, format = 0, width = 1, height = 1, levels = 1, pitch = 0;
    bool cube = false, normalized = true;
    u32 wrap = 0, filter = 0, border = 0;
    float lod_bias = 0, min_lod = 0, max_lod = 0;
};
// Blending, as the colour output stage does it
struct BlendState
{
    bool enabled = false;
    u32 sfactor = 0, dfactor = 0, equation = 0;
    u32 color = 0; // ARGB
};

// XB_GPU=1 (or the PS4's setting): whether drawing goes to the GPU. Initializes on first use; if
// that fails it says why once and stays off.
bool Enabled();
void SetWanted(bool on); // the PS4's setting, before the first draw
void SetGuardFile(const std::string& path); // the console's step file (a start that dies is skipped next time)
const char* Status();    // for the log and the diagnostics: the device, or why not
// the draws left to software since the last call, by reason ("" if none): for the stats line
std::string DeclinedSince();

// Draws `count` triangles (3 vertices each, already clipped and culled) with the current NV2A state.
// False: this state isn't done on the GPU yet; the caller draws them in software (after Sync).
bool DrawTriangles(const Vertex* vertices, u32 count);
// CLEAR_SURFACE on the GPU's copy of the surfaces. False: the caller clears in software (after Sync).
bool Clear(u32 flags);
// Everything the GPU drew, written back to the guest's memory; the GPU copies re-read from memory if
// it changed before they are drawn into again. Called each vblank and before any CPU-side access.
void Sync();
// Before the CPU side reads [address, address + size) (a texture made from a surface): that
// surface, if the GPU has drawn into it, written back.
void SyncRange(u32 address, u32 size);

} // namespace xb::gpu
