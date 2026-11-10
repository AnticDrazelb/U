#version 450
// The NV2A's pixel stage, as pixel.cpp does it (the reference: same arithmetic, same order): the
// texture stages' programs, the general combiner stages and the final combiner, the alpha test. One
// shader for every state: the state is a uniform buffer, decoded here as the CPU decodes it.
layout(location = 0) in vec4 vDiffuse;
layout(location = 1) in vec4 vSpecular;
layout(location = 2) in vec4 vFog;
layout(location = 3) in vec4 vTex0;
layout(location = 4) in vec4 vTex1;
layout(location = 5) in vec4 vTex2;
layout(location = 6) in vec4 vTex3;
layout(location = 0) out vec4 outColor;

layout(std140, binding = 0) uniform State
{
    uvec4 stageWords[8]; // icw, ocw, aicw, aocw
    vec4 stageC0[8];
    vec4 stageC1[8];
    uvec4 control;       // stages (0: no combiners, the diffuse colour), cw0, cw1, flags (1 uses ef, 2 uses sum, 4 alpha test, 8 rgb, ...)
    uvec4 stageFlags;    // per stage: bit 0 rgb writes, bit 1 alpha writes
    uvec4 alphaTest;     // function, reference
    vec4 fogColor;
    vec4 finalC0;
    vec4 finalC1;
    uvec4 program;       // each texture stage's program (0: none)
    uvec4 enabled;       // a texture bound
    uvec4 stageInput;    // which stage's result is the input of stages 2, 3
    uvec4 kind;          // 0 2D, 1 cube
    vec4 texScale[4];    // coordinates to [0, 1]: 1 for swizzled/compressed, 1/size for linear textures
    vec4 bump[4];        // the bump environment matrices
    vec4 bumpLum[4];     // x scale, y offset
    uvec4 clipMode;      // x: SET_SHADER_CLIP_PLANE_MODE
    vec4 depth;          // 1 / the depth's largest value, polygon offset bias and factor, 1 with the w-buffer
} st;

layout(binding = 1) uniform sampler2D tex2D[4];
layout(binding = 2) uniform samplerCube texCube[4];

vec4 R[16];

float clamp01(float v) { return clamp(v, 0.0, 1.0); }

vec4 sampleStage(int s, vec2 uv)
{
    uv *= st.texScale[s].xy;
    switch (s)
    {
    case 0: return texture(tex2D[0], uv);
    case 1: return texture(tex2D[1], uv);
    case 2: return texture(tex2D[2], uv);
    default: return texture(tex2D[3], uv);
    }
}

vec4 sampleCube(int s, vec3 d)
{
    switch (s)
    {
    case 0: return texture(texCube[0], d);
    case 1: return texture(texCube[1], d);
    case 2: return texture(texCube[2], d);
    default: return texture(texCube[3], d);
    }
}

// texture colours have their channels as the CPU's V4: r, g, b, a
float sign3(float x) { x *= 255.0; return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0; }

vec4 tc(int s)
{
    switch (s)
    {
    case 0: return vTex0;
    case 1: return vTex1;
    case 2: return vTex2;
    default: return vTex3;
    }
}

bool textureStages(out vec4 tex[4])
{
    for (int s = 0; s < 4; s++)
    {
        tex[s] = vec4(0.0);
        const uint program = st.program[s];
        const bool enabled = st.enabled[s] != 0u;
        const vec4 t = tc(s);
        const vec4 inp = tex[st.stageInput[s] < 4u ? int(st.stageInput[s]) : 0];
        if (program == 1u || program == 2u)
        {
            if (enabled)
            {
                const float q = program == 1u && t.w != 0.0 ? t.w : 1.0;
                tex[s] = sampleStage(s, t.xy / q);
            }
        }
        else if (program == 3u)
        {
            if (enabled)
                tex[s] = sampleCube(s, t.xyz);
        }
        else if (program == 4u)
            tex[s] = clamp(t, 0.0, 1.0);
        else if (program == 5u)
        {
            for (int j = 0; j < 4; j++)
            {
                const bool compare = ((st.clipMode.x >> (4 * s + j)) & 1u) != 0u;
                if (compare ? t[j] >= 0.0 : t[j] < 0.0)
                    return false;
            }
        }
        else if (program == 6u || program == 7u)
        {
            const float du = sign3(inp.b), dv = sign3(inp.g);
            const vec4 m = st.bump[s];
            const vec2 uv = vec2(t.x + du * m.x + dv * m.z, t.y + du * m.y + dv * m.w);
            if (enabled)
                tex[s] = sampleStage(s, uv);
            if (program == 7u)
                tex[s] *= clamp(st.bumpLum[s].x * inp.r + st.bumpLum[s].y, 0.0, 1.0);
        }
        else if (program == 15u)
        {
            if (enabled)
                tex[s] = sampleStage(s, vec2(inp.a, inp.r));
        }
        else if (program == 16u)
        {
            if (enabled)
                tex[s] = sampleStage(s, vec2(inp.g, inp.b));
        }
    }
    return true;
}

