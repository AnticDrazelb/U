// Frames drawn in many steps and flipped as Direct3D flips them (the next frame in the back buffer,
// shown from the vblank once it is done): what is shown must always be a whole frame. Each frame:
// a blue clear, 120 bands over most of the screen (each its own draw, sent in 8 pieces with a pause
// after each, as a game sends its frame while it builds it: a frame that spans several vblanks), and
// last a red square at the bottom right. Run with XB_PICTURE_PROBE=600,460,ff0000: the log's "picture probe" lines count the
// times the shown picture had no red square there (a frame shown half drawn): none expected.
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <windows.h>

static uint32_t *f4(uint32_t *p, DWORD m, float a, float b, float c, float d)
{
    pb_push(p++, m, 4);
    *(p++) = *(DWORD *)&a, *(p++) = *(DWORD *)&b, *(p++) = *(DWORD *)&c, *(p++) = *(DWORD *)&d;
    return p;
}

static uint32_t *quad(uint32_t *p, float x0, float y0, float x1, float y1, DWORD abgr)
{
    const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
    for (int i = 0; i < 4; i++)
    {
        p = pb_push1(p, NV097_SET_DIFFUSE_COLOR4I, abgr);
        p = f4(p, NV097_SET_VERTEX4F, xs[i], ys[i], 0.5f, 1);
    }
    return pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    if (pb_init())
        return 1;
    pb_show_front_screen();
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (unsigned frame = 0;; frame++)
    {
        pb_reset();
        pb_target_back_buffer();
        pb_erase_depth_stencil_buffer(0, 0, 640, 480);
        pb_fill(0, 0, 640, 480, 0xff0000ff); // blue
        while (pb_busy())
            ;
        uint32_t *p = pb_begin();
        p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_FIXED);
        p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
        p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
        p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
        p = pb_push1(p, NV097_SET_FOG_ENABLE, 0);
        // the final combiner: the diffuse colour
        p = pb_push1(p, NV097_SET_COMBINER_CONTROL, 1);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x04);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x1400);
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, 0);
        pb_push(p++, NV097_SET_COMPOSITE_MATRIX, 16);
        for (int i = 0; i < 16; i++)
            *(p++) = *(const DWORD *)&identity[i];
        p = f4(p, NV097_SET_VIEWPORT_OFFSET, 0, 0, 0, 0);
        pb_end(p);
        // the bands, top to bottom, each its own draw, sent in 8 pushes (green and grey alternating,
        // shifting each frame)
        for (int b = 0; b < 120; b++)
        {
            if (b % 15 == 0)
                p = pb_begin();
            // the whole screen each time: a frame long enough to draw that it spans vblanks (as a
            // game's at 30 or 20 frames a second), the picture's last band on top
            p = quad(p, 0, 0, 640, 480 - b * 2, ((b + frame) & 1) ? 0xff00ff00 : 0xff808080);
            if (b % 15 == 14)
            {
                // sent, and drawn, before the next push is made (as a game's Direct3D sends its
                // frame in pieces while it builds it): the GPU waits between them, mid-frame
                pb_end(p);
                while (pb_busy())
                    ;
                Sleep(3);
            }
        }
        p = pb_begin();
        p = quad(p, 580, 440, 620, 476, 0xff0000ff); // red (4UB: red in the low byte)
        pb_end(p);
        while (pb_busy())
            ;
        while (pb_finished())
            ;
    }
}
