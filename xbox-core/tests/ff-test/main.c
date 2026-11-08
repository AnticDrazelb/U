// The NV2A's fixed-function pipeline (no vertex program), with results worked out by hand and
// checked from the screen (tests/ff-test/check.py):
//   left    an unlit quad, a colour at each corner (red, green, blue, white)
//   middle  a lit quad: one infinite light of diffuse (1, 0.5, 0.25) along the normal and a scene
//           ambient of 0.1: (1.1 -> 1, 0.6, 0.35) = (255, 153, 89) everywhere
//   right   a quad lit by a point light above its centre: brightest there, dimmer outwards
//   bottom  a red quad in linear fog, its fog coordinate 0 on the left and 1 on the right, the fog
//           blue: red on the left, (128, 0, 128) in the middle, blue on the right
// Positions are in pixels: the composite matrix is the identity, the viewport offset 0.
#include <hal/video.h>
#include <pbkit/pbkit.h>
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

static uint32_t *quad(uint32_t *p, float x0, float y0, float x1, float y1, const DWORD colors[4])
{
    const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
    for (int i = 0; i < 4; i++)
    {
        const float normal[3] = {0, 0, 1}, v[4] = {xs[i], ys[i], 0.5f, 1};
        p = push_floats(p, NV097_SET_NORMAL3F, 3, normal);
        p = pb_push1(p, NV097_SET_DIFFUSE_COLOR4I, colors[i]);
        p = push_floats(p, NV097_SET_VERTEX4F, 4, v);
    }
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    return p;
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    if (pb_init())
        return 1;
    pb_show_front_screen();
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
        p = pb_push1(p, NV097_SET_COMBINER_CONTROL, 0);
        p = pb_push1(p, NV097_SET_FOG_ENABLE, 0);
        p = push_floats(p, NV097_SET_COMPOSITE_MATRIX, 16, identity);
        p = push_floats(p, NV097_SET_MODEL_VIEW_MATRIX, 16, identity);
        p = push_floats(p, NV097_SET_INVERSE_MODEL_VIEW_MATRIX, 16, identity);
        const float zero4[4] = {0, 0, 0, 0};
        p = push_floats(p, NV097_SET_VIEWPORT_OFFSET, 4, zero4);
        p = pb_push1(p, NV097_SET_SKIN_MODE, NV097_SET_SKIN_MODE_OFF);
        p = pb_push1(p, NV097_SET_SPECULAR_ENABLE, 0);

        // unlit, colours per corner (4UB: the low byte is x, red here)
        p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
        const DWORD corners[4] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};
        p = quad(p, 20, 140, 200, 340, corners);

        // lit by an infinite light along the normal
        p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 1);
        p = pb_push1(p, NV097_SET_COLOR_MATERIAL, 0); // every colour from the material
        const float ambient[3] = {0.1f, 0.1f, 0.1f}, black[3] = {0, 0, 0}, diffuse[3] = {1, 0.5f, 0.25f};
        const float direction[3] = {0, 0, 1}, half[3] = {0, 0, 1};
        p = push_floats(p, NV097_SET_SCENE_AMBIENT_COLOR, 3, ambient);
        p = push_floats(p, NV097_SET_MATERIAL_EMISSION, 3, black);
        p = pb_push1(p, NV097_SET_MATERIAL_ALPHA, 0x3f800000);
        p = push_floats(p, NV097_SET_LIGHT_AMBIENT_COLOR, 3, black);
        p = push_floats(p, NV097_SET_LIGHT_DIFFUSE_COLOR, 3, diffuse);
        p = push_floats(p, NV097_SET_LIGHT_SPECULAR_COLOR, 3, black);
        p = push_floats(p, NV097_SET_LIGHT_INFINITE_DIRECTION, 3, direction);
        p = push_floats(p, NV097_SET_LIGHT_INFINITE_HALF_VECTOR, 3, half);
        p = pb_push1(p, NV097_SET_LIGHT_ENABLE_MASK, NV097_SET_LIGHT_ENABLE_MASK_LIGHT0_INFINITE);
        const DWORD gray[4] = {0xff808080, 0xff808080, 0xff808080, 0xff808080};
        p = quad(p, 230, 140, 410, 340, gray);
        pb_end(p);

        // a local (point) light 60 pixels above the right quad's centre: brightest under it
        p = pb_begin();
        const float white[3] = {1, 1, 1}, position[3] = {530, 240, -60}, attenuation[3] = {1, 0, 0};
        const float range = 1e30f;
        p = push_floats(p, NV097_SET_LIGHT_DIFFUSE_COLOR, 3, white);
        p = push_floats(p, NV097_SET_LIGHT_LOCAL_POSITION, 3, position);
        p = push_floats(p, NV097_SET_LIGHT_LOCAL_ATTENUATION, 3, attenuation);
        p = push_floats(p, NV097_SET_LIGHT_LOCAL_RANGE, 1, &range);
        p = push_floats(p, NV097_SET_SCENE_AMBIENT_COLOR, 3, black);
        p = pb_push1(p, NV097_SET_LIGHT_ENABLE_MASK, 2); // light 0: local
        // a normal facing the light (towards -z), finely divided so the lighting per vertex shows
        for (int y = 140; y < 340; y += 20)
            for (int x = 440; x < 620; x += 20)
            {
                const float xs[4] = {x, x + 20, x + 20, x}, ys[4] = {y, y, y + 20, y + 20};
                p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
                for (int i = 0; i < 4; i++)
                {
                    const float normal[3] = {0, 0, -1}, v[4] = {xs[i], ys[i], 0, 1};
                    p = push_floats(p, NV097_SET_NORMAL3F, 3, normal);
                    p = push_floats(p, NV097_SET_VERTEX4F, 4, v);
                }
                p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
            }
        pb_end(p);

        // fog: linear, from the fog coordinate (start 0, end 1: x = 2, y = -1), blue; the final
        // combiner blends v0 into it by the fog factor (A = fog.a, B = v0, C = fog.rgb, G = v0.a)
        p = pb_begin();
        p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
        p = pb_push1(p, NV097_SET_COMBINER_CONTROL, 1);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x13040300);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x00001400);
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, 0);
        p = pb_push1(p, NV097_SET_FOG_ENABLE, 1);
        p = pb_push1(p, NV097_SET_FOG_MODE, NV097_SET_FOG_MODE_V_LINEAR);
        p = pb_push1(p, NV097_SET_FOG_GEN_MODE, NV097_SET_FOG_GEN_MODE_V_FOG_X);
        const float fog_params[3] = {2, -1, 0};
        p = push_floats(p, NV097_SET_FOG_PARAMS, 3, fog_params);
        p = pb_push1(p, NV097_SET_FOG_COLOR, 0xffff0000); // ABGR: blue
        p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
        const float fx[4] = {20, 620, 620, 20}, fy[4] = {370, 370, 460, 460}, fogc[4] = {0, 1, 1, 0};
        for (int i = 0; i < 4; i++)
        {
            const float v[4] = {fx[i], fy[i], 0, 1};
            p = pb_push1(p, NV097_SET_DIFFUSE_COLOR4I, 0xff0000ff);
            p = push_floats(p, NV097_SET_FOG_COORD, 1, &fogc[i]);
            p = push_floats(p, NV097_SET_VERTEX4F, 4, v);
        }
        p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
        pb_end(p);
        while (pb_busy())
            ;
        while (pb_finished())
            ;
    }
}
