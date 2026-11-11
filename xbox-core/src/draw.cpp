// Drawing for the software NV2A: vertices from the vertex arrays (or inline), the vertex program
// (the NV2A's own microcode, interpreted), primitives assembled and rasterized into the colour and
// depth surfaces; the pixel stage (textures, combiners, blending) is pixel.cpp. Rasterization is
// shared by a pool of threads, each drawing every triangle of a draw into its own bands of rows, in
// order, so the result is the same as one thread's.
//
// The vertex program's encoding is the NV2A's (xemu documents it in pgraph/glsl/vsh-prog.c).
#include <algorithm>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <string>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "core.h"
#include "diagnostics.h"
#include "gpu.h"
#include "draw.h"
#include "nv2a.h"

namespace xb {

std::atomic<bool> g_zpass_enabled{false};
std::atomic<u64> g_zpass_count{0};

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

// The vertex unit's arithmetic, as nxdk_pgraph_tests' Vertex_shader_rounding captures and notes
// measure it: a product is truncated toward zero; a sum (ADD, MAD's addition, DP3/DPH/DP4's
// accumulation) aligns every term to the largest one's leading bit less 28 (each truncated toward
// zero to that step, no sticky bit), adds them exactly, and truncates the result toward zero.
float TruncToFloat(double d)
{
    float f = float(d);
    if (std::isfinite(f) && std::fabs(double(f)) > std::fabs(d))
        f = std::nextafter(f, 0.f);
    return f;
}

float VMul(float a, float b)
{
    if (a == 0 || b == 0) // anything times 0 is 0, infinities and NaN too (Attrib_float's captures; as xemu)
        return 0;
    const float p = a * b;
    if (!std::isfinite(p) || !std::isfinite(a) || !std::isfinite(b))
        return p;
    return TruncToFloat(double(a) * double(b)); // exact in double
}

float VSum(const float* t, int n)
{
    double largest = 0;
    for (int i = 0; i < n; i++)
    {
        if (!std::isfinite(t[i]))
        {
            float s = 0;
            for (int k = 0; k < n; k++)
                s += t[k];
            return s;
        }
        largest = std::max(largest, std::fabs(double(t[i])));
    }
    if (largest == 0)
        return t[0] + (n > 1 ? t[1] : 0.f); // signed zeros as the FPU has them
    int e;
    std::frexp(largest, &e); // largest = m 2^e, m in [0.5, 1): its leading bit is 2^(e - 1)
    const double step = std::ldexp(1.0, e - 1 - 28);
    double sum = 0;
    for (int i = 0; i < n; i++)
        sum += std::trunc(double(t[i]) / step) * step; // exact: 30 bits and a few terms in a double
    return TruncToFloat(sum);
}

// A position's w as the NV2A takes it: its magnitude between 2^-64 and 2^64, +0 as +2^-64 (as xemu
// has it; the console's captures draw vertices with w 0 and infinity, nxdk_pgraph_tests' W param)
float ClampW(float w)
{
    constexpr float kLo = 0x1p-64f, kHi = 0x1p64f;
    if (std::isnan(w))
        return w;
    if (w > 0 || std::bit_cast<u32>(w) == 0)
        return std::clamp(w, kLo, kHi);
    return std::clamp(w, -kHi, -kLo);
}

// oFog's value before a program runs: a NaN with a payload no arithmetic makes, to tell an unwritten
// oFog from a written one
const float kUnwrittenFog = std::bit_cast<float>(0x7fc0f06du);
bool UnwrittenFog(float f) { return std::bit_cast<u32>(f) == 0x7fc0f06du; }

Vec4 RunProgram(const Vec4 in[16], Vec4 out[16])
{
    Vec4 r[13] = {}; // R0-R11, R12 (= oPos)
    int a0 = 0;
    for (int i = 0; i < 16; i++)
        out[i] = Vec4{};
    out[3] = out[4] = Vec4{{0, 0, 0, 1}};
    out[5][0] = kUnwrittenFog; // oFog left unwritten: the carried value (EmitVertex)
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
        auto all = [](float x) { return Vec4{{x, x, x, x}}; };
        switch (mac)
        {
        case 1: m = A; break;
        case 2: for (int k = 0; k < 4; k++) m[k] = VMul(A[k], B[k]); break;
        case 3:
            for (int k = 0; k < 4; k++)
            {
                const float t[2] = {A[k], C[k]};
                m[k] = VSum(t, 2);
            }
            break;
        case 4:
            for (int k = 0; k < 4; k++)
            {
                const float t[2] = {VMul(A[k], B[k]), C[k]};
                m[k] = VSum(t, 2);
            }
            break;
        case 5:
        case 6:
        case 7:
        {
            float t[4];
            for (int k = 0; k < 3; k++)
                t[k] = VMul(A[k], B[k]);
            t[3] = mac == 7 ? VMul(A[3], B[3]) : B[3];
            m = all(VSum(t, mac == 5 ? 3 : 4));
            break;
        }
        case 8: m = Vec4{{1, VMul(A[1], B[1]), A[2], B[3]}}; break;
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
            const Vec4& v = Field(t[3], 2, 1) ? l : m;
            if (o == 5) // oFog is one value: the x of what's written, whatever the mask (Fog_coord_vec4's
                        // captures: "mov oFog.yw, c.yw" is swizzle ywww, and its fog is c.y)
                out[5] = Vec4{{v[0], v[0], v[0], v[0]}};
            else
                write(out[o], v, o_mask);
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

// ---- the fixed-function pipeline (SET_TRANSFORM_EXECUTION_MODE fixed): what Direct3D's fixed
// pipeline asks of the NV2A, from the method registers. Matrices are 4 rows of 4 floats; a vector
// times one is its dot product with each row.

float Dot4(const Vec4& a, const Vec4& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; }
float Dot3(const Vec4& a, const Vec4& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec4 Normalize3(const Vec4& a)
{
    const float l = std::sqrt(Dot3(a, a));
    return l > 0 ? Vec4{{a[0] / l, a[1] / l, a[2] / l, 0}} : a;
}

// the 4 floats of a method's data at `method`
Vec4 Floats(u32 method, int n = 4, float w = 0)
{
    Vec4 v{{0, 0, 0, w}};
    for (int i = 0; i < n; i++)
        v[i] = g_kelvin.F(method + u32(i) * 4);
    return v;
}

// v times the matrix whose rows start at `method`
Vec4 Transform(const Vec4& v, u32 method)
{
    Vec4 out;
    for (int r = 0; r < 4; r++)
        out[r] = Dot4(v, Floats(method + u32(r) * 16));
    return out;
}

void FixedFunction(const Vec4 in[16], Vec4 out[16])
{
    const KelvinState& k = g_kelvin;
    const Vec4 &position = in[0], &weight = in[1], &diffuse = in[3], &specular = in[4];
    const Vec4 normal{{in[2][0], in[2][1], in[2][2], 0}};
    // skinning (SET_SKIN_MODE): blended model-view matrices; "G" modes make the last weight 1 - the rest
    static const int kCount[7] = {0, 2, 2, 3, 3, 4, 4};
    const u32 skin = std::min<u32>(k.Get(0x328), 6);
    const int count = kCount[skin];
    const bool generated = skin == 1 || skin == 3 || skin == 5;
    Vec4 eye_position{{0, 0, 0, 0}}, eye_normal{{0, 0, 0, 0}};
    if (count == 0)
    {
        eye_position = Transform(position, 0x480);
        eye_normal = Transform(normal, 0x580);
    }
    else
    {
        float rest = 1;
        for (int i = 0; i < count; i++)
        {
            float w = weight[i];
            if (generated)
            {
                w = i < count - 1 ? weight[i] : rest;
                rest -= weight[i];
            }
            const Vec4 p = Transform(position, 0x480 + u32(i) * 0x40), n = Transform(normal, 0x580 + u32(i) * 0x40);
            for (int c = 0; c < 4; c++)
                eye_position[c] += p[c] * w, eye_normal[c] += n[c] * w;
        }
    }
    eye_normal[3] = 0;
    if (k.Get(0x3a4)) // SET_NORMALIZATION_ENABLE
        eye_normal = Normalize3(eye_normal);

    // texture coordinates: texgen per channel (SET_TEXGEN_S..Q), then the texture matrix
    const bool infinite_viewer = k.Get(0x9cc) == 1;
    for (int s = 0; s < 4; s++)
    {
        Vec4 t = in[9 + s];
        for (int c = 0; c < 4; c++)
        {
            const u32 mode = k.Get(0x3c0 + u32(s) * 0x10 + u32(c) * 4);
            if (mode == 0)
                continue;
            const Vec4 plane = Floats(0x840 + u32(s) * 0x40 + u32(c) * 0x10);
            if (mode == 0x2400) // eye linear
                t[c] = Dot4(plane, eye_position);
            else if (mode == 0x2401) // object linear
                t[c] = Dot4(plane, position);
            else if (c < 3 && mode == 0x8511) // normal map
                t[c] = eye_normal[c];
            else if (c < 3 && (mode == 0x2402 || mode == 0x8512)) // sphere map, reflection map
            {
                const Vec4 u = infinite_viewer ? Vec4{{0, 0, 1, 0}} : Normalize3(eye_position);
                const float d = 2 * Dot3(eye_normal, u);
                const Vec4 r{{u[0] - d * eye_normal[0], u[1] - d * eye_normal[1], u[2] - d * eye_normal[2], 0}};
                if (mode == 0x8512)
                    t[c] = r[c];
                else
                {
                    const float m = 2 * std::sqrt(r[0] * r[0] + r[1] * r[1] + (r[2] + 1) * (r[2] + 1));
                    t[c] = m > 0 ? r[c] / m + 0.5f : 0.5f;
                }
            }
        }
        if (k.Get(0x420 + u32(s) * 4)) // SET_TEXTURE_MATRIX_ENABLE
            t = Transform(t, 0x6c0 + u32(s) * 0x40);
        out[9 + s] = t;
    }

    // lighting, for the front (the normal as it is) or the back (reversed, with the back material and
    // the lights' back colours: SET_LIGHT_TWO_SIDE_ENABLE)
    const u32 light_control = k.Get(0x294);
    const bool lighting = k.Get(0x314), separate_specular = light_control & 1, local_eye = light_control & (1u << 16);
    const bool specular_enable = k.Get(0x3b8);
    auto light = [&](bool back, Vec4& d0, Vec4& d1) {
    d0 = diffuse, d1 = specular;
    const Vec4 n = back ? Vec4{{-eye_normal[0], -eye_normal[1], -eye_normal[2], 0}} : eye_normal;
    if (lighting)
    {
        // sources: 0 the material, 1 diffuse, 2 specular (the back's in bits 8-15)
        const u32 color_material = back ? k.Get(0x298) >> 8 : k.Get(0x298);
        const u32 emission_src = color_material & 3, ambient_src = (color_material >> 2) & 3;
        const u32 diffuse_src = (color_material >> 4) & 3, specular_src = (color_material >> 6) & 3;
        auto source = [&](u32 src) -> const Vec4& { return src == 2 ? specular : diffuse; };
        const float alpha = diffuse_src == 0 ? k.F(back ? 0x17ac : 0x3b4) : source(diffuse_src)[3];
        const Vec4 scene_ambient = Floats(back ? 0x17a0 : 0xa10, 3), emission = Floats(back ? 0x17b0 : 0x3a8, 3);
        if (emission_src == 0 || ambient_src == 0)
        {
            d0 = scene_ambient;
            const u32 added = emission_src != 0 ? emission_src : ambient_src != 0 ? ambient_src : 0;
            if (added)
                for (int c = 0; c < 3; c++)
                    d0[c] += source(added)[c] * emission[c];
        }
        else
            for (int c = 0; c < 3; c++)
                d0[c] = source(emission_src)[c] + source(ambient_src)[c] * emission[c];
        d0[3] = alpha;
        d1 = Vec4{{0, 0, 0, specular[3]}};
        const Vec4 params = Floats(back ? 0x1e28 : 0x9e0, 3); // SET_SPECULAR_PARAMS: the specular curve (x + L) / (M x + N);
                                              // all zero is full specular wherever n.h > 0
        const Vec4 eye = Floats(0xa50);
        Vec4 eye_dir{{0, 0, 0, 0}};
        const float pw = eye_position[3] != 0 ? eye_position[3] : 1;
        const Vec4 p{{eye_position[0] / pw, eye_position[1] / pw, eye_position[2] / pw, 0}};
        if (local_eye && eye[3] != 0)
            eye_dir = Normalize3(Vec4{{eye[0] / eye[3] - p[0], eye[1] / eye[3] - p[1], eye[2] / eye[3] - p[2], 0}});
        const u32 mask = k.Get(0x3bc);
        for (int i = 0; i < 8; i++)
        {
            const u32 type = (mask >> (2 * i)) & 3; // 1 infinite, 2 local, 3 spot
            if (!type)
                continue;
            const u32 l = 0x1000 + u32(i) * 0x80;
            float attenuation = 1, n_dot_vp, n_dot_hv;
            if (type == 1)
            {
                const Vec4 direction = Normalize3(Floats(l + 0x34, 3));
                n_dot_vp = std::max(0.f, Dot3(n, direction));
                if (local_eye)
                {
                    const Vec4 h = Normalize3(Vec4{{direction[0] + eye_dir[0], direction[1] + eye_dir[1], direction[2] + eye_dir[2], 0}});
                    n_dot_hv = std::max(0.f, Dot3(n, h));
                }
                else
                    n_dot_hv = std::max(0.f, Dot3(n, Floats(l + 0x28, 3)));
            }
            else
            {
                const Vec4 at = Floats(l + 0x5c, 3);
                Vec4 vp{{at[0] - p[0], at[1] - p[1], at[2] - p[2], 0}};
                const float d = std::sqrt(Dot3(vp, vp));
                if (d > k.F(l + 0x24)) // out of range
                    continue;
                vp = Normalize3(vp);
                const Vec4 att = Floats(l + 0x68, 3);
                // a zero denominator: an infinitely strong light, saturating wherever it reaches (the
                // console's captures, Lighting_spotlight's all-zero attenuation)
                const float denominator = att[0] + att[1] * d + att[2] * d * d;
                attenuation = denominator != 0 ? 1 / denominator : INFINITY;
                const Vec4 h = Normalize3(Vec4{{vp[0] + eye_dir[0], vp[1] + eye_dir[1], vp[2] + eye_dir[2], 0}});
                n_dot_vp = std::max(0.f, Dot3(n, vp));
                n_dot_hv = std::max(0.f, Dot3(n, h));
                if (type == 3) // spot: s = dot(direction, VP) + w, through (s + L) / (M s + N)
                {
                    const Vec4 spot = Floats(l + 0x4c);
                    const float sv = Dot3(spot, vp) + spot[3];
                    if (sv <= 0)
                        attenuation = 0;
                    else if (sv < 1)
                    {
                        const Vec4 falloff = Floats(l + 0x40, 3);
                        float factor = 0;
                        if (sv > -falloff[0])
                        {
                            // (a zero denominator: infinite inside the cone, as the console's all-zero
                            // falloff shows)
                            const float denom = falloff[1] * sv + falloff[2];
                            factor = denom == 0 ? INFINITY : denom > 0 ? std::clamp((sv + falloff[0]) / denom, 0.f, 1.f) : 0;
                        }
                        attenuation = factor == 0 ? 0.f : attenuation * factor; // 0 times infinity is 0
                    }
                }
            }
            float pf = 0;
            if (n_dot_vp > 0 && n_dot_hv > -params[0])
            {
                const float denom = params[1] * n_dot_hv + params[2];
                pf = denom == 0 ? 10000.f : denom > 0 ? std::clamp((n_dot_hv + params[0]) / denom, 0.f, 10000.f) : 0;
            }
            const u32 colors = back ? 0xc00 + u32(i) * 0x40 : l;
            const Vec4 ambient = Floats(colors, 3), diffuse_color = Floats(colors + 0xc, 3),
                       specular_color = Floats(colors + 0x18, 3);
            // products as the vertex unit does them: anything times 0 is 0 (an infinite attenuation too)
            auto mul = [](float x, float y) { return x == 0 || y == 0 ? 0.f : x * y; };
            for (int c = 0; c < 3; c++)
            {
                const float a = mul(ambient[c], attenuation), dl = mul(mul(diffuse_color[c], attenuation), n_dot_vp);
                const float sl = mul(mul(specular_color[c], attenuation), pf);
                d0[c] += ambient_src == 0 ? a : mul(source(ambient_src)[c], a);
                d0[c] += diffuse_src == 0 ? dl : mul(source(diffuse_src)[c], dl);
                d1[c] += specular_src == 0 ? sl : mul(source(specular_src)[c], sl);
            }
        }
    }
    // without SEPARATE_SPECULAR the lit specular goes into the diffuse colour and the vertex's specular
    // passes through; with SET_SPECULAR_ENABLE off SEPARATE_SPECULAR is ignored: the lit specular goes
    // into the diffuse all the same (the console's captures, Lighting_control's NoSpec tests)
    if ((!separate_specular || !specular_enable) && lighting)
        for (int c = 0; c < 3; c++)
            d0[c] += d1[c];
    if (!specular_enable)
        d1 = Vec4{{0, 0, 0, 1}};
    else
    {
        if (!separate_specular)
            d1 = specular;
        if (!(light_control & (1u << 17))) // alpha from the material's specular: else 1
            d1[3] = 1;
    }
    for (int c = 0; c < 4; c++)
        d0[c] = std::clamp(d0[c], 0.f, 1.f), d1[c] = std::clamp(d1[c], 0.f, 1.f);
    };
    light(false, out[3], out[4]);
    if (k.Get(0x17c4) && lighting)
        light(true, out[7], out[8]);
    else
        out[7] = out[3], out[8] = out[4];
    // the point size (oPts): SET_POINT_SIZE (1/8 pixels), or with SET_POINT_PARAMS_ENABLE from the eye
    // distance: 1/sqrt(A + B d + C d^2) + bias, scaled by the size range from the minimum size, the
    // range clamped (at most 63.875)
    if (k.Get(0x318))
    {
        const float d = std::sqrt(Dot3(eye_position, eye_position));
        const float a = k.F(0xa30), b = k.F(0xa34), c = k.F(0xa38), range = k.F(0xa3c), bias = k.F(0xa48),
                    min_size = k.F(0xa4c);
        const float lo = std::min(min_size, 63.875f), hi = std::min(range + lo, 63.875f);
        const float q = a + b * d + c * d * d;
        const float size = (q > 0 ? 1.f / std::sqrt(q) : 1e30f) + bias;
        out[6] = Vec4{{std::clamp(size * range + min_size, lo, hi), 0, 0, 1}};
    }
    else
        out[6] = Vec4{{std::max(1.f, float(std::min<u32>(k.Get(0x43c), 0x1ff)) / 8.f), 0, 0, 1}};
    // the fog distance (SET_FOG_GEN_MODE): turned into the fog factor in EmitVertex
    switch (k.Get(0x2a0))
    {
    case 0: out[5][0] = std::clamp(specular[3], 0.f, 1.f); break;                          // specular alpha
    case 1: out[5][0] = std::sqrt(Dot3(eye_position, eye_position)); break;                // radial
    case 2:                                                                                // planar
    case 3:                                                                                // abs planar
    {
        const Vec4 plane = Floats(0x9d0);
        out[5][0] = Dot3(plane, eye_position) + plane[3];
        if (k.Get(0x2a0) == 3)
            out[5][0] = std::fabs(out[5][0]);
        break;
    }
    default: out[5][0] = in[5][0];                                                         // the fog coordinate
    }

    // the position: the composite matrix (the model-view one already in it, unless skinning), the
    // divide by w, the viewport's offset
    Vec4 pos = Transform(count == 0 ? position : eye_position, 0x680);
    const float w = ClampW(pos[3]);
    const Vec4 offset = Floats(0xa20);
    out[0] = Vec4{{pos[0] / w + offset[0], pos[1] / w + offset[1], pos[2] / w, w}};
}

// The fog factor from the fog distance in output 5 (SET_FOG_MODE, SET_FOG_PARAMS), as the NV2A
// computes it per vertex: linear x + d*y; exp and exp2 through powers of two; "abs" modes use the
// distance's magnitude. 1 when fog is off.
void FogFactor(Vec4 out[16])
{
    const KelvinState& k = g_kelvin;
    if (!k.Get(0x2a4)) // SET_FOG_ENABLE
    {
        out[5] = Vec4{{1, 1, 1, 1}};
        return;
    }
    const u32 mode = k.Get(0x29c);
    // the "abs" modes (EXP_ABS 0x802, EXP2_ABS 0x803, LINEAR_ABS 0x804) take the distance's magnitude
    const bool abs_mode = mode == 0x802 || mode == 0x803 || mode == 0x804;
    const float d = abs_mode ? std::fabs(out[5][0]) : out[5][0], x = k.F(0x9c0), y = k.F(0x9c4);
    float f;
    if (mode == 0x800) // exp
        f = x + std::exp2(d * y * 16.f) - 1.5f;
    else if (mode == 0x802) // exp_abs: the exponent's magnitude, negated (as the console shows it)
        f = x + std::exp2(-std::fabs(d * y) * 16.f) - 1.5f;
    else if (mode == 0x801 || mode == 0x803) // exp2
        f = x + std::exp2(-d * d * y * y * 32.f) - 1.5f;
    else // linear
        f = x + d * y - 1.f;
    // an infinite or NaN distance: full fog in EXP2 and the EXP/EXP2 abs modes, none in EXP and
    // LINEAR (the console's captures, Fog_exceptional_value)
    const bool full_fog = mode == 0x801 || mode == 0x802 || mode == 0x803;
    if (std::isinf(d))
        f = full_fog ? 0.f : 1.f;
    else if (std::isnan(f))
        f = full_fog ? 0.f : 1.f;
    out[5] = Vec4{{f, f, f, f}};
}

// The fog coordinate a vertex carries over: six values, a vertex's slot the next round robin across
// vertices and draws (even draws that write nothing). A program that leaves oFog unwritten gets its
// slot's value; a vertex with a fog coordinate leaves it there (the console's captures and notes,
// nxdk_pgraph_tests' Fog_carryover: points 5 and 11 of a batch after a marker carry its fog)
Vec4 g_carried_fog[6] = {};
u32 g_fog_slot = 0;

// the first vertex of the draw being built, before and after the transform (for the stats' note)
Vec4 g_note_in{}, g_note_out{};

void EmitVertex(const Vec4 in[16])
{
    Vertex v;
    if ((g_kelvin.Get(0x1e94) & 3) == 2) // SET_TRANSFORM_EXECUTION_MODE: a program
    {
        RunProgram(in, v.o);
        v.o[0][3] = ClampW(v.o[0][3]);
        // the colour outputs clamped to 0..1 per vertex, before they're interpolated, a NaN as 1
        // (Attrib_float's captures: an infinite colour interpolates as 1; as xemu)
        for (int o : {3, 4, 7, 8})
            for (int c = 0; c < 4; c++)
                v.o[o][c] = std::isnan(v.o[o][c]) ? 1.f : std::clamp(v.o[o][c], 0.f, 1.f);
        if (UnwrittenFog(v.o[5][0]))
            v.o[5] = g_carried_fog[g_fog_slot];
        // the specular outputs (oD1, oB1) as SET_SPECULAR_ENABLE and SET_LIGHT_CONTROL take them, as
        // for the fixed function's: off, black and opaque; on, opaque unless bit 17 (alpha from the
        // specular) is set (the console's captures, Lighting_control's VS tests)
        if (!g_kelvin.Get(0x3b8))
            v.o[4] = v.o[8] = Vec4{{0, 0, 0, 1}};
        else if (!(g_kelvin.Get(0x294) & (1u << 17)))
            v.o[4][3] = v.o[8][3] = 1;
    }
    else
        FixedFunction(in, v.o);
    g_carried_fog[g_fog_slot] = v.o[5];
    g_fog_slot = (g_fog_slot + 1) % 6;
    FogFactor(v.o);
    if (g_vertices.empty())
        g_note_in = in[0], g_note_out = v.o[0];
    g_vertices.push_back(v);
}

// An attribute read from an array stays the attribute's current value afterwards: a later draw
// without that array gets the last vertex's (as the console carries it over).
void CarryOver(const Vec4 in[16])
{
    for (int a = 1; a < 16; a++)
        if ((g_kelvin.Get(0x1760 + u32(a) * 4) >> 4) & 0xf)
            g_attribute_default[a] = in[a];
}

void ArrayVertex(u32 index)
{
    Vec4 in[16];
    for (int a = 0; a < 16; a++)
        in[a] = FetchAttribute(a, index);
    EmitVertex(in);
    CarryOver(in);
}

// ---- the attributes' current values (SET_VERTEX3F, SET_NORMAL3F, SET_DIFFUSE_COLOR4UB, ...,
// SET_VERTEX_DATA*): what an attribute without an array holds; the position's last part ends a vertex

void CurrentAttribute(u32 method, u32 parameter)
{
    auto f = [&] { float v; std::memcpy(&v, &parameter, 4); return v; };
    auto lo = [&] { return float(int16_t(parameter & 0xffff)); };
    auto hi = [&] { return float(int16_t(parameter >> 16)); };
    auto ub = [&](Vec4& v) {
        for (int c = 0; c < 4; c++)
            v[c] = float((parameter >> (8 * c)) & 0xff) / 255.f;
    };
    Vec4* a = g_attribute_default;
    bool ends_vertex = false;
    // floats into an attribute from `base`, `n` of them; `w1`: the fourth set to 1
    auto floats = [&](u32 base, int attr, bool w1, int last) {
        const int slot = int(method - base) / 4;
        a[attr][slot] = f();
        if (w1)
            a[attr][3] = 1;
        ends_vertex = attr == 0 && slot == last;
    };
    auto shorts2 = [&](int attr) { a[attr] = Vec4{{lo(), hi(), 0, 1}}; };
    auto shorts4 = [&](u32 base, int attr) {
        const int part = int(method - base) / 4;
        a[attr][part * 2] = lo(), a[attr][part * 2 + 1] = hi();
    };
    if (method < 0x150c) floats(0x1500, 0, true, 2);                                // VERTEX3F
    else if (method >= 0x1518 && method < 0x1528) floats(0x1518, 0, false, 3);      // VERTEX4F
    else if (method >= 0x1530 && method < 0x153c) floats(0x1530, 2, false, -1);     // NORMAL3F
    else if (method >= 0x1540 && method < 0x1548)                                   // NORMAL3S
    {
        const int part = int(method - 0x1540) / 4;
        a[2][part * 2] = std::max(-1.f, lo() / 32767.f);
        if (part == 0)
            a[2][1] = std::max(-1.f, hi() / 32767.f);
    }
    else if (method >= 0x1550 && method < 0x1560) floats(0x1550, 3, false, -1);     // DIFFUSE_COLOR4F
    else if (method >= 0x1560 && method < 0x156c) floats(0x1560, 3, true, -1);      // DIFFUSE_COLOR3F
    else if (method == 0x156c) ub(a[3]);                                            // DIFFUSE_COLOR4UB
    else if (method >= 0x1570 && method < 0x1580) floats(0x1570, 4, false, -1);     // SPECULAR_COLOR4F
    else if (method >= 0x1580 && method < 0x158c) floats(0x1580, 4, true, -1);      // SPECULAR_COLOR3F
    else if (method == 0x158c) ub(a[4]);                                            // SPECULAR_COLOR4UB
    else if (method >= 0x1590 && method < 0x1640)                                   // TEXCOORD0..3
    {
        // per stage: 2F, 2S, 4F, 4S at these offsets
        static const u32 k2f[4] = {0x1590, 0x15b8, 0x15e0, 0x1608}, k2s[4] = {0x1598, 0x15c0, 0x15e8, 0x1610};
        static const u32 k4f[4] = {0x15a0, 0x15c8, 0x15f0, 0x1620}, k4s[4] = {0x15b0, 0x15d8, 0x1600, 0x1630};
        for (int s = 0; s < 4; s++)
        {
            const int attr = 9 + s;
            if (method >= k2f[s] && method < k2f[s] + 8) { floats(k2f[s], attr, false, -1); a[attr][2] = 0, a[attr][3] = 1; }
            else if (method == k2s[s]) shorts2(attr);
            else if (method >= k4f[s] && method < k4f[s] + 16) floats(k4f[s], attr, false, -1);
            else if (method >= k4s[s] && method < k4s[s] + 8) shorts4(k4s[s], attr);
        }
    }
    else if (method == 0x1698) { const float v = f(); a[5] = Vec4{{v, v, v, v}}; }  // FOG_COORD
    else if (method == 0x169c) a[1] = Vec4{{f(), 0, 0, 1}};                          // WEIGHT1F
    else if (method >= 0x16a0 && method < 0x16d0)                                   // WEIGHT2F..4F
    {
        const u32 base = method < 0x16b0 ? 0x16a0 : method < 0x16c0 ? 0x16b0 : 0x16c0;
        floats(base, 1, false, -1);
    }
    else if (method >= 0x1880 && method < 0x1900)                                   // VERTEX_DATA2F_M
    {
        const int slot = int(method - 0x1880) / 4, attr = slot / 2, part = slot % 2;
        a[attr][part] = f(), a[attr][2] = 0, a[attr][3] = 1;
        ends_vertex = attr == 0 && part == 1;
    }
    else if (method >= 0x1900 && method < 0x1940)                                   // VERTEX_DATA2S
    {
        const int attr = int(method - 0x1900) / 4;
        shorts2(attr);
        ends_vertex = attr == 0;
    }
    else if (method >= 0x1940 && method < 0x1980)                                   // VERTEX_DATA4UB
    {
        const int attr = int(method - 0x1940) / 4;
        ub(a[attr]);
        ends_vertex = attr == 0;
    }
    else if (method >= 0x1980 && method < 0x1a00)                                   // VERTEX_DATA4S_M
    {
        const int slot = int(method - 0x1980) / 4, attr = slot / 2, part = slot % 2;
        a[attr][part * 2] = lo(), a[attr][part * 2 + 1] = hi();
        ends_vertex = attr == 0 && part == 1;
    }
    else if (method >= 0x1a00 && method < 0x1b00)                                   // VERTEX_DATA4F_M
    {
        const int slot = int(method - 0x1a00) / 4, attr = slot / 4, part = slot % 4;
        a[attr][part] = f();
        ends_vertex = attr == 0 && part == 3;
    }
    if (ends_vertex && g_primitive)
        EmitVertex(g_attribute_default);
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
        CarryOver(in);
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
    // positions are snapped (truncated) to the rasterizer's 1/16-pixel grid: what the 0.53125 offset
    // Direct3D-style setups add is meant for (the console covers pixel 0 from 0.53125)
    u32 ax, ay;
    AntiAliasing(ax, ay);
    s.x = std::floor(v.o[0][0] * 16.f) / 16.f * float(ax);
    s.y = std::floor(v.o[0][1] * 16.f) / 16.f * float(ay);
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

// ---- depth and stencil: the tests and their writes, for a pixel at a time

struct DepthStencil
{
    Surface zs;
    bool depth_test = false, depth_write = false, stencil = false, active = false;
    u32 depth_func = 0, stencil_func = 0, stencil_ref = 0, stencil_mask = 0, stencil_write = 0;
    u32 op_fail = 0, op_zfail = 0, op_zpass = 0;
    float z_max = 0;
    bool z_float = false; // SET_CONTROL0's Z_FORMAT: depth stored as floats
    bool w_buffer = false; // SET_CONTROL0's Z_PERSPECTIVE_ENABLE: w stored in place of z
    bool z_clamp = false;  // SET_ZMIN_MAX_CONTROL: depths outside SET_CLIP_MIN/MAX clamped (else culled)
    float clip_min = 0, clip_max = 0;

    // A primitive's polygon offset (SET_POLY_OFFSET_POINT/LINE/FILL_ENABLE, the method given): the
    // bias (SET_POLYGON_OFFSET_BIAS), and the scale factor (SET_POLYGON_OFFSET_SCALE_FACTOR) times
    // the primitive's steepest slope in screen space, of z, or with the w-buffer of 1/w (as xemu
    // has it; its notes say the console takes the slope at the first pixel drawn, the same on a plane)
    struct Offset
    {
        float bias = 0, factor = 0, slope = 0;
    };
    static Offset OffsetFor(u32 enable_method, float slope)
    {
        const KelvinState& k = g_kelvin;
        Offset o;
        if (!k.Get(enable_method))
            return o;
        o.bias = std::bit_cast<float>(k.Get(0x388));
        o.factor = std::bit_cast<float>(k.Get(0x384));
        o.slope = std::isfinite(slope) ? slope : 0;
        return o;
    }

    // The depth a pixel stores, from its interpolated z and w (perspective-correct): w with the
    // w-buffer (the console's captures store floor(w), or w as a float), with the offset; outside
    // the clip range clamped or culled (false), as SET_ZMIN_MAX_CONTROL says
    bool Value(float z, float w, const Offset& o, float& out) const
    {
        float v;
        if (w_buffer)
        {
            v = w;
            if (v > 0)
                v += o.bias + o.factor * o.slope * v * v;
            else
                v = 3.4028235e38f; // past the horizon: as far as can be
            if (std::isnan(v))
                v = 3.4028235e38f;
        }
        else
            v = z + o.bias + o.factor * o.slope;
        if (clip_max > clip_min)
        {
            if (z_clamp)
                v = std::clamp(v, clip_min, clip_max);
            else if (!(w_buffer ? z >= clip_min && z <= clip_max : v >= clip_min && v <= clip_max))
                return false; // culled by z, the w-buffer's too (ZMinMaxControl's WBuf_ZCULL captures)
        }
        out = v;
        return true;
    }

    // a depth as stored: fixed point, or the float formats (24 bits: a float without its sign and
    // last 7 mantissa bits; 16 bits: 4 exponent bits with a bias of 7, 12 mantissa), which order
    // as integers do
    u32 Encode(float z) const
    {
        z = std::clamp(z, 0.f, z_max);
        if (!z_float) // truncated, as the console stores it (its captures: 0xff8691 where rounding gives 0xff8692)
            return std::min(u32(double(z)), u32(z_max));
        u32 bits;
        std::memcpy(&bits, &z, 4);
        if (zs.bytes == 4)
            return bits >> 7;
        const int e = int((bits >> 23) & 0xff) - 120;
        if (e <= 0)
            return std::min(4095u, u32(z * 262144.f)); // below 2^-6: m / 4096 * 2^-6
        if (e >= 16)
            return 0xffff;
        return u32(e) << 12 | ((bits >> 11) & 0xfff);
    }

    DepthStencil()
    {
        const KelvinState& k = g_kelvin;
        zs = ZetaSurface();
        if (!zs.address)
            return;
        depth_test = k.Get(0x30c), depth_write = k.Get(0x35c), depth_func = k.Get(0x354);
        // SET_STENCIL_TEST_ENABLE, its function, reference, read and write masks and operations; the
        // stencil is the low byte of a Z24S8 buffer
        stencil = k.Get(0x32c) && zs.bytes == 4;
        stencil_func = k.Get(0x364), stencil_ref = k.Get(0x368) & 0xff, stencil_mask = k.Get(0x36c) & 0xff;
        stencil_write = k.Get(0x360) & 0xff;
        op_fail = k.Get(0x370), op_zfail = k.Get(0x374), op_zpass = k.Get(0x378);
        z_float = k.Get(0x290) & (1u << 12);
        w_buffer = k.Get(0x290) & (1u << 16);
        z_clamp = k.Get(0x1d78) & 0x10;
        clip_min = std::bit_cast<float>(k.Get(0x394)), clip_max = std::bit_cast<float>(k.Get(0x398));
        z_max = z_float ? (zs.bytes == 2 ? 511.9375f : 3.4028235e38f) : zs.bytes == 2 ? 65535.f : 16777215.f;
        active = depth_test || depth_write || stencil;
        static const bool debug_stencil = std::getenv("XB_DEBUG_STENCIL") != nullptr;
        if (debug_stencil)
            Log("debug: draw: depth test %u write %u func %#x; stencil %u func %#x ref %u mask %#x write %#x ops %#x %#x %#x; colour mask %#x",
                depth_test, depth_write, depth_func, stencil, stencil_func, stencil_ref, stencil_mask, stencil_write, op_fail,
                op_zfail, op_zpass, k.Get(0x358));
    }

    static u8 Op(u32 op, u8 s, u8 ref)
    {
        switch (op)
        {
        case 0x1e00: return s;                              // keep
        case 0x0000: return 0;                              // zero
        case 0x1e01: return ref;                            // replace
        case 0x1e02: return u8(s == 255 ? 255 : s + 1);     // increment, saturated
        case 0x1e03: return u8(s == 0 ? 0 : s - 1);         // decrement, saturated
        case 0x150a: return u8(~s);                         // invert
        case 0x8507: return u8(s + 1);                      // increment, wrapping
        case 0x8508: return u8(s - 1);                      // decrement, wrapping
        default: return s;
        }
    }

    // Whether the pixel at (x, y) with depth z survives; the depth and stencil written as it decides.
    bool Test(u32 x, u32 y, float z) const
    {
        if (!active || x >= zs.width || y >= zs.height)
            return true;
        const u32 p = PixelAddress(zs, x, y);
        const u32 word = zs.bytes == 2 ? Read<u16>(p) : Read<u32>(p);
        const u32 stored = zs.bytes == 2 ? word : word >> 8;
        const u8 old_stencil = zs.bytes == 2 ? 0 : u8(word);
        const u32 zi = Encode(z);
        u8 s = old_stencil;
        bool pass = true;
        if (stencil && !DepthTest(stencil_func, float(stencil_ref & stencil_mask), float(old_stencil & stencil_mask)))
        {
            s = Op(op_fail, s, u8(stencil_ref));
            pass = false;
        }
        else
        {
            const bool depth_ok = !depth_test || DepthTest(depth_func, float(zi), float(stored));
            if (stencil)
                s = Op(depth_ok ? op_zpass : op_zfail, s, u8(stencil_ref));
            pass = depth_ok;
        }
        u32 new_z = pass && depth_write ? zi : stored;
        const u8 new_s = u8((old_stencil & ~stencil_write) | (s & stencil_write));
        if (zs.bytes == 2)
        {
            if (new_z != stored)
                Write<u16>(p, u16(new_z));
        }
        else if (new_z != stored || new_s != old_stencil)
            Write<u32>(p, (new_z << 8) | new_s);
        return pass;
    }
};

// rows are dealt to the threads in bands of this many
constexpr int kBand = 16;

// The window clip (SET_WINDOW_CLIP_TYPE, _HORIZONTAL/_VERTICAL: 8 rectangles, min | max << 16,
// both inclusive): inclusive draws only inside one of them, exclusive only outside all. Until a
// program sets a window there's no clipping (nxdk's own pbkit never does).
bool g_window_clip_set = false;

struct WindowClip
{
    bool active = false, exclusive = false;
    int x0[8], x1[8], y0[8], y1[8];

    WindowClip()
    {
        if (!g_window_clip_set)
            return;
        const KelvinState& k = g_kelvin;
        exclusive = k.Get(0x2b4) & 1;
        u32 ax, ay;
        AntiAliasing(ax, ay);
        for (int i = 0; i < 8; i++)
        {
            const u32 h = k.Get(0x2c0 + i * 4), v = k.Get(0x2e0 + i * 4);
            x0[i] = int((h & 0xfff) * ax), x1[i] = int((((h >> 16) & 0xfff) + 1) * ax);
            y0[i] = int((v & 0xfff) * ay), y1[i] = int((((v >> 16) & 0xfff) + 1) * ay);
        }
        active = true;
    }

    bool Allows(int x, int y) const
    {
        if (!active)
            return true;
        for (int i = 0; i < 8; i++)
            if (x >= x0[i] && x < x1[i] && y >= y0[i] && y < y1[i])
                return !exclusive;
        return exclusive;
    }
};

} // namespace

// The window clip as one rectangle [x0, x1) x [y0, y1), when it is one (none set: the whole
// surface; inclusive with every non-empty window inside the largest; exclusive with no window): the
// GPU path's scissor. False when it takes more than a rectangle.
bool WindowClipRect(int& x0, int& y0, int& x1, int& y1)
{
    x0 = y0 = 0, x1 = y1 = 1 << 30;
    const WindowClip w;
    if (!w.active)
        return true;
    int best = -1;
    long best_area = -1;
    for (int i = 0; i < 8; i++)
        if (w.x1[i] > w.x0[i] && w.y1[i] > w.y0[i])
        {
            const long area = long(w.x1[i] - w.x0[i]) * (w.y1[i] - w.y0[i]);
            if (area > best_area)
                best = i, best_area = area;
        }
    if (w.exclusive)
        return best < 0; // nothing excluded
    if (best < 0)
    {
        x1 = y1 = 0; // nothing included: nothing drawn
        return true;
    }
    for (int i = 0; i < 8; i++)
        if (w.x1[i] > w.x0[i] && w.y1[i] > w.y0[i] &&
            (w.x0[i] < w.x0[best] || w.x1[i] > w.x1[best] || w.y0[i] < w.y0[best] || w.y1[i] > w.y1[best]))
            return false;
    x0 = w.x0[best], y0 = w.y0[best], x1 = w.x1[best], y1 = w.y1[best];
    return true;
}

namespace {

// Draws the part of the triangle in bands `part`, `part + parts`, ...
void Triangle(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c, int part = 0, int parts = 1)
{
    const KelvinState& k = g_kelvin;
    const Surface cs = ColorSurface(), zs = ZetaSurface();
    if (!cs.address || !cs.pitch)
        return;
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    static int logged = 0;
    if (g_verbose >= 2 && logged < 3 && part == 0)
    {
        logged++;
        Log("nv2a: triangle area %g; surface %#x %ux%u swizzled %d; cull %u/%#x front %#x; depth test %u func %#x "
            "write %u; color mask %#x; zeta %#x",
            area, cs.address, cs.width, cs.height, cs.swizzled, k.Get(0x308), k.Get(0x39c), k.Get(0x3a0), k.Get(0x30c),
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
        const bool front = k.Get(0x3a0) == 0x901 ? !clockwise : clockwise;
        const u32 cull = k.Get(0x39c);
        if ((cull == 0x404 && front) || (cull == 0x405 && !front) || cull == 0x408)
            return;
    }
    u32 ax, ay;
    AntiAliasing(ax, ay);
    const u32 clip_x0 = (k.Get(0x200) & 0xffff) * ax, clip_y0 = (k.Get(0x204) & 0xffff) * ay;
    const int x0 = std::max<int>(int(clip_x0), int(std::floor(std::min({a.x, b.x, c.x}))));
    const int x1 = std::min<int>(int(cs.width) - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max<int>(int(clip_y0), int(std::floor(std::min({a.y, b.y, c.y}))));
    const int y1 = std::min<int>(int(cs.height) - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    const DepthStencil ds;
    const bool perspective = k.Get(0x290) & (1u << 20); // SET_CONTROL0's TEXTURE_PERSPECTIVE_ENABLE
    // the polygon offset's slope: the steepest of z's (or with the w-buffer 1/w's) gradients
    DepthStencil::Offset offset;
    if (ds.active)
    {
        auto f = [&](const ScreenVertex& v) { return ds.w_buffer ? v.inv_w : v.z; };
        const float f0 = f(a), f1 = f(b), f2 = f(c);
        const float dx = ((f1 - f0) * (c.y - a.y) - (f2 - f0) * (b.y - a.y)) / area;
        const float dy = ((f2 - f0) * (b.x - a.x) - (f1 - f0) * (c.x - a.x)) / area;
        offset = DepthStencil::OffsetFor(0x338, std::max(std::fabs(dx), std::fabs(dy)));
    }
    const WindowClip window;
    // with the alpha test on, a pixel it rejects must leave depth and stencil alone: shade first
    const bool shade_first = k.Get(0x300);
    // SET_COLOR_MASK as written: 0 writes no colour (it starts with every channel on: KelvinState)
    const u32 write_mask = (k.Get(0x358) & 0x01000000 ? 0xff000000u : 0) | (k.Get(0x358) & 0x00010000 ? 0x00ff0000u : 0) |
                           (k.Get(0x358) & 0x00000100 ? 0x0000ff00u : 0) | (k.Get(0x358) & 0x00000001 ? 0x000000ffu : 0);
    // the outputs the pixel stage reads
    const u32 inputs = PixelInputMask();
    int used[16], used_count = 0;
    for (int o = 0; o < 16; o++)
        if (inputs & (1u << o))
            used[used_count++] = o;
    // each texture stage's level of detail, from its coordinates' gradients over the triangle
    float lods[4] = {0, 0, 0, 0};
    const ScreenVertex& c_ = c;
    for (int st = 0; st < 4; st++)
    {
        if (!(inputs & (1u << (9 + st))))
            continue;
        const int o = 9 + st;
        auto proj = [&](const ScreenVertex& v, int c) { return v.o[o][3] != 0 ? v.o[o][c] / v.o[o][3] : v.o[o][c]; };
        float d[2][2];
        for (int c = 0; c < 2; c++)
        {
            const float f0 = proj(a, c), f1 = proj(b, c), f2 = proj(c_, c);
            d[c][0] = ((f1 - f0) * (c_.y - a.y) - (f2 - f0) * (b.y - a.y)) / area;
            d[c][1] = ((f2 - f0) * (b.x - a.x) - (f1 - f0) * (c_.x - a.x)) / area;
        }
        lods[st] = TextureLod(st, d[0][0], d[1][0], d[0][1], d[1][1]);
    }
    // edge functions, stepped per pixel: e(x+1) = e(x) + dx. In doubles: with positions on the 1/16
    // grid and centres at +0.5 they're exact, so a centre on an edge is seen as on it, and the fill
    // rule decides: such a pixel is the triangle's if the edge is a left edge (the inside to its
    // right) or a top edge (horizontal, the inside below), so neighbours sharing an edge don't both
    // draw it.
    const double sign = area < 0 ? -1.0 : 1.0;
    const double exact_area = std::fabs((double(b.x) - a.x) * (double(c.y) - a.y) - (double(b.y) - a.y) * (double(c.x) - a.x));
    const double inv_area = 1.0 / exact_area;
    auto edge = [&](const ScreenVertex& p, const ScreenVertex& q, double px, double py) {
        return sign * ((p.x - px) * (q.y - py) - (p.y - py) * (q.x - px));
    };
    auto top_left = [&](const ScreenVertex& p, const ScreenVertex& q) {
        const double dx_step = sign * (double(p.y) - q.y), dy_step = sign * (double(q.x) - p.x);
        return dx_step > 0 || (dx_step == 0 && dy_step > 0);
    };
    const bool tla = top_left(b, c), tlb = top_left(c, a), tlc = top_left(a, b);
    const double dax = -sign * (double(c.y) - b.y), dbx = -sign * (double(a.y) - c.y), dcx = -sign * (double(b.y) - a.y);
    for (int y = y0; y <= y1; y++)
    {
        if ((y / kBand) % parts != part)
        {
            y = (y / kBand + 1) * kBand - 1; // to the end of this band
            continue;
        }
        const double py = y + 0.5;
        // the row's span: each edge function is linear along the row, so where all three are >= 0
        // is solved for, a pixel wider each side (the test below stays exact, the fill rule too)
        int xs = x0, xe = x1;
        {
            const double e0[3] = {edge(b, c, x0 + 0.5, py), edge(c, a, x0 + 0.5, py), edge(a, b, x0 + 0.5, py)};
            const double d[3] = {dax, dbx, dcx};
            bool empty = false;
            for (int i = 0; i < 3 && !empty; i++)
            {
                if (d[i] > 0)
                {
                    const double first = std::ceil(-e0[i] / d[i]) - 1; // the first x where it's >= 0, less one
                    if (first > double(xs - x0))
                        xs = first > double(x1 - x0) ? x1 + 1 : x0 + int(first);
                }
                else if (d[i] < 0)
                {
                    const double last = std::floor(e0[i] / -d[i]) + 1; // the last x where it's >= 0, plus one
                    if (last < double(xe - x0))
                        xe = last < 0 ? x0 - 1 : x0 + int(last);
                }
                else if (e0[i] < 0)
                    empty = true;
            }
            if (empty || xs > xe)
                continue;
        }
        double ea = edge(b, c, xs + 0.5, py), eb = edge(c, a, xs + 0.5, py), ec = edge(a, b, xs + 0.5, py);
        for (int x = xs; x <= xe; x++, ea += dax, eb += dbx, ec += dcx)
        {
            if (ea < 0 || eb < 0 || ec < 0 || (ea == 0 && !tla) || (eb == 0 && !tlb) || (ec == 0 && !tlc))
                continue;
            if (!window.Allows(x, y))
                continue;
            const float wa = float(ea * inv_area), wb = float(eb * inv_area), wc = float(ec * inv_area);
            // perspective-correct interpolation of the outputs (and w: 1 / the interpolated 1/w)
            // (with SET_CONTROL0's TEXTURE_PERSPECTIVE_ENABLE clear: the outputs interpolated linearly
            // on the screen, as the console's captures of Texture_perspective show)
            const float pa0 = wa * a.inv_w, pb0 = wb * b.inv_w, pc0 = wc * c.inv_w;
            const float norm = 1.f / (pa0 + pb0 + pc0);
            const float pa = perspective ? pa0 * norm : wa, pb = perspective ? pb0 * norm : wb,
                        pc = perspective ? pc0 * norm : wc;
            float z = 0;
            // w for the w-buffer in doubles, from the edge functions (exact on the 1/16 grid): equal w's
            // give exactly that w (in float, 0.99999994 for 1, stored as 0 where the console has 1)
            if (ds.active && !ds.Value(a.z * wa + b.z * wb + c.z * wc,
                                       ds.w_buffer ? float(exact_area / (ea / double(a.o[0][3]) + eb / double(b.o[0][3]) +
                                                                         ec / double(c.o[0][3])))
                                                   : norm,
                                       offset, z))
                continue;
            if (!shade_first && !ds.Test(u32(x), u32(y), z))
                continue;
            PixelInputs in;
            std::memcpy(in.lod, lods, sizeof(lods));
            for (int n = 0; n < used_count; n++)
            {
                const int o = used[n];
                for (int i = 0; i < 4; i++)
                    in.v[o][i] = a.o[o][i] * pa + b.o[o][i] * pb + c.o[o][i] * pc;
            }
            u32 color;
            if (!ShadePixel(in, cs, u32(x), u32(y), color))
                continue;
            if (shade_first && !ds.Test(u32(x), u32(y), z))
                continue;
            if (g_zpass_enabled.load(std::memory_order_relaxed))
                g_zpass_count.fetch_add(1, std::memory_order_relaxed);
            WritePixel(cs, u32(x), u32(y), color, write_mask);
        }
    }
}

} // namespace

// ---- points and lines: one pixel at a time on the drawing thread (they're few and thin)

struct Fragments
{
    Surface cs;
    DepthStencil ds;
    WindowClip window;
    bool shade_first = false;
    u32 write_mask = 0xffffffff;
    int used[16] = {}, used_count = 0;
    int x0 = 0, y0 = 0;

    Fragments()
    {
        const KelvinState& k = g_kelvin;
        cs = ColorSurface();
        shade_first = k.Get(0x300);
        const u32 m = k.Get(0x358);
        write_mask = (m & 0x01000000 ? 0xff000000u : 0) | (m & 0x00010000 ? 0x00ff0000u : 0) |
                     (m & 0x00000100 ? 0x0000ff00u : 0) | (m & 0x00000001 ? 0x000000ffu : 0);
        const u32 inputs = PixelInputMask();
        for (int o = 0; o < 16; o++)
            if (inputs & (1u << o))
                used[used_count++] = o;
        x0 = int(k.Get(0x200) & 0xffff), y0 = int(k.Get(0x204) & 0xffff);
    }
    bool Usable() const { return cs.address && cs.pitch; }

    // the pixel at (x, y), a fraction t of the way from a to b
    const Vec4* sprite = nullptr; // a point sprite's stage 3 coordinates, for the pixel being drawn
    DepthStencil::Offset offset;  // the polygon offset: lines' or points' (bias only: no slope)

    void Pixel(int x, int y, const ScreenVertex& a, const ScreenVertex& b, float t)
    {
        if (x < x0 || y < y0 || x >= int(cs.width) || y >= int(cs.height) || !window.Allows(x, y))
            return;
        // perspective-correct along the line
        const float pa = (1 - t) * a.inv_w, pb = t * b.inv_w, norm = 1.f / (pa + pb);
        float z = 0;
        if (ds.active &&
            !ds.Value(a.z + (b.z - a.z) * t,
                      ds.w_buffer ? float(1.0 / ((1.0 - t) / double(a.o[0][3]) + double(t) / double(b.o[0][3]))) : norm, offset, z))
            return;
        if (!shade_first && !ds.Test(u32(x), u32(y), z))
            return;
        PixelInputs in;
        in.lod[0] = in.lod[1] = in.lod[2] = in.lod[3] = 0;
        const bool perspective = g_kelvin.Get(0x290) & (1u << 20); // else linearly along the line
        for (int n = 0; n < used_count; n++)
            for (int i = 0; i < 4; i++)
                in.v[used[n]][i] = perspective ? (a.o[used[n]][i] * pa + b.o[used[n]][i] * pb) * norm
                                               : a.o[used[n]][i] * (1 - t) + b.o[used[n]][i] * t;
        if (sprite)
            for (int i = 0; i < 4; i++)
                in.v[12][i] = (*sprite)[i];
        u32 color;
        if (!ShadePixel(in, cs, u32(x), u32(y), color))
            return;
        if (shade_first && !ds.Test(u32(x), u32(y), z))
            return;
        if (g_zpass_enabled.load(std::memory_order_relaxed))
            g_zpass_count.fetch_add(1, std::memory_order_relaxed);
        WritePixel(cs, u32(x), u32(y), color, write_mask);
    }

    // A line, as the console draws it (nxdk_pgraph_tests' captures, Line width and W buffering): a
    // rectangle SET_LINE_WIDTH / 8 pixels wide along it, ending square at its two ends, covering the
    // pixels whose centres are inside (a width under 1 can cover none: the console's 0.x widths draw
    // nothing). Half-open along it (the last end's pixels left out, as a strip's next line starts there)
    // and across (one side's edge in, the other's out), so neighbouring lines don't overlap.
    void Line(const ScreenVertex& a, const ScreenVertex& b)
    {
        offset = DepthStencil::OffsetFor(0x334, 0);
        u32 ax, ay;
        AntiAliasing(ax, ay);
        // in the picture's pixels: with antialiasing the surface has ax by ay samples a pixel, and each
        // sample is tested against the rectangle as the picture has it (a 1-pixel line stays 1 pixel)
        const double sx = ax, sy = ay;
        const double width = double(g_kelvin.Get(0x380) & 0x1ff) / 8.0;
        if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y))
            return;
        const double ax0 = a.x / sx, ay0 = a.y / sy;
        const double dx = double(b.x) / sx - ax0, dy = double(b.y) / sy - ay0, len = std::sqrt(dx * dx + dy * dy);
        if (len == 0 || width <= 0)
            return;
        const double ux = dx / len, uy = dy / len, half = width / 2.0;
        // the sample rows the rectangle's corners span, within the surface
        const double ny = ux * half;
        const double ys[4] = {ay0 + ny, ay0 - ny, ay0 + dy + ny, ay0 + dy - ny};
        const int py0 = int(std::max(std::floor(*std::min_element(ys, ys + 4) * sy), double(y0)));
        const int py1 = int(std::min(std::ceil(*std::max_element(ys, ys + 4) * sy), double(cs.height) - 1));
        for (int y = py0; y <= py1; y++)
        {
            // along = (px - a) ux + ry uy in [0, len), across = ry ux - (px - a) uy in [-half, half),
            // px the sample's centre in pixels: each a range of px on this row (a sample wider each
            // side; the test below is exact)
            const double ry = (y + 0.5) / sy - ay0;
            double lo = -1e300, hi = 1e300;
            auto range = [&](double k, double c, double from, double to) { // from <= k * (px - a) + c < to
                if (k == 0)
                {
                    if (c < from || c >= to)
                        lo = 1, hi = 0;
                    return;
                }
                const double p = (from - c) / k, q = (to - c) / k;
                lo = std::max(lo, std::min(p, q)), hi = std::min(hi, std::max(p, q));
            };
            range(ux, ry * uy, 0, len);
            range(-uy, ry * ux, -half, half);
            if (lo > hi)
                continue;
            const int xs = int(std::max(std::floor((ax0 + lo) * sx - 0.5) - 1, double(x0)));
            const int xe = int(std::min(std::ceil((ax0 + hi) * sx - 0.5) + 1, double(cs.width) - 1));
            for (int x = xs; x <= xe; x++)
            {
                const double rx = (x + 0.5) / sx - ax0;
                const double along = rx * ux + ry * uy, across = ry * ux - rx * uy;
                if (along < 0 || along >= len || across < -half || across >= half)
                    continue;
                Pixel(x, y, a, b, float(along / len));
            }
        }
    }

    // a point: a square of its size (oPts; SET_POINT_SIZE when a program leaves it) around it; with
    // SET_POINT_SMOOTH_ENABLE a sprite, stage 3's coordinates going 0..1 across it
    void Point(const ScreenVertex& p)
    {
        offset = DepthStencil::OffsetFor(0x330, 0);
        u32 ax, ay;
        AntiAliasing(ax, ay);
        // the size: oPts with SET_POINT_PARAMS_ENABLE (the fixed function's from the eye distance, or a
        // program's), else SET_POINT_SIZE alone, whatever a program writes (Point_size's captures and notes)
        const float base = g_kelvin.Get(0x318) ? p.o[6][0] : std::max(1.f, float(std::min<u32>(g_kelvin.Get(0x43c), 0x1ff)) / 8.f);
        // the pixels whose centres are inside the square (at least the one under the vertex), in each
        // axis's samples (as the console's captures: a size 2.25 point is 3 pixels where centres allow)
        const double sx = double(std::min(base, 512.f)) * ax, sy = double(std::min(base, 512.f)) * ay;
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            return;
        auto span = [](double c, double size, int& first, int& last) {
            first = int(std::ceil(c - size / 2 - 0.5)), last = int(std::ceil(c + size / 2 - 0.5)) - 1;
            if (last < first)
                first = last = int(std::floor(c));
        };
        int x0p, x1p, y0p, y1p;
        span(p.x, sx, x0p, x1p);
        span(p.y, sy, y0p, y1p);
        x0p = std::max(x0p, x0 - 1), y0p = std::max(y0p, y0 - 1);
        x1p = std::min(x1p, int(cs.width)), y1p = std::min(y1p, int(cs.height));
        const bool sprites = g_kelvin.Get(0x31c);
        Vec4 coords{{0, 0, 1, 1}};
        for (int y = y0p; y <= y1p; y++)
            for (int x = x0p; x <= x1p; x++)
            {
                if (sprites)
                {
                    coords[0] = float((x + 0.5 - (p.x - sx / 2)) / sx), coords[1] = float((y + 0.5 - (p.y - sy / 2)) / sy);
                    sprite = &coords;
                }
                Pixel(x, y, p, p, 0);
            }
        sprite = nullptr;
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
// The last few draws, for the stats line (what lands where: a picture that stays black says why)
struct DrawNoteData
{
    u32 tris = 0, offscreen = 0, color = 0, tex_address = 0, tex_format = 0, tex_size = 0;
    bool textured = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    u32 mode = 0, start = 0;   // SET_TRANSFORM_EXECUTION_MODE, SET_TRANSFORM_PROGRAM_START
    Vec4 in{}, out{};          // the first vertex's position before and after the transform
    float composite[4] = {};   // the composite matrix's diagonal
    float scale[2] = {}, offset[2] = {}; // the viewport's
};
std::mutex g_notes_mutex;
DrawNoteData g_notes[3];
u32 g_notes_next = 0;

template <class Tris> // the drawing function's own triangles
void NoteDraw(const std::vector<ScreenVertex>& sv, const Tris& tris)
{
    DrawNoteData n;
    n.tris = u32(tris.size());
    bool first = true;
    for (const auto& t : tris)
        for (u32 v : t.v)
        {
            const ScreenVertex& x = sv[v];
            if (first)
                n.x0 = n.x1 = x.x, n.y0 = n.y1 = x.y, first = false;
            n.x0 = std::min(n.x0, x.x), n.x1 = std::max(n.x1, x.x);
            n.y0 = std::min(n.y0, x.y), n.y1 = std::max(n.y1, x.y);
            if (!(x.x >= -4096 && x.x <= 8192 && x.y >= -4096 && x.y <= 8192)) // NaN too
                n.offscreen++;
        }
    n.color = g_kelvin.Get(0x210);
    n.mode = g_kelvin.Get(0x1e94), n.start = g_kelvin.Get(0x1ea0);
    n.in = g_note_in, n.out = g_note_out;
    for (int i = 0; i < 4; i++)
    {
        const u32 bits = g_kelvin.Get(0x680 + u32(i) * 20); // SET_COMPOSITE_MATRIX: [i][i]
        std::memcpy(&n.composite[i], &bits, 4);
    }
    for (int i = 0; i < 2; i++)
    {
        const u32 s = g_kelvin.Get(0xaf0 + u32(i) * 4), o = g_kelvin.Get(0xa20 + u32(i) * 4);
        std::memcpy(&n.scale[i], &s, 4), std::memcpy(&n.offset[i], &o, 4);
    }
    n.textured = (g_kelvin.Get(0x1b0c) >> 30) & 1;
    n.tex_address = g_kelvin.Get(0x1b00), n.tex_format = g_kelvin.Get(0x1b04), n.tex_size = g_kelvin.Get(0x1b1c);
    std::lock_guard lock(g_notes_mutex);
    g_notes[g_notes_next++ % 3] = n;
}


namespace {

void Flush()
{
    diag::g_draws++;
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
    // the primitive as triangles (indices into s), each with its provoking vertex (whose colours a flat
    // shaded triangle takes: SET_FLAT_SHADE_OP, 1 the first, 0 the last) and the edges that belong to
    // the primitive's outline (bit 0: v0-v1, 1: v1-v2, 2: v2-v0; for polygon modes)
    struct Tri
    {
        u32 v[3];
        u32 provoking;
        u8 edges;
    };
    std::vector<Tri> tris;
    const bool first = g_kelvin.Get(0x9fc) == 1;
    auto add = [&](size_t i, size_t j, size_t k, size_t provoking, u8 edges = 7) {
        tris.push_back({{u32(i), u32(j), u32(k)}, u32(provoking), edges});
    };
    switch (g_primitive)
    {
    case 5: // triangles
        for (size_t i = 0; i + 2 < n; i += 3)
            add(i, i + 1, i + 2, first ? i : i + 2);
        break;
    case 6: // triangle strip
        for (size_t i = 0; i + 2 < n; i++)
            if (i & 1)
                add(i + 1, i, i + 2, first ? i : i + 2);
            else
                add(i, i + 1, i + 2, first ? i : i + 2);
        break;
    case 7: // triangle fan
        for (size_t i = 1; i + 1 < n; i++)
            add(0, i, i + 1, first ? i : i + 1);
        break;
    case 10: // polygon: its outline is the edges between consecutive vertices
        for (size_t i = 1; i + 1 < n; i++)
            add(0, i, i + 1, 0, u8((i == 1 ? 1 : 0) | 2 | (i + 2 == n ? 4 : 0))); // first vertex either way
        break;
    case 8: // quads (flat: the last vertex either way)
        for (size_t i = 0; i + 3 < n; i += 4)
            add(i, i + 1, i + 2, i + 3, 3), add(i, i + 2, i + 3, i + 3, 6);
        break;
    case 9: // quad strip: split as a triangle strip is (flat: the last vertex either way)
        for (size_t i = 0; i + 3 < n; i += 2)
            add(i, i + 1, i + 2, i + 3, 5), add(i + 1, i + 3, i + 2, i + 3, 3);
        break;
    case 1: // points
    case 2: // lines
    case 3: // line loop
    case 4: // line strip
    {
        PreparePixels();
        if (gpu::Enabled())
            gpu::Sync(); // drawn in software, on the surfaces' current content
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
    if (tris.empty())
    {
        g_vertices.clear();
        return;
    }
    // flat shading (SET_SHADE_MODEL 0x1d00): the provoking vertex's colours over the whole triangle;
    // two-sided lighting (SET_LIGHT_TWO_SIDE_ENABLE): a back-facing triangle's colours are its back
    // colours (oB0, oB1)
    std::vector<ScreenVertex> flat;
    const bool flat_shading = g_kelvin.Get(0x37c) == 0x1d00, two_sided = g_kelvin.Get(0x17c4);
    if (flat_shading || two_sided)
    {
        flat.reserve(tris.size() * 3);
        for (Tri& t : tris)
        {
            const ScreenVertex &a = s[t.v[0]], &b = s[t.v[1]], &c = s[t.v[2]];
            const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            const bool back = two_sided && (g_kelvin.Get(0x3a0) == 0x901 ? area > 0 : area < 0);
            for (u32& v : t.v)
            {
                ScreenVertex copy = s[v];
                const ScreenVertex& from = flat_shading ? s[t.provoking] : s[v];
                copy.o[3] = from.o[back ? 7 : 3];
                copy.o[4] = from.o[back ? 8 : 4];
                flat.push_back(copy);
                v = u32(flat.size() - 1);
            }
        }
    }
    const std::vector<ScreenVertex>* source = flat.empty() ? &s : &flat;
    // Clipping, as the NV2A clips: in front of the eye (w > 0) and, unless SET_ZMIN_MAX_CONTROL clamps
    // z (bit 4), between SET_CLIP_MIN and SET_CLIP_MAX in z. A vertex behind the eye was divided by
    // its w and lands mirrored on the screen, so a triangle crossing the eye's plane came out as a
    // huge inverted one (garbage across the picture, and hundreds of full-screen fills a frame).
    // (x*w, y*w, z*w, w) of the divided position is linear in clip space, and so are the other
    // outputs: each triangle is cut against the planes there (Sutherland-Hodgman) and divided again.
    std::vector<ScreenVertex> clipped_vertices;
    {
        constexpr float kMinW = std::numeric_limits<float>::min(); // any positive w: the console draws w 5.4e-20
        const bool clip_z = !(g_kelvin.Get(0x1d78) & 0x10);
        const float zmin = std::bit_cast<float>(g_kelvin.Get(0x394)), zmax = std::bit_cast<float>(g_kelvin.Get(0x398));
        const bool z_range = clip_z && zmax > zmin;
        // CULL_NEAR_FAR (SET_ZMIN_MAX_CONTROL bit 0) with depths clamped: a primitive wholly in front of
        // SET_CLIP_MIN or wholly behind SET_CLIP_MAX is dropped, the rest clamped per pixel (the
        // console's captures, ZMinMaxControl's NEARFAR_ZCLAMP), judged on z, the w-buffer's too
        if (!clip_z && (g_kelvin.Get(0x1d78) & 1) && zmax > zmin)
        {
            auto depth = [&](u32 v) { return (*source)[v].o[0][2]; };
            std::erase_if(tris, [&](const Tri& t) {
                const float a = depth(t.v[0]), b = depth(t.v[1]), c = depth(t.v[2]);
                return (a < zmin && b < zmin && c < zmin) || (a > zmax && b > zmax && c > zmax);
            });
        }
        auto distances = [&](const ScreenVertex& v, float d[3]) {
            const float w = v.o[0][3], hz = v.o[0][2] * w;
            d[0] = w - kMinW;
            d[1] = z_range ? hz - zmin * w : 1.f;
            d[2] = z_range ? zmax * w - hz : 1.f;
        };
        bool any = false;
        for (const Tri& t : tris)
            for (u32 v : t.v)
            {
                float d[3];
                distances((*source)[v], d);
                any |= !(d[0] >= 0 && d[1] >= 0 && d[2] >= 0);
            }
        if (any)
        {
            std::vector<Tri> kept;
            kept.reserve(tris.size());
            clipped_vertices = *source;
            // the point a fraction t from p to q, as a new vertex
            auto between = [&](u32 pi, u32 qi, float t) {
                const ScreenVertex p = clipped_vertices[pi], q = clipped_vertices[qi];
                Vertex v;
                for (int i = 1; i < 16; i++)
                    for (int c = 0; c < 4; c++)
                        v.o[i][c] = p.o[i][c] + (q.o[i][c] - p.o[i][c]) * t;
                const float wp = p.o[0][3], wq = q.o[0][3], w = wp + (wq - wp) * t;
                for (int c = 0; c < 3; c++)
                {
                    const float hp = p.o[0][c] * wp, hq = q.o[0][c] * wq;
                    v.o[0][c] = (hp + (hq - hp) * t) / w;
                }
                v.o[0][3] = w;
                clipped_vertices.push_back(ToScreen(v));
                return u32(clipped_vertices.size() - 1);
            };
            for (const Tri& t : tris)
            {
                std::vector<u32> poly(t.v, t.v + 3), next;
                for (int plane = 0; plane < 3 && !poly.empty(); plane++)
                {
                    next.clear();
                    for (size_t e = 0; e < poly.size(); e++)
                    {
                        const u32 a = poly[e], b = poly[(e + 1) % poly.size()];
                        float da[3], db[3];
                        distances(clipped_vertices[a], da);
                        distances(clipped_vertices[b], db);
                        if (da[plane] >= 0)
                            next.push_back(a);
                        if ((da[plane] >= 0) != (db[plane] >= 0))
                            next.push_back(between(a, b, da[plane] / (da[plane] - db[plane])));
                    }
                    poly.swap(next);
                }
                if (poly.size() == 3 && poly[0] == t.v[0] && poly[1] == t.v[1] && poly[2] == t.v[2])
                    kept.push_back(t);
                else
                    for (size_t i = 1; i + 1 < poly.size(); i++)
                        kept.push_back({{poly[0], poly[i], poly[i + 1]}, t.provoking, 7});
            }
            tris.swap(kept);
            source = &clipped_vertices;
            if (tris.empty())
            {
                g_vertices.clear();
                return;
            }
        }
    }
    const std::vector<ScreenVertex>& sv = *source;
    NoteDraw(sv, tris);
    PreparePixels();
    // On the GPU when it's on and does this state: the triangles the rasterizer would draw (culled
    // and zero-area ones left out as Triangle leaves them), as screen-space vertices. (Not while
    // pixels are being counted for an occlusion report: those draws are counted in software.)
    if (gpu::Enabled() && !g_zpass_enabled.load(std::memory_order_relaxed))
    {
        const bool fill = g_kelvin.Get(0x38c) != 0x1b00 && g_kelvin.Get(0x38c) != 0x1b01 &&
                          g_kelvin.Get(0x390) != 0x1b00 && g_kelvin.Get(0x390) != 0x1b01;
        int wx0, wy0, wx1, wy1;
        if (fill && WindowClipRect(wx0, wy0, wx1, wy1))
        {
            static thread_local std::vector<gpu::Vertex> gv;
            gv.clear();
            const bool culling = g_kelvin.Get(0x308);
            const u32 cull = g_kelvin.Get(0x39c);
            const bool ccw_front = g_kelvin.Get(0x3a0) == 0x901;
            for (const Tri& t : tris)
            {
                const ScreenVertex &a = sv[t.v[0]], &b = sv[t.v[1]], &c = sv[t.v[2]];
                const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                if (area == 0)
                    continue;
                if (culling)
                {
                    const bool front = ccw_front ? !(area > 0) : area > 0;
                    if ((cull == 0x404 && front) || (cull == 0x405 && !front) || cull == 0x408)
                        continue;
                }
                for (u32 v : t.v)
                {
                    const ScreenVertex& x = sv[v];
                    gpu::Vertex out;
                    out.pos[0] = x.x, out.pos[1] = x.y, out.pos[2] = x.z, out.pos[3] = x.o[0][3];
                    std::memcpy(out.diffuse, x.o[3].v, 16);
                    std::memcpy(out.specular, x.o[4].v, 16);
                    std::memcpy(out.fog, x.o[5].v, 16);
                    for (int i = 0; i < 4; i++)
                        std::memcpy(out.tex[i], x.o[9 + i].v, 16);
                    gv.push_back(out);
                }
            }
            if (gpu::DrawTriangles(gv.data(), u32(gv.size() / 3)))
            {
                g_vertices.clear();
                return;
            }
        }
        gpu::Sync(); // drawn in software, on the surfaces' current content
    }
    // polygon modes (SET_FRONT/BACK_POLYGON_MODE: 0x1b00 points, 0x1b01 lines, else filled): those
    // triangles become their outline's lines or vertices, on this thread
    const u32 front_mode = g_kelvin.Get(0x38c), back_mode = g_kelvin.Get(0x390);
    if (front_mode == 0x1b00 || front_mode == 0x1b01 || back_mode == 0x1b00 || back_mode == 0x1b01)
    {
        std::vector<Tri> filled;
        Fragments f;
        for (const Tri& t : tris)
        {
            const ScreenVertex &a = sv[t.v[0]], &b = sv[t.v[1]], &c = sv[t.v[2]];
            const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            const bool front = g_kelvin.Get(0x3a0) == 0x901 ? area < 0 : area > 0;
            if (g_kelvin.Get(0x308))
            {
                const u32 cull = g_kelvin.Get(0x39c);
                if ((cull == 0x404 && front) || (cull == 0x405 && !front) || cull == 0x408)
                    continue;
            }
            const u32 mode = front ? front_mode : back_mode;
            if (mode != 0x1b00 && mode != 0x1b01)
            {
                filled.push_back(t);
                continue;
            }
            if (!f.Usable())
                continue;
            for (int e = 0; e < 3; e++)
                if (t.edges & (1 << e))
                {
                    if (mode == 0x1b01)
                        f.Line(sv[t.v[e]], sv[t.v[(e + 1) % 3]]);
                    else
                        f.Point(sv[t.v[e]]);
                }
        }
        tris.swap(filled);
    }
    float pixels = 0;
    const Surface on = ColorSurface();
    for (const auto& t : tris)
    {
        const ScreenVertex &a = sv[t.v[0]], &b = sv[t.v[1]], &c = sv[t.v[2]];
        // its area, but no more than the part of its bounding box on the surface (one reaching far
        // past the screen covers at most the screen)
        const float bw = std::clamp(std::max({a.x, b.x, c.x}), 0.f, float(on.width)) - std::clamp(std::min({a.x, b.x, c.x}), 0.f, float(on.width));
        const float bh = std::clamp(std::max({a.y, b.y, c.y}), 0.f, float(on.height)) - std::clamp(std::min({a.y, b.y, c.y}), 0.f, float(on.height));
        pixels += std::min(std::fabs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) * 0.5f, bw * bh);
    }
    const int parts = pixels < 4096 ? 1 : RasterThreads();
    const auto start = std::chrono::steady_clock::now();
    RunParts(parts, [&](int part) {
        for (const auto& t : tris)
            Triangle(sv[t.v[0]], sv[t.v[1]], sv[t.v[2]], part, parts);
    });
    // -v 1: how long drawing takes, every 2 seconds
    static double busy = 0, area = 0;
    static auto report = start;
    const auto end = std::chrono::steady_clock::now();
    busy += std::chrono::duration<double>(end - start).count();
    diag::g_raster_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
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

// The fixed function's state is in the transform engine's context, the same 192 rows a vertex program
// reads as constants (the NV2A's XF context, as xemu has it: NV_IGRAPH_XF_XFCTX_*): the matrices, the
// texgen planes, the fog plane, the eye position, the viewport's scale and offset. A program reads
// them there: Direct3D ends every vertex shader with the viewport (c[-38], c[-37]: VPSCL and VPOFF).
void MirrorToContext(u32 method, u32 parameter)
{
    int row = -1, column = 0;
    auto matrices = [&](u32 base, u32 count, int first_row, int rows_apart) {
        if (method < base || method >= base + count * 64)
            return false;
        const u32 slot = (method - base) / 4, matrix = slot / 16, entry = slot % 16;
        row = first_row + int(matrix) * rows_apart + int(entry / 4), column = int(entry % 4);
        return true;
    };
    auto vector = [&](u32 base, int to_row) {
        if (method < base || method >= base + 16)
            return false;
        row = to_row, column = int((method - base) / 4);
        return true;
    };
    if (matrices(0x440, 1, 0x04, 0) ||       // SET_PROJECTION_MATRIX: PMAT0
        matrices(0x480, 4, 0x08, 8) ||       // SET_MODEL_VIEW_MATRIX: MMAT0..3
        matrices(0x580, 4, 0x0c, 8) ||       // SET_INVERSE_MODEL_VIEW_MATRIX: IMMAT0..3
        matrices(0x680, 1, 0x00, 0) ||       // SET_COMPOSITE_MATRIX: CMAT0
        matrices(0x6c0, 4, 0x44, 8) ||       // SET_TEXTURE_MATRIX: T0MAT..
        matrices(0x840, 4, 0x40, 8) ||       // SET_TEXGEN_PLANE_S..Q: TG0MAT..
        vector(0x9d0, 0x39) ||               // SET_FOG_PLANE: FOG
        vector(0xa20, 0x3b) ||               // SET_VIEWPORT_OFFSET: VPOFF
        vector(0xa50, 0x38) ||               // SET_EYE_POSITION: EYEP
        vector(0xaf0, 0x3a))                 // SET_VIEWPORT_SCALE: VPSCL
        std::memcpy(&g_constants[row][column], &parameter, 4);
}

void DrawMethod(u32 method, u32 parameter)
{
    KelvinState& k = g_kelvin;
    if (method >= 0x440 && method < 0xb00)
        MirrorToContext(method, parameter);
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
    if ((method >= 0x1500 && method < 0x1700) || (method >= 0x1880 && method < 0x1b00))
    {
        CurrentAttribute(method, parameter);
        return;
    }
    if (method >= 0x2c0 && method < 0x300) // SET_WINDOW_CLIP_HORIZONTAL/VERTICAL: this window and the ones after it
    {
        const u32 base = method < 0x2e0 ? 0x2c0 : 0x2e0;
        for (u32 slot = (method - base) / 4; slot < 8; slot++)
            g_kelvin.regs[(base + slot * 4) / 4] = parameter;
        g_window_clip_set = true;
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

std::string LastDrawsNote()
{
    std::lock_guard lock(g_notes_mutex);
    std::string out;
    for (u32 k = 0; k < 3 && k < g_notes_next; k++)
    {
        const DrawNoteData& n = g_notes[(g_notes_next - 1 - k) % 3];
        char b[400];
        std::snprintf(b, sizeof(b), "%s%u triangles over (%.0f,%.0f)-(%.0f,%.0f)%s into %#x%s", k ? "; " : "", n.tris, n.x0,
                      n.y0, n.x1, n.y1, n.offscreen ? " (some far off or not numbers)" : "", n.color,
                      n.textured ? "" : ", untextured");
        out += b;
        if (n.textured)
        {
            std::snprintf(b, sizeof(b), ", texture 0 at %#x format %#x (colour %#x) %ux%u", n.tex_address, n.tex_format,
                          (n.tex_format >> 8) & 0xff, n.tex_size >> 16, n.tex_size & 0xffff);
            out += b;
        }
        std::snprintf(b, sizeof(b),
                      " [transform %s (mode %#x, start %u); vertex (%g %g %g %g) -> (%g %g %g %g); composite diagonal "
                      "%g %g %g %g; viewport scale %g %g offset %g %g]",
                      (n.mode & 3) == 2 ? "program" : "fixed", n.mode, n.start, n.in[0], n.in[1], n.in[2], n.in[3],
                      n.out[0], n.out[1], n.out[2], n.out[3], n.composite[0], n.composite[1], n.composite[2],
                      n.composite[3], n.scale[0], n.scale[1], n.offset[0], n.offset[1]);
        out += b;
    }
    return out.empty() ? "none" : out;
}

} // namespace xb
