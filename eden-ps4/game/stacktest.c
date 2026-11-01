// Aliased memory (svcMapMemory, what thread stacks are) and threads, for the kernel's lining up of
// aliases with the PS4's 16 KiB pages (Eden's k_page_table_base.cpp moves an alias's memory there):
// whatever the alias's address and size, it reads what its source held, its writes reach the
// source after svcUnmapMemory, the pages beside it stay free, and threads on such stacks run.
// Prints "stacktest: all checks passed" (svcOutputDebugString) when they do.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

static int failures = 0;

static void say(const char* format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    svcOutputDebugString(text, n > 0 ? (u64)n : 0);
}

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            say("stacktest: FAIL " __VA_ARGS__);                                                   \
        }                                                                                          \
    } while (0)

static u32 pattern(u32 seed, size_t i) {
    u32 x = seed ^ (u32)(i * 2654435761u);
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    return x ^ (x >> 15);
}

static bool is_free(uintptr_t address) {
    MemoryInfo info;
    u32 page_info;
    if (R_FAILED(svcQueryMemory(&info, &page_info, address)))
        return false;
    return info.type == MemType_Unmapped;
}

// One alias: `size` bytes at `offset` pages into a free area, of heap memory filled with a pattern
static void alias_test(size_t size, size_t offset_pages, u32 seed) {
    const size_t guard = 0x8000;
    u32* src = aligned_alloc(0x1000, size);
    if (!src) {
        CHECK(false, "alloc %zx", size);
        return;
    }
    for (size_t i = 0; i < size / 4; i++)
        src[i] = pattern(seed, i);

    virtmemLock();
    u8* area = virtmemFindStack(size + 0x4000, guard);
    u32* dst = (u32*)(area + offset_pages * 0x1000);
    VirtmemReservation* reservation = virtmemAddReservation(area, size + 0x4000);
    virtmemUnlock();
    Result rc = svcMapMemory(dst, src, size);
    CHECK(R_SUCCEEDED(rc), "svcMapMemory %p <- %p +%zx: %x", (void*)dst, (void*)src, size, rc);
    if (R_FAILED(rc))
        goto out;

    // the alias holds the source's data; the pages beside it are free
    size_t bad = 0;
    for (size_t i = 0; i < size / 4; i++)
        bad += dst[i] != pattern(seed, i);
    CHECK(bad == 0, "alias %p +%zx (offset %zu pages): %zu words differ", (void*)dst, size, offset_pages, bad);
    CHECK(is_free((uintptr_t)dst - 0x1000), "the page before the alias at %p isn't free", (void*)dst);
    CHECK(is_free((uintptr_t)dst + size), "the page after the alias at %p isn't free", (void*)dst);
    MemoryInfo info;
    u32 page_info;
    svcQueryMemory(&info, &page_info, (uintptr_t)dst);
    CHECK(info.addr == (uintptr_t)dst && info.size == size && info.type == MemType_MappedMemory,
          "alias at %p: query %lx +%lx type %x", (void*)dst, info.addr, info.size, info.type);

    // writes through the alias reach the source once it's unmapped
    for (size_t i = 0; i < size / 4; i++)
        dst[i] = pattern(seed + 1, i);
    rc = svcUnmapMemory(dst, src, size);
    CHECK(R_SUCCEEDED(rc), "svcUnmapMemory %p: %x", (void*)dst, rc);
    bad = 0;
    for (size_t i = 0; i < size / 4; i++)
        bad += src[i] != pattern(seed + 1, i);
    CHECK(bad == 0, "source %p +%zx after the alias (offset %zu pages): %zu words differ", (void*)src, size,
          offset_pages, bad);
    CHECK(is_free((uintptr_t)dst), "the alias at %p is still mapped", (void*)dst);

out:
    virtmemLock();
    virtmemRemoveReservation(reservation);
    virtmemUnlock();
    free(src);
}

// Threads: recursion on their stacks, a sum that depends on every frame
static u64 recurse(u32 depth, u32 seed) {
    volatile u32 frame[64];
    for (u32 i = 0; i < 64; i++)
        frame[i] = pattern(seed + depth, i);
    u64 sum = depth == 0 ? 0 : recurse(depth - 1, seed);
    for (u32 i = 0; i < 64; i++)
        sum += frame[i] ^ i;
    return sum;
}

