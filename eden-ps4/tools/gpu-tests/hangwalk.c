// (04.39) The GPU hang report's walker (port/mesa/ac_ps4_drm.c, between its "hang walker: begin"
// and "end" lines), built on a PC by hangwalk.sh with the sanitizers:
//  1. hand-made command streams with RADV trace ids: a dispatch whose shader never ends (the CP
//     waits at the CS_PARTIAL_FLUSH after it), a WAIT_REG_MEM on memory nothing writes, a packet
//     the CP is stuck on with no wait before the next trace id, the CP in the second of two
//     unfinished submissions, and no trace id passed at all: the analysis must name the right
//     packet and suspects;
//  2. a real hang's command buffers (Eden 04.38, Zelda: Echoes of Wisdom, submission 30753),
//     rebuilt from the dwords in the report's decode (the uploaded log as the argument);
//  3. random dwords: the walk must not read outside memory.
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))

// GPU memory: regions at their GPU addresses
struct region {
    uint64_t va;
    uint32_t* data;
    size_t dwords;
};
static struct region g_regions[64];
static int g_num_regions;

static const void* ps4_hw_read(uint64_t va, uint64_t bytes);
#include "walker.inc"

static const void* ps4_hw_read(uint64_t va, uint64_t bytes) {
    for (int i = 0; i < g_num_regions; i++) {
        const struct region* r = &g_regions[i];
        if (va >= r->va && va + bytes <= r->va + r->dwords * 4 && va % 4 == 0)
            return (const char*)r->data + (va - r->va);
    }
    return NULL;
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

static void poke(uint64_t va, uint32_t value) {
    uint32_t* p = (uint32_t*)ps4_hw_read(va, 4);
    if (!p) {
        fprintf(stderr, "poke %#llx: not mapped\n", (unsigned long long)va);
        exit(1);
    }
    *p = value;
}

// ---- a command stream builder (PKT3 packets as RADV emits them on GFX7)
struct cs {
    uint32_t* buf;
    unsigned n;
};
static void emit(struct cs* c, uint32_t v) { c->buf[c->n++] = v; }
static uint32_t pkt3(unsigned op, unsigned count) { return 0xc0000000u | ((count - 1) << 16) | (op << 8); }
static void set_sh(struct cs* c, unsigned reg, uint32_t v) {
    emit(c, pkt3(0x76, 2));
    emit(c, reg);
    emit(c, v);
}
static const uint64_t kTrace = 0x4400010000ull, kSem = 0x4400010100ull, kIdx = 0x4400020000ull;
static void trace(struct cs* c, uint32_t id) {  // radv_cmd_buffer_trace_emit
    emit(c, pkt3(0x37, 4));
    emit(c, 5u << 8 | 1u << 20);
    emit(c, (uint32_t)kTrace);
    emit(c, (uint32_t)(kTrace >> 32));
    emit(c, id);
    emit(c, pkt3(0x10, 1));
    emit(c, 0xcafe0000u | id);
}
static void dispatch(struct cs* c, uint64_t shader, unsigned x) {
    set_sh(c, 0x20c, (uint32_t)(shader >> 8));
    emit(c, pkt3(0x76, 4));
    emit(c, 0x207);
    emit(c, 64);
    emit(c, 1);
    emit(c, 1);
    emit(c, pkt3(0x15, 4));
    emit(c, x);
    emit(c, 1);
    emit(c, 1);
    emit(c, 0x45);
}
static void draw(struct cs* c, uint64_t vs, uint64_t ps, unsigned count) {
    set_sh(c, 0x48, (uint32_t)(vs >> 8));
    set_sh(c, 0x08, (uint32_t)(ps >> 8));
    emit(c, pkt3(0x2f, 1));
    emit(c, 2);
    emit(c, pkt3(0x27, 5));
    emit(c, 0x1000);
    emit(c, (uint32_t)kIdx);
    emit(c, (uint32_t)(kIdx >> 32));
    emit(c, count);
    emit(c, 0);
}
static void event(struct cs* c, unsigned type, unsigned index) {
    emit(c, pkt3(0x46, 1));
    emit(c, type | index << 8);
}
static void surface_sync(struct cs* c) {
    emit(c, pkt3(0x43, 4));
    emit(c, 0x06c07fc0);
    emit(c, 0xffffffff);
    emit(c, 0);
    emit(c, 10);
}
static void wait_mem(struct cs* c, uint64_t va, uint32_t ref) {
    emit(c, pkt3(0x3c, 6));
    emit(c, 3 | 1u << 4);  // ==, memory
    emit(c, (uint32_t)va);
    emit(c, (uint32_t)(va >> 32));
    emit(c, ref);
    emit(c, 0xffffffff);
    emit(c, 4);
}
static void ib(struct cs* c, uint64_t va, unsigned dw, bool chain) {
    emit(c, pkt3(0x3f, 3));
    emit(c, (uint32_t)va);
    emit(c, (uint32_t)(va >> 32));
    emit(c, dw | (chain ? 1u << 20 : 0) | 1u << 23);
}

static int g_failures;

static char* run_analysis(struct ps4_walk* w, const uint64_t* seqs, uint32_t trace_now, bool have_trace,
                          unsigned* shaders) {
    char* text = NULL;
    size_t size = 0;
    FILE* f = open_memstream(&text, &size);
    char path[] = "/tmp/hangwalk-XXXXXX";
    const int fd = mkstemp(path);
    ps4_walk_analyze(f, w, seqs, have_trace ? kTrace : 0, trace_now, fd);
    fclose(f);
    *shaders = 0;
    const off_t end = lseek(fd, 0, SEEK_END);
    char* dump = calloc(1, (size_t)end + 1);
    if (pread(fd, dump, (size_t)end, 0) == end)
        for (const char* at = dump; (at = strstr(at, "==== shader ")); at++)
            (*shaders)++;
    if (*shaders && !strstr(dump, "bf810000")) {
        printf("FAIL: a shader dump without its s_endpgm\n%s\n", dump);
        g_failures++;
    }
    free(dump);
    close(fd);
    unlink(path);
    return text;
}

static void expect(const char* text, const char* what, const char* name) {
    if (!strstr(text, what)) {
        printf("FAIL %s: no \"%s\" in:\n%s\n", name, what, text);
        g_failures++;
    }
}

static const uint64_t kShaderA = 0x44ffa21300ull, kShaderB = 0x44ffb21400ull, kVs = 0x44ff95a000ull,
                      kPs = 0x44fffef800ull;

static void map_shaders(void) {
    const uint64_t shaders[] = {kShaderA, kShaderB, kVs, kPs};
    for (int i = 0; i < 4; i++) {
        uint32_t* s = add_region(shaders[i], 512);
        for (int k = 0; k < 40; k++)
            s[k] = 0x7e000200u + k;  // v_mov_b32 ...
        s[40] = 0xbf810000;          // s_endpgm
    }
}

static void test_cases(void) {
    // case A: dispatch with trace 11 passed, then CS_PARTIAL_FLUSH: the CP waits there, the dispatch
    // is the suspect
    {
        map_shaders();
        add_region(kTrace, 64);
        add_region(kSem, 64);
        add_region(kIdx, 1024);
        uint32_t* pre = add_region(0x4401000000ull, 64);
        uint32_t* nested = add_region(0x4401001000ull, 64);
        uint32_t* main1 = add_region(0x4401002000ull, 4096);
        uint32_t* main2 = add_region(0x4401010000ull, 4096);
        struct cs n = {nested, 0}, p = {pre, 0}, a = {main1, 0}, b = {main2, 0};
        emit(&n, pkt3(0x28, 2));  // CONTEXT_CONTROL
        emit(&n, 0x80000000);
        emit(&n, 0x80000000);
        ib(&p, 0x4401001000ull, n.n, false);
        trace(&a, 10);
        draw(&a, kVs, kPs, 72);
        trace(&a, 11);
        event(&a, 0x10, 4);  // PS_PARTIAL_FLUSH
        surface_sync(&a);
        trace(&a, 12);
        ib(&a, 0x4401010000ull, 0, true);  // patched below
        dispatch(&b, kShaderA, 192);
        trace(&b, 13);
        event(&b, 0x07, 4);  // CS_PARTIAL_FLUSH
        trace(&b, 14);
        dispatch(&b, kShaderB, 4);
        trace(&b, 15);
        a.buf[a.n - 1] = b.n | 1u << 20 | 1u << 23;
        struct ps4_pkt pkts[512];
        struct ps4_walk w = {0};
        w.pkts = pkts;
        w.max = 512;
        poke(kTrace, 13);
        ps4_walk_collect(&w, 0x4401000000ull, p.n, 0);
        ps4_walk_collect(&w, 0x4401002000ull, a.n, 0);
        const uint64_t seqs[] = {30753};
        unsigned shaders;
        char* t = run_analysis(&w, seqs, 13, true, &shaders);
        expect(t, "passed trace id 13", "A");
        expect(t, "it waits at", "A");
        expect(t, "CS_PARTIAL_FLUSH (waits)", "A");
        expect(t, "1 draws and dispatches", "A");
        expect(t, "DISPATCH_DIRECT 192x1x1 groups of 64x1x1, CS 0x44ffa21300", "A");
        expect(t, "> 0x4401010034: trace id 13 (in memory now)", "A");
        if (shaders != 1) {
            printf("FAIL A: %u shaders dumped, 1 expected\n%s\n", shaders, t);
            g_failures++;
        }
        printf("---- case A ----\n%s", t);
        free(t);

        // case B: no wait between trace 11 and 12 replaced: trace 14 passed, the next trace (15) not, with only
        // a dispatch between: stuck on a packet between
        w.count = 0;
        w.dropped = 0;
        poke(kTrace, 14);
        ps4_walk_collect(&w, 0x4401002000ull, a.n, 0);
        t = run_analysis(&w, seqs, 14, true, &shaders);
        expect(t, "there is no wait between", "B");
        expect(t, "DISPATCH_DIRECT 4x1x1", "B");
        free(t);

        // case C: no trace id passed (memory holds a number from an earlier submission)
        t = run_analysis(&w, seqs, 7, true, &shaders);
        expect(t, "in none of the unfinished submissions", "C");
        free(t);

        // case D: without trace ids
        t = run_analysis(&w, seqs, 0, false, &shaders);
        expect(t, "no trace ids", "D");
        free(t);
        reset_regions();
    }
    // case E: a WAIT_REG_MEM on memory nothing writes, in the second of two submissions
    {
        map_shaders();
        add_region(kTrace, 64);
        add_region(kSem, 64);
        add_region(kIdx, 1024);
        uint32_t* s1 = add_region(0x4402000000ull, 1024);
        uint32_t* s2 = add_region(0x4402010000ull, 1024);
        struct cs a = {s1, 0}, b = {s2, 0};
        trace(&a, 100);
        draw(&a, kVs, kPs, 36);
        trace(&a, 101);
        event(&a, 0x15, 1);  // ZPASS_DONE without an address: as EVENT_WRITE with one body dword
        trace(&b, 102);
        dispatch(&b, kShaderB, 8);
        trace(&b, 103);
        wait_mem(&b, kSem, 1);
        trace(&b, 104);
        poke(kSem, 0);
        poke(kTrace, 103);
        struct ps4_pkt pkts[512];
        struct ps4_walk w = {0};
        w.pkts = pkts;
        w.max = 512;
        w.sub = 0;
        ps4_walk_collect(&w, 0x4402000000ull, a.n, 0);
        w.sub = 1;
        ps4_walk_collect(&w, 0x4402010000ull, b.n, 0);
        const uint64_t seqs[] = {500, 501};
        unsigned shaders;
        char* t = run_analysis(&w, seqs, 103, true, &shaders);
        expect(t, "in submission 501", "E");
        expect(t, "NOT SATISFIED", "E");
        expect(t, "a wait on memory that isn't satisfied", "E");
        expect(t, "submission 500:", "E");
        printf("---- case E ----\n%s", t);
        free(t);
        // satisfied now: then it's just the end of the stream
        poke(kSem, 1);
        poke(kTrace, 104);
        w.count = 0;
        w.sub = 0;
        ps4_walk_collect(&w, 0x4402000000ull, a.n, 0);
        w.sub = 1;
        ps4_walk_collect(&w, 0x4402010000ull, b.n, 0);
        t = run_analysis(&w, seqs, 104, true, &shaders);
        expect(t, "the CP got to the end", "E2");
        free(t);
        reset_regions();
    }
    // case F: a chain into memory that isn't mapped, and a stream that isn't packets
    {
        uint32_t* s = add_region(0x4403000000ull, 64);
        struct cs a = {s, 0};
        emit(&a, pkt3(0x42, 1));
        emit(&a, 0);
        ib(&a, 0x4403100000ull, 100, false);
        emit(&a, 0x12345678);  // type 0: not a packet
        struct ps4_pkt pkts[64];
        struct ps4_walk w = {0};
        w.pkts = pkts;
        w.max = 64;
        ps4_walk_collect(&w, 0x4403000000ull, a.n, 0);
        const uint64_t seqs[] = {1};
        unsigned shaders;
        char* t = run_analysis(&w, seqs, 0, true, &shaders);
        expect(t, "isn't mapped", "F");
        expect(t, "not a packet", "F");
        free(t);
        reset_regions();
    }
}

// case G (04.42): SET_UCONFIG_REG of the GS/tessellation ring registers recognised, and the ones
// every draw sets beside them (VGT_PRIMITIVE_TYPE, VGT_NUM_INSTANCES) left alone; a draw's stages
static void test_rings(void) {
    uint32_t* s = add_region(0x4405000000ull, 256);
    struct cs a = {s, 0};
    emit(&a, pkt3(0x79, 2));  // VGT_PRIMITIVE_TYPE alone: not a ring register
    emit(&a, 0x242);
    emit(&a, 4);
    emit(&a, pkt3(0x79, 3));  // VGT_ESGS_RING_SIZE, VGT_GSVS_RING_SIZE
    emit(&a, 0x240);
    emit(&a, 0x8000);
    emit(&a, 0x8000);
    emit(&a, pkt3(0x79, 4));  // VGT_TF_RING_SIZE, VGT_HS_OFFCHIP_PARAM, VGT_TF_MEMORY_BASE
    emit(&a, 0x24e);
    emit(&a, 0x2000);
    emit(&a, 0x1ff);
    emit(&a, 0x44001234);
    emit(&a, pkt3(0x79, 3));  // VGT_NUM_INSTANCES (0x24d) and VGT_TF_RING_SIZE: mixed
    emit(&a, 0x24d);
    emit(&a, 1);
    emit(&a, 0x2000);
    emit(&a, pkt3(0x69, 2));  // VGT_SHADER_STAGES_EN: ES, GS, VS
    emit(&a, 0x2d5);
    emit(&a, 0x2 << 3 | 1 << 5 | 1 << 6);
    emit(&a, pkt3(0x2d, 2));  // DRAW_INDEX_AUTO
    emit(&a, 3);
    emit(&a, 2);
    struct ps4_pkt pkts[16];
    struct ps4_walk w = {0};
    w.pkts = pkts;
    w.max = 16;
    w.light = true;
    ps4_walk_collect(&w, 0x4405000000ull, a.n, 0);
    unsigned rings = 0, all = 0, gs_draws = 0;
    for (unsigned i = 0; i < w.count; i++) {
        rings += w.pkts[i].op == 0x79;
        all += w.pkts[i].op == 0x79 && w.pkts[i].v[7];
        gs_draws += ps4_pkt_is_work(&w.pkts[i]) && (w.pkts[i].stages & 0x20);
    }
    if (rings != 3 || all != 2 || gs_draws != 1) {
        printf("FAIL G: %u ring packets (3), %u only ring registers (2), %u GS draws (1)\n", rings, all, gs_draws);
        g_failures++;
    }
    reset_regions();
}

// ---- 2: the uploaded log's hang report, submission 30753
static void test_report(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        printf("(no report file %s: part 2 skipped)\n", path);
        return;
    }
    // the decode's dwords: "xxxxxxxx" at the start of a line (4 more spaces in a nested IB)
    static uint32_t top[3][40000];
    static uint32_t nested[4096];
    unsigned ntop[3] = {0}, nnested = 0;
    int ib = -1;
    bool in_nested = false, in_report = false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "===== gpu/gpu-hang.txt"))
            in_report = true;
        else if (in_report && !strncmp(line, "===== ", 6))
            break;
        if (!in_report)
            continue;
        char* begin = strstr(line, "submission 30753 IB ");
        if (begin && strstr(line, " begin - GFX")) {
            ib = begin[20] - '0';
            continue;
        }
        if (begin && strstr(line, " end - GFX")) {
            ib = -1;
            continue;
        }
        if (strstr(line, "nested begin")) {
            in_nested = true;
            continue;
        }
        if (strstr(line, "nested end")) {
            in_nested = false;
            continue;
        }
        if (ib < 0 || ib > 2)
            continue;
        // strip colour codes
        char clean[1024];
        size_t k = 0;
        for (size_t i = 0; line[i] && k + 1 < sizeof(clean); i++) {
            if (line[i] == 0x1b) {
                while (line[i] && line[i] != 'm')
                    i++;
                continue;
            }
            clean[k++] = line[i];
        }
        clean[k] = 0;
        const char* s = clean + (in_nested ? 4 : 0);
        if (in_nested && strncmp(clean, "    ", 4))
            continue;
        bool hex = true;
        for (int i = 0; i < 8; i++)
            hex &= (s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f');
        if (!hex || (s[8] != ' ' && s[8] != '\n' && s[8] != 0))
            continue;
        const uint32_t v = (uint32_t)strtoul(s, NULL, 16);
        if (in_nested) {
            if (nnested < 4096)
                nested[nnested++] = v;
        } else if (ntop[ib] < 40000) {
            top[ib][ntop[ib]++] = v;
        }
    }
    fclose(f);
    printf("report: IB 0 %u dwords (+%u nested), IB 1 %u (352 + chained), IB 2 %u\n", ntop[0], nnested, ntop[1], ntop[2]);
    if (ntop[0] != 48 || nnested != 112 || ntop[2] != 16 || ntop[1] != 352 + 29552) {
        printf("FAIL report: unexpected sizes\n");
        g_failures++;
        return;
    }
    memcpy(add_region(0x43fb278000ull, 48), top[0], 48 * 4);
    memcpy(add_region(0x43fff70000ull, 112), nested, 112 * 4);
    memcpy(add_region(0x43f9190000ull, 352), top[1], 352 * 4);
    memcpy(add_region(0x43dec40000ull, 29552), top[1] + 352, 29552 * 4);
    memcpy(add_region(0x43fffc8000ull, 16), top[2], 16 * 4);
    add_region(0x43fdb4c000ull, 16)[0] = 1;  // Eden's WaitForIdle event: set
    struct ps4_walk w = {0};
    w.max = 16384;
    w.pkts = calloc(w.max, sizeof(*w.pkts));
    const uint64_t vas[3] = {0x43fb278000ull, 0x43f9190000ull, 0x43fffc8000ull};
    const unsigned dws[3] = {48, 352, 16};
    for (int i = 0; i < 3; i++)
        ps4_walk_collect(&w, vas[i], dws[i], 0);
    unsigned draws = 0, dispatches = 0, waits = 0, unsatisfied = 0, indirect = 0;
    for (unsigned i = 0; i < w.count; i++) {
        const struct ps4_pkt* p = &w.pkts[i];
        draws += p->op == 0x27 || p->op == 0x2d || p->op == 0x38;
        dispatches += p->op == 0x15 || p->op == 0x16;
        indirect += p->op == 0x38;
        waits += p->op == 0x3c;
        unsatisfied += p->op == 0x3c && p->done == 0;
    }
    printf("report: %u packets kept: %u draws (%u indirect), %u dispatches, %u WAIT_REG_MEM (%u not satisfied)\n",
           w.count, draws, indirect, dispatches, waits, unsatisfied);
    // the decode has 134 draws + 1 indirect, 199 dispatches, 11 WAIT_REG_MEM (all on the event, set)
    if (draws != 135 || dispatches != 199 || waits != 11 || unsatisfied != 0 || indirect != 1) {
        printf("FAIL report: counts\n");
        g_failures++;
    }
    const uint64_t seqs[] = {30753};
    unsigned shaders;
    char* t = run_analysis(&w, seqs, 0, false, &shaders);
    printf("---- the report, without trace ids ----\n%s", t);
    free(t);
    // pretend the CP passed the third-to-last dispatch's position: a fake trace address at a
    // WRITE_DATA isn't there, so: as if the trace id were in none
    t = run_analysis(&w, seqs, 5, true, &shaders);
    expect(t, "in none of the unfinished submissions", "report");
    free(t);
    // print every kept packet once (the printer on every kind)
    char* all = NULL;
    size_t size = 0;
    FILE* m = open_memstream(&all, &size);
    for (unsigned i = 0; i < w.count; i++)
        ps4_pkt_print(m, &w.pkts[i], kTrace, "  ");
    fclose(m);
    expect(all, "DRAW_INDEX_INDIRECT_MULTI (arguments at 0x43fdd50e00, 1 draws, stride 0) arguments not readable",
           "report");
    expect(all, "WAIT_REG_MEM memory 0x43fdb4c000 & 0xffffffff == 0x1: now 0x1, satisfied", "report");
    printf("report: %zu bytes of packet lines, e.g.\n", size);
    char* cut = all;
    for (int i = 0; i < 6 && cut; i++)
        cut = strchr(cut + 1, '\n');
    if (cut)
        *cut = 0;
    printf("%s\n", all);
    free(all);
    free(w.pkts);
    reset_regions();
}

