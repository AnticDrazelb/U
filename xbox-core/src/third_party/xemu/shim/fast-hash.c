#include "qemu/fast-hash.h"
#define XXH_INLINE_ALL
#include <xxhash.h>

uint64_t fast_hash(const uint8_t *data, size_t len)
{
    return XXH3_64bits(data, len);
}
