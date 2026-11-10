// The NV2A's 3D engine (Kelvin, class 0x97), drawn in software into guest memory: the surfaces the
// driver set up, clears, and (draw.cpp) primitives through the vertex program and the combiners.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core.h"
#include "diagnostics.h"
#include "draw.h"
#include "gpu.h"
#include "nv2a.h"

namespace xb {

KelvinState g_kelvin;

namespace {

// surface formats (NV097_SET_SURFACE_FORMAT_COLOR): bytes per pixel
u32 ColorBytes(u32 format)
{
    switch (format)
    {
    case 0x1: case 0x2: case 0x3: return 2; // X1R5G5B5 (two), R5G6B5
    case 0x9: return 1;                     // B8
    case 0xa: return 2;                     // G8B8
    default: return 4;                      // X8R8G8B8, A8R8G8B8, ...
    }
}

} // namespace

// SET_SURFACE_FORMAT's anti-aliasing (bits 12-15): 1 CENTER_CORNER_2 stores two samples across a
// pixel, 2 SQUARE_OFFSET_4 two by two; the surface is that much bigger and drawing is scaled into it
void AntiAliasing(u32& x, u32& y)
{
    switch ((g_kelvin.Get(0x208) >> 12) & 0xf)
    {
    case 1: x = 2, y = 1; return;
    case 2: x = 2, y = 2; return;
    default: x = 1, y = 1; return;
    }
}

Surface ColorSurface()
{
    const KelvinState& k = g_kelvin;
    Surface s;
    const u32 format = k.Get(0x208);
    s.format = format & 0xf;
    s.bytes = ColorBytes(s.format);
    s.pitch = k.Get(0x20c) & 0xffff;
    u32 ax, ay;
    AntiAliasing(ax, ay);
    s.width = ((k.Get(0x200) >> 16) + (k.Get(0x200) & 0xffff)) * ax;
    s.height = ((k.Get(0x204) >> 16) + (k.Get(0x204) & 0xffff)) * ay;
    s.swizzled = ((format >> 8) & 0xf) == 2;
    if (s.swizzled) // SET_SURFACE_FORMAT's log2 width and height
    {
        s.width = 1u << ((format >> 16) & 0xff);
        s.height = 1u << ((format >> 24) & 0xff);
        s.pitch = s.width * s.bytes;
    }
    const u32 base = Nv2aDmaAddress(k.Get(0x194));
    s.address = base ? base + k.Get(0x210) : 0;
    return s;
}

Surface ZetaSurface()
{
    const KelvinState& k = g_kelvin;
    Surface s;
    const u32 zformat = (k.Get(0x208) >> 4) & 0xf; // 1: Z16, 2: Z24S8
    s.format = zformat;
    s.bytes = zformat == 1 ? 2 : 4;
    s.pitch = k.Get(0x20c) >> 16;
    u32 ax, ay;
    AntiAliasing(ax, ay);
    s.width = ((k.Get(0x200) >> 16) + (k.Get(0x200) & 0xffff)) * ax;
    s.height = ((k.Get(0x204) >> 16) + (k.Get(0x204) & 0xffff)) * ay;
    const u32 format = k.Get(0x208);
    s.swizzled = ((format >> 8) & 0xf) == 2;
    if (s.swizzled)
    {
        s.width = 1u << ((format >> 16) & 0xff);
        s.height = 1u << ((format >> 24) & 0xff);
        s.pitch = s.width * s.bytes;
    }
    const u32 base = Nv2aDmaAddress(k.Get(0x198));
    s.address = base && zformat ? base + k.Get(0x214) : 0;
    return s;
}

// a color as 0xAARRGGBB, written in the surface's format
// The bits a colour format leaves unused ("X") are stored as zeros (the _Z formats) or ones (_O):
// format 1 X1R5G5B5_Z, 2 _O, 4 X8R8G8B8_Z, 5 _O, 6 X1A7R8G8B8_Z, 7 _O.
u32 FixedBits(u32 format, u32 value)
{
    switch (format)
    {
    case 0x1: return value & 0x7fff;
    case 0x2: return value | 0x8000;
    case 0x4: return value & 0x00ffffff;
    case 0x5: return value | 0xff000000;
    case 0x6: return value & 0x7fffffff;
    case 0x7: return value | 0x80000000;
    }
    return value;
}

void WritePixel(const Surface& s, u32 x, u32 y, u32 argb, u32 mask)
{
    const u32 p = PixelAddress(s, x, y);
    if (s.bytes == 4)
    {
        const u32 old = Read<u32>(p);
        if (s.format == 0x6 || s.format == 0x7) // X1A7R8G8B8: alpha's top 7 bits in bits 24-30
            argb = (argb & 0x00ffffffu) | ((argb >> 25) << 24), mask = (mask & 0x00ffffffu) | (mask & 0xff000000u ? 0x7f000000u : 0);
        Write<u32>(p, FixedBits(s.format, (old & ~mask) | (argb & mask)));
    }
    else if (s.format == 0x3) // R5G6B5
        Write<u16>(p, u16(((argb >> 8) & 0xf800) | ((argb >> 5) & 0x07e0) | ((argb >> 3) & 0x001f)));
    else if (s.format == 0xa) // G8B8: green high, blue low
    {
        const u16 old = Read<u16>(p), m = u16(((mask >> 8) & 0xff00) | (mask & 0xff));
        Write<u16>(p, u16((old & ~m) | ((((argb >> 8) & 0xff) << 8 | (argb & 0xff)) & m)));
    }
    else if (s.bytes == 2)
        Write<u16>(p, u16(FixedBits(s.format, ((argb >> 9) & 0x7c00) | ((argb >> 6) & 0x03e0) | ((argb >> 3) & 0x001f))));
    else
        Write<u8>(p, u8(argb));
}

u32 ReadPixel(const Surface& s, u32 x, u32 y)
{
    const u32 p = PixelAddress(s, x, y);
    if (s.bytes == 4)
    {
        // a format without alpha reads alpha 1 whatever its X bits hold; X1A7R8G8B8's 7 bits widened
        // (nxdk_pgraph_tests' Blend_surface captures: blending by the destination's alpha)
        const u32 v = Read<u32>(p);
        if (s.format == 0x4 || s.format == 0x5)
            return v | 0xff000000u;
        if (s.format == 0x6 || s.format == 0x7)
        {
            const u32 a7 = (v >> 24) & 0x7f;
            return (v & 0x00ffffffu) | ((a7 << 1) | (a7 >> 6)) << 24;
        }
        return v;
    }
    if (s.format == 0xa) // G8B8
    {
        const u16 v = Read<u16>(p);
        return 0xff000000u | u32(v >> 8) << 8 | u32(v & 0xff);
    }
    if (s.format == 0x3)
    {
        const u16 v = Read<u16>(p);
        return 0xff000000u | u32((v >> 11) << 3) << 16 | u32(((v >> 5) & 63) << 2) << 8 | u32((v & 31) << 3);
    }
    if (s.bytes == 2)
    {
        const u16 v = Read<u16>(p);
        return 0xff000000u | u32(((v >> 10) & 31) << 3) << 16 | u32(((v >> 5) & 31) << 3) << 8 | u32((v & 31) << 3);
    }
    return Read<u8>(p) * 0x010101u | 0xff000000u;
}

namespace {

void Clear(u32 flags)
{
    const KelvinState& k = g_kelvin;
    const u32 h = k.Get(0x1d98), v = k.Get(0x1d9c);
    u32 ax, ay;
    AntiAliasing(ax, ay);
    // the clear's rectangle, within the surface clip (SET_SURFACE_CLIP_HORIZONTAL/VERTICAL: nothing
    // outside it is touched, as nxdk_pgraph_tests' Surface_clip captures show)
    const u32 cx = k.Get(0x200), cy = k.Get(0x204);
    const u32 x0 = std::max((h & 0xffff) * ax, (cx & 0xffff) * ax), y0 = std::max((v & 0xffff) * ay, (cy & 0xffff) * ay);
    const u32 x1 = std::min(((h >> 16) + 1) * ax, ((cx & 0xffff) + (cx >> 16)) * ax) - 1;
    const u32 y1 = std::min(((v >> 16) + 1) * ay, ((cy & 0xffff) + (cy >> 16)) * ay) - 1;
    if (int(x1) < int(x0) || int(y1) < int(y0))
        return;
    if (g_verbose >= 2)
    {
        const Surface s = ColorSurface();
        Log("nv2a: clear %#x, rect %u-%u x %u-%u, color %#x; surface %#x pitch %u %ux%u format %#x", flags, x0, x1, y0,
            y1, k.Get(0x1d90), s.address, s.pitch, s.width, s.height, s.format);
    }
    if (flags & 0xf0)
    {
        const Surface s = ColorSurface();
        if (s.address && s.pitch)
        {
            u32 mask = 0;
            if (flags & 0x10) mask |= 0x00ff0000; // R
            if (flags & 0x20) mask |= 0x0000ff00; // G
            if (flags & 0x40) mask |= 0x000000ff; // B
            if (flags & 0x80) mask |= 0xff000000; // A
            const u32 color = k.Get(0x1d90);
            const u32 rows = y1 < y0 ? 0 : std::min(y1, s.height - 1) - y0 + 1;
            const int parts = rows * (x1 - x0 + 1) > 16384 ? RasterThreads() : 1;
            // a 16-bit surface takes the clear value's low half as it is (its own R, G, B fields)
            u16 mask16 = 0;
            if (s.format == 0xa) // G8B8: green the high byte, blue the low
            {
                if (flags & 0x20) mask16 |= 0xff00;
                if (flags & 0x40) mask16 |= 0x00ff;
            }
            else if (s.bytes == 2)
            {
                const bool r565 = s.format == 0x3;
                if (flags & 0x10) mask16 |= r565 ? 0xf800 : 0x7c00;
                if (flags & 0x20) mask16 |= r565 ? 0x07e0 : 0x03e0;
                if (flags & 0x40) mask16 |= 0x001f;
            }
            RunParts(parts, [&](int part) {
                for (u32 y = y0 + u32(part); y <= y1 && y < s.height; y += u32(parts))
                    for (u32 x = x0; x <= x1 && x < s.width; x++)
                        if (s.bytes == 2)
                        {
                            const u32 p = PixelAddress(s, x, y);
                            Write<u16>(p, u16(FixedBits(s.format, (Read<u16>(p) & ~mask16) | (color & mask16))));
                        }
                        else
                            WritePixel(s, x, y, color, mask);
            });
        }
    }
    if (flags & 0x3)
    {
        const Surface z = ZetaSurface();
        if (z.address && z.pitch)
        {
            const u32 value = k.Get(0x1d8c);
            static const bool debug_stencil = std::getenv("XB_DEBUG_STENCIL") != nullptr;
            if (debug_stencil)
                Log("debug: zeta clear flags %#x value %#x rect %u..%u x %u..%u", flags, value, x0, x1, y0, y1);
            const int parts = (y1 - y0 + 1) * (x1 - x0 + 1) > 16384 ? RasterThreads() : 1;
            RunParts(parts, [&](int part) {
            for (u32 y = y0 + u32(part); y <= y1 && y < z.height; y += u32(parts))
                for (u32 x = x0; x <= x1 && x < z.width; x++)
                {
                    const u32 p = PixelAddress(z, x, y);
                    if (z.bytes == 2)
                        Write<u16>(p, u16(value));
                    else
                    {
                        u32 mask = 0;
                        if (flags & 1) mask |= 0xffffff00;
                        if (flags & 2) mask |= 0x000000ff;
                        Write<u32>(p, (Read<u32>(p) & ~mask) | (value & mask));
                    }
                }
            });
        }
    }
}

} // namespace

void KelvinMethod(u32 method, u32 parameter)
{
    KelvinState& k = g_kelvin;
    // SET_LINE_WIDTH past its largest (63.875, in eighths) isn't taken: the console's captures of
    // 64.0 to 64.7 and 0xffffffff draw lines as the width before (1 there)
    if (method == 0x380 && parameter > 0x1ff)
        return;
    if (method < 0x2000)
        k.regs[method / 4] = parameter;
    switch (method)
    {
    case 0x1d94: // CLEAR_SURFACE: on the GPU's copies of the surfaces when the GPU draws
        if (gpu::Enabled())
        {
            if (gpu::Clear(parameter))
                break;
            gpu::Sync();
        }
        Clear(parameter);
        break;
    case 0x17c8: // CLEAR_REPORT_VALUE: the occlusion count to 0
        g_zpass_count = 0;
        break;
    case 0x17cc: // SET_ZPASS_PIXEL_COUNT_ENABLE
        if (parameter && gpu::Enabled())
            gpu::Sync(); // what the GPU drew, in memory before software draws on it
        g_zpass_enabled = parameter != 0;
        break;
    case 0x17d0: // GET_REPORT: the count, at the offset in the report DMA (a 64-bit timestamp, the
                 // count, then 0), as xemu documents it
        if (const u32 base = Nv2aDmaAddress(k.Get(0x1a8)))
        {
            const u32 at = base + (parameter & 0x00ffffff);
            const u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
            Write<u32>(at, u32(ns));
            Write<u32>(at + 4, u32(ns >> 32));
            Write<u32>(at + 8, u32(std::min<u64>(g_zpass_count.load(), 0xffffffffu)));
            Write<u32>(at + 12, 0);
        }
        break;
    case 0x1d70: // BACK_END_WRITE_SEMAPHORE_RELEASE: a value written where the driver waits for it
        if (const u32 base = Nv2aDmaAddress(k.Get(0x1a4)))
            Write<u32>(base + k.Get(0x1d6c), parameter);
        break;
    default:
        DrawMethod(method, parameter);
    }
}

// ---- the 2D engines: context surfaces (0x62: source and destination, format, pitches), beta
// (0x12: the blend factor), image blit (0x9f: a rectangle from source to destination)

namespace {

struct Surfaces2D
{
    u32 dma_source = 0, dma_dest = 0, format = 0, source_pitch = 0, dest_pitch = 0, source_offset = 0, dest_offset = 0;
} g_surfaces2d;
u32 g_beta = 0;
struct ImageBlit
{
    u32 operation = 3, in_x = 0, in_y = 0, out_x = 0, out_y = 0;
    bool clipped = false; // bound to a clip rectangle object (class 0x19)
} g_blit;
// the clip rectangle object (0x19): SET_POINT, SET_SIZE
u32 g_clip_x = 0, g_clip_y = 0, g_clip_w = 0x7fff, g_clip_h = 0x7fff;

void Blit(u32 width, u32 height)
{
    if (gpu::Enabled())
        gpu::Sync(); // the surfaces' current content, in memory
    const Surfaces2D& s = g_surfaces2d;
    u32 bpp;
    switch (s.format)
    {
    case 0x01: bpp = 1; break;          // Y8
    case 0x04: bpp = 2; break;          // R5G6B5
    case 0x06: case 0x07: case 0x0a: case 0x0b: bpp = 4; break; // X8R8G8B8 (two), A8R8G8B8, Y32
    default:
        Log("nv2a: blit from surfaces of format %#x isn't done", s.format);
        return;
    }
    const u32 source_base = Nv2aDmaAddress(s.dma_source), dest_base = Nv2aDmaAddress(s.dma_dest);
    if (!source_base || !dest_base)
        return;
    // bound to a clip rectangle: only the destination pixels inside it (the console's captures,
    // nxdk_pgraph_tests' ImgBlt_Clip)
    u32 in_x = g_blit.in_x, in_y = g_blit.in_y, out_x = g_blit.out_x, out_y = g_blit.out_y;
    if (g_blit.clipped)
    {
        const u32 x0 = std::max(out_x, g_clip_x), y0 = std::max(out_y, g_clip_y);
        const u32 x1 = std::min(out_x + width, g_clip_x + g_clip_w), y1 = std::min(out_y + height, g_clip_y + g_clip_h);
        if (x1 <= x0 || y1 <= y0)
            return;
        in_x += x0 - out_x, in_y += y0 - out_y, out_x = x0, out_y = y0, width = x1 - x0, height = y1 - y0;
    }
    const u32 source = source_base + s.source_offset + in_y * s.source_pitch + in_x * bpp;
    const u32 dest = dest_base + s.dest_offset + out_y * s.dest_pitch + out_x * bpp;
    const u32 row_bytes = width * bpp;
    if (g_verbose >= 2)
        Log("nv2a: blit %ux%u from %#x (pitch %u) to %#x (pitch %u), operation %u", width, height, source, s.source_pitch,
            dest, s.dest_pitch, g_blit.operation);
    // rows in the order that survives an overlap
    const bool bottom_up = dest > source;
    for (u32 n = 0; n < height; n++)
    {
        const u32 y = bottom_up ? height - 1 - n : n;
        u8* d = Ptr<u8>(dest + y * s.dest_pitch);
        const u8* src = Ptr<u8>(source + y * s.source_pitch);
        if (g_blit.operation == 2 && bpp == 4) // BLEND_AND: by beta (eight bits of fraction)
        {
            const u32 beta = g_beta >> 16, max = 0x7f80;
            for (u32 x = 0; x < width; x++)
                for (u32 c = 0; c < 3; c++)
                    d[x * 4 + c] = u8((src[x * 4 + c] * beta + d[x * 4 + c] * (max - beta)) / max);
        }
        else
            std::memmove(d, src, row_bytes);
        if (s.format == 0x07 || s.format == 0x06) // X8R8G8B8: alpha 0xff; with Z8: 0
            for (u32 x = 0; x < width; x++)
                d[x * 4 + 3] = s.format == 0x07 ? 0xff : 0;
    }
}

} // namespace

void RenderOtherClass(u32 cls, u32 method, u32 parameter)
{
    switch (cls)
    {
    case 0x62: // context surfaces 2D
        switch (method)
        {
        case 0x184: g_surfaces2d.dma_source = parameter; return;
        case 0x188: g_surfaces2d.dma_dest = parameter; return;
        case 0x300: g_surfaces2d.format = parameter; return;
        case 0x304: g_surfaces2d.source_pitch = parameter & 0xffff, g_surfaces2d.dest_pitch = parameter >> 16; return;
        case 0x308: g_surfaces2d.source_offset = parameter & 0x07ffffff; return;
        case 0x30c: g_surfaces2d.dest_offset = parameter & 0x07ffffff; return;
        }
        break;
    case 0x12: // beta
        if (method == 0x300)
            g_beta = parameter & 0x80000000 ? 0 : parameter & 0x7f800000;
        if (method == 0x300 || method == 0)
            return;
        break;
    case 0x19: // clip rectangle
        if (method == 0x300)
        {
            g_clip_x = parameter & 0xffff, g_clip_y = parameter >> 16;
            return;
        }
        if (method == 0x304)
        {
            g_clip_w = parameter & 0xffff, g_clip_h = parameter >> 16;
            return;
        }
        break;
    case 0x9f: // image blit
        switch (method)
        {
        case 0x188: g_blit.clipped = Nv2aObjectClass(parameter) == 0x19; return; // SET_CONTEXT_CLIP_RECTANGLE
        case 0x19c: return; // its context surfaces: the one 0x62 object
        case 0x2fc: g_blit.operation = parameter; return;
        case 0x300: g_blit.in_x = parameter & 0xffff, g_blit.in_y = parameter >> 16; return;
        case 0x304: g_blit.out_x = parameter & 0xffff, g_blit.out_y = parameter >> 16; return;
        case 0x308:
            if ((parameter & 0xffff) && (parameter >> 16))
                Blit(parameter & 0xffff, parameter >> 16);
            return;
        }
        break;
    }
    static bool reported[256][0x800 / 4] = {};
    if (method && method < 0x800 && !reported[cls & 0xff][method / 4])
    {
        reported[cls & 0xff][method / 4] = true;
        char what[64];
        std::snprintf(what, sizeof(what), "class %#x method %#x", cls, method);
        diag::Note("GPU method", what);
    }
}

} // namespace xb
