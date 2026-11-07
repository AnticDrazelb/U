// Xbox CPU Bench kernels. The same source is built twice:
//  - as 32-bit x86 for a Pentium III (the original Xbox's CPU: 733 MHz Coppermine, MMX and SSE,
//    no SSE2), linked at 0x00010000 where an XBE's image goes. It runs on the host CPU itself,
//    in 32-bit compatibility mode, the way Cxbx-Reloaded runs Xbox code on Windows: no
//    recompiler at all.
//  - as x86-64 for the host (BENCH_PREFIX=native_), as the reference.
// Every kernel is deterministic and returns a 32-bit checksum, so the two can be compared bit for
// bit. Floats use SSE on both sides (scalar and 4-wide, as Xbox games built for the Pentium III's
// SSE do); doubles use the x87 FPU in the guest (no SSE2 on the Xbox) with its precision set to
// 53 bits, as the Xbox's runtime leaves it, which rounds every operation as SSE2 does natively.
//
// Kernels get their buffers as arguments: in the guest those are addresses below 4 GiB in the
// emulated Xbox memory.

#include <stddef.h>
#include <stdint.h>

#ifndef BENCH_PREFIX
#define BENCH_PREFIX
#endif
#define BENCH_CAT2(a, b) a##b
#define BENCH_CAT(a, b) BENCH_CAT2(a, b)
#define NAME(x) BENCH_CAT(BENCH_PREFIX, x)

#if defined(__i386__)
#define GUEST 1
// Freestanding guest: what the compiler may call for struct copies and zeroing. "rep movsd", as
// the Xbox's own runtime copies.
void* memcpy(void* dst, const void* src, size_t n)
{
    void* d = dst;
    size_t words = n >> 2, rest = n & 3;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(src), "+c"(words) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(rest) : : "memory");
    return dst;
}
void* memset(void* dst, int c, size_t n)
{
    void* d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}
#else
#define GUEST 0
#endif

static inline uint32_t rotl32(uint32_t x, int r)
{
    return (x << r) | (x >> (32 - r));
}

static inline uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Integer ALU: xorshift, multiplies, divides, rotates, compares.
uint32_t NAME(xb_int)(uint32_t n)
{
    uint32_t a = 0x12345678u, b = 0x9abcdef1u, acc = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        a ^= a << 13;
        a ^= a >> 17;
        a ^= a << 5;
        b = b * 0x01000193u + i;
        uint32_t d = (b | 1u);
        acc += (a / d) + (a % d);
        acc ^= rotl32(a + b, (int)(i & 31) | 1);
        acc += (a > b) ? a - b : b - a;
    }
    return acc;
}

// Game logic: an entity state machine over an array (AI, physics flags, timers).
struct Entity
{
    int32_t x, y, vx, vy;
    uint32_t state, timer, hp, flags;
};

uint32_t NAME(xb_branchy)(uint32_t n, struct Entity* ents, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t r = mix32(i + 1);
        ents[i].x = (int32_t)(r & 0xFFF);
        ents[i].y = (int32_t)((r >> 12) & 0xFFF);
        ents[i].vx = (int32_t)((r >> 3) & 7) - 3;
        ents[i].vy = (int32_t)((r >> 7) & 7) - 3;
        ents[i].state = r & 3;
        ents[i].timer = (r >> 20) & 63;
        ents[i].hp = 100;
        ents[i].flags = 0;
    }
    uint32_t sum = 0;
    for (uint32_t step = 0; step < n; step++)
        for (uint32_t i = 0; i < count; i++)
        {
            struct Entity* e = &ents[i];
            switch (e->state)
            {
            case 0: // idle
                if (e->timer-- == 0)
                    e->state = 1, e->timer = 30 + (i & 15);
                break;
            case 1: // walk
                e->x += e->vx, e->y += e->vy;
                if (e->x < 0 || e->x > 4095)
                    e->vx = -e->vx, e->flags |= 1;
                if (e->y < 0 || e->y > 4095)
                    e->vy = -e->vy, e->flags |= 2;
                if (e->timer-- == 0)
                    e->state = (e->hp < 50) ? 3 : 2, e->timer = 10;
                break;
            case 2: // attack
                if ((e->x ^ e->y ^ (int32_t)step) & 4)
                    e->hp -= (e->hp > 5) ? 5 : 0;
                if (e->timer-- == 0)
                    e->state = 0, e->timer = 20;
                break;
            default: // flee and heal
                e->x -= e->vx, e->y -= e->vy;
                e->hp += (e->hp < 100) ? 3 : 0;
                if (e->timer-- == 0)
                    e->state = 1, e->timer = 40;
                break;
            }
            sum += (uint32_t)e->x * 3u + (uint32_t)e->y + e->hp + e->state;
        }
    return sum;
}

