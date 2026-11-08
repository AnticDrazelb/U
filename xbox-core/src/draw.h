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
// the NV2A's swizzle: the bits of x and y interleaved (x first) while both have them
inline u32 Swizzle(u32 x, u32 y, u32 width, u32 height)
{
    u32 offset = 0, bit = 0;
    for (u32 i = 1; i < width || i < height; i <<= 1)
    {
        if (i < width)
            offset |= (x & i ? 1u : 0u) << bit++;
        if (i < height)
            offset |= (y & i ? 1u : 0u) << bit++;
    }
    return offset;
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

// draw.cpp: runs fn(0) .. fn(parts - 1) on the rasterizer's threads; how many it has
void RunParts(int parts, const std::function<void(int)>& fn);
int RasterThreads();

// draw.cpp: vertices, the vertex program, rasterization and the combiners
void DrawMethod(u32 method, u32 parameter);

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
u32 PixelInputMask();  // which of PixelInputs' v[] the prepared state reads (bit per output)
bool ShadePixel(const PixelInputs& in, const Surface& target, u32 x, u32 y, u32& out);

} // namespace xb
