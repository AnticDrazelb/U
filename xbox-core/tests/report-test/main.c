// Occlusion reports (SET_ZPASS_PIXEL_COUNT_ENABLE, GET_REPORT, CLEAR_REPORT_VALUE), as Direct3D's
// visibility tests use them: the pixels that pass every test while counting is on, written into
// the report DMA (a 64-bit timestamp, the count, 0). Each check a DbgPrint PASS/FAIL line.
#include <hal/debug.h>
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

static int failures = 0;
static void check(const char *what, int ok)
{
    DbgPrint("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

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

static uint32_t *quad(uint32_t *p, float x0, float y0, float x1, float y1, float z)
{
    const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
    for (int i = 0; i < 4; i++)
    {
        const float v[4] = {xs[i], ys[i], z, 1};
        p = pb_push1(p, NV097_SET_DIFFUSE_COLOR4I, 0xffffffff);
        p = push_floats(p, NV097_SET_VERTEX4F, 4, v);
    }
    return pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
}

static void finish(uint32_t *p)
{
    pb_end(p);
    while (pb_busy())
        ;
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    if (pb_init())
        return 1;
    pb_show_front_screen();
    // the reports land here (DMA object 3 is all of memory: offsets are physical addresses)
    volatile DWORD *report = MmAllocateContiguousMemoryEx(4096, 0, 0xffffff, 0, PAGE_READWRITE);
    const DWORD at = (DWORD)MmGetPhysicalAddress((PVOID)report);
    memset((void *)report, 0xcc, 64);

    pb_wait_for_vbl();
    pb_reset();
    pb_target_back_buffer();
    pb_erase_depth_stencil_buffer(0, 0, 640, 480);
    pb_fill(0, 0, 640, 480, 0xff000000);
    while (pb_busy())
        ;
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_FIXED);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
    p = pb_push1(p, NV097_SET_COMBINER_CONTROL, 0);
    p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, 0);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = push_floats(p, NV097_SET_COMPOSITE_MATRIX, 16, identity);
    const float zero4[4] = {0, 0, 0, 0};
    p = push_floats(p, NV097_SET_VIEWPORT_OFFSET, 4, zero4);
    p = pb_push1(p, NV097_SET_SKIN_MODE, NV097_SET_SKIN_MODE_OFF);
    p = pb_push1(p, NV097_SET_CONTEXT_DMA_REPORT, 3);
    // depth as z (pbkit starts with the w-buffer on: SET_CONTROL0 0x00110001; with w 1 both quads
    // below would store 1)
    p = pb_push1(p, NV097_SET_CONTROL0, 0x00100001);

    // 1: a 100x50 quad with no depth test: all 5000 pixels counted
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_CLEAR_REPORT_VALUE, NV097_CLEAR_REPORT_VALUE_TYPE_ZPASS_PIXEL_CNT);
    p = pb_push1(p, NV097_SET_ZPASS_PIXEL_COUNT_ENABLE, 1);
    p = quad(p, 100, 100, 200, 150, 0.5f);
    p = pb_push1(p, NV097_SET_ZPASS_PIXEL_COUNT_ENABLE, 0);
    p = pb_push1(p, NV097_GET_REPORT, (NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT << 24) | at);
    finish(p);
    check("a 100x50 quad: 5000 pixels", report[2] == 5000 && report[3] == 0);
    DbgPrint("  count %u, done word %u\n", (unsigned)report[2], (unsigned)report[3]);

    // 2: the depth test against the first quad (depth 4096 in the 24-bit buffer):
    // a quad behind it (depth 8192, less-or-equal) over the right half of it and beyond: only the part
    // outside the first quad passes (50x50 of its 100x50)
    p = pb_begin();
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 1);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 1);
    p = pb_push1(p, NV097_SET_DEPTH_FUNC, 0x203); // less or equal
    p = pb_push1(p, NV097_CLEAR_REPORT_VALUE, NV097_CLEAR_REPORT_VALUE_TYPE_ZPASS_PIXEL_CNT);
    p = quad(p, 100, 100, 200, 150, 4096.0f); // not counted: counting is off; writes the depth
    p = pb_push1(p, NV097_SET_ZPASS_PIXEL_COUNT_ENABLE, 1);
    p = quad(p, 150, 100, 250, 150, 8192.0f);
    p = pb_push1(p, NV097_SET_ZPASS_PIXEL_COUNT_ENABLE, 0);
    p = pb_push1(p, NV097_GET_REPORT, (NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT << 24) | (at + 16));
    finish(p);
    check("the depth test: 2500 of 5000 pixels", report[6] == 2500);
    DbgPrint("  count %u\n", (unsigned)report[6]);

    // 3: CLEAR_REPORT_VALUE resets the count; nothing drawn reports 0
    p = pb_begin();
    p = pb_push1(p, NV097_CLEAR_REPORT_VALUE, NV097_CLEAR_REPORT_VALUE_TYPE_ZPASS_PIXEL_CNT);
    p = pb_push1(p, NV097_GET_REPORT, (NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT << 24) | (at + 32));
    finish(p);
    check("a cleared count reports 0", report[10] == 0);

    DbgPrint("report test: %d failures\n", failures);
    debugPrint("report test: %d failures\n", failures);
    for (;;)
        pb_wait_for_vbl();
}