// Calls: recursion and indirect calls through a table (virtual functions, callbacks).
typedef uint32_t (*Op)(uint32_t, uint32_t);
__attribute__((noinline)) static uint32_t op_add(uint32_t a, uint32_t b) { return a + b; }
__attribute__((noinline)) static uint32_t op_xor(uint32_t a, uint32_t b) { return a ^ (b * 3u); }
__attribute__((noinline)) static uint32_t op_mul(uint32_t a, uint32_t b) { return a * (b | 1u); }
__attribute__((noinline)) static uint32_t op_rot(uint32_t a, uint32_t b) { return rotl32(a, (int)(b & 31) | 1) + b; }
static Op const ops[4] = {op_add, op_xor, op_mul, op_rot};

__attribute__((noinline)) static uint32_t fib(uint32_t n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

uint32_t NAME(xb_calls)(uint32_t n)
{
    Op const* volatile table = ops; // an indirect call, not a switch
    uint32_t acc = 1;
    for (uint32_t i = 0; i < n; i++)
    {
        acc += fib(18 + (i & 1));
        for (uint32_t j = 0; j < 256; j++)
            acc = table[(acc ^ j) & 3](acc, j);
    }
    return acc;
}

// Memory: pointer chasing over a buffer (cache misses) and a sequential scan.
uint32_t NAME(xb_memory)(uint32_t n, uint32_t* buf, uint32_t words)
{
    // a random cycle through all the words (Sattolo's shuffle)
    for (uint32_t i = 0; i < words; i++)
        buf[i] = i;
    uint32_t seed = 0x2545F491u;
    for (uint32_t i = words - 1; i > 0; i--)
    {
        seed = mix32(seed + i);
        uint32_t j = seed % i;
        uint32_t t = buf[i];
        buf[i] = buf[j];
        buf[j] = t;
    }
    uint32_t sum = 0, p = 0;
    for (uint32_t round = 0; round < n; round++)
    {
        for (uint32_t k = 0; k < words / 4; k++)
            p = buf[p], sum += p;
        for (uint32_t k = 0; k < words; k++)
            sum += buf[k] ^ k;
    }
    return sum;
}

// Copies: 64-byte blocks (vertex data into a push buffer, sprite lists).
struct Block
{
    uint32_t w[16];
};

uint32_t NAME(xb_copy)(uint32_t n, struct Block* buf, uint32_t blocks)
{
    struct Block* src = buf;
    struct Block* dst = buf + blocks / 2;
    const uint32_t half = blocks / 2;
    for (uint32_t i = 0; i < half; i++)
        for (int k = 0; k < 16; k++)
            src[i].w[k] = mix32(i * 16 + (uint32_t)k);
    uint32_t sum = 0;
    for (uint32_t round = 0; round < n; round++)
    {
        for (uint32_t i = 0; i < half; i++)
        {
            uint32_t from = (i * 7 + round) % half;
            dst[i] = src[from];
        }
        sum += dst[round % half].w[round & 15];
        struct Block* t = src;
        src = dst;
        dst = t;
    }
    return sum;
}

// Vector floats: 4-wide transforms and blends (a vertex shader done on the CPU, skinning),
// SSE on both sides.
typedef float v4 __attribute__((vector_size(16)));

uint32_t NAME(xb_sse)(uint32_t n, float* a, float* b, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        a[i] = (float)(mix32(i) & 0xFFFF) / 65536.0f;
        b[i] = (float)(mix32(i + 99991u) & 0xFFFF) / 65536.0f;
    }
    v4 m0 = {0.9f, 0.1f, 0.0f, 0.0f}, m1 = {-0.1f, 0.9f, 0.05f, 0.0f}, m2 = {0.0f, -0.05f, 1.0f, 0.0f},
       m3 = {0.01f, 0.02f, 0.03f, 1.0f};
    const v4 k = {0.5f, 0.5f, 0.5f, 0.5f};
    for (uint32_t r = 0; r < n; r++)
        for (uint32_t i = 0; i + 4 <= len; i += 4)
        {
            v4 p, q;
            __builtin_memcpy(&p, a + i, 16);
            __builtin_memcpy(&q, b + i, 16);
            v4 t = m0 * p[0] + m1 * p[1] + m2 * p[2] + m3 * p[3];
            t = t * k + q * (1.0f - k[0]);
            __builtin_memcpy(a + i, &t, 16);
        }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
    {
        uint32_t bits;
        __builtin_memcpy(&bits, &a[i], 4);
        sum = sum * 31u + bits;
    }
    return sum;
}

