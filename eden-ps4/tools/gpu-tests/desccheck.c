// (04.44) The descriptor check (port/mesa/ac_ps4_drm.c, between "descriptor check: begin" and "end"),
// built on a PC: fake GPU buffers in two GPU windows like the PS4 port's (16 GiB and a 32-bit one;
// at 0x10000000 and 0x40000000 here, smaller: ASan's shadow covers the console's, and render target bases have 40 bits), descriptor sets in them, and command streams
// whose draws point at the sets through user data. A real set with a wild buffer descriptor: the draw
// becomes a NOP; a wild one in a set with a single valid descriptor: described, not skipped; a buffer
// range far past its buffer: an overrun; random data a user-data value points at: never skipped.
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PS4_VA_SIZE 0x20000000ull /* (the test's: low memory, below ASan's shadow) */
#define PS4_VA32_SIZE 0x10000000ull
#define PS4_TAIL 0x10000ull

struct amdgpu_bo {
    uint32_t handle;
    uint64_t size;
    void* cpu;
    bool tail_mapped;
};
typedef struct ac_drm_device {
    struct amdgpu_bo** bos;
    unsigned num_bos;
    uint64_t va_start, va32_start;
} ac_drm_device;

static int g_lines;
static char g_last[400];
static void ps4_boot_trace(const char* line) {
    g_lines++;
    snprintf(g_last, sizeof(g_last), "%s", line);
    printf("  trace: %s\n", line);
}
static bool ps4_mapped(const void* p, uint64_t len) { (void)p; (void)len; return true; }
static void ps4_flush_cpu_cache(const void* p, uint64_t n) { (void)p; (void)n; }

#include "desccheck.inc"

static struct amdgpu_bo* g_bos[16];
static ac_drm_device g_dev = {g_bos, 1, 0x10000000ull, 0x40000000ull};

static void* map_at(uint64_t va, size_t size) {
    void* p = mmap((void*)va, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void*)va) {
        perror("mmap");
        exit(2);
    }
    return p;
}
static void add_bo(uint64_t va, size_t size) {
    struct amdgpu_bo* b = calloc(1, sizeof(*b));
    b->handle = g_dev.num_bos;
    b->size = size;
    b->cpu = map_at(va, size);
    g_bos[g_dev.num_bos++] = b;
}
static void vsharp(uint32_t* d, uint64_t base, uint32_t records) {
    d[0] = (uint32_t)base;
    d[1] = (uint32_t)(base >> 32) & 0xffff;
    d[2] = records;
    d[3] = 0x27fac;  // x y z w, float, 32
}
static void tsharp(uint32_t* d, uint64_t base) {
    memset(d, 0, 32);
    d[0] = (uint32_t)(base >> 8);
    d[1] = (uint32_t)(base >> 40) & 0xff;
    d[3] = (9u << 28) | 0xfac;  // 2D
}
static unsigned build_draw(uint32_t* ib, uint32_t ud0) {
    unsigned n = 0;
    ib[n++] = 0xc0000000u | (1u << 16) | (0x76u << 8);  // SET_SH_REG, 2 dwords: VS user data 0
    ib[n++] = 0x4c;
    ib[n++] = ud0;
    ib[n++] = 0xc0000000u | (1u << 16) | (0x2du << 8);  // DRAW_INDEX_AUTO
    ib[n++] = 3;
    ib[n++] = 2;
    return n;
}
static int g_failures;
static void run(const char* name, uint32_t ud0, unsigned want_wild, unsigned want_over, bool want_skip) {
    uint32_t ib[16];
    const unsigned n = build_draw(ib, ud0);
    struct ps4_live_range ranges[16];
    struct ps4_check c;
    memset(&c, 0, sizeof(c));
    c.dev = &g_dev;
    c.ranges = ranges;
    for (unsigned h = 1; h < g_dev.num_bos; h++) {
        const uint64_t start = (uintptr_t)g_bos[h]->cpu;
        ranges[c.num_ranges++] = (struct ps4_live_range){start, start + g_bos[h]->size, g_bos[h]->size, h};
    }
    qsort(ranges, c.num_ranges, sizeof(ranges[0]), ps4_live_range_cmp);
    printf("-- %s\n", name);
    ps4_check_ib(&c, ib, n, 7, 0, true);
    const bool skipped = ((ib[3] >> 8) & 0xff) == 0x10;
    if (c.wild != want_wild || c.overruns != want_over || skipped != want_skip || (c.skipped != 0) != want_skip) {
        printf("FAIL %s: wild %u (%u), overruns %u (%u), skipped %d (%d)\n", name, c.wild, want_wild, c.overruns,
               want_over, skipped, want_skip);
        g_failures++;
    }
}

