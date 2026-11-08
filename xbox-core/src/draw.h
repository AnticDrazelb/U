// The Kelvin state and the software renderer's parts (render.cpp, draw.cpp).
#pragma once

#include <array>

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
    bool swizzled = false;
};
Surface ColorSurface();
Surface ZetaSurface();
void WritePixel(const Surface& s, u32 x, u32 y, u32 argb, u32 mask = 0xffffffff);
u32 ReadPixel(const Surface& s, u32 x, u32 y);

// draw.cpp: vertices, the vertex program, rasterization and the combiners
void DrawMethod(u32 method, u32 parameter);

// pixel.cpp: a pixel's colour from its interpolated inputs (textures, the register combiners, the
// alpha test and blending with what `target` holds); false when the alpha test discards it
struct PixelInputs
{
    float v[16][4] = {}; // indexed like the vertex outputs: 3 diffuse, 4 specular, 9-12 texture coordinates
};
bool ShadePixel(const PixelInputs& in, const Surface& target, u32 x, u32 y, u32& out);

} // namespace xb
