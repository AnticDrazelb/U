// The APU with many voices at once (as a game's busy scene plays): 48 looped voices of a 1 kHz sine
// at different pitches, quietly, through the 2D list, half of them through a 3D (HRTF) voice slot.
// For the frame thread's load: the stats line's "busy" share (xbox-core --seconds 12).
#include <hal/debug.h>
#include <hal/video.h>
#include <math.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define APU(offset) (*(volatile DWORD *)(0xFE800000 + (offset)))
#define METHOD(m, v) (APU(0x20000 + (m)) = (v))

static void *contiguous(DWORD size)
{
    void *p = MmAllocateContiguousMemoryEx(size, 0, 0x3ffffff, 4096, PAGE_READWRITE);
    memset(p, 0, size);
    return p;
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    const int samples = 48000;
    short *sine = contiguous(samples * 2);
    for (int i = 0; i < samples; i++)
    {
        float s = 2048.0f * sinf(2.0f * 3.14159265f * 1000.0f * i / 48000.0f);
        sine[i] = (short)s;
    }
    DWORD *sge = contiguous(4096);
    for (int page = 0; page < (samples * 2 + 4095) / 4096; page++)
        sge[page * 2] = MmGetPhysicalAddress((char *)sine + page * 4096);
    void *voices = contiguous(256 * 0x80);
    unsigned char *notifiers = contiguous(16 * (2 + 256 * 4));
    APU(0x202c) = MmGetPhysicalAddress(voices);
    APU(0x2030) = MmGetPhysicalAddress(sge);
    APU(0x115c) = MmGetPhysicalAddress(notifiers);
    APU(0x2054) = APU(0x2060) = APU(0x206c) = 0xffff;
    APU(0x2000) = 1 << 3;
    for (int k = 0; k < 48; k++)
    {
        const int v = k < 24 ? 10 + k : 100 + k; // 0..63: the 3D voices
        METHOD(0x2f8, v);
        METHOD(0x300, 0 | 1 << 5 | 31 << 10 | 31 << 16 | 31 << 21 | 31 << 26);
        METHOD(0x304, 1u << 28 | 1u << 30 | 1u << 25 | 31 | 31 << 5);
        METHOD(0x360, (DWORD)(12 * 64) << 4 | (DWORD)(12 * 64) << 20 | 0x000f000f);
        METHOD(0x364, 0xffffffff);
        METHOD(0x368, 0xffffffff);
        METHOD(0x37c, (DWORD)((k * 97 - 2000) & 0xffff) << 16); // pitches around the original
        METHOD(0x3a0, 0);
        METHOD(0x3a4, 0);
        METHOD(0x3dc, samples - 1);
        METHOD(0x3d8, 0);
        METHOD(0x120, 1 << 16);
        METHOD(0x124, v);
    }
    DbgPrint("APU load test: 48 voices playing\n");
    for (;;)
        Sleep(1000);
}