// ---- 3: garbage
static uint64_t g_rng = 88172645463325252ull;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)g_rng;
}

static void test_garbage(unsigned rounds) {
    for (unsigned round = 0; round < rounds; round++) {
        const uint64_t base = 0x4404000000ull;
        uint32_t* s = add_region(base, 2048);
        uint32_t* t = add_region(base + 0x10000, 2048);
        add_region(kTrace, 64);
        for (int i = 0; i < 2048; i++) {
            // packet-like dwords, addresses into the two regions, small values
            const uint32_t r = rnd();
            switch (r % 6) {
            case 0: s[i] = pkt3(rnd() % 0x100, 1 + rnd() % 12); break;
            case 1: s[i] = (uint32_t)(base + (rnd() % 0x20000)); break;
            case 2: s[i] = 0x44; break;
            case 3: s[i] = rnd() % 64; break;
            case 4: s[i] = 0x80000000u | (rnd() & 0xfffff); break;
            default: s[i] = rnd(); break;
            }
            t[i] = rnd() & 1 ? pkt3(0x3f, 3) : rnd();
        }
        struct ps4_pkt* pkts = calloc(4096, sizeof(*pkts));
        struct ps4_walk w = {0};
        w.pkts = pkts;
        w.max = 1 + rnd() % 4096;
        ps4_walk_collect(&w, base, 1 + rnd() % 2048, 0);
        ps4_walk_collect(&w, base + 0x10000, 2048, 0);
        const uint64_t seqs[] = {1};
        unsigned shaders;
        uint32_t trace_now = rnd() % 64;
        for (unsigned i = 0; i < w.count && rnd() % 4; i++)
            if (w.pkts[i].op == 0x37)
                trace_now = w.pkts[i].v[0];
        char* text = run_analysis(&w, seqs, trace_now, rnd() & 1, &shaders);
        free(text);
        for (unsigned i = 0; i < w.count; i++) {
            char* line = NULL;
            size_t size = 0;
            FILE* m = open_memstream(&line, &size);
            ps4_pkt_print(m, &w.pkts[i], kTrace, "");
            fclose(m);
            free(line);
        }
        free(pkts);
        reset_regions();
    }
}

int main(int argc, char** argv) {
    test_cases();
    test_rings();
    test_report(argc > 1 ? argv[1] : "");
    const unsigned rounds = argc > 2 ? (unsigned)atoi(argv[2]) : 2000;
    test_garbage(rounds);
    printf("%s: %d failures (%u garbage rounds)\n", g_failures ? "FAILED" : "ok", g_failures, rounds);
    return g_failures != 0;
}
