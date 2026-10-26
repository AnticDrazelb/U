// Switch-CPU benchmark kernels. The same source is built twice:
//  - for AArch64 (the "Switch" side), run through dynarmic as Eden would run game code;
//  - for the host (PS4 / x86-64), run natively for comparison.
// Every kernel is deterministic and returns a checksum, so the recompiled run can be checked
// against the native one bit for bit (built with -ffp-contract=off on both sides; the FMA kernel
// uses explicit fused multiply-adds, which both sides compute exactly).
//
// Kernels get their buffers as arguments: in the guest those are addresses in emulated memory,
// so their loads and stores go through dynarmic's page table like a game's would.

#include <stddef.h>
#include <stdint.h>

#ifndef BENCH_PREFIX
#define BENCH_PREFIX
#endif
#define BENCH_CAT2(a, b) a##b
#define BENCH_CAT(a, b) BENCH_CAT2(a, b)
#define NAME(x) BENCH_CAT(BENCH_PREFIX, x)

#if defined(__aarch64__)
// Freestanding guest: what the compiler may call for struct copies and zeroing.
void* memcpy(void* dst, const void* src, size_t n)
{
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--)
        *d++ = *s++;
    return dst;
}
void* memset(void* dst, int c, size_t n)
{
    uint8_t* d = (uint8_t*)dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}
#endif

// Fused multiply-add that never turns into a C library call (a freestanding guest has no libm):
// one FMADD on AArch64, an exact fma() natively.
#define FMA(a, b, c) __builtin_elementwise_fma((a), (b), (c))

static inline uint64_t mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

static inline uint64_t rotl(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

// Integer ALU: xorshift, multiplies, divides, rotates, compares.
uint64_t NAME(bench_int)(uint64_t n)
{
    uint64_t s0 = 0x9e3779b97f4a7c15ull, s1 = 0xd1b54a32d192ed03ull, acc = 0;
    for (uint64_t i = 0; i < n; i++)
    {
        const uint64_t r = s0 + s1;
        s1 ^= s0;
        s0 = rotl(s0, 55) ^ s1 ^ (s1 << 14);
        s1 = rotl(s1, 36);
        acc += r * (i | 1);
        acc ^= acc >> 17;
        if ((r & 31) == 0)
            acc += r / ((i & 0xffff) + 3); // some integer divides
        acc += (uint32_t)r < (uint32_t)acc ? 1 : 2;
    }
    return mix64(acc ^ s0 ^ s1);
}

// Game-logic-like control flow: a small "entity" array updated through a state machine.
typedef struct
{
    int32_t x, y, vx, vy;
    uint32_t state, timer, hp, flags;
} Entity;

uint64_t NAME(bench_branchy)(uint64_t n, Entity* ents, uint64_t count)
{
    uint64_t seed = 12345;
    for (uint64_t i = 0; i < count; i++)
    {
        seed = mix64(seed + i);
        ents[i].x = (int32_t)(seed & 1023);
        ents[i].y = (int32_t)((seed >> 10) & 1023);
        ents[i].vx = (int32_t)((seed >> 20) & 7) - 3;
        ents[i].vy = (int32_t)((seed >> 23) & 7) - 3;
        ents[i].state = (uint32_t)(seed >> 26) % 5;
        ents[i].timer = (uint32_t)(seed >> 29) & 63;
        ents[i].hp = 100;
        ents[i].flags = 0;
    }
    uint64_t acc = 0;
    for (uint64_t frame = 0; frame < n; frame++)
    {
        for (uint64_t i = 0; i < count; i++)
        {
            Entity* e = &ents[i];
            switch (e->state)
            {
            case 0: // idle
                if (--e->timer == 0 || e->timer > 1000)
                {
                    e->state = 1;
                    e->timer = 30;
                }
                break;
            case 1: // walk
                e->x += e->vx;
                e->y += e->vy;
                if (e->x < 0 || e->x > 1023)
                    e->vx = -e->vx;
                if (e->y < 0 || e->y > 1023)
                    e->vy = -e->vy;
                if (--e->timer == 0)
                    e->state = (e->hp < 50) ? 3 : 2;
                break;
            case 2: // attack
                e->flags |= 1u << (frame & 7);
                e->hp -= (e->hp > 3) ? 3 : 0;
                e->state = 0;
                e->timer = 15 + (uint32_t)(frame & 15);
                break;
            case 3: // flee
                e->x -= e->vx * 2;
                e->y -= e->vy * 2;
                e->hp += 1;
                if (e->hp >= 80)
                    e->state = 0;
                e->timer = 5;
                break;
            default: // stunned
                if ((frame + i) % 7 == 0)
                    e->state = 1;
                break;
            }
            acc += (uint64_t)(e->x ^ e->y) + e->state + e->hp;
        }
    }
    return mix64(acc);
}

// Calls: recursion and indirect calls through a table (return-stack and dispatch costs).
static uint64_t fib(uint32_t k)
{
    return k < 2 ? k : fib(k - 1) + fib(k - 2);
}
static uint64_t op_add(uint64_t a, uint64_t b) { return a + b; }
static uint64_t op_xor(uint64_t a, uint64_t b) { return a ^ (b << 1); }
static uint64_t op_mul(uint64_t a, uint64_t b) { return a * (b | 1); }
static uint64_t op_sub(uint64_t a, uint64_t b) { return a - (b >> 3); }
typedef uint64_t (*Op)(uint64_t, uint64_t);

uint64_t NAME(bench_calls)(uint64_t n)
{
    static Op const ops[4] = {op_add, op_xor, op_mul, op_sub};
    volatile Op const* table = ops; // keep the calls indirect
    uint64_t acc = 0;
    for (uint64_t i = 0; i < n; i++)
    {
        acc += fib(18 + (uint32_t)(i & 1));
        for (uint32_t j = 0; j < 64; j++)
            acc = table[(acc + j) & 3](acc, j + i);
    }
    return mix64(acc);
}

// Memory: a random walk (pointer chasing) over the buffer plus sequential read-modify-write.
uint64_t NAME(bench_memory)(uint64_t n, uint32_t* buf, uint64_t words)
{
    // a single-cycle permutation (Sattolo) so the walk visits every word
    for (uint64_t i = 0; i < words; i++)
        buf[i] = (uint32_t)i;
    uint64_t seed = 0x1234567;
    for (uint64_t i = words - 1; i > 0; i--)
    {
        seed = mix64(seed);
        const uint64_t j = seed % i;
        const uint32_t t = buf[i];
        buf[i] = buf[j];
        buf[j] = t;
    }
    uint64_t acc = 0;
    uint32_t p = 0;
    for (uint64_t round = 0; round < n; round++)
    {
        for (uint64_t k = 0; k < words / 4; k++)
        {
            p = buf[p];
            acc += p;
        }
        for (uint64_t k = 0; k < words; k += 16)
        {
            const uint32_t v = buf[k];
            acc += v;
        }
    }
    return mix64(acc ^ p);
}

// Block copies, 16 bytes at a time (asset / buffer copies).
typedef struct
{
    uint64_t lo, hi;
} Pair;

uint64_t NAME(bench_copy)(uint64_t n, Pair* buf, uint64_t pairs)
{
    const uint64_t half = pairs / 2;
    for (uint64_t i = 0; i < pairs; i++)
    {
        buf[i].lo = mix64(i);
        buf[i].hi = ~buf[i].lo;
    }
    uint64_t acc = 0;
    for (uint64_t round = 0; round < n; round++)
    {
        Pair* src = (round & 1) ? buf + half : buf;
        Pair* dst = (round & 1) ? buf : buf + half;
        for (uint64_t i = 0; i < half; i++)
        {
            Pair p = src[i];
            p.lo += round;
            dst[i] = p;
        }
        acc += dst[round % half].lo ^ dst[(round * 7) % half].hi;
    }
    return mix64(acc);
}

static inline uint32_t f2u(float f)
{
    union {
        float f;
        uint32_t u;
    } c = {f};
    return c.u;
}
static inline uint64_t d2u(double d)
{
    union {
        double d;
        uint64_t u;
    } c = {d};
    return c.u;
}

// Float arrays in loops the compiler vectorises (NEON in the guest, SSE natively).
uint64_t NAME(bench_float)(uint64_t n, float* a, float* b, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++)
    {
        a[i] = (float)(i % 97) * 0.25f + 1.0f;
        b[i] = (float)(i % 31) * 0.5f - 3.0f;
    }
    float total = 0.0f;
    for (uint64_t round = 0; round < n; round++)
    {
        for (uint64_t i = 0; i < len; i++)
            a[i] = a[i] * 0.999f + b[i] * 0.001f;
        for (uint64_t i = 0; i < len; i++)
            b[i] = b[i] * 0.995f - a[i] * 0.002f + 0.01f;
        float dot = 0.0f;
        for (uint64_t i = 0; i < len; i += 4)
            dot += a[i] * b[i];
        total += dot * 1e-6f;
    }
    return ((uint64_t)f2u(total) << 32) ^ f2u(a[len / 3]) ^ ((uint64_t)f2u(b[len / 5]) << 16);
}

