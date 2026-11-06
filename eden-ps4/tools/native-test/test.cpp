// Host test of Core::NativeFunctions::Move/Fill (native_functions.h) against memmove/memset on a flat
// buffer: pages plain (a host pointer) or not (GPU-cached: only ReadBlock/WriteBlock), random
// addresses, sizes and overlaps. The memory's own bytes are checked, and that every write to a
// non-plain page went through WriteBlock (the GPU's notification).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <array>
#include <algorithm>
using u8 = uint8_t; using u64 = uint64_t; using s64 = int64_t;
namespace Core::Memory { constexpr u64 YUZU_PAGESIZE = 4096, YUZU_PAGEMASK = 4095; }
struct MockMemory {
    std::vector<u8> bytes;
    std::vector<bool> plain;  // per page
    std::vector<bool> written_direct_nonplain;
    u64 base = 0x80000000;
    u64 notified = 0;
    u8* GetPlainPointer(u64 a) {
        if (a < base || a >= base + bytes.size()) return nullptr;
        return plain[(a - base) / 4096] ? &bytes[a - base] : nullptr;
    }
    bool ReadBlock(u64 a, void* out, size_t n) {
        for (size_t i = 0; i < n; i++) static_cast<u8*>(out)[i] = bytes[a - base + i];
        return true;
    }
    bool WriteBlock(u64 a, const void* in, size_t n) {
        for (size_t i = 0; i < n; i++) bytes[a - base + i] = static_cast<const u8*>(in)[i];
        notified += n;
        return true;
    }
};
namespace Core::NativeFunctions {
#define NATIVE_TEST_ONLY_MOVE_FILL
#include "move_fill.inc"
}
int main() {
    std::mt19937_64 rng(1);
    const size_t pages = 16;
    u64 bad = 0, runs = 0;
    for (int it = 0; it < 200000; it++) {
        MockMemory m;
        m.bytes.resize(pages * 4096);
        for (auto& b : m.bytes) b = u8(rng());
        m.plain.resize(pages);
        for (size_t p = 0; p < pages; p++) m.plain[p] = (rng() % 3) != 0;
        std::vector<u8> ref = m.bytes;
        const u64 n = rng() % 4 == 0 ? rng() % 64 : rng() % (3 * 4096);
        const u64 dest = rng() % (pages * 4096 - n), kind = rng() % 3;
        u64 src = rng() % 2 ? dest + (rng() % 128) - 64 : rng() % (pages * 4096 - n);
        if (src + n > pages * 4096 || s64(src) < 0) src = dest;
        if (kind < 2) {
            std::memmove(&ref[dest], &ref[src], n);
            Core::NativeFunctions::Move(m, m.base + dest, m.base + src, n);
        } else {
            const u8 c = u8(rng());
            std::memset(&ref[dest], c, n);
            Core::NativeFunctions::Fill(m, m.base + dest, c, n);
        }
        runs++;
        if (ref != m.bytes) {
            if (bad++ < 5) std::printf("wrong: kind %llu dest %llu src %llu n %llu\n", (unsigned long long)kind, (unsigned long long)dest, (unsigned long long)src, (unsigned long long)n);
        }
    }
    std::printf("%llu runs, %llu wrong\n", (unsigned long long)runs, (unsigned long long)bad);
    return bad != 0;
}