typedef struct {
    u32 seed;
    u32 depth;
    u64 result;
} Work;

static void worker(void* arg) {
    Work* work = arg;
    work->result = recurse(work->depth, work->seed);
}

int main(void) {
    // aliases of 1 to 35 pages, at each place in a 16 KiB page
    static const size_t sizes[] = {0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x7000, 0x8000, 0x10000, 0x23000, 0x40000};
    u32 seed = 1;
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
        for (size_t offset = 0; offset < 4; offset++)
            alias_test(sizes[s], offset, seed++);
    say("stacktest: aliases done (%d failures)", failures);

    // two aliases side by side, one page apart (each one's spare pages where the other is)
    {
        const size_t size = 0x3000;
        u32* a = aligned_alloc(0x1000, size);
        u32* b = aligned_alloc(0x1000, size);
        for (size_t i = 0; i < size / 4; i++) {
            a[i] = pattern(100, i);
            b[i] = pattern(200, i);
        }
        virtmemLock();
        u8* area = virtmemFindStack(0x10000, 0x8000);
        VirtmemReservation* reservation = virtmemAddReservation(area, 0x10000);
        virtmemUnlock();
        u32* da = (u32*)(area + 0x1000);
        u32* db = (u32*)(area + 0x1000 + size + 0x1000);
        CHECK(R_SUCCEEDED(svcMapMemory(da, a, size)), "side by side: map a");
        CHECK(R_SUCCEEDED(svcMapMemory(db, b, size)), "side by side: map b");
        size_t bad = 0;
        for (size_t i = 0; i < size / 4; i++)
            bad += (da[i] != pattern(100, i)) + (db[i] != pattern(200, i));
        CHECK(bad == 0, "side by side: %zu words differ", bad);
        CHECK(is_free((uintptr_t)da + size), "side by side: the page between isn't free");
        for (size_t i = 0; i < size / 4; i++) {
            da[i] = pattern(101, i);
            db[i] = pattern(201, i);
        }
        CHECK(R_SUCCEEDED(svcUnmapMemory(da, a, size)), "side by side: unmap a");
        bad = 0;
        for (size_t i = 0; i < size / 4; i++)
            bad += (a[i] != pattern(101, i)) + (db[i] != pattern(201, i));
        CHECK(bad == 0, "side by side: after unmapping a, %zu words differ", bad);
        CHECK(R_SUCCEEDED(svcUnmapMemory(db, b, size)), "side by side: unmap b");
        bad = 0;
        for (size_t i = 0; i < size / 4; i++)
            bad += b[i] != pattern(201, i);
        CHECK(bad == 0, "side by side: after unmapping b, %zu words differ", bad);
        virtmemLock();
        virtmemRemoveReservation(reservation);
        virtmemUnlock();
        free(a);
        free(b);
    }
    say("stacktest: side by side done (%d failures)", failures);

    // threads (libnx maps each one's stack with svcMapMemory), many times over
    u64 expected[8];
    for (u32 t = 0; t < 8; t++)
        expected[t] = recurse(20 + t, 1000 + t);
    for (int round = 0; round < 40; round++) {
        Thread threads[8];
        Work work[8];
        for (u32 t = 0; t < 8; t++) {
            work[t] = (Work){1000 + t, 20 + t, 0};
            const size_t stack = 0x4000 * (2 + (t + round) % 5) + 0x1000 * ((t + round) % 3);
            Result rc = threadCreate(&threads[t], worker, &work[t], NULL, stack, 0x2C, -2);
            CHECK(R_SUCCEEDED(rc), "threadCreate round %d thread %u (stack %zx): %x", round, t, stack, rc);
            if (R_SUCCEEDED(rc))
                threadStart(&threads[t]);
            else
                work[t].result = expected[t];
        }
        for (u32 t = 0; t < 8; t++) {
            if (threads[t].handle) {
                threadWaitForExit(&threads[t]);
                threadClose(&threads[t]);
            }
            CHECK(work[t].result == expected[t], "round %d thread %u: %lx, not %lx", round, t, work[t].result,
                  expected[t]);
        }
    }
    say("stacktest: threads done (%d failures)", failures);

    if (failures == 0)
        say("stacktest: all checks passed");
    else
        say("stacktest: %d checks failed", failures);
    return failures == 0 ? 0 : 1;
}
