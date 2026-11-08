// A program whose image reaches far past 0x400000, where the PS4 puts the app's own executable: 6 MiB
// of initialized data and 6 MiB of zeros. Retail games' images often reach past 4 MiB; this checks
// that one loads and that every page of it is the program's. Build with nxdk: make NXDK_DIR=~/emu/nxdk
#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define BIG (6 * 1024 * 1024)
static unsigned char data[BIG] = {1, 2, 3, 4};
static unsigned char zeros[BIG];

static int failures = 0;
static void check(const char *what, int ok)
{
    DbgPrint("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    DbgPrint("big image: data %p-%p, zeros %p-%p\n", data, data + BIG, zeros, zeros + BIG);
    check("the image reaches past 0x400000", (unsigned)(zeros + BIG) > 0x00800000);
    check("its initialized data was loaded", data[0] == 1 && data[1] == 2 && data[3] == 4);
    int clean = 1;
    for (unsigned i = 0; i < BIG; i += 4096)
        if (zeros[i] != 0 || (i > 4096 && data[i] != 0))
            clean = 0;
    check("its zero-filled data is zeros", clean);
    for (unsigned i = 0; i < BIG; i += 4096)
        data[i] = (unsigned char)(i >> 12), zeros[i] = (unsigned char)~(i >> 12);
    int kept = 1;
    for (unsigned i = 0; i < BIG; i += 4096)
        if (data[i] != (unsigned char)(i >> 12) || zeros[i] != (unsigned char)~(i >> 12))
            kept = 0;
    check("every page of it holds what is written", kept);
    DbgPrint("big image test: %d failures\n", failures);
    debugPrint("big image test: %d failures\n", failures);
    for (;;)
        Sleep(1000);
}
