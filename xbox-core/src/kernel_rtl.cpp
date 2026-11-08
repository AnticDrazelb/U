// Kernel exports that stand on their own: the Rtl library (memory, strings, integers, printf), the
// pool (ExAllocatePool), interlocked lists, the Xc crypto that saves and Live use (RC4, HMAC-SHA1),
// and the debug, HAL and Mm calls that need little more than an answer.
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "core.h"
#include "devices.h"
#include "kernel.h"
#include "sha1.h"

namespace xb {

namespace {

constexpr u32 STATUS_SUCCESS = 0, STATUS_BUFFER_OVERFLOW = 0x80000005, STATUS_NO_MEMORY = 0xC0000017,
              STATUS_INVALID_PARAMETER = 0xC000000D, STATUS_BUFFER_TOO_SMALL = 0xC0000023;

// ---- the pool: small blocks in size classes carved from contiguous memory, big ones directly

std::mutex g_pool_mutex;
std::map<u32, u32> g_pool_blocks;      // block -> its size class (bytes), or its size for big ones
std::vector<u32> g_pool_free[9];       // 16, 32, ... 4096
constexpr u32 kChunk = 64 * 1024;

int SizeClass(u32 size)
{
    u32 c = 16;
    for (int i = 0; i < 9; i++, c <<= 1)
        if (size <= c)
            return i;
    return -1;
}

// ---- strings: ANSI_STRING and UNICODE_STRING are { u16 Length, u16 MaximumLength, u32 Buffer }

u16 Length(u32 s) { return Read<u16>(s); }
u16 MaxLength(u32 s) { return Read<u16>(s + 2); }
u32 Buffer(u32 s) { return Read<u32>(s + 4); }
void SetString(u32 s, u16 length, u16 max, u32 buffer)
{
    Write<u16>(s, length);
    Write<u16>(s + 2, max);
    Write<u32>(s + 4, buffer);
}

u32 Wcslen(u32 p)
{
    u32 n = 0;
    while (Read<u16>(p + n * 2))
        n++;
    return n;
}

char Upper(char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; }
char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
u16 UpperW(u16 c) { return c >= 'a' && c <= 'z' ? u16(c - 32) : c; }
u16 LowerW(u16 c) { return c >= 'A' && c <= 'Z' ? u16(c + 32) : c; }

// characters of a string as u16 (ANSI widened)
std::vector<u16> Chars(u32 s, bool wide)
{
    std::vector<u16> out;
    const u32 n = wide ? Length(s) / 2u : Length(s), b = Buffer(s);
    for (u32 i = 0; i < n && b; i++)
        out.push_back(wide ? Read<u16>(b + i * 2) : Read<u8>(b + i));
    return out;
}

s32 CompareChars(const std::vector<u16>& a, const std::vector<u16>& b, bool insensitive)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++)
    {
        const u16 x = insensitive ? UpperW(a[i]) : a[i], y = insensitive ? UpperW(b[i]) : b[i];
        if (x != y)
            return s32(x) - s32(y);
    }
    return s32(a.size()) - s32(b.size());
}

// Fills a string with `chars` (ANSI or wide); allocates its buffer from the pool when `allocate`.
u32 StoreString(u32 dst, const std::vector<u16>& chars, bool wide, bool allocate)
{
    const u32 unit = wide ? 2 : 1;
    const u32 bytes = u32(chars.size()) * unit;
    if (allocate)
    {
        const u32 b = PoolAlloc(bytes + unit);
        if (!b)
            return STATUS_NO_MEMORY;
        SetString(dst, u16(bytes), u16(bytes + unit), b);
    }
    else if (bytes > MaxLength(dst))
        return STATUS_BUFFER_OVERFLOW;
    const u32 b = Buffer(dst);
    for (size_t i = 0; i < chars.size(); i++)
        wide ? Write<u16>(b + u32(i) * 2, chars[i]) : Write<u8>(b + u32(i), u8(chars[i] < 0x100 ? chars[i] : '?'));
    if (bytes + unit <= MaxLength(dst))
        wide ? Write<u16>(b + bytes, 0) : Write<u8>(b + bytes, 0);
    Write<u16>(dst, u16(bytes));
    return STATUS_SUCCESS;
}

