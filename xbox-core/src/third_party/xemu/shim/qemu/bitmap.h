/* xbox-core: QEMU's bitmaps (what xemu's NV2A uses) */
#ifndef XB_XEMU_BITMAP_H
#define XB_XEMU_BITMAP_H
#include "qemu/osdep.h"
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BITS_TO_LONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
static inline unsigned long *bitmap_new(long nbits) { return calloc(BITS_TO_LONGS(nbits), sizeof(unsigned long)); }
static inline bool test_bit(long nr, const unsigned long *addr) { return (addr[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1; }
static inline void bitmap_set(unsigned long *map, long start, long nr)
{
    for (long i = start; i < start + nr; i++) {
        map[i / BITS_PER_LONG] |= 1UL << (i % BITS_PER_LONG);
    }
}
static inline void bitmap_clear(unsigned long *map, long start, long nr)
{
    for (long i = start; i < start + nr; i++) {
        map[i / BITS_PER_LONG] &= ~(1UL << (i % BITS_PER_LONG));
    }
}
static inline unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{
    for (unsigned long i = offset; i < size; i++) {
        if (test_bit(i, addr)) {
            return i;
        }
    }
    return size;
}
#endif
