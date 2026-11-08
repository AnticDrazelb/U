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

    // lighting
    const u32 light_control = k.Get(0x294);
    const bool lighting = k.Get(0x314), separate_specular = light_control & 1, local_eye = light_control & (1u << 16);
    const bool specular_enable = k.Get(0x3b8);
    Vec4 d0 = diffuse, d1 = specular;
    if (lighting)
    {
        const u32 color_material = k.Get(0x298); // sources: 0 the material, 1 diffuse, 2 specular
        const u32 emission_src = color_material & 3, ambient_src = (color_material >> 2) & 3;
        const u32 diffuse_src = (color_material >> 4) & 3, specular_src = (color_material >> 6) & 3;
        auto source = [&](u32 src) -> const Vec4& { return src == 2 ? specular : diffuse; };
        const float alpha = diffuse_src == 0 ? k.F(0x3b4) : source(diffuse_src)[3];
        const Vec4 scene_ambient = Floats(0xa10, 3), emission = Floats(0x3a8, 3);
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
        Vec4 params = Floats(0x9e0, 3); // SET_SPECULAR_PARAMS: the specular curve (x + L) / (M x + N)
        if (params[0] == 0 && params[1] == 0 && params[2] == 0)
            params = Vec4{{0, -0.494592f, 1.494592f, 0}};
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
                n_dot_vp = std::max(0.f, Dot3(eye_normal, direction));
                if (local_eye)
                {
                    const Vec4 h = Normalize3(Vec4{{direction[0] + eye_dir[0], direction[1] + eye_dir[1], direction[2] + eye_dir[2], 0}});
                    n_dot_hv = std::max(0.f, Dot3(eye_normal, h));
                }
                else
                    n_dot_hv = std::max(0.f, Dot3(eye_normal, Floats(l + 0x28, 3)));
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
                const float denominator = att[0] + att[1] * d + att[2] * d * d;
                attenuation = denominator != 0 ? 1 / denominator : 1;
                const Vec4 h = Normalize3(Vec4{{vp[0] + eye_dir[0], vp[1] + eye_dir[1], vp[2] + eye_dir[2], 0}});
                n_dot_vp = std::max(0.f, Dot3(eye_normal, vp));
                n_dot_hv = std::max(0.f, Dot3(eye_normal, h));
                if (type == 3) // spot: s = dot(direction, VP) + w, through (s + L) / (M s + N)
                {
                    const Vec4 spot = Floats(l + 0x4c);
                    const float sv = Dot3(spot, vp) + spot[3];
                    if (sv <= 0)
                        attenuation = 0;
                    else if (sv < 1)
                    {
                        Vec4 falloff = Floats(l + 0x40, 3);
                        if (falloff[0] == 0 && falloff[1] == 0 && falloff[2] == 0)
                            falloff = Vec4{{0, -0.494592f, 1.494592f, 0}};
                        float factor = 0;
                        if (sv > -falloff[0])
                        {
                            const float denom = falloff[1] * sv + falloff[2];
                            factor = denom == 0 ? 1 : denom > 0 ? std::clamp((sv + falloff[0]) / denom, 0.f, 1.f) : 0;
                        }
                        attenuation *= factor;
                    }
                }
            }
            float pf = 0;
            if (n_dot_vp > 0 && n_dot_hv > -params[0])
            {
                const float denom = params[1] * n_dot_hv + params[2];
                pf = denom == 0 ? 10000.f : denom > 0 ? std::clamp((n_dot_hv + params[0]) / denom, 0.f, 10000.f) : 0;
            }
            const Vec4 ambient = Floats(l, 3), diffuse_color = Floats(l + 0xc, 3), specular_color = Floats(l + 0x18, 3);
            for (int c = 0; c < 3; c++)
            {
                const float a = ambient[c] * attenuation, dl = diffuse_color[c] * attenuation * n_dot_vp;
                const float sl = specular_color[c] * attenuation * pf;
                d0[c] += ambient_src == 0 ? a : source(ambient_src)[c] * a;
                d0[c] += diffuse_src == 0 ? dl : source(diffuse_src)[c] * dl;
                d1[c] += specular_src == 0 ? sl : source(specular_src)[c] * sl;
            }
        }
    }
    if (!specular_enable)
        d1 = Vec4{{0, 0, 0, 1}};
    else
    {
        if (!separate_specular)
        {
            if (lighting)
                for (int c = 0; c < 3; c++)
                    d0[c] += d1[c];
            d1 = specular;
        }
        if (!(light_control & (1u << 17))) // alpha from the material's specular: else 1
            d1[3] = 1;
    }
    for (int c = 0; c < 4; c++)
        d0[c] = std::clamp(d0[c], 0.f, 1.f), d1[c] = std::clamp(d1[c], 0.f, 1.f);
    out[3] = d0, out[4] = d1;
    out[7] = in[7], out[8] = in[8];
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
    float w = pos[3];
    if (std::fabs(w) < 1e-20f)
        w = w < 0 ? -1e-20f : 1e-20f;
    const Vec4 offset = Floats(0xa20);
    out[0] = Vec4{{pos[0] / w + offset[0], pos[1] / w + offset[1], pos[2] / w, w}};
}

