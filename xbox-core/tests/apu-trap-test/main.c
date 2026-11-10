// The APU's front-end trap as DirectSound uses it: idle-voice traps forced (FETFORCE1), the trap's
// interrupt on (IEN), a service routine that reads the trapped method and voice (FEDECMETH,
// FEDECPARAM), acknowledges ISTS, unlinks the voice from its list and lets the front end run again.
// Checked (DbgPrint PASS/FAIL):
//   - each of three voices that end in the same frame is reported once, alone (one trap at a time);
//   - an acknowledged trap's interrupt doesn't come back (no delivery replays a resumed trap);
//   - methods written while the front end is halted wait in its FIFO (PIO_FREE shows them, they
//     take effect only when it runs again).
#include <hal/debug.h>
#include <hal/video.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define APU(offset) (*(volatile DWORD *)(0xFE800000 + (offset)))
#define METHOD(m, v) (APU(0x20000 + (m)) = (v))

static int failures = 0;
static void check(const char *what, int ok)
{
    DbgPrint("%s %s\n", ok ? "PASS" : "FAIL", what);
    debugPrint("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

static void *contiguous(DWORD size)
{
    void *p = MmAllocateContiguousMemoryEx(size, 0, 0x3ffffff, 4096, PAGE_READWRITE);
    memset(p, 0, size);
    return p;
}

static volatile DWORD *voices; // 256 voices of 0x80 bytes
static volatile LONG deliveries, traps, stale, wrong_method;
static volatile DWORD reported[16];

// unlinks voice v from the 2D list (top in TVL2D, each voice's next in TAR_PITCH_LINK's low half)
static void unlink(DWORD v)
{
    DWORD prev = 0xffff, cur = APU(0x2054) & 0xffff;
    for (int guard = 0; cur != 0xffff && guard < 256; guard++)
    {
        const DWORD next = voices[cur * 32 + 0x7c / 4] & 0xffff;
        if (cur == v)
        {
            if (prev == 0xffff)
                APU(0x2054) = next;
            else
                voices[prev * 32 + 0x7c / 4] = (voices[prev * 32 + 0x7c / 4] & ~0xffffu) | next;
            return;
        }
        prev = cur, cur = next;
    }
}

static BOOLEAN NTAPI isr(PKINTERRUPT interrupt, PVOID context)
{
    (void)interrupt, (void)context;
    const DWORD ists = APU(0x1000);
    if (!(ists & 1))
        return FALSE;
    deliveries++;
    APU(0x1000) = ists; // acknowledged before the front end runs again, as DirectSound does
    if (ists & 0x10)
    {
        const DWORD fectl = APU(0x1100);
        if ((fectl & 0xe0) != 0xe0)
        {
            stale++; // a trap's interrupt with the front end not trapped: a replay
            return TRUE;
        }
        if (APU(0x1300) != 0x8000)
            wrong_method++;
        const DWORD v = APU(0x1304);
        if (traps < 16)
            reported[traps] = v;
        traps++;
        unlink(v);
        APU(0x1100) = fectl & ~0xe0u; // free-running again (the reason left at REQUESTED)
    }
    return TRUE;
}

static void voice(int v, DWORD end)
{
    METHOD(0x2f8, v);                                   // SET_CURRENT_VOICE
    METHOD(0x300, 0 | 1 << 5 | 31 << 10 | 31 << 16 | 31 << 21 | 31 << 26);
    METHOD(0x304, 1u << 28 | 1u << 30 | 31 | 31 << 5);  // S16 in B16, mono, not looped
    METHOD(0x360, 0x000f000f | 0xfff << 4 | 0xfffu << 20); // silent
    METHOD(0x364, 0xffffffff);
    METHOD(0x368, 0xffffffff);
    METHOD(0x37c, 0);
    METHOD(0x3a0, 0);
    METHOD(0x3a4, 0);
    METHOD(0x3dc, end);
    METHOD(0x3d8, 0);
    METHOD(0x120, 1 << 16);                             // antecedent: the 2D list's top
    METHOD(0x124, v);                                   // VOICE_ON
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    short *pcm = contiguous(4096);
    DWORD *sge = contiguous(4096);
    sge[0] = MmGetPhysicalAddress(pcm);
    voices = contiguous(256 * 0x80);
    unsigned char *notifiers = contiguous(16 * (2 + 256 * 4));
    APU(0x202c) = MmGetPhysicalAddress((void *)voices); // VPVADDR
    APU(0x2030) = MmGetPhysicalAddress(sge);            // VPSGEADDR
    APU(0x115c) = MmGetPhysicalAddress(notifiers);      // FENADDR
    APU(0x2054) = APU(0x2060) = APU(0x206c) = 0xffff;   // the voice lists, empty
    APU(0x1504) = 1 << 15;                              // FETFORCE1: trap on idle voices
    APU(0x1000) = 0xffffffff;                           // ISTS clear
    APU(0x1004) = 0x11;                                 // IEN: the global bit and FETINTSTS

    KIRQL irql;
    const ULONG vector = HalGetInterruptVector(5, &irql);
    static KINTERRUPT interrupt;
    KeInitializeInterrupt(&interrupt, isr, NULL, vector, irql, LevelSensitive, TRUE);
    check("the APU's interrupt connects", KeConnectInterrupt(&interrupt));

    // the front end's FIFO: halted, a method waits in it
    APU(0x1100) = 0x80; // FECTL: halted
    APU(0x1110) = 3;    // FECV
    METHOD(0x2f8, 7);   // SET_CURRENT_VOICE 7
    const DWORD free_halted = APU(0x20010), fecv_halted = APU(0x1110);
    APU(0x1100) = 0;    // free-running
    const DWORD free_running = APU(0x20010), fecv_running = APU(0x1110);
    DbgPrint("  halted: PIO_FREE %#x, FECV %u; running: PIO_FREE %#x, FECV %u\n", (unsigned)free_halted,
             (unsigned)fecv_halted, (unsigned)free_running, (unsigned)fecv_running);
    check("a method written while the front end is halted waits in its FIFO", free_halted == 0x7c && fecv_halted == 3);
    check("it runs once the front end runs", free_running == 0x80 && fecv_running == 7);

    // three voices of 10 ms each, ending together: three idle voices in one frame
    APU(0x2000) = 1 << 3; // SECTL: the sample counter (frames) running
    voice(10, 479);
    voice(11, 479);
    voice(12, 479);
    Sleep(300);

    DbgPrint("  %d deliveries, %d traps (voices %u %u %u), %d replays, %d other methods\n", (int)deliveries,
             (int)traps, (unsigned)reported[0], (unsigned)reported[1], (unsigned)reported[2], (int)stale,
             (int)wrong_method);
    int distinct = traps == 3 && reported[0] != reported[1] && reported[1] != reported[2] && reported[0] != reported[2];
    for (int i = 0; i < 3 && distinct; i++)
        distinct = reported[i] >= 10 && reported[i] <= 12;
    check("each idle voice is trapped on once (three voices, three traps, each its own)", distinct);
    check("the trapped method is SE2FE_IDLE_VOICE", wrong_method == 0);
    check("an acknowledged trap's interrupt doesn't come back", stale == 0);
    check("the voice list ends empty", (APU(0x2054) & 0xffff) == 0xffff);

    DbgPrint("APU trap test: %d failures\n", failures);
    debugPrint("APU trap test: %d failures\n", failures);
    for (;;)
        Sleep(1000);
}
