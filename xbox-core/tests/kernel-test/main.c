// Calls kernel exports with known inputs and reports each result through DbgPrint ("PASS"/"FAIL"),
// for xbox-core's log. Build with nxdk: make NXDK_DIR=~/emu/nxdk
#include <hal/debug.h>
#include <hal/video.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

static int failures = 0;
static void check(const char *what, int ok)
{
    DbgPrint("%s %s\n", ok ? "PASS" : "FAIL", what);
    debugPrint("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

static int apc_calls;
static DWORD apc_error, apc_bytes;
static VOID WINAPI on_read_done(DWORD error, DWORD bytes, LPOVERLAPPED overlapped)
{
    apc_calls++;
    apc_error = error;
    apc_bytes = bytes;
}

static HANDLE open_path(const char *path, ACCESS_MASK access, ULONG disposition)
{
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    RtlInitAnsiString(&name, path);
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NtCreateFile(&h, access | SYNCHRONIZE, &attributes, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                     disposition, FILE_SYNCHRONOUS_IO_NONALERT) != 0)
        return NULL;
    return h;
}

static volatile LONG dpc_count = 0, apc_count = 0, timer_apc_count = 0;
static ULONG apc_seen_context = 0;

static VOID NTAPI dpc_routine(PKDPC dpc, PVOID context, PVOID a, PVOID b)
{
    (void)dpc; (void)context; (void)a; (void)b;
    InterlockedIncrement((LONG *)&dpc_count);
}

static VOID NTAPI apc_routine(PVOID context, PVOID status, PVOID reserved)
{
    (void)status; (void)reserved;
    apc_seen_context = (ULONG)context;
    InterlockedIncrement((LONG *)&apc_count);
}

static VOID NTAPI timer_apc(PVOID context, ULONG low, LONG high)
{
    (void)context; (void)low; (void)high;
    InterlockedIncrement((LONG *)&timer_apc_count);
}

static LONGLONG ms(LONG n) { return -(LONGLONG)n * 10000; }

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    // the pool
    PVOID p = ExAllocatePoolWithTag(100, 'tset');
    check("ExAllocatePoolWithTag", p != NULL);
    // the retail kernel's data: three cache partitions (XAPI's utility drive divides by them), no
    // debugger, a disk with a model number
    check("HalDiskCachePartitionCount is 3", HalDiskCachePartitionCount == 3);
    check("KdDebuggerNotPresent", KdDebuggerNotPresent == 1);
    {
        // the EEPROM's settings at their indices (XC_LANGUAGE 7, XC_FACTORY_GAME_REGION 0x104, ...)
        ULONG type = 0, value = 0, length = 0;
        check("EEPROM language: English",
              ExQueryNonVolatileSetting(7, &type, &value, 4, &length) == 0 && value == 1 && type == 4 && length == 4);
        check("EEPROM game region: North America", ExQueryNonVolatileSetting(0x104, &type, &value, 4, &length) == 0 && value == 1);
        check("EEPROM AV region: NTSC-M 60 Hz", ExQueryNonVolatileSetting(0x103, &type, &value, 4, &length) == 0 && value == 0x00400100);
        UCHAR all[256];
        check("the whole EEPROM",
              ExQueryNonVolatileSetting(0xFFFF, &type, all, sizeof(all), &length) == 0 && length == 256 && all[0x2C] == 1 && all[0x90] == 1);
        check("an unknown setting isn't found", ExQueryNonVolatileSetting(0x50, &type, &value, 4, &length) != 0);
        // the SMBus: the SMC's AV pack and version; nothing at an empty address
        ULONG smc = 0;
        check("SMC AV pack: S-Video", HalReadSMBusValue(0x20, 0x04, FALSE, &smc) == 0 && smc == 4);
        ULONG v[3] = {0};
        HalWriteSMBusValue(0x20, 0x01, FALSE, 0);
        HalReadSMBusValue(0x20, 0x01, FALSE, &v[0]);
        HalReadSMBusValue(0x20, 0x01, FALSE, &v[1]);
        HalReadSMBusValue(0x20, 0x01, FALSE, &v[2]);
        check("SMC version P01", v[0] == 'P' && v[1] == '0' && v[2] == '1');
        check("no device at an empty SMBus address", HalReadSMBusValue(0x40, 0x00, FALSE, &smc) != 0);
        check("HalBootSMCVideoMode: S-Video", HalBootSMCVideoMode == 4);
        // the PCI configuration space: the NV2A on bus 1, the first USB controller where its
        // registers are, an empty slot all ones
        ULONG id = 0, bar = 0, empty = 0;
        UCHAR irq = 0;
        HalReadWritePCISpace(1, 0, 0, &id, 4, FALSE);
        check("PCI: the NV2A on bus 1", id == 0x02a010de);
        HalReadWritePCISpace(0, 2, 0x10, &bar, 4, FALSE);
        HalReadWritePCISpace(0, 2, 0x3c, &irq, 1, FALSE);
        check("PCI: USB 0 at 0xfed00000, IRQ 1", bar == 0xfed00000 && irq == 1);
        ULONG ones = 0xffffffff, size = 0;
        HalReadWritePCISpace(0, 2, 0x10, &ones, 4, TRUE);
        HalReadWritePCISpace(0, 2, 0x10, &size, 4, FALSE);
        HalReadWritePCISpace(0, 2, 0x10, &bar, 4, TRUE);
        check("PCI: a BAR's size", size == 0xfffff000);
        HalReadWritePCISpace(0, 20, 0, &empty, 4, FALSE);
        check("PCI: an empty slot", empty == 0xffffffff);
        // the second USB controller (OHCI 1.0a): it answers, resets, and keeps the HCCA aligned
        volatile ULONG *usb1 = (volatile ULONG *)0xfed08000;
        check("USB 1: OHCI 1.0", (usb1[0] & 0xff) == 0x10);
        usb1[0x18 / 4] = 0x12345678;
        check("USB 1: the HCCA's low bits read 0", usb1[0x18 / 4] == 0x12345600);
        usb1[0x04 / 4] = 0x80;
        usb1[0x08 / 4] = 1; // host controller reset
        check("USB 1: reset", usb1[0x04 / 4] == 0 && (usb1[0x08 / 4] & 1) == 0 && usb1[0x18 / 4] == 0 &&
                                  usb1[0x34 / 4] == 0x27782edf && usb1[0x44 / 4] == 0x628);
        check("USB 1: nothing plugged in", (usb1[0x54 / 4] & 1) == 0 && (usb1[0x5c / 4] & 1) == 0);
        // the AC'97's global status: the primary codec ready, its status bits read-only, the rest kept
        {
            volatile ULONG *glob_sta = (volatile ULONG *)0xfec00130;
            const ULONG before = *glob_sta;
            *glob_sta = 0x000300fe;
            const ULONG after = *glob_sta;
            *glob_sta = 0;
            check("AC97: the global status", (before & 0x100) && (before & 0xe0) == 0 &&
                                                 after == (0x30000 | 0x100) && *glob_sta == 0x100);
        }
        // a box run with its interrupts off, polled through the global status (DirectSound waits on
        // the S/PDIF box this way, at raised IRQL): its bit (nForce's 0x10) comes when the buffer ends
        {
            ULONG *desc = MmAllocateContiguousMemory(4096);
            short *samples = (short *)(desc + 16);
            for (int i = 0; i < 256; i++)
                samples[i] = 0;
            desc[0] = MmGetPhysicalAddress(samples);
            desc[1] = 0x80000000 | 256; // IOC, 256 samples
            volatile UCHAR *ac97 = (volatile UCHAR *)0xfec00000;
            volatile ULONG *glob_sta = (volatile ULONG *)0xfec00130;
            *(volatile ULONG *)(ac97 + 0x170) = MmGetPhysicalAddress(desc);
            ac97[0x175] = 0;   // last valid: the first
            ac97[0x17b] = 1;   // run, no interrupt enables
            const ULONG start = KeTickCount;
            while (!(*glob_sta & 0x10) && KeTickCount - start < 500)
                ;
            check("AC97: a finished S/PDIF buffer shows in the global status (interrupts off)", (*glob_sta & 0x10) != 0);
            ac97[0x17b] = 2;                              // reset the box
            *(volatile USHORT *)(ac97 + 0x176) = 0x1c;    // its status bits cleared
            check("AC97: and goes once the box's status is cleared", (*glob_sta & 0x10) == 0);
            MmFreeContiguousMemory(desc);
        }
        // the GP DSP runs its microcode: booted from scratch memory (its scatter-gather list), as
        // DirectSound loads it: "bset #0, X:$10" then "jmp $1"; the frames running, X:$10 becomes 1
        {
            ULONG *table = MmAllocateContiguousMemory(4096), *code = MmAllocateContiguousMemory(4096);
            for (int i = 0; i < 1024; i++)
                code[i] = 0;
            code[0] = 0x0A1020; // bset #0, X:$10
            code[1] = 0x0C0001; // jmp $1
            table[0] = MmGetPhysicalAddress(code);
            table[1] = 0;
            volatile ULONG *apu = (volatile ULONG *)0xfe800000;
            apu[0x2040 / 4] = MmGetPhysicalAddress(table); // GPSADDR
            apu[0x20d4 / 4] = 0;                          // GPSMAXSGE: one page
            volatile ULONG *gp = (volatile ULONG *)0xfe830000;
            gp[0x10] = 0;
            gp[0xfffc / 4] = 0;            // in reset
            gp[0xfffc / 4] = 3;            // out: the bootstrap copies the microcode in
            const ULONG sectl = apu[0x2000 / 4];
            apu[0x2000 / 4] = sectl | 0x8;  // the frames run (XCNTMODE)
            const ULONG start = KeTickCount;
            while (gp[0x10] != 1 && KeTickCount - start < 500)
                ;
            check("APU: the GP DSP runs the microcode it boots from scratch memory", gp[0x10] == 1);
            check("APU: its program memory holds that microcode", gp[0xa000 / 4] == 0x0A1020 && gp[0xa004 / 4] == 0x0C0001);
            gp[0xfffc / 4] = 0;
            apu[0x2000 / 4] = sectl;
            MmFreeContiguousMemory(table);
            MmFreeContiguousMemory(code);
        }
        // string copies to and from device memory (DirectSound copies the GP DSP's memory with rep
        // movsd): rep stosd in, rep movsd out, then a backwards rep movsd in from RAM
        {
            ULONG out[4] = {0}, src[3] = {0x111111, 0x222222, 0x333333};
            void *gp = (void *)0xfe830200, *dst = out;
            ULONG n = 3, value = 0x5aa5a5; // 24-bit words, as the DSP keeps them
            __asm__ volatile("cld; rep stosl" : "+D"(gp), "+c"(n) : "a"(value) : "memory");
            gp = (void *)0xfe830200;
            n = 3;
            __asm__ volatile("rep movsl" : "+S"(gp), "+D"(dst), "+c"(n) : : "memory");
            check("APU: rep stosd into the GP's memory, rep movsd out",
                  out[0] == value && out[1] == value && out[2] == value && out[3] == 0 && n == 0 &&
                      gp == (void *)0xfe83020c && dst == &out[3]);
            void *from = &src[2];
            gp = (void *)0xfe830208;
            n = 3;
            __asm__ volatile("std; rep movsl; cld" : "+S"(from), "+D"(gp), "+c"(n) : : "memory");
            volatile ULONG *g = (volatile ULONG *)0xfe830200;
            check("APU: a backwards rep movsd from RAM", g[0] == src[0] && g[1] == src[1] && g[2] == src[2]);
        }
        // kernel queues: in order, at the head, a timeout when empty, a rundown of what is left
        static KQUEUE q;
        static LIST_ENTRY e1, e2, e3;
        KeInitializeQueue(&q, 0);
        KeInsertQueue(&q, &e1);
        KeInsertQueue(&q, &e2);
        KeInsertHeadQueue(&q, &e3);
        check("KeRemoveQueue: the head insert first, then in order",
              KeRemoveQueue(&q, KernelMode, NULL) == &e3 && KeRemoveQueue(&q, KernelMode, NULL) == &e1);
        LARGE_INTEGER brief;
        brief.QuadPart = -10000; // 1 ms
        check("KeRundownQueue: what is left", KeRundownQueue(&q) == &e2 && KeRundownQueue(&q) == NULL);
        check("KeRemoveQueue: a timeout when empty", (ULONG_PTR)KeRemoveQueue(&q, KernelMode, &brief) == 0x102);
        // device queues: busy after the first insert, then by key
        static KDEVICE_QUEUE dq;
        static KDEVICE_QUEUE_ENTRY d1, d2, d3;
        KeInitializeDeviceQueue(&dq);
        const BOOLEAN first_queued = KeInsertDeviceQueue(&dq, &d1);
        KeInsertByKeyDeviceQueue(&dq, &d2, 20);
        KeInsertByKeyDeviceQueue(&dq, &d3, 10);
        check("device queue: the first goes straight to the device", !first_queued && dq.Busy);
        check("device queue: by key", KeRemoveDeviceQueue(&dq) == &d3 && KeRemoveByKeyDeviceQueue(&dq, 15) == &d2);
        check("device queue: idle when empty", KeRemoveDeviceQueue(&dq) == NULL && !dq.Busy);
        // share access: a reader that doesn't share writing keeps a writer out
        static SHARE_ACCESS sa;
        static FILE_OBJECT f1, f2;
        IoSetShareAccess(0x1 /* FILE_READ_DATA */, 1 /* FILE_SHARE_READ */, &f1, &sa);
        check("IoCheckShareAccess: a writer refused while a reader doesn't share writing",
              IoCheckShareAccess(0x2 /* FILE_WRITE_DATA */, 3, &f2, &sa, FALSE) == 0xC0000043);
        check("IoCheckShareAccess: another reader allowed", IoCheckShareAccess(0x1, 1, &f2, &sa, TRUE) == 0 && sa.OpenCount == 2);
        // an object a program makes and names, found by its name
        ANSI_STRING oname;
        RtlInitAnsiString(&oname, "\\xbox-core-test-object");
        OBJECT_ATTRIBUTES oa = {NULL, &oname, 0};
        PVOID made = NULL, found = NULL;
        HANDLE oh = NULL;
        check("ObCreateObject / ObInsertObject", ObCreateObject(&ExEventObjectType, &oa, sizeof(KEVENT), &made) == 0 &&
                                                     ObInsertObject(made, &oa, 0, &oh) == 0 && oh != NULL);
        check("ObReferenceObjectByName", ObReferenceObjectByName(&oname, 0, NULL, NULL, &found) == 0 && found == made);
        // what AvSetSavedDataAddress leaves, AvGetSavedDataAddress gives
        AvSetSavedDataAddress((PVOID)0x1234000);
        check("AvGetSavedDataAddress", AvGetSavedDataAddress() == (PVOID)0x1234000);
        AvSetSavedDataAddress(NULL);
        // the clock's variables move, and interrupt time counts from the start
        const ULONG before = KeInterruptTime.LowPart;
        LARGE_INTEGER wait;
        wait.QuadPart = -20 * 10000; // 20 ms
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
        check("KeInterruptTime moves", KeInterruptTime.LowPart != before);
        check("KeSystemTime is the date", KeSystemTime.High1Time > 0x01000000);
        check("KeQueryInterruptTime counts from the start", KeQueryInterruptTime() < 36000000000ULL);
    }
    check("ExQueryPoolBlockSize", ExQueryPoolBlockSize(p) >= 100);
    memset(p, 0xab, 100);
    ExFreePool(p);

    // strings
    ANSI_STRING a, back;
    UNICODE_STRING u;
    RtlInitAnsiString(&a, "Hello Xbox");
    check("RtlAnsiStringToUnicodeString", RtlAnsiStringToUnicodeString(&u, &a, TRUE) == 0 && u.Length == 20 && u.Buffer[6] == L'X');
    check("RtlUnicodeStringToAnsiString", RtlUnicodeStringToAnsiString(&back, &u, TRUE) == 0 && back.Length == 10 &&
                                              memcmp(back.Buffer, "Hello Xbox", 10) == 0);
    ANSI_STRING lower;
    RtlInitAnsiString(&lower, "hello xbox");
    check("RtlCompareString (case-insensitive)", RtlCompareString(&a, &lower, TRUE) == 0 && RtlCompareString(&a, &lower, FALSE) < 0);
    check("RtlEqualUnicodeString", RtlEqualUnicodeString(&u, &u, FALSE));
    RtlFreeUnicodeString(&u);
    RtlFreeAnsiString(&back);
    ULONG value = 0;
    check("RtlCharToInteger", RtlCharToInteger("0x1F", 0, &value) == 0 && value == 31);
    char digits[16];
    check("RtlIntegerToChar", RtlIntegerToChar(255, 16, sizeof(digits), digits) == 0 && strcmp(digits, "FF") == 0);
    char buffer[64];
    RtlSprintf(buffer, "%d-%s-%04x-%*d", 42, "ab", 255, 4, 7);
    check("RtlSprintf", strcmp(buffer, "42-ab-00ff-   7") == 0);

    // 64-bit arithmetic
    LARGE_INTEGER big = {.QuadPart = 0x100000000LL};
    check("RtlExtendedIntegerMultiply", RtlExtendedIntegerMultiply(big, 3).QuadPart == 0x300000000LL);
    ULONG remainder = 0;
    big.QuadPart = 1000000007LL;
    check("RtlExtendedLargeIntegerDivide", RtlExtendedLargeIntegerDivide(big, 10, &remainder).QuadPart == 100000000 && remainder == 7);

    // interlocked lists
    SLIST_HEADER head;
    memset(&head, 0, sizeof(head));
    SINGLE_LIST_ENTRY e1, e2;
    InterlockedPushEntrySList(&head, &e1);
    InterlockedPushEntrySList(&head, &e2);
    check("InterlockedPop/PushEntrySList", InterlockedPopEntrySList(&head) == &e2 && InterlockedPopEntrySList(&head) == &e1 &&
                                             InterlockedPopEntrySList(&head) == NULL);

    // crypto: RFC 2202 test 1, and the classic RC4 vector
    UCHAR key[20], digest[20];
    memset(key, 0x0b, sizeof(key));
    XcHMAC(key, 20, (PUCHAR) "Hi There", 8, NULL, 0, digest);
    static const UCHAR expected[20] = {0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b,
                                       0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00};
    check("XcHMAC (HMAC-SHA1)", memcmp(digest, expected, 20) == 0);
    UCHAR rc4[258], text[9];
    memcpy(text, "Plaintext", 9);
    XcRC4Key(rc4, 3, (PUCHAR) "Key");
    XcRC4Crypt(rc4, 9, text);
    static const UCHAR cipher[9] = {0xbb, 0xf3, 0x16, 0xe8, 0xd9, 0x40, 0xaf, 0x0a, 0xd3};
    check("XcRC4", memcmp(text, cipher, 9) == 0);

    // a one-shot timer
    KTIMER timer;
    KeInitializeTimerEx(&timer, NotificationTimer);
    LARGE_INTEGER due = {.QuadPart = ms(200)};
    ULONG start = KeTickCount;
    KeSetTimer(&timer, due, NULL);
    NTSTATUS status = KeWaitForSingleObject(&timer, Executive, KernelMode, FALSE, NULL);
    ULONG took = KeTickCount - start;
    check("KeSetTimer + wait (200 ms)", status == 0 && took >= 190 && took < 400);

    // a periodic timer with a DPC: 100 ms, for half a second
    KDPC dpc;
    KeInitializeDpc(&dpc, dpc_routine, NULL);
    KeInitializeTimerEx(&timer, SynchronizationTimer);
    due.QuadPart = ms(100);
    KeSetTimerEx(&timer, due, 100, &dpc);
    LARGE_INTEGER wait = {.QuadPart = ms(550)};
    KeDelayExecutionThread(KernelMode, FALSE, &wait);
    check("KeCancelTimer", KeCancelTimer(&timer));
    DbgPrint("  periodic timer: %d DPCs\n", (int)dpc_count);
    check("KeSetTimerEx periodic DPCs", dpc_count >= 4 && dpc_count <= 6);

    // a user APC, run by an alertable wait
    NtQueueApcThread(NtCurrentThread(), apc_routine, (PVOID)0x1234, NULL, NULL);
    check("APC not run before an alertable wait", apc_count == 0);
    wait.QuadPart = ms(1000);
    status = KeDelayExecutionThread(UserMode, TRUE, &wait);
    check("NtQueueApcThread + alertable wait", status == STATUS_USER_APC && apc_count == 1 && apc_seen_context == 0x1234);

    // an NT timer with an APC routine
    HANDLE ntimer;
    NtCreateTimer(&ntimer, NULL, NotificationTimer);
    due.QuadPart = ms(50);
    NtSetTimerEx(ntimer, &due, timer_apc, UserMode, NULL, FALSE, 0, NULL);
    wait.QuadPart = ms(1000);
    status = KeDelayExecutionThread(UserMode, TRUE, &wait);
    check("NtSetTimerEx APC", status == STATUS_USER_APC && timer_apc_count == 1);
    status = NtWaitForSingleObject(ntimer, FALSE, NULL);
    check("NT timer signaled", status == 0);
    NtClose(ntimer);

    // kernel mutant
    KMUTANT mutant;
    KeInitializeMutant(&mutant, TRUE);
    check("KeReleaseMutant", KeReleaseMutant(&mutant, 0, FALSE, FALSE) == 0);
    check("KMUTANT free after release", KeWaitForSingleObject(&mutant, Executive, KernelMode, FALSE, NULL) == 0);

    // read/write lock
    ERWLOCK rw;
    ExInitializeReadWriteLock(&rw);
    ExAcquireReadWriteLockShared(&rw);
    ExAcquireReadWriteLockShared(&rw);
    ExReleaseReadWriteLock(&rw);
    ExReleaseReadWriteLock(&rw);
    ExAcquireReadWriteLockExclusive(&rw);
    ExReleaseReadWriteLock(&rw);
    check("ERWLOCK shared, shared, exclusive", 1);

    // events: duplicate, query, signal-and-wait
    HANDLE ev1, ev2, dup;
    NtCreateEvent(&ev1, NULL, NotificationEvent, FALSE);
    NtCreateEvent(&ev2, NULL, NotificationEvent, TRUE);
    check("NtDuplicateObject", NtDuplicateObject(ev1, &dup, 0) == 0);
    EVENT_BASIC_INFORMATION info;
    NtSetEvent(dup, NULL);
    check("NtQueryEvent through the duplicate", NtQueryEvent(ev1, &info) == 0 && info.EventState == 1);
    NtClearEvent(ev1);
    check("NtSignalAndWaitForSingleObjectEx", NtSignalAndWaitForSingleObjectEx(ev1, ev2, KernelMode, FALSE, NULL) == 0 &&
                                                  NtQueryEvent(ev1, &info) == 0 && info.EventState == 1);


    // file I/O ends as NT's I/O manager ends it: the event set, the APC queued, the completion port told
    {
        HANDLE f = open_path("\\Device\\CdRom0\\default.xbe", GENERIC_READ, FILE_OPEN);
        if (!f)
            f = CreateFileA("D:\\default.xbe", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        char buf[64];
        IO_STATUS_BLOCK iosb;
        LARGE_INTEGER at, zero;
        at.QuadPart = 0;
        zero.QuadPart = 0;
        HANDLE ev;
        NtCreateEvent(&ev, NULL, NotificationEvent, FALSE);
        NTSTATUS st = NtReadFile(f, ev, NULL, NULL, &iosb, buf, 64, &at);
        check("a read's event is set when it ends", (st == 0 || st == STATUS_PENDING) &&
                                                       NtWaitForSingleObject(ev, FALSE, &zero) == 0 &&
                                                       iosb.Information == 64 && buf[0] == 'X' && buf[1] == 'B');
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        st = NtReadFile(f, NULL, (PIO_APC_ROUTINE)NtUserIoApcDispatcher, (PVOID)on_read_done, (PIO_STATUS_BLOCK)&ov, buf,
                        16, &at);
        check("a read's APC waits for an alertable wait", apc_calls == 0);
        LARGE_INTEGER shortly;
        shortly.QuadPart = -10000; // 1 ms
        st = KeDelayExecutionThread(UserMode, TRUE, &shortly);
        check("ReadFileEx's completion routine runs (NtUserIoApcDispatcher)",
              st == STATUS_USER_APC && apc_calls == 1 && apc_error == 0 && apc_bytes == 16);
        HANDLE port;
        FILE_COMPLETION_INFORMATION bind;
        check("NtCreateIoCompletion", NtCreateIoCompletion(&port, 0, NULL, 0) == 0);
        bind.Port = port;
        bind.Key = (PVOID)0x1234;
        check("a file bound to a completion port",
              NtSetInformationFile(f, &iosb, &bind, sizeof(bind), FileCompletionInformation) == 0);
        NtReadFile(f, NULL, NULL, (PVOID)0x5678, &iosb, buf, 8, &at);
        PVOID key = NULL, context = NULL;
        IO_STATUS_BLOCK got;
        st = NtRemoveIoCompletion(port, &key, &context, &got, &zero);
        check("a read's packet on the completion port",
              st == 0 && key == (PVOID)0x1234 && context == (PVOID)0x5678 && got.Information == 8);
        FILE_FS_FULL_SIZE_INFORMATION full;
        check("FileFsFullSizeInformation", NtQueryVolumeInformationFile(f, &iosb, &full, sizeof(full),
                                                                        FileFsFullSizeInformation) == 0 &&
                                               full.BytesPerSector == 512 && full.TotalAllocationUnits.QuadPart > 0);
        NtClose(port);
        NtClose(ev);
        NtClose(f);
    }
    {
        HANDLE f = open_path("\\Device\\Harddisk0\\Partition1\\xc-rename-a.txt", GENERIC_WRITE | DELETE, FILE_OVERWRITE_IF);
        IO_STATUS_BLOCK iosb;
        char rename_buf[sizeof(FILE_RENAME_INFORMATION) + 64];
        FILE_RENAME_INFORMATION *r = (FILE_RENAME_INFORMATION *)rename_buf;
        memset(rename_buf, 0, sizeof(rename_buf));
        r->ReplaceIfExists = TRUE;
        RtlInitAnsiString(&r->FileName, "\\Device\\Harddisk0\\Partition1\\xc-rename-b.txt");
        NTSTATUS st = f ? NtSetInformationFile(f, &iosb, r, sizeof(rename_buf), FileRenameInformation) : -1;
        if (f)
            NtClose(f);
        HANDLE g = open_path("\\Device\\Harddisk0\\Partition1\\xc-rename-b.txt", GENERIC_READ, FILE_OPEN);
        HANDLE old = open_path("\\Device\\Harddisk0\\Partition1\\xc-rename-a.txt", GENERIC_READ, FILE_OPEN);
        check("FileRenameInformation (MoveFile)", f && st == 0 && g && !old);
        if (g)
            NtClose(g);
    }

    DbgPrint("kernel test: %d failures\n", failures);
    debugPrint("kernel test: %d failures\n", failures);
    for (;;)
        Sleep(1000);
}