// Scalar doubles: physics and game math done in double precision (the x87 FPU in the guest).
uint32_t NAME(xb_x87)(uint32_t n)
{
#if GUEST
    // precision 53 bits, round to nearest, exceptions masked: every operation rounds as SSE2 does
    const uint16_t control = 0x027F;
    __asm__ volatile("fldcw %0" : : "m"(control));
#endif
    double x = 0.5, y = 0.25, vx = 0.001, vy = -0.002, energy = 0.0;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        const double ax = -x * 0.01 + y * 0.0003, ay = -y * 0.01 - x * 0.0002;
        vx = vx * 0.9999 + ax * 0.016;
        vy = vy * 0.9999 + ay * 0.016;
        x += vx * 0.016;
        y += vy * 0.016;
        energy = energy * 0.5 + (vx * vx + vy * vy) / (1.0 + x * x + y * y);
        if ((i & 1023) == 0)
        {
            uint64_t bits;
            __builtin_memcpy(&bits, &energy, 8);
            sum = sum * 33u + (uint32_t)bits + (uint32_t)(bits >> 32);
            vx += 0.001, vy -= 0.0005;
        }
    }
    uint64_t bits;
    __builtin_memcpy(&bits, &x, 8);
    return sum ^ (uint32_t)bits ^ (uint32_t)(bits >> 32);
}

#if GUEST
// ---- guest only: the costs of what an Xbox emulator does around the game's code

// Returns its argument: one call from the emulator into Xbox code and back.
uint32_t xb_empty(uint32_t x)
{
    return x;
}

// Hardware registers: n stores and n loads at `reg`, a page nothing is mapped at (the NV2A's
// registers start at 0xFD000000). Each access faults and the emulator's fault handler does it
// instead, as Cxbx-Reloaded handles MMIO. The instructions are fixed ("mov [edx], eax" and
// "mov eax, [edx]") so the handler's decoder knows them; a real one decodes any MOV.
uint32_t xb_mmio(uint32_t n, volatile uint32_t* reg)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t v = i * 3u + 1u, got;
        __asm__ volatile("movl %1, (%2)" : "=m"(*reg) : "a"(v), "d"(reg) : "memory");
        __asm__ volatile("movl (%1), %0" : "=a"(got) : "d"(reg), "m"(*reg) : "memory");
        sum += got;
    }
    return sum;
}

// Kernel calls: n calls to `export_fn`, a gate to a host (64-bit) function, as an Xbox game calls
// xboxkrnl.exe's exports through its thunk table and an HLE kernel answers them on the host.
typedef uint32_t (*KernelExport)(uint32_t a, uint32_t b);
uint32_t xb_thunk(uint32_t n, KernelExport export_fn)
{
    uint32_t acc = 7;
    for (uint32_t i = 0; i < n; i++)
        acc = export_fn(acc, i);
    return acc;
}

// The thread's FS segment: Xbox code reads its KPCR through fs (fs:[0x28] is the current thread,
// fs:[0x20] the PRCB). Returns fs:[offset].
uint32_t xb_fs_read(uint32_t offset)
{
    uint32_t v;
    __asm__ volatile("movl %%fs:(%1), %0" : "=r"(v) : "r"(offset));
    return v;
}

// The table the emulator finds at the image's load address.
__attribute__((section(".bench_table"), used)) const uint32_t xb_table[] = {
    (uint32_t)xb_int,  (uint32_t)xb_branchy, (uint32_t)xb_calls, (uint32_t)xb_memory,
    (uint32_t)xb_copy, (uint32_t)xb_sse,     (uint32_t)xb_x87,   (uint32_t)xb_empty,
    (uint32_t)xb_mmio, (uint32_t)xb_thunk,   (uint32_t)xb_fs_read,
};
#endif
