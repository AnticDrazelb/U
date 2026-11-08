// The texture stages' other programs (check.py reads the screen):
//   top row     a cube map, six faces of six colours: six quads, each sending one direction (+X -X +Y
//               -Y +Z -Z) as its texture coordinates, show the face's colour
//   bottom left bump environment mapping: stage 0's texel (signed blue +0.504) through the matrix
//               (0.5, 0, 0, 0.5) moves stage 1's lookup from u 0.375 (red) to 0.627 (green); with a
//               zero matrix it stays red (the quad beside it)
//   bottom right a clip plane stage: coordinates from -1 to 1 across the quad discard its left half
//   last row    mipmaps: a 64x64 texture whose 7 levels are 7 colours, drawn whole at 64, 32 and 16
//               pixels (levels 0, 1, 2: red, green, blue with nearest-mipmap filtering) and at 45
//               (trilinear: level 0.51, half red half green)
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <string.h>
#include <windows.h>

static uint32_t *push_floats(uint32_t *p, DWORD method, int n, const float *v)
{
    pb_push(p++, method, n);
    for (int i = 0; i < n; i++)
    {
        float f = v[i];
        *(p++) = *(uint32_t *)&f;
    }
    return p;
}

static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// a quad; the texture coordinates of stage `stage` per corner (4 each), or the same for all
static uint32_t *quad(uint32_t *p, float x0, float y0, float x1, float y1, const float tc[4][4], DWORD tc_method)
{
    const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
    for (int i = 0; i < 4; i++)
    {
        const float v[4] = {xs[i], ys[i], 0.5f, 1};
        p = push_floats(p, tc_method, 4, tc[i]);
        p = push_floats(p, NV097_SET_VERTEX4F, 4, v);
    }
    return pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
}

static void *contiguous(DWORD size)
{
    void *p = MmAllocateContiguousMemoryEx(size, 0, 0x3ffffff, 4096, PAGE_READWRITE);
    memset(p, 0, size);
    return p;
}

// stage s: a swizzled A8R8G8B8 texture of 2^log x 2^log at `offset`, nearest, clamped
static uint32_t *texture(uint32_t *p, int s, DWORD offset, int log, int cube)
{
    p = pb_push2(p, NV097_SET_TEXTURE_OFFSET + s * 64, offset,
                 0x2 | (cube ? 4 : 0) | 2 << 4 | 0x06 << 8 | 1 << 16 | log << 20 | log << 24);
    p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS + s * 64, 0x00030303);
    p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + s * 64, 0x4003ffc0);
    return pb_push1(p, NV097_SET_TEXTURE_FILTER + s * 64, 0x01010000);
}

