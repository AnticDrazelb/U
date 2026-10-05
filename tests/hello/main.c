// CemuPS4 test homebrew: boots through the normal Wii U process path (WHBProc), draws with
// OSScreen (no GX2 shaders), counts frames and shows the GamePad buttons. A run on the PS4 checks
// boot, the PPC recompiler, coreinit threads/timers, OSScreen scan-out, Vulkan present and input.
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <vpad/input.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/proc.h>

#include <stdio.h>

static unsigned fib(unsigned n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main(int argc, char** argv)
{
    WHBProcInit();
    WHBLogConsoleInit();
    WHBLogPrintf("CemuPS4 hello test");
    // A little real work for the recompiler: recursion, divides, floats.
    OSTime t0 = OSGetTime();
    unsigned f = fib(24);
    double acc = 0;
    for (int i = 1; i <= 100000; i++)
        acc += 1.0 / (double)i;
    OSTime t1 = OSGetTime();
    WHBLogPrintf("fib(24) = %u (expect 46368)", f);
    WHBLogPrintf("harmonic(1e5) = %.6f (expect 12.090146)", acc);
    WHBLogPrintf("compute took %lld us", (long long)OSTicksToMicroseconds(t1 - t0));
    WHBLogPrintf("press buttons on the GamePad; HOME quits");
    WHBLogConsoleDraw();

    unsigned frames = 0;
    uint32_t lastHold = 0xFFFFFFFF;
    OSTime start = OSGetTime();
    while (WHBProcIsRunning())
    {
        VPADStatus vpad;
        VPADReadError err;
        VPADRead(VPAD_CHAN_0, &vpad, 1, &err);
        frames++;
        if (err == VPAD_READ_SUCCESS && vpad.hold != lastHold)
        {
            lastHold = vpad.hold;
            WHBLogPrintf("buttons %08x  stick L %.2f %.2f", vpad.hold, vpad.leftStick.x, vpad.leftStick.y);
        }
        if (frames % 300 == 0)
        {
            double secs = (double)OSTicksToMilliseconds(OSGetTime() - start) / 1000.0;
            WHBLogPrintf("frame %u, %.1f s, %.1f fps", frames, secs, frames / secs);
        }
        WHBLogConsoleDraw();
        OSSleepTicks(OSMillisecondsToTicks(16));
    }

    WHBLogPrintf("exiting");
    WHBLogConsoleFree();
    WHBProcShutdown();
    return 0;
}