// (04.48) a draw with color target 0 (and optionally depth) set through SET_CONTEXT_REG
static void ctx_reg(uint32_t* ib, unsigned* n, unsigned reg, uint32_t v) {
    ib[(*n)++] = 0xc0000000u | (1u << 16) | (0x69u << 8);
    ib[(*n)++] = reg;
    ib[(*n)++] = v;
}
static struct ps4_hole g_hole; // (04.50) a hole the mapping audit found, for run_rt
static void run_rt(const char* name, uint64_t cb, unsigned slice_tile_max, unsigned mode, uint64_t z,
                   unsigned want_wild, unsigned want_over, bool want_skip) {
    uint32_t ib[64];
    unsigned n = 0;
    ctx_reg(ib, &n, 0x202, mode << 4);                    // CB_COLOR_CONTROL
    ctx_reg(ib, &n, 0x8e, 0xf);                           // CB_TARGET_MASK
    ctx_reg(ib, &n, 0x318, (uint32_t)(cb >> 8));          // CB_COLOR0_BASE
    ctx_reg(ib, &n, 0x31a, slice_tile_max);               // CB_COLOR0_SLICE
    ctx_reg(ib, &n, 0x31c, (0x0c << 2) | (4 << 8));       // CB_COLOR0_INFO: 16_16_16_16 float
    if (z) {
        ctx_reg(ib, &n, 0x10, 3);                         // DB_Z_INFO: Z_32_FLOAT
        ctx_reg(ib, &n, 0x12, (uint32_t)(z >> 8));        // DB_Z_READ_BASE
        ctx_reg(ib, &n, 0x14, (uint32_t)(z >> 8));        // DB_Z_WRITE_BASE
        ctx_reg(ib, &n, 0x17, 15);                        // DB_DEPTH_SLICE: 32x32
    }
    const unsigned draw = n;
    ib[n++] = 0xc0000000u | (1u << 16) | (0x2du << 8);  // DRAW_INDEX_AUTO
    ib[n++] = 3;
    ib[n++] = 2;
    struct ps4_live_range ranges[16];
    struct ps4_check c;
    memset(&c, 0, sizeof(c));
    c.dev = &g_dev;
    c.ranges = ranges;
    for (unsigned h = 1; h < g_dev.num_bos; h++) {
        const uint64_t start = (uintptr_t)g_bos[h]->cpu;
        ranges[c.num_ranges++] = (struct ps4_live_range){start, start + g_bos[h]->size, g_bos[h]->size, h};
    }
    qsort(ranges, c.num_ranges, sizeof(ranges[0]), ps4_live_range_cmp);
    if (g_hole.end) {
        c.holes[c.num_holes++] = g_hole;
        c.holes_trusted = true;
    }
    printf("-- %s\n", name);
    ps4_check_ib(&c, ib, n, 9, 0, true);
    const bool skipped = ((ib[draw] >> 8) & 0xff) == 0x10;
    if (c.wild != want_wild || c.overruns != want_over || skipped != want_skip) {
        printf("FAIL %s: wild %u (%u), overruns %u (%u), skipped %d (%d)\n", name, c.wild, want_wild, c.overruns,
               want_over, skipped, want_skip);
        g_failures++;
    }
}

