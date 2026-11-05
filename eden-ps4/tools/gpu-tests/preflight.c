// (04.46) The pre-flight record (port/mesa/ac_ps4_drm.c, between its "pre-flight record: begin"
// and "end" lines), built on a PC by preflight.sh with the sanitizers:
//  1. two submissions: IBs with a nested IB and a chained one, shaders set in each; the record must
//     hold every IB (in the CP's order of reaching them), each shader once, and decode
//     (gpu-preflight.py) to the packet after the last trace id in a progress file;
//  2. a submission past the record's limit: cut, with the file near the limit;
//  3. random dwords: the walk must not read outside memory.
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct region {
    uint64_t va;
    uint32_t* data;
    size_t dwords;
};
static struct region g_regions[64];
static int g_num_regions;

static const void* ps4_hw_read(uint64_t va, uint64_t bytes);
void ps4_boot_trace(const char* stage);
#include "walker.inc"
#include "preflight.inc"

static const void* ps4_hw_read(uint64_t va, uint64_t bytes) {
    for (int i = 0; i < g_num_regions; i++) {
        const struct region* r = &g_regions[i];
        if (va >= r->va && va + bytes <= r->va + r->dwords * 4 && va % 4 == 0)
            return (const char*)r->data + (va - r->va);
    }
    return NULL;
}

// (04.57) the app's boot trace: lines counted, and those naming a shader built for the 0x96 window
static int g_boot_traces, g_other_window;
void ps4_boot_trace(const char* stage) {
    g_boot_traces++;
    g_other_window += strstr(stage, "built for a 32-bit GPU window at 0x9600000000, not this one (0x4400000000)") != NULL;
}

static uint32_t* add_region(uint64_t va, size_t dwords) {
    struct region* r = &g_regions[g_num_regions++];
    r->va = va;
    r->dwords = dwords;
    r->data = calloc(dwords, 4);
    return r->data;
}

static void reset_regions(void) {
    for (int i = 0; i < g_num_regions; i++)
        free(g_regions[i].data);
    g_num_regions = 0;
}

struct cs {
    uint32_t* buf;
    unsigned n;
};
static void emit(struct cs* c, uint32_t v) { c->buf[c->n++] = v; }
static uint32_t pkt3(unsigned op, unsigned count) { return 0xc0000000u | ((count - 1) << 16) | (op << 8); }
static void set_sh2(struct cs* c, unsigned reg, uint32_t a, uint32_t b) {
    emit(c, pkt3(0x76, 3));
    emit(c, reg);
    emit(c, a);
    emit(c, b);
}
static const uint64_t kTrace = 0x4400010000ull;
static void trace(struct cs* c, uint32_t id) {
    emit(c, pkt3(0x37, 4));
    emit(c, 5u << 8);
    emit(c, (uint32_t)kTrace);
    emit(c, (uint32_t)(kTrace >> 32));
    emit(c, id);
}
static void ib_call(struct cs* c, uint64_t va, unsigned dw, bool chain) {
    emit(c, pkt3(0x3f, 3));
    emit(c, (uint32_t)va);
    emit(c, (uint32_t)(va >> 32) & 0xffff);
    emit(c, dw | (chain ? 1u << 20 : 0));
}
static void shader_at(uint64_t va, unsigned dwords) {
    uint32_t* s = add_region(va, dwords + 256);
    for (unsigned i = 0; i < dwords; i++)
        s[i] = 0xbe800000u + i;
    s[dwords - 1] = 0xbf810000u;  // s_endpgm
}
static void shader_regs(struct cs* c, unsigned lo_reg, uint64_t va) {
    set_sh2(c, lo_reg, (uint32_t)(va >> 8), (uint32_t)(va >> 40));
}

static int count(const char* path, const char* what) {
    FILE* f = fopen(path, "r");
    char line[512];
    int n = 0;
    while (f && fgets(line, sizeof(line), f))
        n += strstr(line, what) != NULL;
    if (f)
        fclose(f);
    return n;
}

