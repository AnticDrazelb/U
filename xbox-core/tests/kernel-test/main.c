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
