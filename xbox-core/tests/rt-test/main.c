// Render targets and the stencil test (check.py compares the screen with what they must give):
//   left   stencil: quad A (40..180 x 160..320) drawn with colour writes off sets the stencil to 1; a
//          red quad over 20..300 x 140..340 drawn where the stencil equals 1 shows only inside A
//   right  render to texture: a 64x64 swizzled texture rendered blue with a green left half, then
//          drawn magnified twice (nearest) at 380..508 x 176..304
// Fixed-function vertices in pixels, as tests/ff-test.
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

// a quad in one colour; `uv` gives it texture coordinates 0..1
static uint32_t *quad(uint32_t *p, float x0, float y0, float x1, float y1, DWORD color, int uv)
{
    const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1}, us[4] = {0, 1, 1, 0}, vs[4] = {0, 0, 1, 1};
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
    for (int i = 0; i < 4; i++)
    {
        const float v[4] = {xs[i], ys[i], 0.5f, 1}, t[2] = {us[i], vs[i]};
        p = pb_push1(p, NV097_SET_DIFFUSE_COLOR4I, color);
        if (uv)
            p = push_floats(p, NV097_SET_TEXCOORD0_2F, 2, t);
        p = push_floats(p, NV097_SET_VERTEX4F, 4, v);
    }
    return pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    if (pb_init())
        return 1;
    pb_show_front_screen();
    DWORD *texture = MmAllocateContiguousMemoryEx(64 * 64 * 4, 0, 0x3ffffff, 4096, PAGE_READWRITE);
    const DWORD texture_offset = MmGetPhysicalAddress(texture);
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
        p = pb_push1(p, NV097_SET_COMBINER_CONTROL, 0);
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, 0);
        p = push_floats(p, NV097_SET_COMPOSITE_MATRIX, 16, identity);
        const float zero4[4] = {0, 0, 0, 0};
        p = push_floats(p, NV097_SET_VIEWPORT_OFFSET, 4, zero4);
        p = pb_push1(p, NV097_SET_SKIN_MODE, NV097_SET_SKIN_MODE_OFF);

        // ---- render to texture: the colour target is the swizzled texture
        p = pb_push1(p, NV097_SET_CONTEXT_DMA_COLOR, 3);                  // all of RAM
        p = pb_push1(p, NV097_SET_SURFACE_COLOR_OFFSET, texture_offset);
        p = pb_push1(p, NV097_SET_SURFACE_FORMAT, 0x8 | 2 << 8 | 6 << 16 | 6 << 24); // A8R8G8B8, swizzled, 64x64
        p = pb_push2(p, NV097_SET_SURFACE_CLIP_HORIZONTAL, 64 << 16, 64 << 16);
        p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
        p = pb_push2(p, NV097_SET_CLEAR_RECT_HORIZONTAL, 63 << 16, 63 << 16);
        p = pb_push1(p, NV097_SET_COLOR_CLEAR_VALUE, 0xff0000ff);         // blue
        p = pb_push1(p, NV097_CLEAR_SURFACE, 0xf0);
        p = quad(p, 0, 0, 32, 64, 0xff00ff00, 0);                          // the left half green
        pb_end(p);
        pb_target_back_buffer();                                         // back to the screen
        p = pb_begin();
        p = pb_push1(p, NV097_SET_SURFACE_COLOR_OFFSET, 0);

        // ---- stencil: quad A sets it to 1 without colour, then red where it is 1
        p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 1);
        p = pb_push1(p, NV097_SET_STENCIL_MASK, 0xff);
        p = pb_push1(p, NV097_SET_STENCIL_FUNC, 0x207);                    // always
        p = pb_push1(p, NV097_SET_STENCIL_FUNC_REF, 1);
        p = pb_push1(p, NV097_SET_STENCIL_FUNC_MASK, 0xff);
        p = pb_push1(p, NV097_SET_STENCIL_OP_FAIL, NV097_SET_STENCIL_OP_V_KEEP);
        p = pb_push1(p, NV097_SET_STENCIL_OP_ZFAIL, NV097_SET_STENCIL_OP_V_KEEP);
        p = pb_push1(p, NV097_SET_STENCIL_OP_ZPASS, NV097_SET_STENCIL_OP_V_REPLACE);
        p = pb_push1(p, NV097_SET_COLOR_MASK, 0);
        p = quad(p, 40, 160, 180, 320, 0xffffffff, 0);
        p = pb_push1(p, NV097_SET_COLOR_MASK, 0x01010101);
        p = pb_push1(p, NV097_SET_STENCIL_FUNC, 0x202);                    // equal
        p = pb_push1(p, NV097_SET_STENCIL_OP_ZPASS, NV097_SET_STENCIL_OP_V_KEEP);
        p = quad(p, 20, 140, 300, 340, 0xff0000ff, 0);                     // red (4I: low byte red)
        p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);

        // ---- the texture on screen: stage 0, 2D; the final combiner shows t0
        p = pb_push2(p, NV097_SET_TEXTURE_OFFSET, texture_offset, 0x2 | 2 << 4 | 0x06 << 8 | 1 << 16 | 6 << 20 | 6 << 24);
        p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS, 0x00030303);
        p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0, 0x4003ffc0);
        p = pb_push1(p, NV097_SET_TEXTURE_FILTER, 0x01010000);            // nearest
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, 1);
        p = pb_push1(p, NV097_SET_COMBINER_CONTROL, 1);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x00000008); // D = t0
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x00001800); // G = t0.a
        p = quad(p, 380, 176, 508, 304, 0xffffffff, 1);
        pb_end(p);
        while (pb_busy())
            ;
        while (pb_finished())
            ;
    }
}
