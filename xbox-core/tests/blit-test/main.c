// The NV2A's 2D engines as pbkit sets them up (subchannel 3: image blit 0x9f, 4: context surfaces
// 0x62): a pattern of filled rectangles, then a 100x80 blit of it from (20, 20) to (300, 200).
// tests/blit-test/check.py compares the two regions of the screen.
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <windows.h>

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
        pb_fill(0, 0, 640, 480, 0xff000000);
        // the pattern: four coloured quarters and a white bar
        pb_fill(20, 20, 50, 40, 0xffff0000);
        pb_fill(70, 20, 50, 40, 0xff00ff00);
        pb_fill(20, 60, 50, 40, 0xff0000ff);
        pb_fill(70, 60, 50, 40, 0xffffff00);
        pb_fill(40, 50, 60, 6, 0xffffffff);
        while (pb_busy())
            ;
        const DWORD offset = (DWORD)pb_back_buffer() & 0x03ffffff;
        uint32_t *p = pb_begin();
        // both surfaces through DMA object 3 (all of RAM): pbkit points 11, its default destination,
        // at the back buffer itself
        p = pb_push1_to(SUBCH_4, p, 0x184, 3);
        p = pb_push1_to(SUBCH_4, p, 0x188, 3);
        p = pb_push1_to(SUBCH_4, p, 0x300, 0x0a);                     // A8R8G8B8
        p = pb_push1_to(SUBCH_4, p, 0x304, (640 * 4) | (640 * 4) << 16); // pitches
        p = pb_push1_to(SUBCH_4, p, 0x308, offset);
        p = pb_push1_to(SUBCH_4, p, 0x30c, offset);
        p = pb_push1_to(SUBCH_3, p, 0x2fc, 3);                        // SRCCOPY
        p = pb_push1_to(SUBCH_3, p, 0x300, 20 | 20 << 16);
        p = pb_push1_to(SUBCH_3, p, 0x304, 300 | 200 << 16);
        p = pb_push1_to(SUBCH_3, p, 0x308, 100 | 80 << 16);
        pb_end(p);
        while (pb_busy())
            ;
        while (pb_finished())
            ;
    }
}
