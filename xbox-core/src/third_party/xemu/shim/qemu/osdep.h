/* xbox-core's stand-in for QEMU's osdep.h: what xemu's APU and DSP code use of QEMU and glib. */
#ifndef XB_XEMU_OSDEP_H
#define XB_XEMU_OSDEP_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <math.h>
#include <sys/types.h>

typedef uint64_t hwaddr;
#define HWADDR_PRIx PRIx64

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
#ifndef M_E
#define M_E 2.7182818284590452354
#endif
#define g_new0(type, n) ((type *)calloc((n), sizeof(type)))
#define g_new(type, n) ((type *)malloc((n) * sizeof(type)))
#define g_malloc0(n) calloc(1, (n))
#define g_free(p) free(p)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x) __builtin_expect(!!(x), 1)
#define QEMU_PACKED __attribute__((packed))

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
static inline int ctz32(uint32_t v) { return v ? __builtin_ctz(v) : 32; }
static inline int ctz64(uint64_t v) { return v ? __builtin_ctzll(v) : 64; }
#define g_malloc0_n(n, size) calloc((n), (size))
#define rcu_register_thread() ((void)0)
#define rcu_unregister_thread() ((void)0)

#define qatomic_read(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define qatomic_set(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#define qatomic_or(p, v) ((void)__atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST))
#define qatomic_and(p, v) ((void)__atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST))

/* the clock */
#include <time.h>
enum { QEMU_CLOCK_REALTIME = 0 };
static inline int64_t qemu_clock_get_us(int clock)
{
    (void)clock;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* guest physical memory and the APU's interrupt line: xbox-core's (src/apu.cpp) */
typedef struct AddressSpace { int unused; } AddressSpace;
extern AddressSpace address_space_memory;
uint32_t xb_phys_ld32(uint64_t addr);
uint32_t xb_phys_ld16(uint64_t addr);
uint32_t xb_phys_ld8(uint64_t addr);
void xb_phys_st32(uint64_t addr, uint32_t v);
void xb_phys_st8(uint64_t addr, uint32_t v);
#define ldl_le_phys(as, a) xb_phys_ld32(a)
#define lduw_le_phys(as, a) xb_phys_ld16(a)
#define ldub_phys(as, a) xb_phys_ld8(a)
#define stl_le_phys(as, a, v) xb_phys_st32((a), (v))
#define stb_phys(as, a, v) xb_phys_st8((a), (v))

/* errors */
typedef struct Error { char message[256]; } Error;
#define error_setg(errp, ...) do { if (errp && !*(errp)) { *(errp) = (Error *)calloc(1, sizeof(Error)); snprintf((*(errp))->message, 256, __VA_ARGS__); } } while (0)
#define warn_reportf_err(err, ...) free(err)
#endif
