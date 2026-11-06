// The native memory functions (04.75-04.80: memcpy, memmove, memset and memcmp run on the host when
// a module's dynamic symbols name them) and thread-local storage in 16 KiB groups (04.80). Built
// with its symbols exported (NX_LDFLAGS=--export-dynamic: newlib's memcpy... in .dynsym, as the
// SDK's are), once with a SysV hash table and once with a GNU one (both of Eden's ways of counting
// the symbols). Random copies, overlapping moves, fills and compares against byte loops; then
// threads, each checking its own thread-local value and a buffer of its own. Prints
// "nativetest: all checks passed" (svcOutputDebugString) when they all do.
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
            if (failures++ < 20)                                                                   \
                say("nativetest: FAIL " __VA_ARGS__);                                              \
        }                                                                                          \
    } while (0)

static u32 rng_state = 0x12345678;
static u32 rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// The references: byte loops through volatile pointers (so the compiler can't make them calls to
// the functions they check)
static void ref_move(volatile u8* dest, const volatile u8* src, size_t n) {
    if (dest < src) {
        for (size_t i = 0; i < n; i++)
            dest[i] = src[i];
    } else {
        for (size_t i = n; i-- > 0;)
            dest[i] = src[i];
    }
}
static void ref_set(volatile u8* dest, u8 c, size_t n) {
    for (size_t i = 0; i < n; i++)
        dest[i] = c;
}
static int ref_cmp(const volatile u8* a, const volatile u8* b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}
static int sign(int x) {
    return (x > 0) - (x < 0);
}

// The calls through pointers the compiler can't see through: real calls to the module's functions
// (the ones the JIT runs natively), not inlined copies
static void* (*volatile p_memcpy)(void*, const void*, size_t) = memcpy;
static void* (*volatile p_memmove)(void*, const void*, size_t) = memmove;
static void* (*volatile p_memset)(void*, int, size_t) = memset;
static int (*volatile p_memcmp)(const void*, const void*, size_t) = memcmp;

#define SIZE 0x10000
static u8 a[SIZE], b[SIZE], ref[SIZE];

static void fill_random(u8* p, size_t n) {
    for (size_t i = 0; i < n; i++)
        p[i] = (u8)rng();
}

static void memory_tests(void) {
    int copies = 0, moves = 0, sets = 0, compares = 0;
    for (int round = 0; round < 20000; round++) {
        fill_random(a, SIZE);
        memcpy(ref, a, SIZE);  // (the reference starts the same)
        // sizes: mostly small, sometimes pages long; offsets anywhere (unaligned)
        const size_t n = (rng() & 3) == 0 ? rng() % 9000 : rng() % 80;
        const size_t d = rng() % (SIZE - n), s = rng() % (SIZE - n);
        const u32 kind = rng() % 4;
        if (kind == 0) {
            // memcpy: no overlap (undefined), so the source from b
            fill_random(b, SIZE);
            void* r = p_memcpy(a + d, b + s, n);
            ref_move(ref + d, b + s, n);
            CHECK(r == a + d, "memcpy's result %p, not %p", r, (void*)(a + d));
            copies++;
        } else if (kind == 1) {
            // memmove: within a, overlapping as often as not
            const size_t s2 = (rng() & 1) ? (size_t)((long)d + (long)(rng() % 64) - 32 + (long)SIZE) % (SIZE - n) : s;
            void* r = p_memmove(a + d, a + s2, n);
            ref_move(ref + d, ref + s2, n);
            CHECK(r == a + d, "memmove's result %p, not %p", r, (void*)(a + d));
            moves++;
        } else if (kind == 2) {
            const int c = (int)rng();  // (only its low byte counts)
            void* r = p_memset(a + d, c, n);
            ref_set(ref + d, (u8)c, n);
            CHECK(r == a + d, "memset's result %p, not %p", r, (void*)(a + d));
            sets++;
        } else {
            // memcmp: equal runs, then (mostly) one byte changed somewhere
            memcpy(b, a, SIZE);
            if (n > 0 && (rng() & 3) != 0)
                b[s + rng() % n] ^= (u8)(1 + rng() % 255);
            const int got = p_memcmp(a + d, b + s, n), want = ref_cmp(a + d, b + s, n);
            CHECK(sign(got) == sign(want), "memcmp(%zu, %zu, %zu) %d, should be %d", d, s, n, got, want);
            const int same = p_memcmp(a + d, a + d, n);
            CHECK(same == 0, "memcmp of a range with itself %d", same);
            compares++;
            continue;
        }
        CHECK(memcmp(a, ref, SIZE) == 0, "round %d (kind %u, %zu bytes at %zu from %zu): wrong bytes", round, kind, n, d,
              s);
    }
    say("nativetest: %d copies, %d moves, %d fills, %d compares", copies, moves, sets, compares);
}

// Threads: each its own thread-local value (libnx keeps the thread's pointer in its TLS slot, read
// through TPIDRRO_EL0) and a buffer of its own, copied and compared many times
static __thread u32 tls_value;
static volatile int thread_failures = 0;

static void worker(void* arg) {
    const u32 id = (u32)(uintptr_t)arg;
    tls_value = 0xC0DE0000u | id;
    u8 mine[512], copy[512];
    for (int i = 0; i < 2000; i++) {
        p_memset(mine, (int)(id + i), sizeof(mine));
        p_memcpy(copy, mine, sizeof(copy));
        if (p_memcmp(copy, mine, sizeof(copy)) != 0 || copy[sizeof(copy) - 1] != (u8)(id + i))
            thread_failures++;
        if (tls_value != (0xC0DE0000u | id))
            thread_failures++;
        if (i % 256 == 0)
            svcSleepThread(0);
    }
}

static void thread_tests(void) {
    for (int round = 0; round < 10; round++) {
        Thread threads[12];
        int started = 0;
        for (int t = 0; t < 12; t++) {
            if (R_SUCCEEDED(threadCreate(&threads[t], worker, (void*)(uintptr_t)(round * 16 + t), NULL, 0x10000, 0x2C, -2))) {
                threadStart(&threads[t]);
                started++;
            } else {
                break;
            }
        }
        for (int t = 0; t < started; t++) {
            threadWaitForExit(&threads[t]);
            threadClose(&threads[t]);
        }
        CHECK(started == 12, "round %d: only %d threads started", round, started);
    }
    CHECK(thread_failures == 0, "%d wrong values in the threads", thread_failures);
}

// Timed: 64 KiB copies, fills and compares (native: the host's own; translated: newlib's loops),
// for the log (EDEN_PS4_JIT_OFF=native against the default shows the difference)
static void timing(void) {
    const u64 start = armGetSystemTick();
    for (int i = 0; i < 2000; i++) {
        p_memcpy(b, a, SIZE);
        p_memset(a, i, SIZE);
        if (p_memcmp(a, b, SIZE) == 0 && i != 0)
            failures += 0;  // (the result used)
    }
    const u64 ns = armTicksToNs(armGetSystemTick() - start);
    say("nativetest: 64 KiB memcpy + memset + memcmp: %llu ns a round", (unsigned long long)(ns / 2000));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    memory_tests();
    timing();
    thread_tests();
    if (failures == 0)
        say("nativetest: all checks passed");
    else
        say("nativetest: %d checks failed", failures);
    return failures == 0 ? 0 : 1;
}
