// Halo's text and screen-space draws, replayed: the methods below are the ones Halo's D3D sent
// (xbox-core's log of its last 256 GPU methods), with the vertices of two quads of our own.
// D3D's vertex program for screen-space vertices:
//   oPos = DPH(v0, c[28..31])                 (pixels to clip space)
//   oT0.xy = v4; oD0 = v9; oD1.w = v10.x
//   oPos.xyz = oPos * c[58] / oPos.w + c[59]  (c[58], c[59]: the viewport's scale and offset)
// c[58] and c[59] are set by the fixed function's viewport methods (0x0af0, 0x0a20), not by a
// constant load: the NV2A's fixed function and vertex programs share those rows. Checked from
// the screen (check.py): a red quad over (248, 359)-(348, 419) and a green one over
// (40, 40)-(140, 100), each at its pixels; black around them.
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <windows.h>

static uint32_t *method(uint32_t *p, DWORD m, int n, const DWORD *v)
{
    pb_push(p++, m, n);
    for (int i = 0; i < n; i++)
        *(p++) = v[i];
    return p;
}

static uint32_t *f2(uint32_t *p, DWORD m, float a, float b)
{
    pb_push(p++, m, 2);
    *(p++) = *(DWORD *)&a;
    *(p++) = *(DWORD *)&b;
    return p;
}

// a quad as Halo draws its text: a triangle fan of four vertices, each its colour (4UB, slot
// 9), texture coordinates (2F, slot 4) and position in pixels (2F, slot 0)
static uint32_t *quad(uint32_t *p, float x0, float y0, float x1, float y1, DWORD abgr)
{
    const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    p = pb_push1(p, 0x17fc, 7);
    for (int i = 0; i < 4; i++)
    {
        p = pb_push1(p, 0x1964, abgr);
        p = f2(p, 0x18a0, 0, 0);
        p = f2(p, 0x1880, xs[i], ys[i]);
    }
    p = pb_push1(p, 0x17fc, 0);
    return p;
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    if (pb_init())
        return 1;
    pb_show_front_screen();
    // the program, as Halo's D3D loads it at slot 0
    static const DWORD program[36] = {
        0, 0xc3801b, 0x836186c, 0x20708800, 0, 0xc3a01b, 0x836186c, 0x20704800,
        0, 0xc3c01b, 0x836186c, 0x20702800, 0, 0xc3e01b, 0x836186c, 0x20701800,
        0, 0x20081b, 0x836106c, 0x2070c848, 0, 0x20121b, 0x836106c, 0x2070f818,
        0, 0x201400, 0x836106c, 0x20701820, 0, 0x647401b, 0xc4361bff, 0x1078e800,
        0, 0x87601b, 0xc400286c, 0x3070e801};
    // c[28..31]: pixels to clip space, as Halo's D3D loads them
    static const DWORD to_clip[16] = {
        0x3b4ccccd, 0, 0, 0xbf803333, 0, 0xbb888889, 0, 0x3f804444,
        0, 0, 0x3f800000, 0, 0, 0, 0, 0x3f800000};
    static const DWORD viewport_offset[4] = {0x43a04400, 0x43708800, 0, 0}; // 320.53, 240.53
    static const DWORD viewport_scale[4] = {0x43a00000, 0xc3700000, 0x4b7fffff, 0}; // 320, -240, 2^24-1
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
        p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
        p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
        p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
        p = pb_push1(p, NV097_SET_FOG_ENABLE, 0);
        // the final combiner: the diffuse colour (D = v0, G = v0.a), one general stage that
        // writes nothing (Halo's text multiplies in its font texture there; no texture here)
        p = pb_push1(p, NV097_SET_COMBINER_CONTROL, 1);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x04);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x1400);
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, 0);
        p = pb_push1(p, 0x1e9c, 0);                       // SET_TRANSFORM_PROGRAM_LOAD
        for (int i = 0; i < 9; i++)
            p = method(p, 0x0b00, 4, program + i * 4);    // SET_TRANSFORM_PROGRAM
        p = pb_push1(p, 0x1e94, 6);                       // SET_TRANSFORM_EXECUTION_MODE: program, privileged
        p = pb_push1(p, 0x1e98, 0);                       // SET_TRANSFORM_PROGRAM_CXT_WRITE_EN
        p = method(p, 0x0a20, 4, viewport_offset);        // SET_VIEWPORT_OFFSET
        p = method(p, 0x0af0, 4, viewport_scale);         // SET_VIEWPORT_SCALE
        p = pb_push1(p, 0x0394, 0);                       // SET_CLIP_MIN
        p = pb_push1(p, 0x0398, 0x4b7fffff);              // SET_CLIP_MAX
        p = pb_push1(p, 0x1ea0, 0);                       // SET_TRANSFORM_PROGRAM_START
        p = pb_push1(p, 0x1ea4, 0x1c);                    // SET_TRANSFORM_CONSTANT_LOAD: c[28]
        p = method(p, 0x0b80, 16, to_clip);               // SET_TRANSFORM_CONSTANT
        p = quad(p, 248, 359, 348, 419, 0xff0000ff);      // red
        p = quad(p, 40, 40, 140, 100, 0xff00ff00);        // green
        pb_end(p);
        while (pb_busy())
            ;
        while (pb_finished())
            ;
    }
}
