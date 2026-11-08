// A pixel's colour: the texture stages, the register combiners (the NV2A's pixel shader: up to eight
// general stages and the final combiner), the alpha test and blending. The register layouts are
// nxdk's nv_regs.h; the combiners' arithmetic is NV_register_combiners'.
#include <algorithm>
#include <atomic>
#include <cmath>

#include "core.h"
#include "draw.h"
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
    bool swizzled = false, compressed = false;
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

// the NV2A's swizzle: the bits of x and y interleaved (x first) while both have them
u32 Swizzle(u32 x, u32 y, u32 width, u32 height)
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
    }
    const u32 palette = k.Get(base + 0x20);
    const u32 palette_base = Nv2aDmaAddress(k.Get(palette & 1 ? 0x188 : 0x184));
    t.palette = palette_base ? palette_base + (palette & ~0x3fu) : 0;
    t.wrap = k.Get(base + 8);
    t.filter = k.Get(base + 0x14);
    t.border = k.Get(base + 0x24);
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
    V4 final_c0{}, final_c1{};
    u32 program[4] = {};
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

V4 Texel(const Texture& t, u32 x, u32 y)
{
    if (t.compressed)
        return DxtTexel(t, x, y);
    const u32 bpt = BytesPerTexel(t.format);
    const u32 p = t.address + (t.swizzled ? Swizzle(x, y, t.width, t.height) * bpt : y * t.pitch + x * bpt);
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

V4 Sample(const Texture& t, float u, float v)
{
    if (!t.address)
        return {0, 0, 0, 1};
    // swizzled and compressed textures take normalized coordinates, linear ones texels
    if (t.swizzled || t.compressed)
        u *= float(t.width), v *= float(t.height);
    const u32 wrap_u = t.wrap & 0xf, wrap_v = (t.wrap >> 8) & 0xf;
    auto fetch = [&](int x, int y) {
        const int ax = Address(x, t.width, wrap_u), ay = Address(y, t.height, wrap_v);
        if (ax < 0 || ay < 0)
            return Unpack(t.border);
        return Texel(t, u32(ax), u32(ay));
    };
    const u32 mag = (t.filter >> 24) & 0xf;
    if (mag == 1) // nearest
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
    for (int i : {0, 1, 2, 3, 6, 7, 12, 13, 14, 15})
        regs.r[i] = {0, 0, 0, 0};
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

void TextureStages(const PixelInputs& in, V4 tex[4])
{
    for (int s = 0; s < 4; s++)
    {
        tex[s] = {0, 0, 0, 0};
        const u32 program = g_ps.program[s];
        const bool enabled = g_ps.enabled[s];
        const float* tc = in.v[9 + s];
        switch (program)
        {
        case 0: break; // none
        case 1:        // 2D projective
        case 2:        // 3D (its first slice)
        case 3:        // cube map (not yet: as 2D)
        {
            if (!enabled)
                break;
            const float q = program == 1 && tc[3] != 0 ? tc[3] : 1.f;
            tex[s] = Sample(g_ps.texture[s], tc[0] / q, tc[1] / q);
            break;
        }
        case 4: // pass-through: the coordinates as a colour
            tex[s] = {Clamp01(tc[0]), Clamp01(tc[1]), Clamp01(tc[2]), Clamp01(tc[3])};
            break;
        default:
        {
            static std::atomic<u32> reported{0};
            if (!(reported.fetch_or(1u << program) & (1u << program)))
            {
                Log("nv2a: texture stage program %u isn't done yet", program);
            }
        }
        }
    }
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

u32 PixelInputMask()
{
    if (!g_ps.combiners)
        return 1u << 3;
    u32 mask = 1u << 3 | 1u << 4;
    for (int s = 0; s < 4; s++)
        if (g_ps.program[s])
            mask |= 1u << (9 + s);
    return mask;
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
        TextureStages(in, tex);
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
