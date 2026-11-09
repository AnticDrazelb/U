// The Kelvin state and the software renderer's parts (render.cpp, draw.cpp).
#pragma once

#include <array>
#include <functional>

#include "core.h"

namespace xb {

struct KelvinState
{
    u32 regs[0x2000 / 4] = {};
    u32 Get(u32 method) const { return regs[method / 4]; }
    float F(u32 method) const
    {
        float f;
        std::memcpy(&f, &regs[method / 4], 4);
        return f;
    }
};
extern KelvinState g_kelvin;

struct Surface
{
    u32 address = 0, pitch = 0, width = 0, height = 0, bytes = 4, format = 0;
    bool swizzled = false; // then width and height are powers of two, pixels in the NV2A's swizzle order
};
// the NV2A's swizzle: the bits of x and y interleaved (x first) while both have them, then the
// larger one's remaining bits. In closed form: the low bits spread apart (Morton order), the rest on top.
inline u32 SpreadBits(u32 v) // bit i to bit 2i (16 bits)
{
    v &= 0xffff;
    v = (v | (v << 8)) & 0x00ff00ff;
    v = (v | (v << 4)) & 0x0f0f0f0f;
    v = (v | (v << 2)) & 0x33333333;
    v = (v | (v << 1)) & 0x55555555;
    return v;
}
inline u32 Swizzle(u32 x, u32 y, u32 width, u32 height)
{
    const u32 wb = width > 1 ? 31 - __builtin_clz(width - 1) + 1 : 0;   // bits of x
    const u32 hb = height > 1 ? 31 - __builtin_clz(height - 1) + 1 : 0; // bits of y
    const u32 m = wb < hb ? wb : hb, low = (1u << m) - 1;
    const u32 interleaved = SpreadBits(x & low) | (SpreadBits(y & low) << 1);
    const u32 rest = wb > hb ? (x & ((1u << wb) - 1)) >> m : (y & ((1u << hb) - 1)) >> m;
    return interleaved | (rest << (2 * m));
}
// the 3D swizzle: x, y and z bits interleaved in turn while each has them
inline u32 Swizzle3(u32 x, u32 y, u32 z, u32 width, u32 height, u32 depth)
{
    u32 offset = 0, bit = 0;
    for (u32 i = 1; i < width || i < height || i < depth; i <<= 1)
    {
        if (i < width)
            offset |= (x & i ? 1u : 0u) << bit++;
        if (i < height)
            offset |= (y & i ? 1u : 0u) << bit++;
        if (i < depth)
            offset |= (z & i ? 1u : 0u) << bit++;
    }
    return offset;
}
inline u32 PixelAddress(const Surface& s, u32 x, u32 y)
{
    return s.swizzled ? s.address + Swizzle(x, y, s.width, s.height) * s.bytes : s.address + y * s.pitch + x * s.bytes;
}
void AntiAliasing(u32& x, u32& y); // the surface's samples per pixel across and down
Surface ColorSurface();
Surface ZetaSurface();
void WritePixel(const Surface& s, u32 x, u32 y, u32 argb, u32 mask = 0xffffffff);
u32 ReadPixel(const Surface& s, u32 x, u32 y);
bool WindowClipRect(int& x0, int& y0, int& x1, int& y1); // the window clip as one rectangle, if it is one
u32 FixedBits(u32 format, u32 value); // a colour's unused bits as the surface format stores them

// draw.cpp: runs fn(0) .. fn(parts - 1) on the rasterizer's threads; how many it has
void RunParts(int parts, const std::function<void(int)>& fn);
int RasterThreads();

// draw.cpp: vertices, the vertex program, rasterization and the combiners
void DrawMethod(u32 method, u32 parameter);
// the occlusion count (SET_ZPASS_PIXEL_COUNT_ENABLE): pixels that passed every test while it is on,
// read by GET_REPORT; while it is on, draws are counted in software (exactly)
extern std::atomic<bool> g_zpass_enabled;
extern std::atomic<u64> g_zpass_count;

// pixel.cpp: a pixel's colour from its interpolated inputs (textures, the register combiners, the
// alpha test and blending with what `target` holds); false when the alpha test discards it
struct PixelInputs
{
    float v[16][4]; // only what PixelInputMask names is set; indexed like the vertex outputs: 3 diffuse, 4 specular, 9-12 texture coordinates
    float lod[4];   // each texture stage's level of detail (log2 of texels a pixel), worked out per triangle
};
// A stage's level of detail from its coordinates' screen derivatives (as given to the stage).
float TextureLod(int stage, float dudx, float dvdx, float dudy, float dvdy);
void PreparePixels(); // the registers ShadePixel reads, decoded for a draw
// The prepared state for the GPU path (gpu.h): false if it uses what the GPU path doesn't do yet.
namespace gpu { struct Uniforms; struct TextureInfo; struct BlendState; }
bool GpuPixelSetup(gpu::Uniforms& u, gpu::TextureInfo tex[4], gpu::BlendState& blend);
// A texture stage's texels, decoded as the software path reads them: level `level` (of face `face`
// for a cube map), width >> level by height >> level ARGB words.
void DecodeStageTexture(int stage, u32 face, u32 level, u32* argb);
u32 PixelInputMask();  // which of PixelInputs' v[] the prepared state reads (bit per output)
bool ShadePixel(const PixelInputs& in, const Surface& target, u32 x, u32 y, u32& out);

} // namespace xb