float mapInput(float x, uint map)
{
    switch (map)
    {
    case 0u: return clamp01(x);
    case 1u: return 1.0 - clamp01(x);
    case 2u: return 2.0 * max(0.0, x) - 1.0;
    case 3u: return -2.0 * max(0.0, x) + 1.0;
    case 4u: return max(0.0, x) - 0.5;
    case 5u: return -max(0.0, x) + 0.5;
    case 6u: return x;
    default: return -x;
    }
}

vec3 colorInput(uint b)
{
    const vec4 s = R[b & 15u];
    const uint map = (b >> 5) & 7u;
    if ((b & 0x10u) != 0u)
        return vec3(mapInput(s.a, map));
    return vec3(mapInput(s.r, map), mapInput(s.g, map), mapInput(s.b, map));
}

float alphaInput(uint b)
{
    const vec4 s = R[b & 15u];
    return mapInput((b & 0x10u) != 0u ? s.a : s.b, (b >> 5) & 7u);
}

float applyOp(float v, uint op)
{
    if (op == 1u) v -= 0.5;
    else if (op == 2u) v *= 2.0;
    else if (op == 3u) v = (v - 0.5) * 2.0;
    else if (op == 4u) v *= 4.0;
    else if (op == 6u) v *= 0.5;
    return clamp(v, -1.0, 1.0);
}

void generalStage(int i)
{
    R[1] = st.stageC0[i];
    R[2] = st.stageC1[i];
    const uvec4 w = st.stageWords[i];
    const uint icw = w.x, ocw = w.y, aicw = w.z, aocw = w.w;
    const bool rgb = (st.stageFlags[i / 4] >> (8u * uint(i % 4)) & 1u) != 0u;
    const bool alpha = (st.stageFlags[i / 4] >> (8u * uint(i % 4)) & 2u) != 0u;
    const bool mux = R[12].a >= 0.5;
    vec3 ab = vec3(0.0), cd = vec3(0.0), sum = vec3(0.0);
    float alphaAb = 0.0, alphaCd = 0.0, alphaSum = 0.0;
    if (rgb || (ocw & (3u << 18)) != 0u)
    {
        const vec3 a = colorInput(icw >> 24), b = colorInput((icw >> 16) & 0xffu);
        const vec3 c = colorInput((icw >> 8) & 0xffu), d = colorInput(icw & 0xffu);
        const float abDot = a.x * b.x + a.y * b.y + a.z * b.z, cdDot = c.x * d.x + c.y * d.y + c.z * d.z;
        const uint op = (ocw >> 15) & 7u;
        for (int k = 0; k < 3; k++)
        {
            ab[k] = (ocw & (1u << 13)) != 0u ? abDot : a[k] * b[k];
            cd[k] = (ocw & (1u << 12)) != 0u ? cdDot : c[k] * d[k];
            sum[k] = (ocw & (1u << 14)) != 0u ? (mux ? cd[k] : ab[k]) : ab[k] + cd[k];
            ab[k] = applyOp(ab[k], op);
            cd[k] = applyOp(cd[k], op);
            sum[k] = applyOp(sum[k], op);
        }
    }
    if (alpha)
    {
        const float aa = alphaInput(aicw >> 24), ab_ = alphaInput((aicw >> 16) & 0xffu);
        const float ac = alphaInput((aicw >> 8) & 0xffu), ad = alphaInput(aicw & 0xffu);
        const uint aop = (aocw >> 15) & 7u;
        alphaAb = applyOp(aa * ab_, aop);
        alphaCd = applyOp(ac * ad, aop);
        alphaSum = applyOp((aocw & (1u << 14)) != 0u ? (mux ? ac * ad : aa * ab_) : aa * ab_ + ac * ad, aop);
    }
    if (rgb)
    {
        uint d = (ocw >> 4) & 0xfu;
        if (d != 0u) R[d].rgb = ab;
        d = ocw & 0xfu;
        if (d != 0u) R[d].rgb = cd;
        d = (ocw >> 8) & 0xfu;
        if (d != 0u) R[d].rgb = sum;
    }
    if (alpha)
    {
        uint d = (aocw >> 4) & 0xfu;
        if (d != 0u) R[d].a = alphaAb;
        d = aocw & 0xfu;
        if (d != 0u) R[d].a = alphaCd;
        d = (aocw >> 8) & 0xfu;
        if (d != 0u) R[d].a = alphaSum;
        if ((ocw & (1u << 19)) != 0u && ((ocw >> 4) & 0xfu) != 0u)
            R[(ocw >> 4) & 0xfu].a = ab.z;
        if ((ocw & (1u << 18)) != 0u && (ocw & 0xfu) != 0u)
            R[ocw & 0xfu].a = cd.z;
    }
}

