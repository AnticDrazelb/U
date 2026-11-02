// zlib (deflate, inflate, crc32, adler32) compiled for 32-bit ARM, run in the harness's image mode:
// real compiler output by the thousands of blocks, for translator changes that rely on the
// procedure call standard. r0 = rounds; the result in r0: a checksum of every round's compressed
// bytes, or 0xbad0000N when a round's data didn't come back.
#include <stddef.h>
#include <stdint.h>
#include "zlib.h"

#define SIZE 24576
static unsigned char input[SIZE], packed[SIZE + 1024], unpacked[SIZE];
static unsigned char arena[400 * 1024];
static size_t arena_used;

void* memcpy(void* d, const void* s, size_t n) { unsigned char* a = d; const unsigned char* b = s; while (n--) *a++ = *b++; return d; }
void* memmove(void* d, const void* s, size_t n) {
    unsigned char* a = d; const unsigned char* b = s;
    if (a < b) while (n--) *a++ = *b++; else while (n--) a[n] = b[n];
    return d;
}
void* memset(void* d, int c, size_t n) { unsigned char* a = d; while (n--) *a++ = (unsigned char)c; return d; }
int memcmp(const void* x, const void* y, size_t n) {
    const unsigned char* a = x; const unsigned char* b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }

static voidpf zalloc(voidpf opaque, uInt items, uInt size) {
    (void)opaque;
    size_t n = ((size_t)items * size + 15) & ~(size_t)15;
    if (arena_used + n > sizeof arena) return Z_NULL;
    void* p = arena + arena_used;
    arena_used += n;
    return p;
}
static void zfree(voidpf opaque, voidpf p) { (void)opaque; (void)p; }

static uint32_t seed = 1;
static uint32_t rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 9; }

static void fill(int round) {
    static const char* words[] = {"kart", "shell", "banana", "drift", "boost", "lap", "coin", "item", "glider", "turbo"};
    size_t i = 0;
    seed = 12345u + (uint32_t)round;
    while (i < SIZE) {
        if (rnd() % 7 == 0) { input[i++] = (unsigned char)rnd(); continue; }
        const char* w = words[rnd() % 10];
        while (*w && i < SIZE) input[i++] = (unsigned char)*w++;
        if (i < SIZE) input[i++] = ' ';
    }
}

uint32_t run(int rounds) {
    uint32_t sum = 0;
    for (int r = 0; r < rounds; r++) {
        fill(r);
        arena_used = 0;
        z_stream s = {0};
        s.zalloc = zalloc; s.zfree = zfree;
        if (deflateInit2(&s, 1 + r % 9, Z_DEFLATED, 15, 8, r % 2 ? Z_DEFAULT_STRATEGY : Z_FILTERED) != Z_OK) return 0xbad00001;
        s.next_in = input; s.avail_in = SIZE; s.next_out = packed; s.avail_out = sizeof packed;
        if (deflate(&s, Z_FINISH) != Z_STREAM_END) return 0xbad00002;
        const uLong packed_size = s.total_out;
        deflateEnd(&s);
        z_stream t = {0};
        t.zalloc = zalloc; t.zfree = zfree;
        if (inflateInit(&t) != Z_OK) return 0xbad00003;
        t.next_in = packed; t.avail_in = (uInt)packed_size; t.next_out = unpacked; t.avail_out = SIZE;
        if (inflate(&t, Z_FINISH) != Z_STREAM_END) return 0xbad00004;
        inflateEnd(&t);
        if (memcmp(input, unpacked, SIZE) != 0) return 0xbad00005;
        sum = sum * 31 + (uint32_t)crc32(0, packed, (uInt)packed_size) + (uint32_t)adler32(1, unpacked, SIZE) + (uint32_t)packed_size;
    }
    return sum;
}