// a number in `base` (0: by its prefix, else 10) to an integer, as RtlCharToInteger reads it
u32 ParseInteger(const std::vector<u16>& s, u32 base, u32& value)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        i++;
    bool negative = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+'))
        negative = s[i++] == '-';
    if (base == 0)
    {
        base = 10;
        if (i + 1 < s.size() && s[i] == '0')
        {
            const u16 p = LowerW(s[i + 1]);
            if (p == 'x') base = 16, i += 2;
            else if (p == 'o') base = 8, i += 2;
            else if (p == 'b') base = 2, i += 2;
        }
    }
    else if (base != 2 && base != 8 && base != 10 && base != 16)
        return STATUS_INVALID_PARAMETER;
    u32 v = 0;
    for (; i < s.size(); i++)
    {
        const u16 c = LowerW(s[i]);
        const u32 d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 99;
        if (d >= base)
            break;
        v = v * base + d;
    }
    value = negative ? u32(-s32(v)) : v;
    return STATUS_SUCCESS;
}

std::string IntegerText(u32 value, u32 base)
{
    if (base == 0)
        base = 10;
    std::string out;
    do
    {
        const u32 d = value % base;
        out.insert(out.begin(), char(d < 10 ? '0' + d : 'A' + d - 10));
        value /= base;
    } while (value);
    return out;
}

// ---- interlocked lists (one lock is plenty: the guest's own locking is gone)

std::mutex g_list_mutex;

// ---- RC4 (XcRC4Key / XcRC4Crypt: the state is 256 bytes of S, then i and j)

void Rc4Key(u8* state, u32 length, const u8* key)
{
    for (int i = 0; i < 256; i++)
        state[i] = u8(i);
    u8 j = 0;
    for (int i = 0; i < 256; i++)
    {
        j = u8(j + state[i] + key[i % length]);
        std::swap(state[i], state[j]);
    }
    state[256] = state[257] = 0;
}

void Rc4Crypt(u8* state, u32 length, u8* data)
{
    u8 i = state[256], j = state[257];
    for (u32 n = 0; n < length; n++)
    {
        i = u8(i + 1);
        j = u8(j + state[i]);
        std::swap(state[i], state[j]);
        data[n] ^= state[u8(state[i] + state[j])];
    }
    state[256] = i, state[257] = j;
}

} // namespace

u32 PoolAlloc(u32 size)
{
    std::lock_guard lock(g_pool_mutex);
    const int c = SizeClass(std::max<u32>(size, 1));
    if (c < 0)
    {
        const u32 p = AllocContiguous((size + 0xfff) & ~0xfffu, 0x1000, 0, 0xffffffff);
        if (p)
            g_pool_blocks[p] = size;
        return p;
    }
    auto& free = g_pool_free[c];
    if (free.empty())
    {
        const u32 chunk = AllocContiguous(kChunk, 0x1000, 0, 0xffffffff);
        if (!chunk)
            return 0;
        for (u32 at = chunk + kChunk; at > chunk;)
            free.push_back(at -= 16u << c);
    }
    const u32 p = free.back();
    free.pop_back();
    g_pool_blocks[p] = 16u << c;
    return p;
}

void PoolFree(u32 p)
{
    std::lock_guard lock(g_pool_mutex);
    auto it = g_pool_blocks.find(p);
    if (it == g_pool_blocks.end())
        return;
    const int c = SizeClass(it->second);
    if (c >= 0 && (16u << c) == it->second)
        g_pool_free[c].push_back(p);
    else
        FreeContiguous(p);
    g_pool_blocks.erase(it);
}