int main(void) {
    add_bo(0x40000000ull, 0x10000);  // 32-bit window: descriptor sets
    add_bo(0x10100000ull, 0x10000);  // a buffer
    add_bo(0x10200000ull, 0x40000);  // an image
    uint32_t* sets = g_bos[1]->cpu;
    // set A (offset 0x100): a valid V#, a valid T#, a wild V# (0x4000900000: in the window, no buffer)
    uint32_t* a = sets + 0x100 / 4;
    vsharp(a, 0x10100000ull, 0x1000);
    tsharp(a + 4, 0x10200000ull);
    vsharp(a + 12, 0x10900000ull, 0x100);
    run("a real set with a wild buffer descriptor", 0x100, 1, 0, true);
    // set B (offset 0x400): one valid V#, one wild T#: described, not skipped
    uint32_t* bset = sets + 0x400 / 4;
    vsharp(bset, 0x10100000ull, 0x100);
    tsharp(bset + 4, 0x10a00000ull);
    run("a single valid descriptor beside a wild one", 0x400, 1, 0, false);
    // set C (offset 0x800): two valid, one 1 MiB past its 64 KiB buffer
    uint32_t* cs = sets + 0x800 / 4;
    vsharp(cs, 0x10100000ull, 0x100);
    tsharp(cs + 4, 0x10200000ull);
    vsharp(cs + 12, 0x10100000ull, 0x110000);
    run("a buffer range 1 MiB past its buffer (04.55: reported, not skipped)", 0x800, 0, 1, false);
    // set D: all valid
    uint32_t* dset = sets + 0xc00 / 4;
    vsharp(dset, 0x10100000ull, 0x10000);
    tsharp(dset + 4, 0x10200000ull);
    run("a valid set", 0xc00, 0, 0, false);
    // a value pointing into no buffer: nothing
    run("a push constant that is no pointer", 0x12345678, 0, 0, false);
    // (04.54) set E: two valid, and a 1024x1024 RGBA8 T# (4 MiB at least) in the 256 KiB image buffer
    uint32_t* eset = sets + 0xe00 / 4;
    vsharp(eset, 0x10100000ull, 0x100);
    tsharp(eset + 4, 0x10200000ull);
    tsharp(eset + 12, 0x10200000ull);
    eset[12 + 1] |= 10u << 20;                    // 8_8_8_8
    eset[12 + 2] = 1023u | (1023u << 14);         // 1024x1024
    eset[12 + 4] = 1023u << 13;                   // pitch 1024
    run("an image descriptor 4 MiB over its 256 KiB buffer (reported, not skipped)", 0xe00, 0, 1, false);
    // (04.48) render targets: the image buffer is 0x600000200000 + 256 KiB
    run_rt("a color target in a live buffer (32x32, 8 bytes a pixel)", 0x10200100ull, 15, 1, 0, 0, 0, false);
    run_rt("a color target in no buffer", 0x10300000ull, 15, 1, 0, 1, 0, true);
    run_rt("a color target running 1.75 MiB past its buffer", 0x10200000ull, 4095, 1, 0, 0, 1, true);
    run_rt("a wild color target with the CB disabled", 0x10300000ull, 15, 0, 0, 0, 0, false);
    run_rt("a color target outside the GPU windows (a VideoOut buffer)", 0x123400000ull, 15, 1, 0, 0, 0, false);
    run_rt("a depth target in no buffer", 0x10200100ull, 15, 1, 0x10900000ull, 2, 0, true);
    // (04.50) a page of the image buffer that the kernel doesn't map as the buffer's
    g_hole = (struct ps4_hole){0x1021c000ull, 0x10220000ull, 3};
    run_rt("a color target on a hole in its buffer", 0x1021c100ull, 15, 1, 0, 1, 0, true);
    run_rt("a color target in the same buffer, off the hole", 0x10200100ull, 15, 1, 0, 0, 0, false);
    run_rt("a color target running into the hole", 0x1021b000ull, 15, 1, 0, 1, 0, true);
    g_hole = (struct ps4_hole){0, 0, 0};
    // (04.52) the test skip by place: three draws, each after a trace id 100, 101, 102; skip 1-2
    {
        uint32_t ib[64];
        unsigned n = 0, draws[3];
        for (unsigned k = 0; k < 3; k++) {
            ib[n++] = 0xc0000000u | (3u << 16) | (0x37u << 8);  // WRITE_DATA: the trace id
            ib[n++] = 5u << 8;
            ib[n++] = 0x10000000u;
            ib[n++] = 0;
            ib[n++] = 100 + k;
            draws[k] = n;
            ib[n++] = 0xc0000000u | (1u << 16) | (0x2du << 8);  // DRAW_INDEX_AUTO
            ib[n++] = 3;
            ib[n++] = 2;
        }
        struct ps4_check c;
        memset(&c, 0, sizeof(c));
        c.dev = &g_dev;
        c.trace_va = 0x10000000u;
        c.first_trace = 100;
        c.skip_from = 1;
        c.skip_to = 2;
        printf("-- skip by place: the draw after trace id 101 only\n");
        ps4_check_ib(&c, ib, n, 10, 0, true);
        const bool ok = ((ib[draws[0]] >> 8) & 0xff) == 0x2d && ((ib[draws[1]] >> 8) & 0xff) == 0x10 &&
                        ((ib[draws[2]] >> 8) & 0xff) == 0x2d && c.skipped == 1;
        if (!ok) {
            printf("FAIL skip by place: %x %x %x, skipped %u\n", ib[draws[0]], ib[draws[1]], ib[draws[2]], c.skipped);
            g_failures++;
        }
    }
    // random data: never skipped, rarely described
    uint64_t rng = 88172645463325252ull;
    unsigned skips = 0, described = 0;
    for (int round = 0; round < 20000; round++) {
        uint32_t* r = sets + 0x1000 / 4;
        for (int i = 0; i < 256; i++) {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            r[i] = (uint32_t)rng;
        }
        uint32_t ib[16];
        const unsigned n = build_draw(ib, 0x1000);
        struct ps4_live_range ranges[16];
        struct ps4_check c;
        memset(&c, 0, sizeof(c));
        c.dev = &g_dev;
        c.ranges = ranges;
        for (unsigned h = 1; h < g_dev.num_bos; h++) {
            const uint64_t start = (uintptr_t)g_bos[h]->cpu;
            ranges[c.num_ranges++] = (struct ps4_live_range){start, start + g_bos[h]->size, g_bos[h]->size, h};
        }
        qsort(ranges, c.num_ranges, sizeof(ranges[0]), ps4_live_range_cmp);
        const int before = g_lines;
        ps4_check_ib(&c, ib, n, 8, 0, true);
        skips += c.skipped;
        described += g_lines != before;
    }
    printf("random data: %u of 20000 sets skipped, %u described\n", skips, described);
    if (skips) {
        printf("FAIL random data skipped\n");
        g_failures++;
    }
    printf("%s: %d failures\n", g_failures ? "FAILED" : "ok", g_failures);
    return g_failures != 0;
}
