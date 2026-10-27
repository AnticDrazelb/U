// hello: a small Switch program that checks what a game needs first, and prints each result
// through the kernel's debug output (svcOutputDebugString). Built without libnx: start.S has the
// entry point and the kernel calls, link.ld the NRO layout.
//
// Everything is addressed relative to the code (no pointers in initialized data): the image is
// linked at 0 and loaded anywhere, with nothing to relocate it.

typedef unsigned char u8;
typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef long long s64;
typedef u32 Result;

Result svcSetHeapSize(u64* out_address, u64 size);
__attribute__((noreturn)) void svcExitProcess(void);
Result svcCreateThread(u32* out_handle, void (*entry)(void*), void* arg, void* stack_top,
                       s32 priority, s32 processor_id);
Result svcStartThread(u32 handle);
__attribute__((noreturn)) void svcExitThread(void);
void svcSleepThread(s64 nanoseconds);
Result svcGetThreadPriority(s32* out_priority, u32 handle);
u32 svcGetCurrentProcessorNumber(void);
Result svcCloseHandle(u32 handle);
Result svcWaitSynchronization(s32* out_index, const u32* handles, s32 count, s64 timeout_ns);
u64 svcGetSystemTick(void);
Result svcOutputDebugString(const char* text, u64 length);
Result svcGetInfo(u64* out, u32 info_type, u32 handle, u64 sub_id);

#define CURRENT_PROCESS 0xFFFF8001u
#define CURRENT_THREAD 0xFFFF8000u
#define INFO_TOTAL_MEMORY_SIZE 6
#define INFO_USED_MEMORY_SIZE 7
#define TICKS_PER_MS 19200ull // the system tick runs at 19.2 MHz

#define HEAP_SIZE (32ull << 20)
#define THREAD_STACK_SIZE (64ull << 10)
#define WORK 4000000ull

// One line of output, built up and then printed with svcOutputDebugString.
typedef struct {
    char text[160];
    u32 length;
} Line;

static void put(Line* line, const char* s) {
    while (*s && line->length < sizeof(line->text))
        line->text[line->length++] = *s++;
}

static void put_dec(Line* line, u64 value) {
    char digits[20];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n && line->length < sizeof(line->text))
        line->text[line->length++] = digits[--n];
}

static void put_hex(Line* line, u64 value) {
    put(line, "0x");
    int shift = 60;
    while (shift > 0 && (value >> shift) == 0)
        shift -= 4;
    for (; shift >= 0 && line->length < sizeof(line->text); shift -= 4)
        line->text[line->length++] = "0123456789abcdef"[(value >> shift) & 0xF];
}

static void print(Line* line) {
    svcOutputDebugString(line->text, line->length);
    line->length = 0;
}

static void say(const char* s) {
    Line line = {.length = 0};
    put(&line, "hello: ");
    put(&line, s);
    print(&line);
}

__attribute__((noreturn)) static void fail(const char* what, Result rc) {
    Line line = {.length = 0};
    put(&line, "hello: FAILED: ");
    put(&line, what);
    if (rc) {
        put(&line, " (result ");
        put_hex(&line, rc);
        put(&line, ")");
    }
    print(&line);
    svcExitProcess();
}

// Integer work for the recompiler: shifts, multiplies, rotates, a loop-carried dependency.
static u64 compute(u64 n) {
    u64 x = 0x9E3779B97F4A7C15ull, acc = 0;
    for (u64 i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        acc += x * (i | 1);
        acc = (acc << 5) | (acc >> 59);
    }
    return acc ^ x;
}

typedef struct {
    u64 result;
    u32 core;
} ThreadWork;

static void thread_entry(void* arg) {
    ThreadWork* work = (ThreadWork*)arg;
    work->core = svcGetCurrentProcessorNumber();
    work->result = compute(WORK);
    svcExitThread();
}

__attribute__((noreturn)) void hello_main(void);

void hello_main(void) {
    Line line = {.length = 0};
    Result rc;

    put(&line, "hello: a Switch program is running, on emulated core ");
    put_dec(&line, svcGetCurrentProcessorNumber());
    print(&line);

    u64 total = 0, used = 0;
    if ((rc = svcGetInfo(&total, INFO_TOTAL_MEMORY_SIZE, CURRENT_PROCESS, 0)) != 0)
        fail("svcGetInfo(TotalMemorySize)", rc);
    if ((rc = svcGetInfo(&used, INFO_USED_MEMORY_SIZE, CURRENT_PROCESS, 0)) != 0)
        fail("svcGetInfo(UsedMemorySize)", rc);
    put(&line, "hello: memory available to it: ");
    put_dec(&line, (total - used) >> 20);
    put(&line, " MiB");
    print(&line);

    // The heap, every word written and read back through the recompiler's page table.
    u64 heap = 0;
    if ((rc = svcSetHeapSize(&heap, HEAP_SIZE)) != 0 || heap == 0)
        fail("svcSetHeapSize", rc);
    volatile u64* words = (volatile u64*)heap;
    for (u64 i = 0; i < HEAP_SIZE / 8; i++)
        words[i] = i * 0x9E3779B97F4A7C15ull;
    for (u64 i = 0; i < HEAP_SIZE / 8; i++)
        if (words[i] != i * 0x9E3779B97F4A7C15ull)
            fail("heap read back a different value", 0);
    put(&line, "hello: heap: ");
    put_dec(&line, HEAP_SIZE >> 20);
    put(&line, " MiB at ");
    put_hex(&line, heap);
    put(&line, ", written and read back");
    print(&line);

    u64 start = svcGetSystemTick();
    const u64 result = compute(WORK);
    u64 ticks = svcGetSystemTick() - start;
    put(&line, "hello: computed ");
    put_hex(&line, result);
    put(&line, " in ");
    put_dec(&line, ticks / TICKS_PER_MS);
    put(&line, " ms");
    print(&line);

    // The scheduler and the emulated clock: a sleep measured with the system tick.
    start = svcGetSystemTick();
    svcSleepThread(250000000);
    ticks = svcGetSystemTick() - start;
    put(&line, "hello: slept 250 ms, the clock says ");
    put_dec(&line, ticks / TICKS_PER_MS);
    put(&line, " ms");
    print(&line);
    if (ticks < 240 * TICKS_PER_MS)
        fail("the sleep ended early", 0);

    // A second thread, on emulated core 1, with its stack and results at the end of the heap.
    s32 priority = 0;
    if ((rc = svcGetThreadPriority(&priority, CURRENT_THREAD)) != 0)
        fail("svcGetThreadPriority", rc);
    ThreadWork* work = (ThreadWork*)(heap + HEAP_SIZE - THREAD_STACK_SIZE - 64);
    work->result = 0;
    work->core = 99;
    u32 thread = 0;
    if ((rc = svcCreateThread(&thread, thread_entry, work, (void*)(heap + HEAP_SIZE), priority, 1)) != 0)
        fail("svcCreateThread", rc);
    if ((rc = svcStartThread(thread)) != 0)
        fail("svcStartThread", rc);
    s32 index = -1;
    if ((rc = svcWaitSynchronization(&index, &thread, 1, 10000000000ll)) != 0)
        fail("svcWaitSynchronization on the thread", rc);
    svcCloseHandle(thread);
    if (work->core != 1)
        fail("the thread did not run on core 1", 0);
    if (work->result != result)
        fail("the thread computed a different result", 0);
    say("a thread on core 1 computed the same");

    say("all checks passed, exiting");
    svcExitProcess();
}