// The fog factor from the fog distance in output 5 (SET_FOG_MODE, SET_FOG_PARAMS), as the NV2A
// computes it per vertex: linear x + d*y; exp and exp2 through powers of two; "abs" modes take the
// magnitude. 1 when fog is off.
void FogFactor(Vec4 out[16])
{
    const KelvinState& k = g_kelvin;
    if (!k.Get(0x2a4)) // SET_FOG_ENABLE
    {
        out[5] = Vec4{{1, 1, 1, 1}};
        return;
    }
    const float d = out[5][0], x = k.F(0x9c0), y = k.F(0x9c4);
    const u32 mode = k.Get(0x29c);
    float f;
    if (mode == 0x800 || mode == 0x802) // exp
        f = x + std::exp2(d * y * 16.f) - 1.5f;
    else if (mode == 0x801 || mode == 0x803) // exp2
        f = x + std::exp2(-d * d * y * y * 32.f) - 1.5f;
    else // linear
        f = x + d * y - 1.f;
    if (mode == 0x802 || mode == 0x803 || mode == 0x804)
        f = std::fabs(f);
    if (std::isinf(d))
        f = mode == 0x802 || mode == 0x803 ? 0.f : 1.f;
    else if (std::isnan(f))
        f = mode == 0x802 || mode == 0x803 ? 0.f : 1.f;
    out[5] = Vec4{{f, f, f, f}};
}

void EmitVertex(const Vec4 in[16])
{
    Vertex v;
    if ((g_kelvin.Get(0x1e94) & 3) == 2) // SET_TRANSFORM_EXECUTION_MODE: a program
        RunProgram(in, v.o);
    else
        FixedFunction(in, v.o);
    FogFactor(v.o);
    g_vertices.push_back(v);
}

void ArrayVertex(u32 index)
{
    Vec4 in[16];
    for (int a = 0; a < 16; a++)
        in[a] = FetchAttribute(a, index);
    EmitVertex(in);
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
    s.x = std::floor(v.o[0][0] * 16.f) / 16.f;
    s.y = std::floor(v.o[0][1] * 16.f) / 16.f;
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
        z_max = zs.bytes == 2 ? 65535.f : 16777215.f;
        active = depth_test || depth_write || stencil;
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
        const u32 zi = u32(std::clamp(z, 0.f, z_max) + 0.5f);
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
    const u32 clip_x0 = k.Get(0x200) & 0xffff, clip_y0 = k.Get(0x204) & 0xffff;
    const int x0 = std::max<int>(int(clip_x0), int(std::floor(std::min({a.x, b.x, c.x}))));
    const int x1 = std::min<int>(int(cs.width) - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max<int>(int(clip_y0), int(std::floor(std::min({a.y, b.y, c.y}))));
    const int y1 = std::min<int>(int(cs.height) - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    const DepthStencil ds;
    // with the alpha test on, a pixel it rejects must leave depth and stencil alone: shade first
    const bool shade_first = k.Get(0x300);
    const u32 write_mask = k.Get(0x358) ? ((k.Get(0x358) & 0x01000000 ? 0xff000000u : 0) | (k.Get(0x358) & 0x00010000 ? 0x00ff0000u : 0) |
                                           (k.Get(0x358) & 0x00000100 ? 0x0000ff00u : 0) | (k.Get(0x358) & 0x00000001 ? 0x000000ffu : 0))
                                        : 0xffffffffu;
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
            if (!shade_first && !ds.Test(u32(x), u32(y), z))
                continue;
            // perspective-correct interpolation of the outputs
            const float pa0 = wa * a.inv_w, pb0 = wb * b.inv_w, pc0 = wc * c.inv_w;
            const float norm = 1.f / (pa0 + pb0 + pc0), pa = pa0 * norm, pb = pb0 * norm, pc = pc0 * norm;
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
        if (m)
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
    void Pixel(int x, int y, const ScreenVertex& a, const ScreenVertex& b, float t)
    {
        if (x < x0 || y < y0 || x >= int(cs.width) || y >= int(cs.height))
            return;
        const float z = a.z + (b.z - a.z) * t;
        if (!shade_first && !ds.Test(u32(x), u32(y), z))
            return;
        // perspective-correct along the line
        const float pa = (1 - t) * a.inv_w, pb = t * b.inv_w, norm = 1.f / (pa + pb);
        PixelInputs in;
        in.lod[0] = in.lod[1] = in.lod[2] = in.lod[3] = 0;
        for (int n = 0; n < used_count; n++)
            for (int i = 0; i < 4; i++)
                in.v[used[n]][i] = (a.o[used[n]][i] * pa + b.o[used[n]][i] * pb) * norm;
        u32 color;
        if (!ShadePixel(in, cs, u32(x), u32(y), color))
            return;
        if (shade_first && !ds.Test(u32(x), u32(y), z))
            return;
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
    if ((method >= 0x1500 && method < 0x1700) || (method >= 0x1880 && method < 0x1b00))
    {
        CurrentAttribute(method, parameter);
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
