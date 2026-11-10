// A pixel's colour: the texture stages, the register combiners (the NV2A's pixel shader: up to eight
// general stages and the final combiner), the alpha test and blending. The register layouts are
// nxdk's nv_regs.h; the combiners' arithmetic is NV_register_combiners'.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "core.h"
#include "draw.h"
#include "gpu.h"
#include "nv2a.h"

namespace xb {

namespace {

using V4 = std::array<float, 4>; // r, g, b, a

float Clamp01(float v) { return std::clamp(v, 0.f, 1.f); }

V4 Unpack(u32 argb)
{
    return {((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f, (argb >> 24) / 255.f};
}

u32 Pack(const V4& c)
{
    auto b = [](float v) { return u32(Clamp01(v) * 255.f + 0.5f); };
    return b(c[3]) << 24 | b(c[0]) << 16 | b(c[1]) << 8 | b(c[2]);
}

// ---- textures

struct Texture
{
    u32 address = 0, palette = 0, format = 0, width = 1, height = 1, pitch = 0, wrap = 0, filter = 0, border = 0;
    bool swizzled = false, compressed = false, cubemap = false;
    bool border_texels = false; // SET_TEXTURE_FORMAT bit 3 clear: the border comes from the texture's own texels
    u32 face_size = 0; // a cube map face's bytes: its mipmap chain, 128-byte aligned
    u32 levels = 1;
    u32 depth = 1, slice = 0; // a volume texture's slices, and the one texels come from
    float lod_bias = 0, min_lod = 0, max_lod = 0;
};

bool IsLinear(u32 f)
{
    return (f >= 0x10 && f <= 0x13) || f == 0x17 || (f >= 0x1b && f <= 0x20) || f == 0x24 || f == 0x25 ||
           (f >= 0x2e && f <= 0x31) || f == 0x35 || (f >= 0x3f && f <= 0x41);
}

u32 BytesPerTexel(u32 f)
{
    switch (f)
    {
    case 0x00: case 0x01: case 0x0b: case 0x13: case 0x19: case 0x1b: case 0x1f: return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x17: case 0x1a: case 0x1c:
    case 0x1d: case 0x20: case 0x24: case 0x25: case 0x27: case 0x28: case 0x29: case 0x2c: case 0x30:
    case 0x31: case 0x32: case 0x35: return 2;
    default: return 4;
    }
}

Texture StageTexture(int stage)
{
    const KelvinState& k = g_kelvin;
    const u32 base = 0x1b00 + u32(stage) * 64;
    Texture t;
    const u32 format = k.Get(base + 4);
    t.format = (format >> 8) & 0xff;
    const u32 dma = (format & 3) == 2 ? 0x188 : 0x184;
    const u32 dma_base = Nv2aDmaAddress(k.Get(dma));
    t.address = dma_base ? dma_base + k.Get(base) : 0;
    t.compressed = t.format == 0x0c || t.format == 0x0e || t.format == 0x0f;
    t.swizzled = !IsLinear(t.format) && !t.compressed;
    if (IsLinear(t.format))
    {
        t.width = std::max<u32>(1, k.Get(base + 0x1c) >> 16);
        t.height = std::max<u32>(1, k.Get(base + 0x1c) & 0xffff);
        t.pitch = k.Get(base + 0x10) >> 16;
    }
    else
    {
        t.width = 1u << ((format >> 20) & 0xf);
        t.height = 1u << ((format >> 24) & 0xf);
        t.pitch = t.width * BytesPerTexel(t.format);
        if (((format >> 4) & 0xf) == 3) // three dimensions: log2 depth in the top bits
            t.depth = 1u << ((format >> 28) & 0xf);
    }
    const u32 palette = k.Get(base + 0x20);
    const u32 palette_base = Nv2aDmaAddress(k.Get(palette & 1 ? 0x188 : 0x184));
    t.palette = palette_base ? palette_base + (palette & ~0x3fu) : 0;
    t.wrap = k.Get(base + 8);
    t.filter = k.Get(base + 0x14);
    t.border = k.Get(base + 0x24);
    t.cubemap = (format & 4) && !IsLinear(t.format);
    // the border from the texture (bit 3 clear; Direct3D and pbkit set it): a swizzled 2D texture is
    // stored twice its size (16 texels under 8; a volume in depth too) with the image at (4, 4) and its
    // border texels around it (nxdk_pgraph_tests' Texture_border, as xemu has it); the base level
    // only; linear and cube ones not yet
    t.border_texels = !(format & 8) && t.swizzled && !t.cubemap;
    if (!IsLinear(t.format))
        t.levels = std::max<u32>(1, (format >> 16) & 0xf);
    // SET_TEXTURE_CONTROL0's level clamps and SET_TEXTURE_FILTER's bias: fixed point, 8 fraction bits
    const u32 control = k.Get(base + 0xc);
    t.max_lod = float((control >> 6) & 0xfff) / 256.f;
    t.min_lod = float((control >> 18) & 0xfff) / 256.f;
    t.lod_bias = float(int32_t(t.filter << 19) >> 19) / 256.f;
    if (t.cubemap)
    {
        const u32 levels = std::max<u32>(1, (format >> 16) & 0xf);
        u32 w = t.width, h = t.height, size = 0;
        for (u32 l = 0; l < levels; l++, w = std::max(1u, w / 2), h = std::max(1u, h / 2))
            size += t.compressed ? ((w + 3) / 4) * ((h + 3) / 4) * (t.format == 0x0c ? 8 : 16) : w * h * BytesPerTexel(t.format);
        t.face_size = (size + 127) & ~127u;
    }
    return t;
}

// What the pixel stage needs from the registers, decoded once per draw (PreparePixels)
struct Stage
{
    u32 icw, ocw, aicw, aocw;
    V4 c0, c1;
    bool rgb, alpha; // whether the RGB and alpha portions write anything
};
struct PixelState
{
    bool combiners = false;
    u32 stages = 0;
    Stage stage[8];
    u32 cw0 = 0, cw1 = 0;
    bool uses_ef = false, uses_sum = false;
    V4 fog_color{};
    V4 final_c0{}, final_c1{};
    u32 program[4] = {};
    u32 dot_map[4] = {}, input[4] = {}, clip_mode = 0, shadow_func = 0;
    float eye[3] = {}; // SET_EYE_VECTOR: dot reflect specular const's eye
    float bump[4][4] = {}, bump_scale[4] = {}, bump_offset[4] = {};
    bool enabled[4] = {};
    Texture texture[4];
    bool alpha_test = false, blend = false;
    u32 alpha_func = 0, alpha_ref = 0, blend_sfactor = 0, blend_dfactor = 0, blend_equation = 0;
    V4 blend_color{};
};
PixelState g_ps;

V4 Expand565(u16 v)
{
    return {((v >> 11) & 31) / 31.f, ((v >> 5) & 63) / 63.f, (v & 31) / 31.f, 1.f};
}

// a texel of a DXT1/3/5 texture (4x4 blocks of 8 or 16 bytes)
V4 DxtTexel(const Texture& t, u32 x, u32 y)
{
    const u32 block_bytes = t.format == 0x0c ? 8 : 16;
    const u32 blocks_wide = std::max<u32>(1, (t.width + 3) / 4);
    const u32 block = t.address + ((y / 4) * blocks_wide + x / 4) * block_bytes;
    const u32 color_block = block_bytes == 16 ? block + 8 : block;
    const u32 i = (y & 3) * 4 + (x & 3);
    const u16 c0 = Read<u16>(color_block), c1 = Read<u16>(color_block + 2);
    const u32 code = (Read<u32>(color_block + 4) >> (i * 2)) & 3;
    const V4 a = Expand565(c0), b = Expand565(c1);
    V4 c;
    const bool four = t.format != 0x0c || c0 > c1;
    switch (code)
    {
    case 0: c = a; break;
    case 1: c = b; break;
    case 2:
        for (int n = 0; n < 3; n++)
            c[n] = four ? (2 * a[n] + b[n]) / 3 : (a[n] + b[n]) / 2;
        c[3] = 1;
        break;
    default:
        if (four)
            for (int n = 0; n < 3; n++)
                c[n] = (a[n] + 2 * b[n]) / 3;
        else
            c = {0, 0, 0, 0};
        c[3] = four ? 1.f : 0.f;
    }
    if (t.format == 0x0e) // DXT3: explicit 4-bit alpha
        c[3] = ((Read<u16>(block + (i / 4) * 2) >> ((i & 3) * 4)) & 15) / 15.f;
    else if (t.format == 0x0f) // DXT5: interpolated alpha
    {
        const float a0 = Read<u8>(block) / 255.f, a1 = Read<u8>(block + 1) / 255.f;
        u64 bits = 0;
        for (int n = 0; n < 6; n++)
            bits |= u64(Read<u8>(block + 2 + n)) << (8 * n);
        const u32 ai = u32(bits >> (3 * i)) & 7;
        if (ai == 0) c[3] = a0;
        else if (ai == 1) c[3] = a1;
        else if (a0 > a1) c[3] = ((8 - ai) * a0 + (ai - 1) * a1) / 7;
        else if (ai == 6) c[3] = 0;
        else if (ai == 7) c[3] = 1;
        else c[3] = ((6 - ai) * a0 + (ai - 1) * a1) / 5;
    }
    return c;
}

V4 TexelRaw(const Texture& t, u32 x, u32 y);

// SET_TEXTURE_FILTER's signed channels (A bit 28, R 29, G 30, B 31): the texel's byte read as two's
// complement over 127 (0x7f is 1, 0x80 and 0xff negative), before filtering; an unsigned input
// mapping then clamps the negatives to 0, as the console's captures show (nxdk_pgraph_tests'
// texture signed component tests)
V4 Texel(const Texture& t, u32 x, u32 y)
{
    V4 c = TexelRaw(t, x, y);
    if (const u32 flags = t.filter >> 28)
    {
        static constexpr int kChannel[4] = {3, 0, 1, 2}; // A, R, G, B -> RGBA index
        for (int b = 0; b < 4; b++)
            if (flags & (1u << b))
            {
                float& v = c[kChannel[b]];
                const int byte = int(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
                v = std::max(-1.f, float(byte >= 128 ? byte - 256 : byte) / 127.f);
            }
    }
    return c;
}

V4 TexelRaw(const Texture& t, u32 x, u32 y)
{
    if (t.compressed)
        return DxtTexel(t, x, y);
    const u32 bpt = BytesPerTexel(t.format);
    const u32 p = t.address + (t.depth > 1 ? (t.swizzled ? Swizzle3(x, y, t.slice, t.width, t.height, t.depth) * bpt
                                                         : (t.slice * t.height + y) * t.pitch + x * bpt)
                                           : t.swizzled ? Swizzle(x, y, t.width, t.height) * bpt : y * t.pitch + x * bpt);
    const u32 v = bpt == 1 ? Read<u8>(p) : bpt == 2 ? Read<u16>(p) : Read<u32>(p);
    auto c8 = [](u32 b) { return (b & 255) / 255.f; };
    switch (t.format)
    {
    case 0x00: case 0x13: return {c8(v), c8(v), c8(v), 1};                            // Y8
    case 0x01: case 0x1b: return {c8(v), c8(v), c8(v), c8(v)};                         // AY8
    case 0x19: case 0x1f: return {1, 1, 1, c8(v)};                                     // A8
    case 0x1a: case 0x20: return {c8(v), c8(v), c8(v), c8(v >> 8)};                    // A8Y8
    case 0x02: case 0x10:                                                              // A1R5G5B5
        return {((v >> 10) & 31) / 31.f, ((v >> 5) & 31) / 31.f, (v & 31) / 31.f, v & 0x8000 ? 1.f : 0.f};
    case 0x03: case 0x1c:                                                              // X1R5G5B5
        return {((v >> 10) & 31) / 31.f, ((v >> 5) & 31) / 31.f, (v & 31) / 31.f, 1};
    case 0x04: case 0x1d:                                                              // A4R4G4B4
        return {((v >> 8) & 15) / 15.f, ((v >> 4) & 15) / 15.f, (v & 15) / 15.f, ((v >> 12) & 15) / 15.f};
    case 0x05: case 0x11: return Expand565(u16(v));                                    // R5G6B5
    case 0x27:                                                                         // R6G5B5
        return {((v >> 10) & 63) / 63.f, ((v >> 5) & 31) / 31.f, (v & 31) / 31.f, 1};
    case 0x17: case 0x28: return {c8(v), c8(v >> 8), c8(v), c8(v >> 8)};               // G8B8 (as xemu)
    case 0x29: return {c8(v >> 8), c8(v), c8(v), c8(v >> 8)};                          // R8B8
    case 0x0b:                                                                         // I8: a palette's index
        return t.palette ? Unpack(Read<u32>(t.palette + (v & 255) * 4)) : V4{0, 0, 0, 1};
    case 0x07: case 0x1e: { V4 c = Unpack(v); c[3] = 1; return c; }                    // X8R8G8B8
    case 0x3a: case 0x3f: return {c8(v), c8(v >> 8), c8(v >> 16), c8(v >> 24)};        // A8B8G8R8
    case 0x3b: case 0x40: return {c8(v >> 8), c8(v >> 16), c8(v >> 24), c8(v)};        // B8G8R8A8
    case 0x3c: case 0x41: return {c8(v >> 24), c8(v >> 16), c8(v >> 8), c8(v)};        // R8G8B8A8
    case 0x2c: case 0x30: case 0x32: case 0x35:                                        // 16-bit depth/luminance
    {
        const float l = (v & 0xffff) / 65535.f;
        return {l, l, l, 1};
    }
    case 0x2e: case 0x2f: { const float l = (v >> 8) / 16777215.f; return {l, l, l, 1}; } // 24-bit depth
    default: return Unpack(v);                                                         // A8R8G8B8
    }
}

// a texel coordinate after the stage's wrap mode (ADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border,
// 5 clamp to edge); -1 for the border
int Address(int c, u32 size, u32 mode)
{
    const int n = int(size);
    switch (mode)
    {
    case 1: return ((c % n) + n) % n;
    case 2:
    {
        const int period = 2 * n, m = ((c % period) + period) % period;
        return m < n ? m : period - 1 - m;
    }
    case 4: return c < 0 || c >= n ? -1 : c;
    default: return std::clamp(c, 0, n - 1);
    }
}

V4 SampleLevel(const Texture& t, float u, float v, bool linear);

// the mipmap level l of a swizzled or compressed texture: smaller, after the ones before it
Texture Level(const Texture& t, u32 l)
{
    Texture out = t;
    u32 w = t.width, h = t.height, d = t.depth;
    for (u32 i = 0; i < l; i++, w = std::max(1u, w / 2), h = std::max(1u, h / 2), d = std::max(1u, d / 2))
        out.address += t.compressed ? ((w + 3) / 4) * ((h + 3) / 4) * (t.format == 0x0c ? 8 : 16) * d
                                    : w * h * d * BytesPerTexel(t.format);
    out.width = w, out.height = h, out.depth = d;
    out.pitch = w * BytesPerTexel(t.format);
    return out;
}

// A sample at the level of detail `lod`: magnified (lod <= 0) by the mag filter; minified by the
// min filter (1 nearest, 2 linear on the base level; 3/4 the nearest level, nearest or linear; 5/6
// between the two nearest levels; 7 the base level's convolution, as linear here)
V4 SampleLod(const Texture& t, float u, float v, float lod)
{
    const float raw = lod + t.lod_bias;
    if (raw <= 0 || t.levels <= 1 || (!t.swizzled && !t.compressed))
    {
        const u32 mode = raw <= 0 ? (t.filter >> 24) & 0xf : (t.filter >> 16) & 0xff;
        return SampleLevel(t, u, v, mode != 1 && mode != 3 && mode != 5);
    }
    const float top = std::min(t.max_lod, float(t.levels - 1));
    const float l = std::clamp(raw, std::min(t.min_lod, top), top);
    const u32 min = (t.filter >> 16) & 0xff;
    switch (min)
    {
    case 1: return SampleLevel(t, u, v, false);
    case 3: return SampleLevel(Level(t, u32(l + 0.5f)), u, v, false);
    case 4: return SampleLevel(Level(t, u32(l + 0.5f)), u, v, true);
    case 5:
    case 6:
    {
        const u32 l0 = u32(l), l1 = std::min(l0 + 1, t.levels - 1);
        const float f = l - float(l0);
        const V4 a = SampleLevel(Level(t, l0), u, v, min == 6);
        if (f == 0 || l1 == l0)
            return a;
        const V4 b = SampleLevel(Level(t, l1), u, v, min == 6);
        V4 c;
        for (int i = 0; i < 4; i++)
            c[i] = a[i] + (b[i] - a[i]) * f;
        return c;
    }
    default: return SampleLevel(t, u, v, true);
    }
}

V4 Sample(const Texture& t, float u, float v)
{
    return SampleLevel(t, u, v, ((t.filter >> 24) & 0xf) != 1);
}

// A volume texture: the level as SampleLod picks it (the nearest), r through ADDRESS's P mode to a
// slice, or two blended when filtering is linear
V4 Sample3D(const Texture& t, float u, float v, float r, float lod)
{
    Texture lt = t;
    const float raw = lod + t.lod_bias;
    bool linear;
    if (raw <= 0 || t.levels <= 1)
    {
        const u32 mag = (t.filter >> 24) & 0xf;
        linear = mag != 1;
    }
    else
    {
        const float top = std::min(t.max_lod, float(t.levels - 1));
        const float l = std::clamp(raw, std::min(t.min_lod, top), top);
        lt = Level(t, u32(l + 0.5f));
        const u32 min = (t.filter >> 16) & 0xff;
        linear = min != 1 && min != 3 && min != 5;
    }
    const u32 wrap_p = (t.wrap >> 16) & 0xf;
    if (t.border_texels) // the base level, its slices as stored: 4 in, the border's around them
    {
        lt = t;
        const int stored_depth = t.depth >= 8 ? int(t.depth) * 2 : 16;
        auto stored_slice = [&](int c) {
            const int s = Address(c, t.depth, wrap_p);
            return u32(std::clamp((s < 0 ? c : s) + 4, 0, stored_depth - 1));
        };
        const float z = r * float(t.depth);
        if (!linear)
        {
            lt.slice = stored_slice(int(std::floor(z)));
            return SampleLevel(lt, u, v, false);
        }
        const float fz = z - 0.5f;
        const int z0 = int(std::floor(fz));
        const float a = fz - float(z0);
        Texture t0 = lt, t1 = lt;
        t0.slice = stored_slice(z0), t1.slice = stored_slice(z0 + 1);
        const V4 c0 = SampleLevel(t0, u, v, true), c1 = SampleLevel(t1, u, v, true);
        V4 c;
        for (int i = 0; i < 4; i++)
            c[i] = c0[i] + (c1[i] - c0[i]) * a;
        return c;
    }
    const float z = t.swizzled ? r * float(lt.depth) : r;
    if (!linear)
    {
        const int s = Address(int(std::floor(z)), lt.depth, wrap_p);
        if (s < 0)
            return Unpack(t.border);
        lt.slice = u32(s);
        return SampleLevel(lt, u, v, false);
    }
    const float fz = z - 0.5f;
    const int z0 = int(std::floor(fz));
    const float a = fz - float(z0);
    const int s0 = Address(z0, lt.depth, wrap_p), s1 = Address(z0 + 1, lt.depth, wrap_p);
    Texture t0 = lt, t1 = lt;
    t0.slice = s0 < 0 ? 0 : u32(s0), t1.slice = s1 < 0 ? 0 : u32(s1);
    const V4 c0 = s0 < 0 ? Unpack(t.border) : SampleLevel(t0, u, v, true);
    const V4 c1 = s1 < 0 ? Unpack(t.border) : SampleLevel(t1, u, v, true);
    V4 c;
    for (int i = 0; i < 4; i++)
        c[i] = c0[i] + (c1[i] - c0[i]) * a;
    return c;
}

V4 SampleLevel(const Texture& t, float u, float v, bool linear)
{
    if (!t.address)
        return {0, 0, 0, 1};
    // swizzled and compressed textures take normalized coordinates, linear ones texels
    if (t.swizzled || t.compressed)
        u *= float(t.width), v *= float(t.height);
    const u32 wrap_u = t.wrap & 0xf, wrap_v = (t.wrap >> 8) & 0xf;
    Texture stored = t; // with border texels: the image as stored, the texture 4 texels in
    if (t.border_texels)
        stored.width = t.width >= 8 ? t.width * 2 : 16, stored.height = t.height >= 8 ? t.height * 2 : 16,
        stored.pitch = stored.width * BytesPerTexel(t.format),
        stored.depth = t.depth > 1 ? (t.depth >= 8 ? t.depth * 2 : 16) : 1; // (the slice: Sample3D's, stored)
    auto fetch = [&](int x, int y) {
        const int ax = Address(x, t.width, wrap_u), ay = Address(y, t.height, wrap_v);
        if (t.border_texels) // outside the texture (border mode): the stored border, as far as it goes
            return Texel(stored, u32(std::clamp(ax < 0 ? x + 4 : ax + 4, 0, int(stored.width) - 1)),
                         u32(std::clamp(ay < 0 ? y + 4 : ay + 4, 0, int(stored.height) - 1)));
        if (ax < 0 || ay < 0)
            return Unpack(t.border);
        return Texel(t, u32(ax), u32(ay));
    };
    if (!linear)
        return fetch(int(std::floor(u)), int(std::floor(v)));
    // bilinear
    const float fu = u - 0.5f, fv = v - 0.5f;
    const int x0 = int(std::floor(fu)), y0 = int(std::floor(fv));
    const float ax = fu - float(x0), ay = fv - float(y0);
    const V4 c00 = fetch(x0, y0), c10 = fetch(x0 + 1, y0), c01 = fetch(x0, y0 + 1), c11 = fetch(x0 + 1, y0 + 1);
    V4 c;
    for (int i = 0; i < 4; i++)
        c[i] = (c00[i] * (1 - ax) + c10[i] * ax) * (1 - ay) + (c01[i] * (1 - ax) + c11[i] * ax) * ay;
    return c;
}

// ---- depth textures: shadow comparisons

bool IsDepthFormat(u32 f) { return f >= 0x2a && f <= 0x31; }

// a depth texel's value: the fixed formats as integers, the float ones (24 bits: 8 exponent, 16
// mantissa; 16 bits: 4 exponent with a bias of 7, 12 mantissa) as their values
float DepthTexel(const Texture& t, u32 x, u32 y)
{
    const u32 bpt = BytesPerTexel(t.format);
    const u32 p = t.address + (t.swizzled ? Swizzle(x, y, t.width, t.height) * bpt : y * t.pitch + x * bpt);
    switch (t.format)
    {
    case 0x2a: case 0x2e: return float(Read<u32>(p) >> 8);
    case 0x2b: case 0x2f:
    {
        const u32 bits = (Read<u32>(p) >> 8) << 7;
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    case 0x2c: case 0x30: return float(Read<u16>(p));
    default: // 0x2d, 0x31
    {
        const u32 v = Read<u16>(p), e = v >> 12, m = v & 0xfff;
        return e ? std::ldexp(1.f + float(m) / 4096.f, int(e) - 7) : std::ldexp(float(m) / 4096.f, -6);
    }
    }
}

// SET_SHADOW_COMPARE_FUNC (0 never, 1 greater, 2 equal, 3 greater or equal, 4 less, 5 not equal,
// 6 less or equal, 7 always) between the reference and the texel's depth: all ones or all zeros
V4 ShadowSample(const Texture& t, float u, float v, float reference)
{
    if (t.swizzled)
        u *= float(t.width), v *= float(t.height);
    const int ax = Address(int(std::floor(u)), t.width, t.wrap & 0xf);
    const int ay = Address(int(std::floor(v)), t.height, (t.wrap >> 8) & 0xf);
    if (ax < 0 || ay < 0)
        return Unpack(t.border);
    const float depth = DepthTexel(t, u32(ax), u32(ay));
    bool pass;
    switch (g_ps.shadow_func)
    {
    case 0: pass = false; break;
    case 1: pass = reference > depth; break;
    case 2: pass = reference == depth; break;
    case 3: pass = reference >= depth; break;
    case 4: pass = reference < depth; break;
    case 5: pass = reference != depth; break;
    case 6: pass = reference <= depth; break;
    default: pass = true;
    }
    const float r = pass ? 1.f : 0.f;
    return {r, r, r, r};
}

// ---- the combiners

// an input mapping (ICW MAP): 0 unsigned identity .. 7 signed negate
float MapInput(float x, u32 map)
{
    switch (map)
    {
    case 0: return Clamp01(x);
    case 1: return 1.f - Clamp01(x);
    case 2: return 2.f * std::max(0.f, x) - 1.f;
    case 3: return -2.f * std::max(0.f, x) + 1.f;
    case 4: return std::max(0.f, x) - 0.5f;
    case 5: return -std::max(0.f, x) + 0.5f;
    case 6: return x;
    default: return -x;
    }
}

struct Registers
{
    V4 r[16]; // set by Combine: what isn't an input starts at zero
};

// one input of a general stage, as RGB (`alpha_portion` false) or as the alpha portion's scalar
V4 ColorInput(const Registers& regs, u32 byte)
{
    const u32 source = byte & 0xf, map = (byte >> 5) & 7;
    const V4& s = regs.r[source];
    V4 v;
    for (int i = 0; i < 3; i++)
        v[i] = MapInput(byte & 0x10 ? s[3] : s[i], map);
    v[3] = 0;
    return v;
}

float AlphaInput(const Registers& regs, u32 byte)
{
    const u32 source = byte & 0xf, map = (byte >> 5) & 7;
    const V4& s = regs.r[source];
    return MapInput(byte & 0x10 ? s[3] : s[2], map); // the alpha bit: alpha, else blue
}

float ApplyOp(float v, u32 op)
{
    switch (op)
    {
    case 1: v -= 0.5f; break;
    case 2: v *= 2; break;
    case 3: v = (v - 0.5f) * 2; break;
    case 4: v *= 4; break;
    case 6: v *= 0.5f; break;
    }
    return std::clamp(v, -1.f, 1.f);
}

void GeneralStage(Registers& regs, const Stage& st)
{
    regs.r[1] = st.c0, regs.r[2] = st.c1;
    const bool mux = regs.r[12][3] >= 0.5f; // the MUX: r0's alpha (its MSB; the LSB isn't kept)
    auto write_rgb = [&](u32 dst, const V4& v) {
        if (dst)
            for (int i = 0; i < 3; i++)
                regs.r[dst][i] = v[i];
    };
    auto write_alpha = [&](u32 dst, float v) {
        if (dst)
            regs.r[dst][3] = v;
    };
    // a stage reads all its inputs before any of its writes
    V4 ab, cd, sum;
    float alpha_ab = 0, alpha_cd = 0, alpha_sum = 0;
    const u32 icw = st.icw, ocw = st.ocw;
    if (st.rgb || (ocw & (3u << 18)))
    {
    const V4 a = ColorInput(regs, icw >> 24), b = ColorInput(regs, (icw >> 16) & 0xff);
    const V4 c = ColorInput(regs, (icw >> 8) & 0xff), d = ColorInput(regs, icw & 0xff);
    const float ab_dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2], cd_dot = c[0] * d[0] + c[1] * d[1] + c[2] * d[2];
    const u32 op = (ocw >> 15) & 7;
    for (int i = 0; i < 3; i++)
    {
        ab[i] = ocw & (1u << 13) ? ab_dot : a[i] * b[i];
        cd[i] = ocw & (1u << 12) ? cd_dot : c[i] * d[i];
        sum[i] = ocw & (1u << 14) ? (mux ? cd[i] : ab[i]) : ab[i] + cd[i];
        ab[i] = ApplyOp(ab[i], op), cd[i] = ApplyOp(cd[i], op), sum[i] = ApplyOp(sum[i], op);
    }
    }
    const u32 aicw = st.aicw, aocw = st.aocw;
    if (st.alpha)
    {
    const float aa = AlphaInput(regs, aicw >> 24), ab_ = AlphaInput(regs, (aicw >> 16) & 0xff);
    const float ac = AlphaInput(regs, (aicw >> 8) & 0xff), ad = AlphaInput(regs, aicw & 0xff);
    const u32 aop = (aocw >> 15) & 7;
    alpha_ab = ApplyOp(aa * ab_, aop), alpha_cd = ApplyOp(ac * ad, aop);
    alpha_sum = ApplyOp(aocw & (1u << 14) ? (mux ? ac * ad : aa * ab_) : aa * ab_ + ac * ad, aop);
    }
    if (st.rgb)
    {
    write_rgb((ocw >> 4) & 0xf, ab);
    write_rgb(ocw & 0xf, cd);
    write_rgb((ocw >> 8) & 0xf, sum);
    }
    if (st.alpha)
    {
    write_alpha((aocw >> 4) & 0xf, alpha_ab);
    write_alpha(aocw & 0xf, alpha_cd);
    write_alpha((aocw >> 8) & 0xf, alpha_sum);
    if (ocw & (1u << 19)) // blue to alpha: AB's blue into its destination's alpha
        write_alpha((ocw >> 4) & 0xf, ab[2]);
    if (ocw & (1u << 18))
        write_alpha(ocw & 0xf, cd[2]);
    }
}

// a final combiner input: unsigned, maybe inverted; `blue_or_alpha` picks a component for G
V4 FinalInput(const Registers& regs, u32 byte, const V4& sum, const V4& ef)
{
    const u32 source = byte & 0xf;
    if (source == 0)
    {
        const float z = byte & 0xe0 ? 1.f : 0.f;
        return {z, z, z, z};
    }
    const V4& s = source == 14 ? sum : source == 15 ? ef : regs.r[source];
    V4 v;
    for (int i = 0; i < 4; i++)
    {
        const float x = Clamp01(byte & 0x10 ? s[3] : s[i]);
        v[i] = byte & 0xe0 ? 1.f - x : x;
    }
    return v;
}

V4 Combine(const PixelInputs& in, const V4 tex[4])
{
    Registers regs;
    for (int i : {0, 1, 2, 6, 7, 12, 13, 14, 15})
        regs.r[i] = {0, 0, 0, 0};
    // fog: its colour (SET_FOG_COLOR, red in the low byte) and the interpolated factor
    regs.r[3] = g_ps.fog_color;
    regs.r[3][3] = Clamp01(in.v[5][0]);
    regs.r[4] = {in.v[3][0], in.v[3][1], in.v[3][2], in.v[3][3]}; // v0: diffuse
    regs.r[5] = {in.v[4][0], in.v[4][1], in.v[4][2], in.v[4][3]}; // v1: specular
    for (int i = 0; i < 4; i++)
        regs.r[8 + i] = tex[i];
    regs.r[12][3] = tex[0][3]; // r0's alpha starts as t0's
    for (u32 s = 0; s < g_ps.stages; s++)
        GeneralStage(regs, g_ps.stage[s]);
    // the final combiner: rgb = A*B + (1-A)*C + D, alpha = G; E*F and spec + r0 as extra sources
    regs.r[1] = g_ps.final_c0, regs.r[2] = g_ps.final_c1;
    const u32 cw0 = g_ps.cw0, cw1 = g_ps.cw1;
    V4 sum{}, ef{};
    if (g_ps.uses_sum)
    {
        for (int i = 0; i < 3; i++)
        {
            sum[i] = regs.r[12][i] + regs.r[5][i];
            if (cw1 & 0x80)
                sum[i] = Clamp01(sum[i]);
        }
        sum[3] = 1;
    }
    if (g_ps.uses_ef)
    {
        const V4 e = FinalInput(regs, cw1 >> 24, sum, ef), f = FinalInput(regs, (cw1 >> 16) & 0xff, sum, ef);
        for (int i = 0; i < 4; i++)
            ef[i] = e[i] * f[i];
    }
    const V4 a = FinalInput(regs, cw0 >> 24, sum, ef), b = FinalInput(regs, (cw0 >> 16) & 0xff, sum, ef);
    const V4 c = FinalInput(regs, (cw0 >> 8) & 0xff, sum, ef), d = FinalInput(regs, cw0 & 0xff, sum, ef);
    // G: the alpha bit picks alpha, else blue
    const u32 g_byte = (cw1 >> 8) & 0xff;
    const V4& gs = (g_byte & 0xf) == 15 ? ef : (g_byte & 0xf) == 14 ? sum : regs.r[g_byte & 0xf];
    float g = Clamp01(g_byte & 0x10 ? gs[3] : gs[2]);
    if (g_byte & 0xe0)
        g = 1 - g;
    V4 out;
    for (int i = 0; i < 3; i++)
        out[i] = Clamp01(a[i] * b[i] + (1 - a[i]) * c[i] + d[i]);
    out[3] = g;
    return out;
}

// ---- the texture stages' programs (SET_SHADER_STAGE_PROGRAM, 5 bits a stage)

// a cube map's texel in direction (x, y, z): the face of the largest component, the other two
// projected onto it (Direct3D's and GL's convention)
V4 SampleCube(const Texture& t, float x, float y, float z)
{
    const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    int face;
    float sc, tc, ma;
    if (ax >= ay && ax >= az)
        face = x >= 0 ? 0 : 1, sc = x >= 0 ? -z : z, tc = -y, ma = ax;
    else if (ay >= az)
        face = y >= 0 ? 2 : 3, sc = x, tc = y >= 0 ? z : -z, ma = ay;
    else
        face = z >= 0 ? 4 : 5, sc = z >= 0 ? x : -x, tc = -y, ma = az;
    if (ma == 0)
        return {0, 0, 0, 0};
    Texture f = t;
    if (t.cubemap)
        f.address += u32(face) * t.face_size;
    return Sample(f, (sc / ma + 1) * 0.5f, (tc / ma + 1) * 0.5f);
}

// signed components from unsigned texels (SET_DOT_RGBMAPPING's conversions)
float Sign1(float x) { return (x * 255.f - 128.f) / 127.f; }
float Sign2(float x) { x *= 255.f; return x >= 128.f ? (x - 255.5f) / 127.5f : (x + 0.5f) / 127.5f; }
float Sign3(float x) { x *= 255.f; return x >= 128.f ? (x - 256.f) / 127.f : x / 127.f; }

V4 DotMap(const V4& c, u32 map)
{
    switch (map)
    {
    case 1: return {Sign1(c[0]), Sign1(c[1]), Sign1(c[2]), 0};
    case 2: return {Sign2(c[0]), Sign2(c[1]), Sign2(c[2]), 0};
    case 3: return {Sign3(c[0]), Sign3(c[1]), Sign3(c[2]), 0};
    case 4: // HILO: two 16-bit values
    case 5: // HILO hemisphere, D3D's signed mapping: z = sqrt(1 - hi^2 - lo^2)
    case 6: // ... GL's
    case 7: // ... two's complement
    {
        auto byte = [](float v) { return u32(Clamp01(v) * 255.f + 0.5f); };
        const u32 hi = byte(c[3]) << 8 | byte(c[0]), lo = byte(c[1]) << 8 | byte(c[2]);
        if (map == 4)
            return {float(hi) / 65535.f, float(lo) / 65535.f, 1, 0};
        auto sign = [map](u32 x) {
            if (map == 5)
                return (float(x) - 32768.f) / 32767.f;
            if (map == 6)
                return x >= 32768 ? (float(x) - 65535.5f) / 32767.5f : (float(x) + 0.5f) / 32767.5f;
            return x >= 32768 ? (float(x) - 65536.f) / 32767.f : float(x) / 32767.f;
        };
        const float h = sign(hi), l = sign(lo);
        return {h, l, std::sqrt(std::max(0.f, 1.f - h * h - l * l)), 0};
    }
    default: return {c[0], c[1], c[2], 0};
    }
}

float Dot3(const float* a, const V4& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// the texture stages' programs (SET_SHADER_STAGE_PROGRAM, 5 bits a stage); false: a clip plane
// stage discards the pixel
bool TextureStages(const PixelInputs& in, V4 tex[4])
{
    float dot[4] = {};
    for (int s = 0; s < 4; s++)
    {
        tex[s] = {0, 0, 0, 0};
        const u32 program = g_ps.program[s];
        const Texture& t = g_ps.texture[s];
        const bool enabled = g_ps.enabled[s] && t.address;
        const float* tc = in.v[9 + s];
        const V4& input = tex[g_ps.input[s] < 4 ? g_ps.input[s] : 0];
        switch (program)
        {
        case 0: break;  // none
        case 1:         // 2D projective
        case 2:         // 3D (its first slice)
        {
            if (!enabled)
                break;
            const float q = program == 1 && tc[3] != 0 ? tc[3] : 1.f;
            if (IsDepthFormat(t.format)) // a shadow map: the depth reference is r (over q)
                tex[s] = ShadowSample(t, tc[0] / q, tc[1] / q, tc[2] / q);
            else if (program == 2 && t.depth > 1)
                tex[s] = Sample3D(t, tc[0], tc[1], tc[2], in.lod[s]);
            else
                tex[s] = SampleLod(t, tc[0] / q, tc[1] / q, in.lod[s]);
            break;
        }
        case 3: // cube map
            if (enabled)
                tex[s] = SampleCube(t, tc[0], tc[1], tc[2]);
            break;
        case 4: // pass-through: the coordinates as a colour
            tex[s] = {Clamp01(tc[0]), Clamp01(tc[1]), Clamp01(tc[2]), Clamp01(tc[3])};
            break;
        case 5: // clip plane: each coordinate's sign against its compare mode
            for (int j = 0; j < 4; j++)
            {
                const bool compare = (g_ps.clip_mode >> (4 * s + j)) & 1;
                if (compare ? tc[j] >= 0 : tc[j] < 0)
                    return false;
            }
            break;
        case 6: // bump environment map: the input's blue and green, signed, through the 2x2 matrix
        case 7: // ... with luminance: its red scaling the result
        {
            // a channel the input texture already reads as signed (SET_TEXTURE_FILTER) is used as it
            // is; an unsigned one is converted (as xemu does both)
            const u32 in_filter = g_ps.texture[g_ps.input[s] < 4 ? g_ps.input[s] : 0].filter;
            const float du = (in_filter & (1u << 31)) ? input[2] : Sign3(input[2]);
            const float dv = (in_filter & (1u << 30)) ? input[1] : Sign3(input[1]);
            const float* m = g_ps.bump[s];
            const float u = tc[0] + du * m[0] + dv * m[2], v = tc[1] + du * m[1] + dv * m[3];
            if (enabled)
                tex[s] = SampleLod(t, u, v, in.lod[s]);
            if (program == 7)
            {
                const float l = std::clamp(g_ps.bump_scale[s] * input[0] + g_ps.bump_offset[s], 0.f, 1.f);
                for (int c = 0; c < 4; c++)
                    tex[s][c] *= l;
            }
            break;
        }
        case 9: // dot ST: (the previous stage's dot, this one's) as coordinates
            dot[s] = Dot3(tc, DotMap(input, g_ps.dot_map[s]));
            if (enabled && s > 0)
                tex[s] = Sample(t, dot[s - 1], dot[s]);
            break;
        case 10: // dot ZW: a depth, not done; the dot is kept
        case 17: // dot product: kept for the stages after
            dot[s] = Dot3(tc, DotMap(input, g_ps.dot_map[s]));
            break;
        case 11: // dot reflect diffuse (stage 2): a normal from stages 1, 2 and the next one's coordinates
        {
            dot[s] = Dot3(tc, DotMap(input, g_ps.dot_map[s]));
            if (s < 3)
            {
                const float dn = Dot3(in.v[9 + s + 1], DotMap(tex[g_ps.input[s + 1] & 3], g_ps.dot_map[s + 1]));
                if (enabled)
                    tex[s] = SampleCube(t, dot[s - 1], dot[s], dn);
            }
            break;
        }
        case 12: // dot reflect specular (stage 3): the eye vector in the coordinates' w, reflected
        case 18: // ... with the eye vector a constant (SET_EYE_VECTOR)
        case 13: // dot STR 3D
        case 14: // dot STR cube
        {
            dot[s] = Dot3(tc, DotMap(input, g_ps.dot_map[s]));
            if (s < 2 || !enabled)
                break;
            const float n[3] = {dot[s - 2], dot[s - 1], dot[s]};
            if (program == 12 || program == 18)
            {
                const float e[3] = {program == 18 ? g_ps.eye[0] : in.v[9 + s - 2][3],
                                    program == 18 ? g_ps.eye[1] : in.v[9 + s - 1][3], program == 18 ? g_ps.eye[2] : tc[3]};
                const float ne = n[0] * e[0] + n[1] * e[1] + n[2] * e[2], nn = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
                const float k = nn != 0 ? 2 * ne / nn : 0;
                tex[s] = SampleCube(t, k * n[0] - e[0], k * n[1] - e[1], k * n[2] - e[2]);
            }
            else if (program == 14)
                tex[s] = SampleCube(t, n[0], n[1], n[2]);
            else if (t.depth > 1)
                tex[s] = Sample3D(t, n[0], n[1], n[2], 0);
            else
                tex[s] = Sample(t, n[0], n[1]);
            break;
        }
        case 15: // dependent on the input's alpha and red
            if (enabled)
                tex[s] = Sample(t, input[3], input[0]);
            break;
        case 16: // dependent on the input's green and blue
            if (enabled)
                tex[s] = Sample(t, input[1], input[2]);
            break;
        default: // 8 (BRDF): not done
        {
            static std::atomic<u32> reported{0};
            if (!(reported.fetch_or(1u << program) & (1u << program)))
                Log("nv2a: texture stage program %u isn't done yet", program);
        }
        }
    }
    return true;
}

// ---- the alpha test and blending

bool Compare(u32 func, u32 a, u32 b)
{
    switch (func & 7)
    {
    case 0: return false;
    case 1: return a < b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a != b;
    case 6: return a >= b;
    default: return true;
    }
}

V4 BlendFactor(u32 f, const V4& src, const V4& dst, const V4& constant)
{
    switch (f)
    {
    case 0x0000: return {0, 0, 0, 0};
    case 0x0001: return {1, 1, 1, 1};
    case 0x0300: return src;
    case 0x0301: return {1 - src[0], 1 - src[1], 1 - src[2], 1 - src[3]};
    case 0x0302: return {src[3], src[3], src[3], src[3]};
    case 0x0303: return {1 - src[3], 1 - src[3], 1 - src[3], 1 - src[3]};
    case 0x0304: return {dst[3], dst[3], dst[3], dst[3]};
    case 0x0305: return {1 - dst[3], 1 - dst[3], 1 - dst[3], 1 - dst[3]};
    case 0x0306: return dst;
    case 0x0307: return {1 - dst[0], 1 - dst[1], 1 - dst[2], 1 - dst[3]};
    case 0x0308:
    {
        const float s = std::min(src[3], 1 - dst[3]);
        return {s, s, s, 1};
    }
    case 0x8001: return constant;
    case 0x8002: return {1 - constant[0], 1 - constant[1], 1 - constant[2], 1 - constant[3]};
    case 0x8003: return {constant[3], constant[3], constant[3], constant[3]};
    case 0x8004: return {1 - constant[3], 1 - constant[3], 1 - constant[3], 1 - constant[3]};
    default: return {1, 1, 1, 1};
    }
}

} // namespace

void PreparePixels()
{
    const KelvinState& k = g_kelvin;
    PixelState& ps = g_ps;
    const u32 control = k.Get(0x1e60);
    ps.stages = std::min<u32>(control & 0xff, 8);
    ps.combiners = ps.stages != 0;
    const bool factor0_each = (control >> 12) & 1, factor1_each = (control >> 16) & 1;
    for (u32 s = 0; s < ps.stages; s++)
    {
        Stage& st = ps.stage[s];
        st.icw = k.Get(0xac0 + s * 4), st.ocw = k.Get(0x1e40 + s * 4);
        st.aicw = k.Get(0x260 + s * 4), st.aocw = k.Get(0xaa0 + s * 4);
        st.c0 = Unpack(k.Get(0xa60 + (factor0_each ? s * 4 : 0)));
        st.c1 = Unpack(k.Get(0xa80 + (factor1_each ? s * 4 : 0)));
        st.rgb = (st.ocw & 0xfff) != 0;
        st.alpha = (st.aocw & 0xfff) != 0 || (st.ocw & (3u << 18));
    }
    ps.cw0 = k.Get(0x288), ps.cw1 = k.Get(0x28c);
    const u32 fog = k.Get(0x2a8);
    ps.fog_color = {(fog & 255) / 255.f, ((fog >> 8) & 255) / 255.f, ((fog >> 16) & 255) / 255.f, (fog >> 24) / 255.f};
    ps.uses_ef = ps.uses_sum = false;
    for (u32 byte : {ps.cw0 >> 24, (ps.cw0 >> 16) & 0xff, (ps.cw0 >> 8) & 0xff, ps.cw0 & 0xff, (ps.cw1 >> 8) & 0xff})
    {
        ps.uses_ef |= (byte & 0xf) == 15;
        ps.uses_sum |= (byte & 0xf) == 14;
    }
    for (u32 byte : {ps.cw1 >> 24, (ps.cw1 >> 16) & 0xff})
        ps.uses_sum |= (byte & 0xf) == 14;
    ps.final_c0 = Unpack(k.Get(0x1e20)), ps.final_c1 = Unpack(k.Get(0x1e24));
    const u32 programs = k.Get(0x1e70);
    // the stages' wiring: SET_DOT_RGBMAPPING (4 bits a stage from 1), SET_SHADER_OTHER_STAGE_INPUT
    // (stages 2 and 3), SET_SHADER_CLIP_PLANE_MODE, the bump environment matrices
    for (int i = 1; i < 4; i++)
        ps.dot_map[i] = (k.Get(0x1e74) >> (4 * (i - 1))) & 0xf;
    ps.input[1] = 0, ps.input[2] = (k.Get(0x1e78) >> 16) & 0xf, ps.input[3] = (k.Get(0x1e78) >> 20) & 0xf;
    ps.clip_mode = k.Get(0x17f8);
    ps.shadow_func = k.Get(0x1e6c) & 7; // SET_SHADOW_COMPARE_FUNC
    for (int i = 0; i < 3; i++)
        ps.eye[i] = k.F(0x181c + u32(i) * 4);
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
            ps.bump[i][j] = k.F(0x1b28 + u32(i) * 64 + u32(j) * 4);
        ps.bump_scale[i] = k.F(0x1b38 + u32(i) * 64), ps.bump_offset[i] = k.F(0x1b3c + u32(i) * 64);
    }
    for (int s = 0; s < 4; s++)
    {
        ps.program[s] = (programs >> (5 * s)) & 0x1f;
        ps.enabled[s] = k.Get(0x1b0c + u32(s) * 64) & (1u << 30);
        if (ps.combiners && ps.enabled[s] && ps.program[s])
            ps.texture[s] = StageTexture(s);
    }
    ps.alpha_test = k.Get(0x300), ps.alpha_func = k.Get(0x33c), ps.alpha_ref = k.Get(0x340) & 0xff;
    ps.blend = k.Get(0x304), ps.blend_sfactor = k.Get(0x344), ps.blend_dfactor = k.Get(0x348);
    ps.blend_equation = k.Get(0x350), ps.blend_color = Unpack(k.Get(0x34c));
}

float TextureLod(int stage, float dudx, float dvdx, float dudy, float dvdy)
{
    const Texture& t = g_ps.texture[stage];
    if (t.levels <= 1 && t.lod_bias == 0)
        return 0; // one level: only magnification against minification matters
    // normalized coordinates are scaled to texels; linear textures' already are
    const float w = t.swizzled || t.compressed ? float(t.width) : 1.f, h = t.swizzled || t.compressed ? float(t.height) : 1.f;
    const float x = std::sqrt(dudx * dudx * w * w + dvdx * dvdx * h * h);
    const float y = std::sqrt(dudy * dudy * w * w + dvdy * dvdy * h * h);
    const float rho = std::max(x, y);
    return rho > 0 ? std::log2(rho) : -16.f;
}

u32 PixelInputMask()
{
    if (!g_ps.combiners)
        return 1u << 3;
    u32 mask = 1u << 3 | 1u << 4 | 1u << 5; // and the fog factor
    for (int s = 0; s < 4; s++)
        if (g_ps.program[s])
            mask |= 1u << (9 + s);
    return mask;
}

namespace {
// the bytes a texture's texels take (every level, every face), for hashing and overlap checks
u32 TextureBytes(const Texture& t)
{
    if (!t.swizzled && !t.compressed)
        return t.pitch * t.height;
    u32 total = 0, w = t.width, h = t.height;
    for (u32 l = 0; l < t.levels; l++, w = std::max(1u, w / 2), h = std::max(1u, h / 2))
        total += t.compressed ? ((w + 3) / 4) * ((h + 3) / 4) * (t.format == 0x0c ? 8 : 16) : w * h * BytesPerTexel(t.format);
    return t.cubemap ? t.face_size * 6 : total;
}
} // namespace

bool GpuPixelSetup(gpu::Uniforms& u, gpu::TextureInfo tex[4], gpu::BlendState& blend)
{
    const PixelState& ps = g_ps;
    std::memset(&u, 0, sizeof(u));
    auto put = [](float* to, const V4& v) {
        for (int i = 0; i < 4; i++)
            to[i] = v[i];
    };
    u.control[0] = ps.combiners ? ps.stages : 0;
    u.control[1] = ps.cw0, u.control[2] = ps.cw1;
    u.control[3] = (ps.uses_ef ? 1u : 0u) | (ps.uses_sum ? 2u : 0u) | (ps.alpha_test ? 4u : 0u);
    u.alpha_test[0] = ps.alpha_func, u.alpha_test[1] = ps.alpha_ref;
    for (u32 s = 0; s < ps.stages; s++)
    {
        const Stage& st = ps.stage[s];
        u.stage_words[s][0] = st.icw, u.stage_words[s][1] = st.ocw, u.stage_words[s][2] = st.aicw, u.stage_words[s][3] = st.aocw;
        put(u.stage_c0[s], st.c0), put(u.stage_c1[s], st.c1);
        u.stage_flags[s / 4] |= ((st.rgb ? 1u : 0u) | (st.alpha ? 2u : 0u)) << (8 * (s % 4));
    }
    put(u.fog_color, ps.fog_color), put(u.final_c0, ps.final_c0), put(u.final_c1, ps.final_c1);
    u.clip_mode[0] = ps.clip_mode;
    for (int s = 0; s < 4; s++)
    {
        tex[s] = gpu::TextureInfo{};
        const u32 program = ps.combiners ? ps.program[s] : 0;
        u.program[s] = program;
        u.stage_input[s] = ps.input[s];
        for (int j = 0; j < 4; j++)
            u.bump[s][j] = ps.bump[s][j];
        u.bump_lum[s][0] = ps.bump_scale[s], u.bump_lum[s][1] = ps.bump_offset[s];
        u.tex_scale[s][0] = u.tex_scale[s][1] = 1;
        u.tex_scale[s][2] = u.tex_scale[s][3] = 0;
        switch (program)
        {
        case 0: case 4: case 5: continue; // no texture read
        case 1: case 2: case 3: case 6: case 7: case 15: case 16: break;
        default: return false; // dot products, BRDF: not on the GPU yet
        }
        const Texture& t = ps.texture[s];
        if (!ps.enabled[s] || !t.address)
            continue;
        if (IsDepthFormat(t.format) || t.depth > 1 || (program == 3) != t.cubemap)
            return false; // shadow maps, volumes: not yet
        if (t.filter >> 28)
            return false; // signed channels: the GPU's textures are unsigned 8-bit (in software for now)
        if (t.border_texels)
            return false; // border texels stored with the texture: in software
        u.enabled[s] = 1;
        u.kind[s] = t.cubemap ? 1 : 0;
        gpu::TextureInfo& i = tex[s];
        i.used = true;
        i.address = t.address, i.bytes = TextureBytes(t), i.palette = t.format == 0x0b ? t.palette : 0;
        i.format = t.format, i.width = t.width, i.height = t.height, i.levels = t.levels, i.pitch = t.pitch;
        i.cube = t.cubemap, i.normalized = t.swizzled || t.compressed;
        i.wrap = t.wrap, i.filter = t.filter, i.border = t.border;
        i.lod_bias = t.lod_bias, i.min_lod = t.min_lod, i.max_lod = t.max_lod;
        if (!i.normalized)
            u.tex_scale[s][0] = 1.f / float(t.width), u.tex_scale[s][1] = 1.f / float(t.height);
        // nearest filtering (mag 1; min 1 or 3): a pixel exactly on a texel's edge takes the texel
        // after it, as the software path's floor(u) does; the GPU's interpolated coordinate can come
        // out a hair under the edge (an nxdk_pgraph_tests checkerboard a pixel off at every square's
        // edge): moved on by 1/256 of a texel, past such rounding and short of any other texel
        const u32 mag = (t.filter >> 24) & 0xf, min = (t.filter >> 16) & 0xff;
        u.tex_scale[s][2] = u.tex_scale[s][3] = 0;
        if (mag == 1 && (min == 1 || min == 3))
            u.tex_scale[s][2] = 1.f / 256.f / float(t.width), u.tex_scale[s][3] = 1.f / 256.f / float(t.height);
    }
    blend.enabled = ps.blend;
    blend.sfactor = ps.blend_sfactor, blend.dfactor = ps.blend_dfactor, blend.equation = ps.blend_equation;
    blend.color = Pack(ps.blend_color);
    return true;
}

void DecodeStageTexture(int stage, u32 face, u32 level, u32* argb)
{
    Texture t = g_ps.texture[stage];
    if (t.cubemap)
        t.address += face * t.face_size;
    const Texture lt = level ? Level(t, level) : t;
    for (u32 y = 0; y < lt.height; y++)
        for (u32 x = 0; x < lt.width; x++)
            argb[y * lt.width + x] = Pack(Texel(lt, x, y));
}

bool ShadePixel(const PixelInputs& in, const Surface& target, u32 x, u32 y, u32& out)
{
    const PixelState& ps = g_ps;
    V4 color;
    if (!ps.combiners) // no combiners set up: the diffuse colour
        color = {in.v[3][0], in.v[3][1], in.v[3][2], in.v[3][3]};
    else
    {
        V4 tex[4];
        if (!TextureStages(in, tex))
            return false;
        color = Combine(in, tex);
    }
    // the alpha test
    if (ps.alpha_test && !Compare(ps.alpha_func, u32(Clamp01(color[3]) * 255.f + 0.5f), ps.alpha_ref))
        return false;
    // blending
    if (ps.blend)
    {
        const V4 dst = Unpack(ReadPixel(target, x, y));
        const V4 sf = BlendFactor(ps.blend_sfactor, color, dst, ps.blend_color);
        const V4 df = BlendFactor(ps.blend_dfactor, color, dst, ps.blend_color);
        for (int i = 0; i < 4; i++)
        {
            const float s = color[i] * sf[i], d = dst[i] * df[i];
            switch (ps.blend_equation)
            {
            case 0x800a: color[i] = s - d; break;
            case 0x800b: color[i] = d - s; break;
            case 0x8007: color[i] = std::min(color[i], dst[i]); break;
            case 0x8008: color[i] = std::max(color[i], dst[i]); break;
            default: color[i] = s + d;
            }
        }
    }
    out = Pack(color);
    return true;
}

} // namespace xb
