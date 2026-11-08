// Drawing for the software NV2A: vertices from the vertex arrays (or inline), the vertex program
// (the NV2A's own microcode, interpreted), primitives assembled and rasterized into the colour and
// depth surfaces; the pixel stage (textures, combiners, blending) is pixel.cpp. Rasterization is
// shared by a pool of threads, each drawing every triangle of a draw into its own bands of rows, in
// order, so the result is the same as one thread's.
//
// The vertex program's encoding is the NV2A's (xemu documents it in pgraph/glsl/vsh-prog.c).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "core.h"
#include "draw.h"
#include "nv2a.h"

namespace xb {

namespace {

struct Vec4
{
    float v[4] = {0, 0, 0, 0};
    float& operator[](int i) { return v[i]; }
    float operator[](int i) const { return v[i]; }
};

// the vertex program and its constants, as uploaded
u32 g_program[136][4] = {};
Vec4 g_constants[192];
u32 g_program_cursor = 0, g_constant_cursor = 0, g_constant_word = 0;

// the current primitive's vertices (outputs: 0 oPos, 3 oD0, 4 oD1, 5 oFog, 6 oPts, 7 oB0, 8 oB1,
// 9-12 oT0-oT3)
struct Vertex
{
    Vec4 o[16];
};
u32 g_primitive = 0; // SET_BEGIN_END's operation (0: none)
std::vector<Vertex> g_vertices;
std::vector<u32> g_inline; // INLINE_ARRAY's words for the current primitive
Vec4 g_attribute_default[16];

u32 Field(u32 token, int start, int bits)
{
    return (token >> start) & ((1u << bits) - 1);
}

// ---- the vertex program

Vec4 RunProgram(const Vec4 in[16], Vec4 out[16])
{
    Vec4 r[13] = {}; // R0-R11, R12 (= oPos)
    int a0 = 0;
    for (int i = 0; i < 16; i++)
        out[i] = Vec4{};
    out[3] = out[4] = Vec4{{0, 0, 0, 1}};
    u32 pc = g_kelvin.Get(0x1ea0) & 0xff; // SET_TRANSFORM_PROGRAM_START
    for (int guard = 0; guard < 136 && pc < 136; guard++, pc++)
    {
        const u32* t = g_program[pc];
        const u32 ilu = Field(t[1], 25, 3), mac = Field(t[1], 21, 4);
        auto input = [&](int mux, int reg, int neg, int sx, int sy, int sz, int sw) {
            Vec4 src;
            if (mux == 1)
                src = reg == 12 ? out[0] : r[std::min(reg, 12)];
            else if (mux == 2)
                src = in[Field(t[1], 9, 4)];
            else
            {
                int c = int(Field(t[1], 13, 8));
                if (Field(t[3], 1, 1))
                    c += a0;
                src = c >= 0 && c < 192 ? g_constants[c] : Vec4{};
            }
            Vec4 v{{src[sx], src[sy], src[sz], src[sw]}};
            if (neg)
                for (int k = 0; k < 4; k++)
                    v[k] = -v[k];
            return v;
        };
        const Vec4 A = input(Field(t[2], 26, 2), Field(t[2], 28, 4), Field(t[1], 8, 1), Field(t[1], 6, 2),
                             Field(t[1], 4, 2), Field(t[1], 2, 2), Field(t[1], 0, 2));
        const Vec4 B = input(Field(t[2], 11, 2), Field(t[2], 13, 4), Field(t[2], 25, 1), Field(t[2], 23, 2),
                             Field(t[2], 21, 2), Field(t[2], 19, 2), Field(t[2], 17, 2));
        const int c_reg = int(Field(t[2], 0, 2) << 2 | Field(t[3], 30, 2));
        const Vec4 C = input(Field(t[3], 28, 2), c_reg, Field(t[2], 10, 1), Field(t[2], 8, 2), Field(t[2], 6, 2),
                             Field(t[2], 4, 2), Field(t[2], 2, 2));
        Vec4 m{}, l{};
        auto dot = [](const Vec4& a, const Vec4& b, int n) {
            float s = 0;
            for (int k = 0; k < n; k++)
                s += a[k] * b[k];
            return s;
        };
        auto all = [](float x) { return Vec4{{x, x, x, x}}; };
        switch (mac)
        {
        case 1: m = A; break;
        case 2: for (int k = 0; k < 4; k++) m[k] = A[k] * B[k]; break;
        case 3: for (int k = 0; k < 4; k++) m[k] = A[k] + C[k]; break;
        case 4: for (int k = 0; k < 4; k++) m[k] = A[k] * B[k] + C[k]; break;
        case 5: m = all(dot(A, B, 3)); break;
        case 6: m = all(dot(A, B, 3) + B[3]); break;
        case 7: m = all(dot(A, B, 4)); break;
        case 8: m = Vec4{{1, A[1] * B[1], A[2], B[3]}}; break;
        case 9: for (int k = 0; k < 4; k++) m[k] = std::min(A[k], B[k]); break;
        case 10: for (int k = 0; k < 4; k++) m[k] = std::max(A[k], B[k]); break;
        case 11: for (int k = 0; k < 4; k++) m[k] = A[k] < B[k] ? 1.f : 0.f; break;
        case 12: for (int k = 0; k < 4; k++) m[k] = A[k] >= B[k] ? 1.f : 0.f; break;
        case 13: a0 = int(std::floor(A[0] + 0.001f)); break; // ARL
        }
        const float x = C[0]; // the ILU works on C's x (its swizzle broadcast)
        switch (ilu)
        {
        case 1: l = C; break;
        case 2: l = all(x == 0 ? INFINITY : 1.f / x); break;
        case 3:
        {
            float v = 1.f / x;
            v = v > 0 ? std::clamp(v, 5.42101e-20f, 1.884467e19f) : std::clamp(v, -1.884467e19f, -5.42101e-20f);
            l = all(v);
            break;
        }
        case 4: l = all(1.f / std::sqrt(std::fabs(x))); break;
        case 5: l = Vec4{{std::exp2(std::floor(x)), x - std::floor(x), std::exp2(x), 1}}; break;
        case 6:
        {
            const float ax = std::fabs(x);
            const float e = std::floor(std::log2(ax));
            l = Vec4{{e, ax / std::exp2(e), std::log2(ax), 1}};
            break;
        }
        case 7:
        {
            const float nx = std::max(C[0], 0.f), ny = std::max(C[1], 0.f), w = std::clamp(C[3], -127.9961f, 127.9961f);
            l = Vec4{{1, nx, nx > 0 ? std::pow(ny, w) : 0, 1}};
            break;
        }
        }
        // writes: masks are xyzw = bits 3..0
        auto write = [](Vec4& dst, const Vec4& v, u32 mask) {
            for (int k = 0; k < 4; k++)
                if (mask & (8 >> k))
                    dst[k] = v[k];
        };
        const int out_r = int(Field(t[3], 20, 4));
        const u32 mac_mask = Field(t[3], 24, 4), ilu_mask = Field(t[3], 16, 4), o_mask = Field(t[3], 12, 4);
        const bool paired = mac && ilu;
        if (mac && mac != 13 && mac_mask && !(paired && out_r == 1))
            write(out_r == 12 ? out[0] : r[std::min(out_r, 12)], m, mac_mask);
        if (ilu && ilu_mask)
            write(paired ? r[1] : (out_r == 12 ? out[0] : r[std::min(out_r, 12)]), l, ilu_mask);
        if (o_mask && Field(t[3], 11, 1)) // to an output register (not a constant)
        {
            const int o = int(Field(t[3], 3, 8) & 0xf);
            write(out[o], Field(t[3], 2, 1) ? l : m, o_mask);
        }
        if (Field(t[3], 0, 1)) // FINAL
            break;
    }
    return out[0];
}

// ---- vertices

// Attribute `a` of vertex `index` from its array.
Vec4 FetchAttribute(int a, u32 index)
{
    const KelvinState& k = g_kelvin;
    const u32 format = k.Get(0x1760 + a * 4), offset = k.Get(0x1720 + a * 4);
    const u32 type = format & 0xf, size = (format >> 4) & 0xf, stride = format >> 8;
    if (!size)
        return g_attribute_default[a];
    const u32 base = Nv2aDmaAddress(k.Get(offset & 0x80000000 ? 0x1a0 : 0x19c));
    if (!base)
        return g_attribute_default[a];
    const u32 p = base + (offset & 0x7fffffff) + index * stride;
    Vec4 v{{0, 0, 0, 1}};
    switch (type)
    {
    case 0: // UB_D3D: a D3DCOLOR (B, G, R, A in memory)
        v = Vec4{{Read<u8>(p + 2) / 255.f, Read<u8>(p + 1) / 255.f, Read<u8>(p) / 255.f, Read<u8>(p + 3) / 255.f}};
        break;
    case 4: // UB_OGL
        for (u32 c = 0; c < size && c < 4; c++)
            v[c] = Read<u8>(p + c) / 255.f;
        break;
    case 1: // S1: normalized shorts
        for (u32 c = 0; c < size && c < 4; c++)
            v[c] = std::max(-1.f, Read<int16_t>(p + c * 2) / 32767.f);
        break;
    case 5: // S32K
        for (u32 c = 0; c < size && c < 4; c++)
            v[c] = float(Read<int16_t>(p + c * 2));
        break;
    case 6: // CMP: 11, 11, 10 bits, normalized
    {
        const u32 w = Read<u32>(p);
        v[0] = float(int32_t(w << 21) >> 21) / 1023.f;
        v[1] = float(int32_t(w << 10) >> 21) / 1023.f;
        v[2] = float(int32_t(w) >> 22) / 511.f;
        break;
    }
    default: // F
        for (u32 c = 0; c < size && c < 4; c++)
            v[c] = Read<float>(p + c * 4);
    }
    return v;
}

void EmitVertex(const Vec4 in[16])
{
    Vertex v;
    RunProgram(in, v.o);
    g_vertices.push_back(v);
}

void ArrayVertex(u32 index)
{
    Vec4 in[16];
    for (int a = 0; a < 16; a++)
        in[a] = FetchAttribute(a, index);
    EmitVertex(in);
}

// INLINE_ARRAY: the enabled attributes of each vertex, one after another
void InlineVertices()
{
    const KelvinState& k = g_kelvin;
    size_t at = 0;
    while (at < g_inline.size())
    {
        Vec4 in[16];
        for (int a = 0; a < 16; a++)
        {
            const u32 format = k.Get(0x1760 + a * 4), type = format & 0xf, size = (format >> 4) & 0xf;
            in[a] = g_attribute_default[a];
            if (!size)
                continue;
            Vec4 v{{0, 0, 0, 1}};
            if (type == 2)
                for (u32 c = 0; c < size && at < g_inline.size(); c++, at++)
                    std::memcpy(&v[c], &g_inline[at], 4);
            else if (at < g_inline.size()) // a packed dword: colours
            {
                const u32 w = g_inline[at++];
                v = type == 0 ? Vec4{{((w >> 16) & 255) / 255.f, ((w >> 8) & 255) / 255.f, (w & 255) / 255.f, (w >> 24) / 255.f}}
                              : Vec4{{(w & 255) / 255.f, ((w >> 8) & 255) / 255.f, ((w >> 16) & 255) / 255.f, (w >> 24) / 255.f}};
            }
            in[a] = v;
        }
        EmitVertex(in);
    }
}

// ---- rasterization

struct ScreenVertex
{
    float x, y, z, inv_w;
    Vec4 o[16];
};

ScreenVertex ToScreen(const Vertex& v)
{
    ScreenVertex s;
    // the NV2A's vertex programs write screen coordinates (the viewport is in the program); w is kept
    // for perspective-correct interpolation
    s.x = v.o[0][0];
    s.y = v.o[0][1];
    s.z = v.o[0][2];
    const float w = v.o[0][3];
    s.inv_w = w != 0 ? 1.f / w : 1.f;
    for (int i = 0; i < 16; i++)
        s.o[i] = v.o[i];
    return s;
}

bool DepthTest(u32 func, float z, float stored)
{
    switch (func & 7) // GL's NEVER .. ALWAYS (0x200-0x207)
    {
    case 0: return false;
    case 1: return z < stored;
    case 2: return z == stored;
    case 3: return z <= stored;
    case 4: return z > stored;
    case 5: return z != stored;
    case 6: return z >= stored;
    default: return true;
    }
}

// rows are dealt to the threads in bands of this many
constexpr int kBand = 16;

// Draws the part of the triangle in bands `part`, `part + parts`, ...
void Triangle(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c, int part = 0, int parts = 1)
{
    const KelvinState& k = g_kelvin;
    const Surface cs = ColorSurface(), zs = ZetaSurface();
    if (!cs.address || !cs.pitch || cs.swizzled)
        return;
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    static int logged = 0;
    if (g_verbose >= 2 && logged < 3 && part == 0)
    {
        logged++;
        Log("nv2a: triangle area %g; surface %#x %ux%u swizzled %d; cull %u/%#x front %#x; depth test %u func %#x "
            "write %u; color mask %#x; zeta %#x",
            area, cs.address, cs.width, cs.height, cs.swizzled, k.Get(0x308), k.Get(0x39c), k.Get(0x3b8), k.Get(0x30c),
            k.Get(0x354), k.Get(0x35c), k.Get(0x358), zs.address);
    }
    if (area == 0)
        return;
    // culling: SET_CULL_FACE_ENABLE, SET_CULL_FACE (front 0x404, back 0x405), SET_FRONT_FACE (cw 0x900)
    if (k.Get(0x308))
    {
        // screen y points down: a positive area is clockwise on screen; front is clockwise unless
        // SET_FRONT_FACE says counter-clockwise (0x901), as in Direct3D
        const bool clockwise = area > 0;
        const bool front = k.Get(0x3b8) == 0x901 ? !clockwise : clockwise;
        const u32 cull = k.Get(0x39c);
        if ((cull == 0x404 && front) || (cull == 0x405 && !front) || cull == 0x408)
            return;
    }
    const u32 clip_x0 = k.Get(0x200) & 0xffff, clip_y0 = k.Get(0x204) & 0xffff;
    const int x0 = std::max<int>(int(clip_x0), int(std::floor(std::min({a.x, b.x, c.x}))));
    const int x1 = std::min<int>(int(cs.width) - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max<int>(int(clip_y0), int(std::floor(std::min({a.y, b.y, c.y}))));
    const int y1 = std::min<int>(int(cs.height) - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    const bool depth_test = k.Get(0x30c) && zs.address;
    const bool depth_write = k.Get(0x35c) && zs.address;
    const float z_max = zs.bytes == 2 ? 65535.f : 16777215.f;
    const u32 write_mask = k.Get(0x358) ? ((k.Get(0x358) & 0x01000000 ? 0xff000000u : 0) | (k.Get(0x358) & 0x00010000 ? 0x00ff0000u : 0) |
                                           (k.Get(0x358) & 0x00000100 ? 0x0000ff00u : 0) | (k.Get(0x358) & 0x00000001 ? 0x000000ffu : 0))
                                        : 0xffffffffu;
    // the outputs the pixel stage reads
    const u32 inputs = PixelInputMask();
    int used[16], used_count = 0;
    for (int o = 0; o < 16; o++)
        if (inputs & (1u << o))
            used[used_count++] = o;
    // edge functions, stepped per pixel: e(x+1) = e(x) + dx, e(y+1) = e(y) + dy
    const float sign = area < 0 ? -1.f : 1.f, inv_area = 1.f / std::fabs(area);
    auto edge = [&](const ScreenVertex& p, const ScreenVertex& q, float px, float py) {
        return sign * ((p.x - px) * (q.y - py) - (p.y - py) * (q.x - px));
    };
    const float dax = -sign * (c.y - b.y), dbx = -sign * (a.y - c.y), dcx = -sign * (b.y - a.y);
    for (int y = y0; y <= y1; y++)
    {
        if ((y / kBand) % parts != part)
        {
            y = (y / kBand + 1) * kBand - 1; // to the end of this band
            continue;
        }
        const float py = y + 0.5f, px0 = x0 + 0.5f;
        float ea = edge(b, c, px0, py), eb = edge(c, a, px0, py), ec = edge(a, b, px0, py);
        for (int x = x0; x <= x1; x++, ea += dax, eb += dbx, ec += dcx)
        {
            if (ea < 0 || eb < 0 || ec < 0)
                continue;
            const float wa = ea * inv_area, wb = eb * inv_area, wc = ec * inv_area;
            const float z = a.z * wa + b.z * wb + c.z * wc;
            u32 zp = 0, zi = 0;
            if (depth_test || depth_write)
            {
                // compared as the integer the buffer holds
                zp = zs.address + u32(y) * zs.pitch + u32(x) * zs.bytes;
                const u32 stored = zs.bytes == 2 ? Read<u16>(zp) : Read<u32>(zp) >> 8;
                zi = u32(std::clamp(z, 0.f, z_max) + 0.5f);
                if (depth_test && !DepthTest(k.Get(0x354), float(zi), float(stored)))
                    continue;
            }
            // perspective-correct interpolation of the outputs
            const float pa0 = wa * a.inv_w, pb0 = wb * b.inv_w, pc0 = wc * c.inv_w;
            const float norm = 1.f / (pa0 + pb0 + pc0), pa = pa0 * norm, pb = pb0 * norm, pc = pc0 * norm;
            PixelInputs in;
            for (int n = 0; n < used_count; n++)
            {
                const int o = used[n];
                for (int i = 0; i < 4; i++)
                    in.v[o][i] = a.o[o][i] * pa + b.o[o][i] * pb + c.o[o][i] * pc;
            }
            u32 color;
            if (!ShadePixel(in, cs, u32(x), u32(y), color))
                continue;
            if (depth_write)
            {
                if (zs.bytes == 2)
                    Write<u16>(zp, u16(zi));
                else
                    Write<u32>(zp, (Read<u32>(zp) & 0xff) | (zi << 8));
            }
            WritePixel(cs, u32(x), u32(y), color, write_mask);
        }
    }
}

} // namespace

// ---- points and lines: one pixel at a time on the drawing thread (they're few and thin)

struct Fragments
{
    Surface cs, zs;
    bool depth_test = false, depth_write = false;
    float z_max = 0;
    u32 write_mask = 0xffffffff;
    int used[16] = {}, used_count = 0;
    int x0 = 0, y0 = 0;

    Fragments()
    {
        const KelvinState& k = g_kelvin;
        cs = ColorSurface(), zs = ZetaSurface();
        depth_test = k.Get(0x30c) && zs.address;
        depth_write = k.Get(0x35c) && zs.address;
        z_max = zs.bytes == 2 ? 65535.f : 16777215.f;
        const u32 m = k.Get(0x358);
        if (m)
            write_mask = (m & 0x01000000 ? 0xff000000u : 0) | (m & 0x00010000 ? 0x00ff0000u : 0) |
                         (m & 0x00000100 ? 0x0000ff00u : 0) | (m & 0x00000001 ? 0x000000ffu : 0);
        const u32 inputs = PixelInputMask();
        for (int o = 0; o < 16; o++)
            if (inputs & (1u << o))
                used[used_count++] = o;
        x0 = int(k.Get(0x200) & 0xffff), y0 = int(k.Get(0x204) & 0xffff);
    }
    bool Usable() const { return cs.address && cs.pitch && !cs.swizzled; }

    // the pixel at (x, y), a fraction t of the way from a to b
    void Pixel(int x, int y, const ScreenVertex& a, const ScreenVertex& b, float t)
    {
        if (x < x0 || y < y0 || x >= int(cs.width) || y >= int(cs.height))
            return;
        const KelvinState& k = g_kelvin;
        const float z = a.z + (b.z - a.z) * t;
        u32 zp = 0, zi = 0;
        if (depth_test || depth_write)
        {
            zp = zs.address + u32(y) * zs.pitch + u32(x) * zs.bytes;
            const u32 stored = zs.bytes == 2 ? Read<u16>(zp) : Read<u32>(zp) >> 8;
            zi = u32(std::clamp(z, 0.f, z_max) + 0.5f);
            if (depth_test && !DepthTest(k.Get(0x354), float(zi), float(stored)))
                return;
        }
        // perspective-correct along the line
        const float pa = (1 - t) * a.inv_w, pb = t * b.inv_w, norm = 1.f / (pa + pb);
        PixelInputs in;
        for (int n = 0; n < used_count; n++)
            for (int i = 0; i < 4; i++)
                in.v[used[n]][i] = (a.o[used[n]][i] * pa + b.o[used[n]][i] * pb) * norm;
        u32 color;
        if (!ShadePixel(in, cs, u32(x), u32(y), color))
            return;
        if (depth_write)
        {
            if (zs.bytes == 2)
                Write<u16>(zp, u16(zi));
            else
                Write<u32>(zp, (Read<u32>(zp) & 0xff) | (zi << 8));
        }
        WritePixel(cs, u32(x), u32(y), color, write_mask);
    }

    // a line: a pixel a step along its longer axis, its last pixel left out (as GL draws them)
    void Line(const ScreenVertex& a, const ScreenVertex& b)
    {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const int steps = int(std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
        for (int i = 0; i < std::max(steps, 1); i++)
        {
            const float t = steps ? float(i) / float(steps) : 0.f;
            Pixel(int(std::floor(a.x + dx * t)), int(std::floor(a.y + dy * t)), a, b, t);
        }
    }

    // a point: a square of SET_POINT_SIZE (in 1/8 pixels) around it
    void Point(const ScreenVertex& p)
    {
        const float size = std::max(1.f, float(g_kelvin.Get(0x43c) & 0x1ff) / 8.f);
        const int x0p = int(std::floor(p.x - size / 2 + 0.5f)), y0p = int(std::floor(p.y - size / 2 + 0.5f));
        const int n = std::max(1, int(size + 0.5f));
        for (int y = y0p; y < y0p + n; y++)
            for (int x = x0p; x < x0p + n; x++)
                Pixel(x, y, p, p, 0);
    }
};

// ---- the rasterizer's threads

namespace {

std::mutex g_pool_mutex;
std::condition_variable g_pool_wake, g_pool_done;
const std::function<void(int)>* g_pool_job = nullptr;
int g_pool_parts = 0, g_pool_next = 0, g_pool_pending = 0;
u64 g_pool_generation = 0;

} // namespace

int RasterThreads()
{
    static const int n = [] {
        if (const char* forced = std::getenv("XB_RASTER_THREADS"))
            return std::clamp(std::atoi(forced), 1, 16);
        const int hw = int(std::thread::hardware_concurrency());
        return std::clamp(hw > 1 ? hw - 1 : 4, 1, 6);
    }();
    return n;
}

namespace {

void PoolWorker()
{
    u64 seen = 0;
    for (;;)
    {
        std::unique_lock lock(g_pool_mutex);
        g_pool_wake.wait(lock, [&] { return g_pool_generation != seen && g_pool_next < g_pool_parts; });
        seen = g_pool_generation;
        while (g_pool_next < g_pool_parts)
        {
            const int part = g_pool_next++;
            const auto* job = g_pool_job;
            lock.unlock();
            (*job)(part);
            lock.lock();
            if (--g_pool_pending == 0)
                g_pool_done.notify_all();
        }
    }
}

} // namespace

// Runs fn(0) .. fn(parts - 1), this thread taking a share; returns when all are done.
void RunParts(int parts, const std::function<void(int)>& fn)
{
    if (parts <= 1)
    {
        fn(0);
        return;
    }
    static std::once_flag started;
    std::call_once(started, [] {
        for (int i = 1; i < RasterThreads(); i++)
            std::thread(PoolWorker).detach();
    });
    std::unique_lock lock(g_pool_mutex);
    g_pool_job = &fn;
    g_pool_parts = parts, g_pool_next = 0, g_pool_pending = parts;
    g_pool_generation++;
    g_pool_wake.notify_all();
    while (g_pool_next < g_pool_parts)
    {
        const int part = g_pool_next++;
        lock.unlock();
        fn(part);
        lock.lock();
        --g_pool_pending;
    }
    g_pool_done.wait(lock, [] { return g_pool_pending == 0; });
    g_pool_job = nullptr;
}

namespace {

void Flush()
{
    std::vector<ScreenVertex> s;
    s.reserve(g_vertices.size());
    for (const Vertex& v : g_vertices)
        s.push_back(ToScreen(v));
    const size_t n = s.size();
    static int logged = 0;
    if (g_verbose >= 2 && logged < 3 && n)
    {
        logged++;
        Log("nv2a: primitive %u, %zu vertices, program start %u", g_primitive, n, g_kelvin.Get(0x1ea0));
        for (size_t i = 0; i < n && i < 4; i++)
            Log("  vertex %zu: pos %g %g %g %g  diffuse %g %g %g %g", i, s[i].o[0][0], s[i].o[0][1], s[i].o[0][2],
                s[i].o[0][3], s[i].o[3][0], s[i].o[3][1], s[i].o[3][2], s[i].o[3][3]);
    }
    // the primitive as triangles (indices into s)
    std::vector<std::array<u32, 3>> tris;
    auto add = [&](size_t i, size_t j, size_t k) { tris.push_back({u32(i), u32(j), u32(k)}); };
    switch (g_primitive)
    {
    case 5: // triangles
        for (size_t i = 0; i + 2 < n; i += 3)
            add(i, i + 1, i + 2);
        break;
    case 6: // triangle strip
        for (size_t i = 0; i + 2 < n; i++)
            if (i & 1)
                add(i + 1, i, i + 2);
            else
                add(i, i + 1, i + 2);
        break;
    case 7: // triangle fan
    case 10: // polygon
        for (size_t i = 1; i + 1 < n; i++)
            add(0, i, i + 1);
        break;
    case 8: // quads
        for (size_t i = 0; i + 3 < n; i += 4)
            add(i, i + 1, i + 2), add(i, i + 2, i + 3);
        break;
    case 9: // quad strip
        for (size_t i = 0; i + 3 < n; i += 2)
            add(i, i + 1, i + 3), add(i, i + 3, i + 2);
        break;
    case 1: // points
    case 2: // lines
    case 3: // line loop
    case 4: // line strip
    {
        PreparePixels();
        Fragments f;
        if (f.Usable())
        {
            if (g_primitive == 1)
                for (size_t i = 0; i < n; i++)
                    f.Point(s[i]);
            else if (g_primitive == 2)
                for (size_t i = 0; i + 1 < n; i += 2)
                    f.Line(s[i], s[i + 1]);
            else
            {
                for (size_t i = 0; i + 1 < n; i++)
                    f.Line(s[i], s[i + 1]);
                if (g_primitive == 3 && n > 2)
                    f.Line(s[n - 1], s[0]);
            }
        }
        g_vertices.clear();
        return;
    }
    default:
        break;
    }
    // how much there is to draw: small draws stay on this thread
    float pixels = 0;
    for (const auto& t : tris)
    {
        const ScreenVertex &a = s[t[0]], &b = s[t[1]], &c = s[t[2]];
        pixels += std::fabs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) * 0.5f;
    }
    if (tris.empty())
    {
        g_vertices.clear();
        return;
    }
    PreparePixels();
    const int parts = pixels < 4096 ? 1 : RasterThreads();
    const auto start = std::chrono::steady_clock::now();
    RunParts(parts, [&](int part) {
        for (const auto& t : tris)
            Triangle(s[t[0]], s[t[1]], s[t[2]], part, parts);
    });
    // -v 1: how long drawing takes, every 2 seconds
    static double busy = 0, area = 0;
    static auto report = start;
    const auto end = std::chrono::steady_clock::now();
    busy += std::chrono::duration<double>(end - start).count();
    area += pixels;
    if (g_verbose >= 1 && end - report > std::chrono::seconds(2))
    {
        const double span = std::chrono::duration<double>(end - report).count();
        Log("nv2a: rasterizing took %.0f%% of the last %.1f s (%.1f Mpixels/s drawn, %d threads)", 100 * busy / span, span,
            area / busy / 1e6, RasterThreads());
        busy = area = 0, report = end;
    }
    g_vertices.clear();
}

} // namespace

void DrawMethod(u32 method, u32 parameter)
{
    KelvinState& k = g_kelvin;
    if (method >= 0xb00 && method < 0xb80) // SET_TRANSFORM_PROGRAM: the cursor's instruction, word by word
    {
        const u32 word = (method - 0xb00) / 4 % 4;
        if (g_program_cursor < 136)
            g_program[g_program_cursor][word] = parameter;
        if (word == 3)
            g_program_cursor++;
        return;
    }
    if (method >= 0xb80 && method < 0xc00) // SET_TRANSFORM_CONSTANT
    {
        if (g_constant_cursor < 192)
            std::memcpy(&g_constants[g_constant_cursor][g_constant_word], &parameter, 4);
        if (++g_constant_word == 4)
            g_constant_word = 0, g_constant_cursor++;
        return;
    }
    if (method >= 0x1500 && method < 0x1700) // SET_VERTEX4F and friends: the attributes' current values
    {
        if (method >= 0x1518 && method < 0x1528)
        {
            std::memcpy(&g_attribute_default[0][(method - 0x1518) / 4], &parameter, 4);
            if (method == 0x1524 && g_primitive) // the fourth component ends a vertex
                EmitVertex(g_attribute_default);
        }
        return;
    }
    switch (method)
    {
    case 0x1e9c: g_program_cursor = parameter; return;                         // SET_TRANSFORM_PROGRAM_LOAD
    case 0x1ea4: g_constant_cursor = parameter, g_constant_word = 0; return;   // SET_TRANSFORM_CONSTANT_LOAD
    case 0x17fc:                                                                // SET_BEGIN_END
        if (parameter == 0)
        {
            if (!g_inline.empty())
                InlineVertices();
            g_inline.clear();
            Flush();
            g_primitive = 0;
        }
        else
        {
            g_primitive = parameter;
            g_vertices.clear();
            g_inline.clear();
        }
        return;
    case 0x1810: // DRAW_ARRAYS: start, count - 1
    {
        const u32 start = parameter & 0xffffff, count = (parameter >> 24) + 1;
        for (u32 i = 0; i < count; i++)
            ArrayVertex(start + i);
        return;
    }
    case 0x1800: // ARRAY_ELEMENT16: two indices
        ArrayVertex(parameter & 0xffff);
        ArrayVertex(parameter >> 16);
        return;
    case 0x1808: // ARRAY_ELEMENT32
        ArrayVertex(parameter);
        return;
    case 0x1818: // INLINE_ARRAY
        g_inline.push_back(parameter);
        return;
    }
    (void)k;
}

} // namespace xb