// the final combiner showing stage `t`'s texel
static uint32_t *show(uint32_t *p, int t)
{
    p = pb_push1(p, NV097_SET_COMBINER_CONTROL, 1);
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW, 0);
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW, 0);
    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW, 0);
    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW, 0);
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x00000008 + t);
    return pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, (0x18 + t) << 8);
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    if (pb_init())
        return 1;
    pb_show_front_screen();
    // the cube: six 16x16 faces (1024 bytes each, already 128-aligned)
    static const DWORD face_colors[6] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff00ff, 0xff00ffff};
    DWORD *cube = contiguous(6 * 1024);
    for (int f = 0; f < 6; f++)
        for (int i = 0; i < 256; i++)
            cube[f * 256 + i] = face_colors[f];
    // bump: a 16x16 texel of blue 0x40 (signed +0.504); the lookup: red where u < 0.5, green after
    DWORD *bump = contiguous(16 * 16 * 4), *split = contiguous(16 * 16 * 4);
    for (int i = 0; i < 256; i++)
        bump[i] = 0xff000040;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
        {
            // swizzled 16x16: x and y bits interleaved, x first
            int o = 0;
            for (int b = 0; b < 4; b++)
                o |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
            split[o] = x < 8 ? 0xffff0000 : 0xff00ff00;
        }
    // the mipmapped texture: levels of 64, 32, ... 1 texels, one colour each
    static const DWORD level_colors[7] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff00ff, 0xff00ffff, 0xffffffff};
    DWORD *mips = contiguous(21844);
    for (int l = 0, at = 0; l < 7; l++)
    {
        const int n = (64 >> l) * (64 >> l);
        for (int i = 0; i < n; i++)
            mips[at + i] = level_colors[l];
        at += n;
    }
    for (;;)
    {
        pb_wait_for_vbl();
        pb_reset();
        pb_target_back_buffer();
        pb_erase_depth_stencil_buffer(0, 0, 640, 480);
        pb_fill(0, 0, 640, 480, 0xff000000);
        while (pb_busy())
            ;
        uint32_t *p = pb_begin();
        p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_FIXED);
        p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
        p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
        p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
        p = push_floats(p, NV097_SET_COMPOSITE_MATRIX, 16, identity);
        const float zero4[4] = {0, 0, 0, 0};
        p = push_floats(p, NV097_SET_VIEWPORT_OFFSET, 4, zero4);
        p = pb_push1(p, NV097_SET_SKIN_MODE, NV097_SET_SKIN_MODE_OFF);

        // the cube map: stage 0, program 3
        p = texture(p, 0, MmGetPhysicalAddress(cube), 4, 1);
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, 3);
        p = show(p, 0);
        static const float dirs[6][3] = {{1, 0.2f, 0.1f}, {-1, 0.2f, 0.1f}, {0.1f, 1, 0.2f}, {0.1f, -1, 0.2f},
                                         {0.2f, 0.1f, 1}, {0.2f, 0.1f, -1}};
        for (int f = 0; f < 6; f++)
        {
            float tc[4][4];
            for (int i = 0; i < 4; i++)
                tc[i][0] = dirs[f][0], tc[i][1] = dirs[f][1], tc[i][2] = dirs[f][2], tc[i][3] = 1;
            p = quad(p, 40 + f * 95, 60, 110 + f * 95, 160, tc, NV097_SET_TEXCOORD0_4F);
        }
        pb_end(p);

        // bump environment map: stage 0 the bump texel (program 1), stage 1 bump env (program 6)
        p = pb_begin();
        p = texture(p, 0, MmGetPhysicalAddress(bump), 4, 0);
        p = texture(p, 1, MmGetPhysicalAddress(split), 4, 0);
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, 1 | 6 << 5);
        p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
        p = show(p, 1);
        const float half[4] = {0.5f, 0, 0, 0.5f}, none[4] = {0, 0, 0, 0};
        float tc[4][4] = {{0.5f, 0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}};
        float tc1[4][4] = {{0.375f, 0.5f, 0, 1}, {0.375f, 0.5f, 0, 1}, {0.375f, 0.5f, 0, 1}, {0.375f, 0.5f, 0, 1}};
        p = push_floats(p, NV097_SET_TEXCOORD0_4F, 4, tc[0]);
        p = push_floats(p, NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 64, 4, half);
        p = quad(p, 40, 240, 160, 400, tc1, NV097_SET_TEXCOORD1_4F);
        p = push_floats(p, NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 64, 4, none);
        p = quad(p, 180, 240, 300, 400, tc1, NV097_SET_TEXCOORD1_4F);
        pb_end(p);

        // a clip plane stage: s from -1 (left) to 1 (right); white where it survives
        p = pb_begin();
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, 5);
        p = pb_push1(p, NV097_SET_SHADER_CLIP_PLANE_MODE, 0);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x00000020 | 0x01); // D = 1 - zero: white
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x00002000 | 0x0100);
        float clip[4][4] = {{-1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {-1, 1, 1, 1}};
        p = quad(p, 360, 240, 600, 400, clip, NV097_SET_TEXCOORD0_4F);
        pb_end(p);

        // mipmaps: stage 0, 7 levels, the max level clamp at 6
        p = pb_begin();
        p = pb_push2(p, NV097_SET_TEXTURE_OFFSET, MmGetPhysicalAddress(mips), 0x2 | 2 << 4 | 0x06 << 8 | 7 << 16 | 6 << 20 | 6 << 24);
        p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS, 0x00030303);
        p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0, 0x40000000 | (6 * 256) << 6);
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, 1);
        p = show(p, 0);
        float uv[4][4] = {{0, 0, 0, 1}, {1, 0, 0, 1}, {1, 1, 0, 1}, {0, 1, 0, 1}};
        p = pb_push1(p, NV097_SET_TEXTURE_FILTER, 0x01030000); // nearest mipmap nearest
        p = quad(p, 20, 410, 84, 474, uv, NV097_SET_TEXCOORD0_4F);
        p = quad(p, 100, 410, 132, 442, uv, NV097_SET_TEXCOORD0_4F);
        p = quad(p, 150, 410, 166, 426, uv, NV097_SET_TEXCOORD0_4F);
        p = pb_push1(p, NV097_SET_TEXTURE_FILTER, 0x02060000); // trilinear
        p = quad(p, 190, 410, 235, 455, uv, NV097_SET_TEXCOORD0_4F);
        pb_end(p);
        while (pb_busy())
            ;
        while (pb_finished())
            ;
    }
}