void RegisterRtlExports(RegisterFn reg)
{
    // ---- the pool
    reg("ExAllocatePool", [](Call& c) -> u64 { return PoolAlloc(c.Arg(0)); });
    reg("ExAllocatePoolWithTag", [](Call& c) -> u64 { return PoolAlloc(c.Arg(0)); });
    reg("ExFreePool", [](Call& c) -> u64 { PoolFree(c.Arg(0)); return 0; });
    reg("ExQueryPoolBlockSize", [](Call& c) -> u64 {
        std::lock_guard lock(g_pool_mutex);
        auto it = g_pool_blocks.find(c.Arg(0));
        return it == g_pool_blocks.end() ? 0 : it->second;
    });

    // ---- memory
    reg("RtlCompareMemory", [](Call& c) -> u64 {
        const u8 *a = Ptr<u8>(c.Arg(0)), *b = Ptr<u8>(c.Arg(1));
        u32 i = 0;
        while (i < c.Arg(2) && a[i] == b[i])
            i++;
        return i;
    });
    reg("RtlCompareMemoryUlong", [](Call& c) -> u64 {
        u32 i = 0;
        while (i + 4 <= c.Arg(1) && Read<u32>(c.Arg(0) + i) == c.Arg(2))
            i += 4;
        return i;
    });
    reg("RtlFillMemory", [](Call& c) -> u64 { std::memset(Ptr(c.Arg(0)), int(c.Arg(2) & 0xff), c.Arg(1)); return 0; });
    reg("RtlFillMemoryUlong", [](Call& c) -> u64 {
        for (u32 i = 0; i + 4 <= c.Arg(1); i += 4)
            Write<u32>(c.Arg(0) + i, c.Arg(2));
        return 0;
    });
    reg("RtlMoveMemory", [](Call& c) -> u64 { std::memmove(Ptr(c.Arg(0)), Ptr(c.Arg(1)), c.Arg(2)); return 0; });
    reg("RtlZeroMemory", [](Call& c) -> u64 { std::memset(Ptr(c.Arg(0)), 0, c.Arg(1)); return 0; });
    reg("RtlUlongByteSwap", [](Call& c) -> u64 { return __builtin_bswap32(c.Arg(0)); });
    reg("RtlUshortByteSwap", [](Call& c) -> u64 { return __builtin_bswap16(u16(c.Arg(0))); });

    // ---- strings
    reg("RtlInitUnicodeString", [](Call& c) -> u64 {
        const u32 src = c.Arg(1);
        const u32 n = src ? Wcslen(src) * 2 : 0;
        SetString(c.Arg(0), u16(n), u16(src ? n + 2 : 0), src);
        return 0;
    });
    reg("RtlCreateUnicodeString", [](Call& c) -> u64 {
        const u32 src = c.Arg(1), n = Wcslen(src);
        std::vector<u16> chars(n);
        for (u32 i = 0; i < n; i++)
            chars[i] = Read<u16>(src + i * 2);
        return StoreString(c.Arg(0), chars, true, true) == STATUS_SUCCESS;
    });
    reg("RtlAnsiStringToUnicodeString", [](Call& c) -> u64 {
        return StoreString(c.Arg(0), Chars(c.Arg(1), false), true, c.Arg(2) & 0xff);
    });
    reg("RtlUnicodeStringToAnsiString", [](Call& c) -> u64 {
        return StoreString(c.Arg(0), Chars(c.Arg(1), true), false, c.Arg(2) & 0xff);
    });
    reg("RtlFreeAnsiString", [](Call& c) -> u64 { PoolFree(Buffer(c.Arg(0))); SetString(c.Arg(0), 0, 0, 0); return 0; });
    reg("RtlFreeUnicodeString", [](Call& c) -> u64 { PoolFree(Buffer(c.Arg(0))); SetString(c.Arg(0), 0, 0, 0); return 0; });
    reg("RtlCopyString", [](Call& c) -> u64 {
        if (!c.Arg(1))
            Write<u16>(c.Arg(0), 0);
        else
        {
            auto chars = Chars(c.Arg(1), false);
            chars.resize(std::min<size_t>(chars.size(), MaxLength(c.Arg(0))));
            StoreString(c.Arg(0), chars, false, false);
        }
        return 0;
    });
    reg("RtlCopyUnicodeString", [](Call& c) -> u64 {
        if (!c.Arg(1))
            Write<u16>(c.Arg(0), 0);
        else
        {
            auto chars = Chars(c.Arg(1), true);
            chars.resize(std::min<size_t>(chars.size(), MaxLength(c.Arg(0)) / 2u));
            StoreString(c.Arg(0), chars, true, false);
        }
        return 0;
    });
    reg("RtlAppendStringToString", [](Call& c) -> u64 {
        auto chars = Chars(c.Arg(0), false);
        const auto more = Chars(c.Arg(1), false);
        chars.insert(chars.end(), more.begin(), more.end());
        return chars.size() > MaxLength(c.Arg(0)) ? STATUS_BUFFER_TOO_SMALL : StoreString(c.Arg(0), chars, false, false);
    });
    reg("RtlAppendUnicodeStringToString", [](Call& c) -> u64 {
        auto chars = Chars(c.Arg(0), true);
        const auto more = Chars(c.Arg(1), true);
        chars.insert(chars.end(), more.begin(), more.end());
        return chars.size() * 2 > MaxLength(c.Arg(0)) ? STATUS_BUFFER_TOO_SMALL : StoreString(c.Arg(0), chars, true, false);
    });
    reg("RtlAppendUnicodeToString", [](Call& c) -> u64 {
        auto chars = Chars(c.Arg(0), true);
        if (const u32 src = c.Arg(1))
            for (u32 i = 0, n = Wcslen(src); i < n; i++)
                chars.push_back(Read<u16>(src + i * 2));
        return chars.size() * 2 > MaxLength(c.Arg(0)) ? STATUS_BUFFER_TOO_SMALL : StoreString(c.Arg(0), chars, true, false);
    });
    reg("RtlCompareString", [](Call& c) -> u64 {
        return u32(CompareChars(Chars(c.Arg(0), false), Chars(c.Arg(1), false), c.Arg(2) & 0xff));
    });
    reg("RtlCompareUnicodeString", [](Call& c) -> u64 {
        return u32(CompareChars(Chars(c.Arg(0), true), Chars(c.Arg(1), true), c.Arg(2) & 0xff));
    });
    reg("RtlEqualString", [](Call& c) -> u64 {
        return CompareChars(Chars(c.Arg(0), false), Chars(c.Arg(1), false), c.Arg(2) & 0xff) == 0;
    });
    reg("RtlEqualUnicodeString", [](Call& c) -> u64 {
        return CompareChars(Chars(c.Arg(0), true), Chars(c.Arg(1), true), c.Arg(2) & 0xff) == 0;
    });
    reg("RtlUpperString", [](Call& c) -> u64 {
        auto chars = Chars(c.Arg(1), false);
        for (auto& ch : chars)
            ch = u8(Upper(char(ch)));
        chars.resize(std::min<size_t>(chars.size(), MaxLength(c.Arg(0))));
        StoreString(c.Arg(0), chars, false, false);
        return 0;
    });
    reg("RtlUpcaseUnicodeString", [](Call& c) -> u64 {
        auto chars = Chars(c.Arg(1), true);
        for (auto& ch : chars)
            ch = UpperW(ch);
        return StoreString(c.Arg(0), chars, true, c.Arg(2) & 0xff);
    });
    reg("RtlDowncaseUnicodeString", [](Call& c) -> u64 {
        auto chars = Chars(c.Arg(1), true);
        for (auto& ch : chars)
            ch = LowerW(ch);
        return StoreString(c.Arg(0), chars, true, c.Arg(2) & 0xff);
    });
    reg("RtlUpcaseUnicodeChar", [](Call& c) -> u64 { return UpperW(u16(c.Arg(0))); });
    reg("RtlDowncaseUnicodeChar", [](Call& c) -> u64 { return LowerW(u16(c.Arg(0))); });
    reg("RtlUpperChar", [](Call& c) -> u64 { return u8(Upper(char(c.Arg(0)))); });
    reg("RtlLowerChar", [](Call& c) -> u64 { return u8(Lower(char(c.Arg(0)))); });
    reg("RtlCharToInteger", [](Call& c) -> u64 {
        std::vector<u16> s;
        for (u32 p = c.Arg(0); Read<u8>(p); p++)
            s.push_back(Read<u8>(p));
        u32 v = 0;
        const u32 status = ParseInteger(s, c.Arg(1), v);
        if (status == STATUS_SUCCESS)
            Write<u32>(c.Arg(2), v);
        return status;
    });
    reg("RtlUnicodeStringToInteger", [](Call& c) -> u64 {
        u32 v = 0;
        const u32 status = ParseInteger(Chars(c.Arg(0), true), c.Arg(1), v);
        if (status == STATUS_SUCCESS)
            Write<u32>(c.Arg(2), v);
        return status;
    });
    reg("RtlIntegerToChar", [](Call& c) -> u64 {
        const std::string t = IntegerText(c.Arg(0), c.Arg(1));
        const s32 length = s32(c.Arg(2));
        if (s32(t.size()) > length)
            return STATUS_BUFFER_OVERFLOW;
        std::memcpy(Ptr(c.Arg(3)), t.c_str(), std::min<size_t>(t.size() + 1, size_t(length)));
        return STATUS_SUCCESS;
    });
    reg("RtlIntegerToUnicodeString", [](Call& c) -> u64 {
        const std::string t = IntegerText(c.Arg(0), c.Arg(1));
        return StoreString(c.Arg(2), std::vector<u16>(t.begin(), t.end()), true, false);
    });
    // multibyte is Latin-1 here
    reg("RtlMultiByteToUnicodeN", [](Call& c) -> u64 {
        const u32 n = std::min(c.Arg(1) / 2, c.Arg(4));
        for (u32 i = 0; i < n; i++)
            Write<u16>(c.Arg(0) + i * 2, Read<u8>(c.Arg(3) + i));
        if (c.Arg(2))
            Write<u32>(c.Arg(2), n * 2);
        return STATUS_SUCCESS;
    });
    reg("RtlMultiByteToUnicodeSize", [](Call& c) -> u64 { Write<u32>(c.Arg(0), c.Arg(2) * 2); return STATUS_SUCCESS; });
    auto to_multibyte = [](Call& c, bool upper) -> u64 {
        const u32 n = std::min(c.Arg(1), c.Arg(4) / 2);
        for (u32 i = 0; i < n; i++)
        {
            u16 ch = Read<u16>(c.Arg(3) + i * 2);
            if (upper)
                ch = UpperW(ch);
            Write<u8>(c.Arg(0) + i, u8(ch < 0x100 ? ch : '?'));
        }
        if (c.Arg(2))
            Write<u32>(c.Arg(2), n);
        return STATUS_SUCCESS;
    };
    reg("RtlUnicodeToMultiByteN", [to_multibyte](Call& c) -> u64 { return to_multibyte(c, false); });
    reg("RtlUpcaseUnicodeToMultiByteN", [to_multibyte](Call& c) -> u64 { return to_multibyte(c, true); });
    reg("RtlUnicodeToMultiByteSize", [](Call& c) -> u64 { Write<u32>(c.Arg(0), c.Arg(2) / 2); return STATUS_SUCCESS; });

    // ---- printf: RtlSprintf(buffer, format, ...), RtlSnprintf(buffer, size, format, ...), and the
    // v forms with a va_list (a pointer to the arguments); cdecl, so the caller pops
    auto put = [](u32 buffer, u32 size, const std::string& s) -> u64 {
        if (size == 0xffffffff || s.size() < size)
        {
            std::memcpy(Ptr(buffer), s.c_str(), s.size() + 1);
            return s.size();
        }
        if (size)
            std::memcpy(Ptr(buffer), s.data(), size);
        return u32(-1);
    };
    reg("RtlSprintf", [put](Call& c) -> u64 {
        return put(c.Arg(0), 0xffffffff, FormatGuest(c.Arg(1), Guest(&c.frame->args[2])));
    });
    reg("RtlSnprintf", [put](Call& c) -> u64 {
        return put(c.Arg(0), c.Arg(1), FormatGuest(c.Arg(2), Guest(&c.frame->args[3])));
    });
    reg("RtlVsprintf", [put](Call& c) -> u64 { return put(c.Arg(0), 0xffffffff, FormatGuest(c.Arg(1), c.Arg(2))); });
    reg("RtlVsnprintf", [put](Call& c) -> u64 { return put(c.Arg(0), c.Arg(1), FormatGuest(c.Arg(2), c.Arg(3))); });

    // ---- 64-bit arithmetic (LARGE_INTEGERs by value take two argument slots)
    reg("RtlExtendedIntegerMultiply", [](Call& c) -> u64 {
        const s64 a = s64(u64(c.Arg(0)) | u64(c.Arg(1)) << 32);
        return u64(a * s32(c.Arg(2)));
    });
    reg("RtlExtendedLargeIntegerDivide", [](Call& c) -> u64 {
        const u64 a = u64(c.Arg(0)) | u64(c.Arg(1)) << 32;
        const u32 d = c.Arg(2);
        if (!d)
            return 0;
        if (c.Arg(3))
            Write<u32>(c.Arg(3), u32(a % d));
        return a / d;
    });
    reg("RtlExtendedMagicDivide", [](Call& c) -> u64 {
        const s64 a = s64(u64(c.Arg(0)) | u64(c.Arg(1)) << 32);
        const u64 magic = u64(c.Arg(2)) | u64(c.Arg(3)) << 32;
        const bool negative = a < 0;
        const u64 q = u64((unsigned __int128)(negative ? u64(-a) : u64(a)) * magic >> 64) >> (c.Arg(4) & 0xff);
        return negative ? u64(-s64(q)) : q;
    });

    // ---- interlocked lists: SLIST_HEADER { Next, Depth u16, Sequence u16 }, LIST_ENTRY { Flink, Blink }
    reg("InterlockedPushEntrySList", [](Call& c) -> u64 { // fastcall: head, entry
        std::lock_guard lock(g_list_mutex);
        const u32 head = c.Arg(0), entry = c.Arg(1), first = Read<u32>(head);
        Write<u32>(entry, first);
        Write<u32>(head, entry);
        Write<u16>(head + 4, u16(Read<u16>(head + 4) + 1));
        Write<u16>(head + 6, u16(Read<u16>(head + 6) + 1));
        return first;
    });
    reg("InterlockedPopEntrySList", [](Call& c) -> u64 {
        std::lock_guard lock(g_list_mutex);
        const u32 head = c.Arg(0), first = Read<u32>(head);
        if (first)
        {
            Write<u32>(head, Read<u32>(first));
            Write<u16>(head + 4, u16(Read<u16>(head + 4) - 1));
            Write<u16>(head + 6, u16(Read<u16>(head + 6) + 1));
        }
        return first;
    });
    reg("InterlockedFlushSList", [](Call& c) -> u64 {
        std::lock_guard lock(g_list_mutex);
        const u32 head = c.Arg(0), first = Read<u32>(head);
        Write<u32>(head, 0);
        Write<u16>(head + 4, 0);
        return first;
    });
    reg("ExfInterlockedInsertHeadList", [](Call& c) -> u64 { // fastcall: head, entry
        std::lock_guard lock(g_list_mutex);
        const u32 head = c.Arg(0), entry = c.Arg(1), first = Read<u32>(head);
        Write<u32>(entry, first);
        Write<u32>(entry + 4, head);
        Write<u32>(first + 4, entry);
        Write<u32>(head, entry);
        return first == head ? 0 : first;
    });
    reg("ExfInterlockedInsertTailList", [](Call& c) -> u64 {
        std::lock_guard lock(g_list_mutex);
        const u32 head = c.Arg(0), entry = c.Arg(1), last = Read<u32>(head + 4);
        Write<u32>(entry, head);
        Write<u32>(entry + 4, last);
        Write<u32>(last, entry);
        Write<u32>(head + 4, entry);
        return last == head ? 0 : last;
    });
    reg("ExfInterlockedRemoveHeadList", [](Call& c) -> u64 {
        std::lock_guard lock(g_list_mutex);
        const u32 head = c.Arg(0), first = Read<u32>(head);
        if (first == head)
            return 0;
        const u32 next = Read<u32>(first);
        Write<u32>(head, next);
        Write<u32>(next + 4, head);
        return first;
    });
    reg("ExInterlockedAddLargeInteger", [](Call& c) -> u64 { // addend, increment (2 slots), lock
        std::lock_guard lock(g_list_mutex);
        const u64 old = Read<u64>(c.Arg(0));
        Write<u64>(c.Arg(0), old + (u64(c.Arg(1)) | u64(c.Arg(2)) << 32));
        return old;
    });
    reg("ExInterlockedAddLargeStatistic", [](Call& c) -> u64 { // fastcall: addend, increment
        __atomic_fetch_add(Ptr<u64>(c.Arg(0)), u64(c.Arg(1)), __ATOMIC_SEQ_CST);
        return 0;
    });
    reg("ExInterlockedCompareExchange64", [](Call& c) -> u64 { // fastcall: destination, exchange*, comparand*
        u64 expected = Read<u64>(c.Arg(2));
        __atomic_compare_exchange_n(Ptr<u64>(c.Arg(0)), &expected, Read<u64>(c.Arg(1)), false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST);
        return expected;
    });

    // ---- crypto
    reg("XcRC4Key", [](Call& c) -> u64 { Rc4Key(Ptr<u8>(c.Arg(0)), c.Arg(1), Ptr<u8>(c.Arg(2))); return 0; });
    reg("XcRC4Crypt", [](Call& c) -> u64 { Rc4Crypt(Ptr<u8>(c.Arg(0)), c.Arg(1), Ptr<u8>(c.Arg(2))); return 0; });
    // XcHMAC(key, key length, data1, length1, data2, length2, digest): HMAC-SHA1 of data1 + data2
    reg("XcHMAC", [](Call& c) -> u64 {
        u8 key[64] = {};
        if (c.Arg(1) > 64)
        {
            Sha1 s;
            s.Init();
            s.Update(Ptr<u8>(c.Arg(0)), c.Arg(1));
            s.Final(key);
        }
        else
            std::memcpy(key, Ptr(c.Arg(0)), c.Arg(1));
        u8 pad[64], inner[20];
        Sha1 s;
        for (int i = 0; i < 64; i++)
            pad[i] = key[i] ^ 0x36;
        s.Init();
        s.Update(pad, 64);
        if (c.Arg(2))
            s.Update(Ptr<u8>(c.Arg(2)), c.Arg(3));
        if (c.Arg(4))
            s.Update(Ptr<u8>(c.Arg(4)), c.Arg(5));
        s.Final(inner);
        for (int i = 0; i < 64; i++)
            pad[i] = key[i] ^ 0x5c;
        s.Init();
        s.Update(pad, 64);
        s.Update(inner, 20);
        s.Final(Ptr<u8>(c.Arg(6)));
        return 0;
    });
    // signatures are taken as valid (what the console's keys sign, we can't check without them)
    reg("XcVerifyPKCS1Signature", [](Call&) -> u64 { return 1; });
    reg("XcPKGetKeyLen", [](Call& c) -> u64 { return c.Arg(0) ? Read<u32>(c.Arg(0) + 0x10) / 8 : 0; });
    reg("XcCryptService", [](Call&) -> u64 { return 0; });
    reg("XcUpdateCrypto", [](Call&) -> u64 { return 0; });

    // ---- debugging
    reg("DbgBreakPoint", [](Call&) -> u64 { Log("kernel: DbgBreakPoint"); return 0; });
    reg("DbgBreakPointWithStatus", [](Call& c) -> u64 { Log("kernel: DbgBreakPointWithStatus %#x", c.Arg(0)); return 0; });
    reg("DbgLoadImageSymbols", [](Call&) -> u64 { return 0; });
    reg("DbgUnLoadImageSymbols", [](Call&) -> u64 { return 0; });
    reg("DbgPrompt", [](Call& c) -> u64 { if (c.Arg(1) && c.Arg(2)) Write<u8>(c.Arg(1), 0); return 0; });
    reg("RtlAssert", [](Call& c) -> u64 {
        Log("kernel: assertion failed: %s (%s:%u) %s", Ptr<const char>(c.Arg(0)), c.Arg(1) ? Ptr<const char>(c.Arg(1)) : "?",
            c.Arg(2), c.Arg(3) ? Ptr<const char>(c.Arg(3)) : "");
        return 0;
    });
    reg("RtlRip", [](Call& c) -> u64 {
        Log("kernel: RtlRip %s %s %s", c.Arg(0) ? Ptr<const char>(c.Arg(0)) : "", c.Arg(1) ? Ptr<const char>(c.Arg(1)) : "",
            c.Arg(2) ? Ptr<const char>(c.Arg(2)) : "");
        return 0;
    });
    reg("RtlGetCallersAddress", [](Call& c) -> u64 {
        if (c.Arg(0)) Write<u32>(c.Arg(0), 0);
        if (c.Arg(1)) Write<u32>(c.Arg(1), 0);
        return 0;
    });
    reg("RtlCaptureStackBackTrace", [](Call& c) -> u64 { if (c.Arg(3)) Write<u32>(c.Arg(3), 0); return 0; });
    reg("RtlWalkFrameChain", [](Call&) -> u64 { return 0; });
    reg("KeBugCheck", [](Call& c) -> u64 { Fatal("kernel: KeBugCheck %#x", c.Arg(0)); return 0; });


    // ---- HAL, SMC and the rest that needs little more than an answer
    reg("HalReadSMCTrayState", [](Call& c) -> u64 { // closed, with a disc
        if (c.Arg(0)) Write<u32>(c.Arg(0), 0x60);
        if (c.Arg(1)) Write<u32>(c.Arg(1), 0);
        return STATUS_SUCCESS;
    });
    reg("HalEnableSecureTrayEject", [](Call&) -> u64 { return 0; });
    reg("HalWriteSMCScratchRegister", [](Call&) -> u64 { return 0; });
    reg("HalIsResetOrShutdownPending", [](Call&) -> u64 { return 0; });
    reg("HalInitiateShutdown", [](Call&) -> u64 { Log("kernel: HalInitiateShutdown"); g_quit = true; return 0; });
    reg("HalEnableSystemInterrupt", [](Call&) -> u64 { return 0; });
    reg("HalDisableSystemInterrupt", [](Call&) -> u64 { return 0; });
    reg("HalRequestSoftwareInterrupt", [](Call&) -> u64 { return 0; });
    reg("HalClearSoftwareInterrupt", [](Call&) -> u64 { return 0; });
    reg("KeStallExecutionProcessor", [](Call& c) -> u64 {
        std::this_thread::sleep_for(std::chrono::microseconds(c.Arg(0)));
        return 0;
    });
    reg("KeSaveFloatingPointState", [](Call&) -> u64 { return STATUS_SUCCESS; });
    reg("KeRestoreFloatingPointState", [](Call&) -> u64 { return STATUS_SUCCESS; });
    reg("KeEnterCriticalRegion", [](Call&) -> u64 { return 0; });
    reg("KeLeaveCriticalRegion", [](Call&) -> u64 { return 0; });
    reg("KeRaiseIrqlToSynchLevel", [](Call&) -> u64 { return 0; });
    reg("KeIsExecutingDpc", [](Call&) -> u64 { return 0; });
    reg("KeBoostPriorityThread", [](Call&) -> u64 { return 0; });
    reg("KeSetDisableBoostThread", [](Call&) -> u64 { return 0; });
    reg("KeSetPriorityProcess", [](Call&) -> u64 { return 8; });
    reg("KeQueryBasePriorityThread", [](Call&) -> u64 { return 0; });
    reg("MmIsAddressValid", [](Call& c) -> u64 {
        u32 start, size;
        const u32 a = c.Arg(0);
        return QueryAllocation(a, start, size) || (a >= kContiguousBase && a < kContiguousBase + kContiguousSize) ||
               (a >= kKernelBase && a < kKernelBase + kKernelSize) || (a >= kStackBase && a < kStackBase + kStackAreaSize);
    });
    reg("MmQueryAddressProtect", [](Call&) -> u64 { return 0x04; }); // PAGE_READWRITE
    reg("MmSetAddressProtect", [](Call&) -> u64 { return 0; });
    reg("MmMapIoSpace", [](Call& c) -> u64 { return c.Arg(0) >= kDeviceBase ? c.Arg(0) : PhysToGuest(c.Arg(0)); });
    reg("MmUnmapIoSpace", [](Call&) -> u64 { return 0; });
    reg("MmCreateKernelStack", [](Call& c) -> u64 {
        return AllocStack((c.Arg(0) + 0xfff) & ~0xfffu); // its top
    });
    reg("MmDeleteKernelStack", [](Call&) -> u64 { return 0; });
    reg("MmDbgAllocateMemory", [](Call&) -> u64 { return 0; });
    reg("MmDbgFreeMemory", [](Call&) -> u64 { return 0; });
    reg("MmDbgQueryAvailablePages", [](Call&) -> u64 { return 0; });
    reg("MmDbgReleaseAddress", [](Call&) -> u64 { return 0; });
    reg("MmDbgWriteCheck", [](Call&) -> u64 { return 0; });
    reg("NtProtectVirtualMemory", [](Call& c) -> u64 { // base*, size*, new protect, old protect*
        if (c.Arg(3))
            Write<u32>(c.Arg(3), 0x04);
        return STATUS_SUCCESS;
    });
    reg("NtSetSystemTime", [](Call&) -> u64 { return STATUS_SUCCESS; });
    reg("PsQueryStatistics", [](Call& c) -> u64 {
        if (Read<u32>(c.Arg(0)) < 12)
            return STATUS_INVALID_PARAMETER;
        Write<u32>(c.Arg(0) + 4, 4); // threads
        Write<u32>(c.Arg(0) + 8, 64); // handles
        return STATUS_SUCCESS;
    });
    reg("PsSetCreateThreadNotifyRoutine", [](Call&) -> u64 { return STATUS_SUCCESS; });
    reg("FscGetCacheSize", [](Call&) -> u64 { return 64; }); // pages of file cache
    reg("FscSetCacheSize", [](Call&) -> u64 { return STATUS_SUCCESS; });
    reg("FscInvalidateIdleBlocks", [](Call&) -> u64 { return 0; });
    reg("PhyGetLinkState", [](Call&) -> u64 { return 0; }); // no network link
    reg("PhyInitialize", [](Call&) -> u64 { return 0; });
    reg("ExReadWriteRefurbInfo", [](Call& c) -> u64 {
        if (!(c.Arg(2) & 0xff) && c.Arg(0))
            std::memset(Ptr(c.Arg(0)), 0, c.Arg(1));
        return STATUS_SUCCESS;
    });
}

} // namespace xb