// Fused multiply-adds (ARM's FMADD/FMLA, used all over game math). The PS4's CPU has no FMA
// instruction, so exact emulation is costly; Eden's "unsafe" CPU setting unfuses them.
uint64_t NAME(bench_fma)(uint64_t n)
{
    double px = 1.0, py = 2.0, pz = 3.0, vx = 0.01, vy = 0.02, vz = -0.01;
    float fx = 0.5f, fy = 0.25f;
    for (uint64_t i = 0; i < n; i++)
    {
        const double dt = 0.001;
        const double ax = FMA(-px, 0.5, vy * 0.1);
        const double ay = FMA(-py, 0.5, vz * 0.1);
        const double az = FMA(-pz, 0.5, vx * 0.1);
        vx = FMA(ax, dt, vx);
        vy = FMA(ay, dt, vy);
        vz = FMA(az, dt, vz);
        px = FMA(vx, dt, px);
        py = FMA(vy, dt, py);
        pz = FMA(vz, dt, pz);
        fx = FMA(fx, 0.9999f, fy * 0.0001f);
        fy = FMA(fy, 0.9998f, 0.00002f);
    }
    return d2u(px) ^ rotl(d2u(py), 21) ^ rotl(d2u(pz), 42) ^ f2u(fx) ^ ((uint64_t)f2u(fy) << 32);
}

#if defined(__aarch64__)
// The harness finds the kernels through this table at the start of the image.
__attribute__((section(".bench_table"), used)) const uint64_t bench_table[] = {
    (uint64_t)NAME(bench_int),   (uint64_t)NAME(bench_branchy), (uint64_t)NAME(bench_calls),
    (uint64_t)NAME(bench_memory), (uint64_t)NAME(bench_copy),   (uint64_t)NAME(bench_float),
    (uint64_t)NAME(bench_fma),
};
#endif
