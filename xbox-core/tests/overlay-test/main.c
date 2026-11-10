// xbox-core's overlay test: a YUY2 frame shown through the NV2A's video overlay (PVIDEO), as games show
// their movies (Halo's Bink intro). A pure red frame (Y 81, U 90, V 240) scaled 2x into a 200x100
// rectangle at (100, 60) over a black picture; the picture saved shows red there, black elsewhere.
#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    volatile ULONG *nv = (volatile ULONG *)0xfd000000;
    unsigned char *fb = (unsigned char *)XVideoGetFB();
    for (int i = 0; i < 640 * 480 * 4; i++)
        fb[i] = 0;
    const int w = 100, h = 50, pitch = w * 2;
    unsigned char *frame = MmAllocateContiguousMemoryEx(pitch * h, 0, 0x3ffffff, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x += 2)
        {
            unsigned char *p = frame + y * pitch + x * 2;
            p[0] = 81, p[1] = 90, p[2] = 81, p[3] = 240; // Y0 U Y1 V
        }
    const ULONG phys = MmGetPhysicalAddress(frame);
    nv[(0x8000 + 0x900) / 4] = 0;                       // BASE
    nv[(0x8000 + 0x908) / 4] = 0x3ffffff;               // LIMIT
    nv[(0x8000 + 0x920) / 4] = phys;                    // OFFSET
    nv[(0x8000 + 0x928) / 4] = (h << 16) | w;            // SIZE_IN
    nv[(0x8000 + 0x930) / 4] = 0;                       // POINT_IN
    nv[(0x8000 + 0x938) / 4] = 0x80000;                 // DS_DX: half a texel a pixel (2x)
    nv[(0x8000 + 0x940) / 4] = 0x80000;                 // DT_DY
    nv[(0x8000 + 0x948) / 4] = (60 << 16) | 100;         // POINT_OUT
    nv[(0x8000 + 0x950) / 4] = (100 << 16) | 200;        // SIZE_OUT
    nv[(0x8000 + 0x958) / 4] = (1 << 16) | pitch;        // FORMAT: Cr8Yb8Cb8Ya8, its pitch
    nv[(0x8000 + 0x700) / 4] = 1;                       // BUFFER 0 in use
    DbgPrint("overlay test: shown\n");
    for (;;)
        Sleep(1000);
    return 0;
}
