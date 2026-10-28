/* libnx with clang: libnx applies the C11 atomic generics (stdatomic.h) to plain, non-_Atomic
 * objects, which gcc accepts and clang rejects. Map them to the __atomic builtins, which take any
 * object (as gcc's own stdatomic.h does). Force-included (-include) when building libnx and code
 * using its headers. */
#ifndef LIBNX_CLANG_ATOMICS_H
#define LIBNX_CLANG_ATOMICS_H
#if defined(__clang__) && !defined(__cplusplus) && !defined(__ASSEMBLER__)
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#undef atomic_load_explicit
#undef atomic_load
#undef atomic_store_explicit
#undef atomic_store
#undef atomic_exchange_explicit
#undef atomic_exchange
#undef atomic_compare_exchange_strong_explicit
#undef atomic_compare_exchange_strong
#undef atomic_compare_exchange_weak_explicit
#undef atomic_compare_exchange_weak
#undef atomic_fetch_add_explicit
#undef atomic_fetch_add
#undef atomic_fetch_sub_explicit
#undef atomic_fetch_sub
#undef atomic_fetch_or_explicit
#undef atomic_fetch_or
#undef atomic_fetch_and_explicit
#undef atomic_fetch_and
#undef atomic_fetch_xor_explicit
#undef atomic_fetch_xor
#define atomic_load_explicit(p, o) \
    ({ __typeof__(*(p)) __nx_v; __atomic_load((p), &__nx_v, (o)); __nx_v; })
#define atomic_load(p) atomic_load_explicit((p), __ATOMIC_SEQ_CST)
#define atomic_store_explicit(p, v, o) \
    ({ __typeof__(*(p)) __nx_v = (v); __atomic_store((p), &__nx_v, (o)); })
#define atomic_store(p, v) atomic_store_explicit((p), (v), __ATOMIC_SEQ_CST)
#define atomic_exchange_explicit(p, v, o) \
    ({ __typeof__(*(p)) __nx_v = (v), __nx_r; __atomic_exchange((p), &__nx_v, &__nx_r, (o)); __nx_r; })
#define atomic_exchange(p, v) atomic_exchange_explicit((p), (v), __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_strong_explicit(p, e, d, s, f) \
    ({ __typeof__(*(p)) __nx_d = (d); __atomic_compare_exchange((p), (e), &__nx_d, 0, (s), (f)); })
#define atomic_compare_exchange_strong(p, e, d) \
    atomic_compare_exchange_strong_explicit((p), (e), (d), __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_weak_explicit(p, e, d, s, f) \
    ({ __typeof__(*(p)) __nx_d = (d); __atomic_compare_exchange((p), (e), &__nx_d, 1, (s), (f)); })
#define atomic_compare_exchange_weak(p, e, d) \
    atomic_compare_exchange_weak_explicit((p), (e), (d), __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_fetch_add_explicit(p, v, o) __atomic_fetch_add((p), (v), (o))
#define atomic_fetch_add(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define atomic_fetch_sub_explicit(p, v, o) __atomic_fetch_sub((p), (v), (o))
#define atomic_fetch_sub(p, v) __atomic_fetch_sub((p), (v), __ATOMIC_SEQ_CST)
#define atomic_fetch_or_explicit(p, v, o) __atomic_fetch_or((p), (v), (o))
#define atomic_fetch_or(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define atomic_fetch_and_explicit(p, v, o) __atomic_fetch_and((p), (v), (o))
#define atomic_fetch_and(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#define atomic_fetch_xor_explicit(p, v, o) __atomic_fetch_xor((p), (v), (o))
#define atomic_fetch_xor(p, v) __atomic_fetch_xor((p), (v), __ATOMIC_SEQ_CST)
#endif
#endif