static int g_fail;
static void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAILED", what);
    g_fail |= !ok;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : ".";
    char path[512], progress[512];
    snprintf(path, sizeof(path), "%s/preflight.txt", dir);
    snprintf(progress, sizeof(progress), "%s/progress.txt", dir);
    uint64_t shaders[64];
    uint8_t stages[64];
    unsigned num_shaders = 0, ibs = 0, fresh = 0;

    // 1. two submissions
    {
        add_region(kTrace, 64);
        shader_at(0x4400200000ull, 40);  // PS
        shader_at(0x4400300000ull, 24);  // CS
        shader_at(0x4400400000ull, 32);  // VS
        // (04.57) the PS built for another 32-bit window (s_mov_b32 s6, s3; s_movk_i32 s3, 0x96;
        // s_load_dwordx8 s[8:15], s[2:3], 0x8), the CS for this one (0x44): only the PS is named
        static const uint32_t other_window[3] = {0xbe860303u, 0xb0030096u, 0xc0c40308u};
        static const uint32_t this_window[3] = {0xbe840303u, 0xb0030044u, 0xc0c40300u};
        memcpy((void*)ps4_hw_read(0x4400200000ull, 12), other_window, 12);
        memcpy((void*)ps4_hw_read(0x4400300000ull, 12), this_window, 12);
        struct cs a = {add_region(0x4400100000ull, 256), 0}, b = {add_region(0x4400110000ull, 256), 0},
                  c = {add_region(0x4400120000ull, 256), 0}, d = {add_region(0x4400130000ull, 256), 0};
        // B (called from A): the compute shader and a dispatch
        shader_regs(&b, 0x20c, 0x4400300000ull);
        emit(&b, pkt3(0x15, 4));
        emit(&b, 4), emit(&b, 2), emit(&b, 1), emit(&b, 1);
        trace(&b, 3);
        // C (A chains to it): a draw
        trace(&c, 4);
        emit(&c, pkt3(0x2d, 2));
        emit(&c, 3), emit(&c, 2);
        trace(&c, 5);
        // A
        shader_regs(&a, 0x008, 0x4400200000ull);
        trace(&a, 1);
        emit(&a, 0xffff1000u);  // PKT3_NOP_PAD
        ib_call(&a, 0x4400110000ull, b.n, false);
        trace(&a, 2);
        ib_call(&a, 0x4400120000ull, c.n, true);
        // D (submission 2): the vertex shader, two trace ids, then the dispatch the CP is "stuck" on
        shader_regs(&d, 0x048, 0x4400400000ull);
        set_sh2(&d, 0x00e, 0x10000, 0x3f800000);  // PS user data 2-3: a 32-bit pointer (the trace page), a float
        trace(&d, 6);
        trace(&d, 7);
        shader_regs(&d, 0x20c, 0x4400300000ull);
        emit(&d, pkt3(0x15, 4));
        emit(&d, 8), emit(&d, 8), emit(&d, 1), emit(&d, 1);
        trace(&d, 8);

        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        const char head[] = "the GPU was idle 1.9 s (idle period 1); ... Trace ids at 0x4400010000. ...\n";
        (void)!write(fd, head, sizeof(head) - 1);
        uint64_t bytes = sizeof(head) - 1;
        void* dcb1[1] = {(void*)(uintptr_t)0x4400100000ull};
        uint32_t sz1[1] = {a.n * 4};
        bytes += ps4_pf_submission(fd, 1, dcb1, sz1, 1, 1, 5, shaders, stages, &num_shaders, 64, bytes, &ibs, &fresh, 0x4400000000ull, 0x4400000000ull, true);
        check(ibs == 3 && fresh == 2 && num_shaders == 2, "submission 1: 3 IBs, 2 new shaders");
        void* dcb2[1] = {(void*)(uintptr_t)0x4400130000ull};
        uint32_t sz2[1] = {d.n * 4};
        bytes += ps4_pf_submission(fd, 2, dcb2, sz2, 1, 6, 8, shaders, stages, &num_shaders, 64, bytes, &ibs, &fresh, 0x4400000000ull, 0x4400000000ull, true);
        check(ibs == 1 && fresh == 1 && num_shaders == 3, "submission 2: 1 IB, 1 new shader (the CS one again isn't)");
        check(g_other_window == 1 && g_boot_traces == 1, "the PS built for the 0x96 window named, only it");
        const off_t size = lseek(fd, 0, SEEK_CUR);
        check((uint64_t)size == bytes, "bytes counted = the file's size");
        close(fd);
        check(count(path, "==== shader ") == 3, "3 shaders in the record");
        check(count(path, "m 4400010000:") == 1, "the memory a user data pointer points to");
        check(count(path, "== IB 0x4400110000, ") == 1 && count(path, "== IB 0x4400120000, ") == 1,
              "the nested and the chained IB in it");
        FILE* p = fopen(progress, "w");
        fprintf(p, "idle period 1: ...\n      0.0: 0 / 0 / 0 (of 2); trace 0; sync 0.0 ms\n"
                   "      1.2: 0 / 2 / 1 (of 2); trace 3; sync 0.2 ms\n"
                   "     12.0: 1 / 2 / 2 (of 2); trace 7; sync 0.3 ms\n");
        fclose(p);
        reset_regions();
    }

    // 2. past the limit
    {
        char big[512];
        snprintf(big, sizeof(big), "%s/preflight-big.txt", dir);
        const unsigned dw = 1u << 20;  // 4 MiB of dwords: ~12 MiB of text
        uint32_t* ib = add_region(0x4401000000ull, dw);
        for (unsigned i = 0; i < dw; i++)
            ib[i] = 0x80000000u;  // type-2 NOPs
        const int fd = open(big, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        void* dcb[2] = {(void*)(uintptr_t)0x4401000000ull, (void*)(uintptr_t)0x4401000000ull};
        uint32_t sz[2] = {dw * 4, dw * 4};
        unsigned n2 = 0;
        const uint64_t bytes = ps4_pf_submission(fd, 9, dcb, sz, 2, 0, 0, shaders, stages, &num_shaders, 64, 0, &ibs, &n2, 0, 0, false);
        close(fd);
        struct stat st;
        stat(big, &st);
        check(bytes == (uint64_t)st.st_size && st.st_size < (off_t)PS4_PF_LIMIT + 65536 &&
                  st.st_size >= (off_t)PS4_PF_LIMIT,
              "cut at the limit");
        check(count(big, "(the rest left out") == 1, "says so once");
        unlink(big);
        reset_regions();
    }

    // 3. random dwords, with IB pointers into the mapped regions
    const int rounds = argc > 2 ? atoi(argv[2]) : 300;
    srand(46);
    char junk[512];
    snprintf(junk, sizeof(junk), "%s/preflight-junk.txt", dir);
    for (int round = 0; round < rounds; round++) {
        const uint64_t base = 0x4402000000ull;
        uint32_t* m = add_region(base, 4096);
        for (unsigned i = 0; i < 4096; i++) {
            uint32_t v = (uint32_t)rand() << 1 ^ (uint32_t)rand();
            switch (rand() % 6) {
            case 0: v = pkt3(0x3f, 3); break;
            case 1: v = (uint32_t)(base + (rand() % 4096) * 4); break;
            case 2: v = (uint32_t)(base >> 32); break;
            case 3: v = pkt3(0x76, 1 + rand() % 6); break;
            default: break;
            }
            m[i] = v;
        }
        const int fd = open(junk, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        const unsigned at = rand() % 4096;
        void* dcb[1] = {(void*)(uintptr_t)(base + at * 4)};
        uint32_t sz[1] = {(4096 - at) * 4};
        unsigned n3 = 0, j = 0;
        ps4_pf_submission(fd, 10, dcb, sz, 1, 0, 0, shaders, stages, &num_shaders, 64, 0, &j, &n3, 0x4400000000ull, 0x4402000000ull, true);
        close(fd);
        reset_regions();
    }
    unlink(junk);
    printf("%s: %d rounds of random dwords\n", "ok", rounds);
    return g_fail;
}