vec4 finalInput(uint b, vec4 sum, vec4 ef)
{
    const uint source = b & 0xfu;
    const bool invert = (b & 0xe0u) != 0u;
    if (source == 0u)
        return vec4(invert ? 1.0 : 0.0);
    const vec4 s = source == 14u ? sum : source == 15u ? ef : R[source];
    vec4 v;
    for (int i = 0; i < 4; i++)
    {
        const float x = clamp01((b & 0x10u) != 0u ? s.a : s[i]);
        v[i] = invert ? 1.0 - x : x;
    }
    return v;
}

bool compare(uint func, uint a, uint b)
{
    switch (func & 7u)
    {
    case 0u: return false;
    case 1u: return a < b;
    case 2u: return a == b;
    case 3u: return a <= b;
    case 4u: return a > b;
    case 5u: return a != b;
    case 6u: return a >= b;
    default: return true;
    }
}

void main()
{
#ifdef XB_DEPTH
    // The depth the console stores (as the software path's DepthStencil::Value): with the w-buffer the
    // perspective-correct w (1 / the interpolated 1/w); the polygon offset's bias, and its factor times
    // the steepest slope of z (of 1/w with the w-buffer, times w squared) across the primitive
    {
        float z;
        if (st.depth.w != 0.0)
        {
            const float w = 1.0 / gl_FragCoord.w;
            const float slope = max(abs(dFdx(gl_FragCoord.w)), abs(dFdy(gl_FragCoord.w)));
            z = w > 0.0 ? w + st.depth.y + st.depth.z * slope * w * w : 3.4e38;
        }
        else
        {
            const float zu = gl_FragCoord.z / st.depth.x;
            z = zu + st.depth.y + st.depth.z * max(abs(dFdx(zu)), abs(dFdy(zu)));
        }
        gl_FragDepth = clamp(z * st.depth.x, 0.0, 1.0);
    }
#endif
    vec4 color;
    if (st.control.x == 0u)
        color = vDiffuse;
    else
    {
        vec4 tex[4];
        if (!textureStages(tex))
            discard;
        for (int i = 0; i < 16; i++)
            R[i] = vec4(0.0);
        R[3] = vec4(st.fogColor.rgb, clamp01(vFog.x));
        R[4] = vDiffuse;
        R[5] = vSpecular;
        for (int i = 0; i < 4; i++)
            R[8 + i] = tex[i];
        R[12].a = tex[0].a;
        for (int i = 0; i < int(st.control.x); i++)
            generalStage(i);
        R[1] = st.finalC0;
        R[2] = st.finalC1;
        const uint cw0 = st.control.y, cw1 = st.control.z;
        vec4 sum = vec4(0.0), ef = vec4(0.0);
        if ((st.control.w & 2u) != 0u)
        {
            sum.rgb = R[12].rgb + R[5].rgb;
            if ((cw1 & 0x80u) != 0u)
                sum.rgb = clamp(sum.rgb, 0.0, 1.0);
            sum.a = 1.0;
        }
        if ((st.control.w & 1u) != 0u)
        {
            const vec4 e = finalInput(cw1 >> 24, sum, ef), f = finalInput((cw1 >> 16) & 0xffu, sum, ef);
            ef = e * f;
        }
        const vec4 a = finalInput(cw0 >> 24, sum, ef), b = finalInput((cw0 >> 16) & 0xffu, sum, ef);
        const vec4 c = finalInput((cw0 >> 8) & 0xffu, sum, ef), d = finalInput(cw0 & 0xffu, sum, ef);
        const uint g = (cw1 >> 8) & 0xffu;
        const vec4 gs = (g & 0xfu) == 15u ? ef : (g & 0xfu) == 14u ? sum : R[g & 0xfu];
        float alpha = clamp01((g & 0x10u) != 0u ? gs.a : gs.b);
        if ((g & 0xe0u) != 0u)
            alpha = 1.0 - alpha;
        color = vec4(clamp(a.rgb * b.rgb + (1.0 - a.rgb) * c.rgb + d.rgb, 0.0, 1.0), alpha);
    }
    if ((st.control.w & 4u) != 0u &&
        !compare(st.alphaTest.x, uint(clamp01(color.a) * 255.0 + 0.5), st.alphaTest.y))
        discard;
    outColor = color;
}
