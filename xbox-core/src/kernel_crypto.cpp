// The kernel's block ciphers and public-key functions: DES and triple DES (ECB and CBC) over key
// tables, and RSA over the console's key blobs (XcPKEncPublic, XcPKDecPrivate, XcModExp,
// XcVerifyPKCS1Signature). Written from the DES standard (FIPS 46-3) and PKCS #1 v1.5.
#include <cstring>
#include <vector>

#include "kernel.h"

namespace xb {

namespace {

// ---- DES

const u8 kIP[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22,
                    14, 6,  64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9,  1,  59, 51, 43, 35,
                    27, 19, 11, 3,  61, 53, 45, 37, 29, 21, 13, 5,  63, 55, 47, 39, 31, 23, 15, 7};
const u8 kFP[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22,
                    62, 30, 37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11,
                    51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};
const u8 kE[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
                   16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
const u8 kP[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
                   2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6, 22, 11, 4,  25};
const u8 kPC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43,
                     35, 27, 19, 11, 3,  60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,  62, 54,
                     46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
const u8 kPC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,  26, 8,  16, 7,  27, 20, 13, 2,
                     41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
const u8 kShifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
const u8 kS[8][64] = {
    {14, 4,  13, 1, 2, 15, 11, 8,  3,  10, 6,  12, 5,  9,  0, 7, 0, 15, 7,  4,  14, 2,  13, 1,  10, 6, 12, 11, 9,  5,  3, 8,
     4,  1,  14, 8, 13, 6, 2,  11, 15, 12, 9,  7,  3,  10, 5, 0, 15, 12, 8, 2,  4,  9,  1,  7,  5,  11, 3, 14, 10, 0, 6, 13},
    {15, 1,  8,  14, 6,  11, 3,  4,  9,  7, 2,  13, 12, 0, 5,  10, 3,  13, 4,  7, 15, 2,  8,  14, 12, 0,  1, 10, 6,  9,  11, 5,
     0,  14, 7,  11, 10, 4,  13, 1,  5,  8, 12, 6,  9,  3, 2,  15, 13, 8,  10, 1, 3,  15, 4,  2,  11, 6,  7, 12, 0,  5,  14, 9},
    {10, 0,  9,  14, 6, 3,  15, 5,  1, 13, 12, 7,  11, 4,  2,  8,  13, 7,  0,  9,  3,  4,  6, 10, 2, 8,  5,  14, 12, 11, 15, 1,
     13, 6,  4,  9,  8, 15, 3,  0,  11, 1, 2,  12, 5,  10, 14, 7,  1,  10, 13, 0,  6,  9,  8, 7,  4, 15, 14, 3,  11, 5,  2,  12},
    {7,  13, 14, 3, 0,  6,  9,  10, 1,  2, 8, 5,  11, 12, 4,  15, 13, 8,  11, 5, 6,  15, 0,  3,  4,  7,  2,  12, 1, 10, 14, 9,
     10, 6,  9,  0, 12, 11, 7,  13, 15, 1, 3, 14, 5,  2,  8,  4,  3,  15, 0,  6, 10, 1,  13, 8,  9,  4,  5,  11, 12, 7, 2,  14},
    {2,  12, 4,  1,  7,  10, 11, 6, 8,  5,  3,  15, 13, 0, 14, 9,  14, 11, 2,  12, 4,  7, 13, 1,  5,  0,  15, 10, 3,  9,  8,  6,
     4,  2,  1,  11, 10, 13, 7,  8, 15, 9,  12, 5,  6,  3, 0,  14, 11, 8,  12, 7,  1,  14, 2, 13, 6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9, 2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11, 10, 15, 4,  2,  7,  12, 9,  5,  6,  1,  13, 14, 0,  11, 3,  8,
     9,  14, 15, 5,  2, 8,  12, 3,  7,  0,  4,  10, 1,  13, 11, 6,  4,  3,  2,  12, 9,  5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13},
    {4,  11, 2,  14, 15, 0, 8,  13, 3,  12, 9, 7,  5,  10, 6, 1,  13, 0,  11, 7,  4,  9,  1,  10, 14, 3,  5,  12, 2,  15, 8,  6,
     1,  4,  11, 13, 12, 3, 7,  14, 10, 15, 6, 8,  0,  5,  9, 2,  6,  11, 13, 8,  1,  4,  10, 7,  9,  5,  0,  15, 14, 2,  3,  12},
    {13, 2,  8,  4, 6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,  1,  15, 13, 8,  10, 3,  7, 4,  12, 5,  6,  11, 0,  14, 9,  2,
     7,  11, 4,  1, 9,  12, 14, 2,  0,  6,  10, 13, 15, 3,  5,  8,  2,  1,  14, 7,  4,  10, 8, 13, 15, 12, 9,  0,  3,  5,  6,  11}};

// table: 1-based source bit numbers, most significant first, of an `in_bits`-bit value
u64 Permute(u64 v, const u8* table, int n, int in_bits)
{
    u64 r = 0;
    for (int i = 0; i < n; i++)
        r = r << 1 | ((v >> (in_bits - table[i])) & 1);
    return r;
}

u64 Be64(const u8* p)
{
    u64 v = 0;
    for (int i = 0; i < 8; i++)
        v = v << 8 | p[i];
    return v;
}

void PutBe64(u8* p, u64 v)
{
    for (int i = 7; i >= 0; i--, v >>= 8)
        p[i] = u8(v);
}

// A key table here: the 16 round keys (48 bits each) as 16 pairs of 32-bit words, 128 bytes, the size
// the console's tables have (triple DES: three of them).
void DesKeyTable(u32* table, const u8* key)
{
    const u64 cd = Permute(Be64(key), kPC1, 56, 64);
    u32 c = u32(cd >> 28), d = u32(cd & 0xfffffff);
    for (int i = 0; i < 16; i++)
    {
        const int s = kShifts[i];
        c = ((c << s) | (c >> (28 - s))) & 0xfffffff;
        d = ((d << s) | (d >> (28 - s))) & 0xfffffff;
        const u64 k = Permute(u64(c) << 28 | d, kPC2, 48, 56);
        table[i * 2] = u32(k >> 24);
        table[i * 2 + 1] = u32(k & 0xffffff);
    }
}

u32 Feistel(u32 r, u64 k)
{
    const u64 x = Permute(r, kE, 48, 32) ^ k;
    u32 out = 0;
    for (int i = 0; i < 8; i++)
    {
        const u32 b = u32(x >> (42 - 6 * i)) & 63;
        out = out << 4 | kS[i][(((b >> 4) & 2) | (b & 1)) * 16 + ((b >> 1) & 15)];
    }
    return u32(Permute(out, kP, 32, 32));
}

void DesBlock(u8* out, const u8* in, const u32* table, bool encrypt)
{
    const u64 v = Permute(Be64(in), kIP, 64, 64);
    u32 l = u32(v >> 32), r = u32(v);
    for (int i = 0; i < 16; i++)
    {
        const int n = encrypt ? i : 15 - i;
        const u64 k = u64(table[n * 2]) << 24 | table[n * 2 + 1];
        const u32 t = r;
        r = l ^ Feistel(r, k);
        l = t;
    }
    PutBe64(out, Permute(u64(r) << 32 | l, kFP, 64, 64));
}

// cipher 0: DES; anything else: triple DES (encrypt, decrypt, encrypt with keys 1, 2, 3)
void CryptBlock(u32 cipher, u8* out, const u8* in, const u32* table, bool encrypt)
{
    if (cipher == 0)
    {
        DesBlock(out, in, table, encrypt);
        return;
    }
    u8 a[8], b[8];
    if (encrypt)
    {
        DesBlock(a, in, table, true);
        DesBlock(b, a, table + 32, false);
        DesBlock(out, b, table + 64, true);
    }
    else
    {
        DesBlock(a, in, table + 64, false);
        DesBlock(b, a, table + 32, true);
        DesBlock(out, b, table, false);
    }
}

// ---- unsigned big integers (32-bit limbs, least significant first)

using Big = std::vector<u32>;

void Trim(Big& a)
{
    while (!a.empty() && !a.back())
        a.pop_back();
}

Big FromBytes(const u8* p, u32 n) // little-endian bytes
{
    Big a((n + 3) / 4, 0);
    for (u32 i = 0; i < n; i++)
        a[i / 4] |= u32(p[i]) << (8 * (i % 4));
    Trim(a);
    return a;
}

void ToBytes(const Big& a, u8* p, u32 n)
{
    for (u32 i = 0; i < n; i++)
        p[i] = i / 4 < a.size() ? u8(a[i / 4] >> (8 * (i % 4))) : 0;
}

Big Mul(const Big& a, const Big& b)
{
    if (a.empty() || b.empty())
        return {};
    Big r(a.size() + b.size(), 0);
    for (size_t i = 0; i < a.size(); i++)
    {
        u64 carry = 0;
        for (size_t j = 0; j < b.size(); j++)
        {
            const u64 t = u64(a[i]) * b[j] + r[i + j] + carry;
            r[i + j] = u32(t);
            carry = t >> 32;
        }
        r[i + b.size()] = u32(carry);
    }
    Trim(r);
    return r;
}

int Compare(const Big& a, const Big& b)
{
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    for (size_t i = a.size(); i-- > 0;)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

Big Add(const Big& a, const Big& b)
{
    Big r(std::max(a.size(), b.size()) + 1, 0);
    u64 carry = 0;
    for (size_t i = 0; i < r.size(); i++)
    {
        const u64 t = u64(i < a.size() ? a[i] : 0) + (i < b.size() ? b[i] : 0) + carry;
        r[i] = u32(t);
        carry = t >> 32;
    }
    Trim(r);
    return r;
}

Big Sub(const Big& a, const Big& b) // a >= b
{
    Big r = a;
    s64 borrow = 0;
    for (size_t i = 0; i < r.size(); i++)
    {
        const s64 t = s64(r[i]) - (i < b.size() ? b[i] : 0) - borrow;
        r[i] = u32(t);
        borrow = t < 0;
    }
    Trim(r);
    return r;
}

// a mod m (m nonzero): Knuth's algorithm D, keeping only the remainder
Big Mod(const Big& a, const Big& m)
{
    if (Compare(a, m) < 0)
        return a;
    const size_t n = m.size();
    if (n == 1)
    {
        u64 r = 0;
        for (size_t i = a.size(); i-- > 0;)
            r = ((r << 32) | a[i]) % m[0];
        return r ? Big{u32(r)} : Big{};
    }
    const int s = __builtin_clz(m.back());
    Big v(n), u(a.size() + 1, 0);
    for (size_t i = n; i-- > 0;)
        v[i] = (m[i] << s) | (s && i ? m[i - 1] >> (32 - s) : 0);
    for (size_t i = a.size(); i-- > 0;)
        u[i] = (a[i] << s) | (s && i ? a[i - 1] >> (32 - s) : 0);
    u[a.size()] = s ? a.back() >> (32 - s) : 0;
    for (size_t j = a.size() - n + 1; j-- > 0;)
    {
        const u64 top = u64(u[j + n]) << 32 | u[j + n - 1];
        u64 q = top / v[n - 1], r = top % v[n - 1];
        while (q > 0xffffffffull || q * v[n - 2] > (r << 32 | u[j + n - 2]))
        {
            q--;
            r += v[n - 1];
            if (r > 0xffffffffull)
                break;
        }
        s64 borrow = 0;
        u64 carry = 0;
        for (size_t i = 0; i < n; i++)
        {
            const u64 p = q * v[i] + carry;
            carry = p >> 32;
            const s64 t = s64(u[i + j]) - s64(u32(p)) - borrow;
            u[i + j] = u32(t);
            borrow = t < 0;
        }
        const s64 t = s64(u[j + n]) - s64(carry) - borrow;
        u[j + n] = u32(t);
        if (t < 0) // q was one too many: add the divisor back
        {
            u64 c = 0;
            for (size_t i = 0; i < n; i++)
            {
                const u64 sum = u64(u[i + j]) + v[i] + c;
                u[i + j] = u32(sum);
                c = sum >> 32;
            }
            u[j + n] += u32(c);
        }
    }
    Big r(n);
    for (size_t i = 0; i < n; i++)
        r[i] = (u[i] >> s) | (s && i + 1 < u.size() ? u64(u[i + 1]) << (32 - s) : 0);
    Trim(r);
    return r;
}

Big ModExp(const Big& base, const Big& exponent, const Big& m)
{
    if (m.empty())
        return {};
    Big result = Mod(Big{1}, m), b = Mod(base, m);
    for (size_t i = exponent.size(); i-- > 0;)
        for (int bit = 31; bit >= 0; bit--)
        {
            result = Mod(Mul(result, result), m);
            if ((exponent[i] >> bit) & 1)
                result = Mod(Mul(result, b), m);
        }
    return result;
}

// ---- the console's RSA key blobs: magic ("RSA1" public, "RSA2" private), modulus buffer bytes,
// bit length, max encodable bytes, public exponent, then the modulus (bit length / 8 + 8 bytes,
// little-endian); a private key follows it with p, q, d mod (p-1), d mod (q-1), q^-1 mod p (each
// bit length / 16 + 4 bytes) and d.
constexpr u32 kRsa1 = 0x31415352, kRsa2 = 0x32415352;

u32 ModulusBytes(u32 key) { return Read<u32>(key + 8) / 8 + 8; }

// The PKCS #1 v1.5 block a SHA-1 digest is signed as, most significant byte first.
std::vector<u8> SignedBlock(const u8* digest, u32 bytes)
{
    static const u8 kSha1Info[15] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e,
                                     0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14};
    std::vector<u8> block(bytes, 0xff);
    if (bytes < 3 + 15 + 20)
        return {};
    block[0] = 0;
    block[1] = 1;
    block[bytes - 36] = 0;
    std::memcpy(&block[bytes - 35], kSha1Info, 15);
    std::memcpy(&block[bytes - 20], digest, 20);
    return block;
}

} // namespace

void RegisterCryptoExports(RegisterFn reg)
{
    reg("XcDESKeyParity", [](Call& c) -> u64 { // key, length: each byte gets odd parity
        u8* key = Ptr<u8>(c.Arg(0));
        for (u32 i = 0; i < c.Arg(1); i++)
            key[i] = u8((key[i] & 0xfe) | !(__builtin_popcount(key[i] >> 1) & 1));
        return 0;
    });
    reg("XcKeyTable", [](Call& c) -> u64 { // cipher (0 DES, else triple DES), table*, key*
        u32* table = Ptr<u32>(c.Arg(1));
        const u8* key = Ptr<u8>(c.Arg(2));
        for (int i = 0; i < (c.Arg(0) == 0 ? 1 : 3); i++)
            DesKeyTable(table + i * 32, key + i * 8);
        return 0;
    });
    // cipher, out*, in*, table*, operation: DES encrypts unless the operation is 0; triple DES only
    // when it is 1 (as the console does)
    reg("XcBlockCrypt", [](Call& c) -> u64 {
        const u32 cipher = c.Arg(0), op = c.Arg(4);
        CryptBlock(cipher, Ptr<u8>(c.Arg(1)), Ptr<u8>(c.Arg(2)), Ptr<u32>(c.Arg(3)), cipher == 0 ? op != 0 : op == 1);
        return 0;
    });
    // cipher, length, out*, in*, table*, operation (1 encrypts, else decrypts), feedback (IV)*: whole
    // blocks, the length rounded up
    reg("XcBlockCryptCBC", [](Call& c) -> u64 {
        const u32 cipher = c.Arg(0), blocks = (c.Arg(1) + 7) / 8;
        const u32* table = Ptr<u32>(c.Arg(4));
        const bool encrypt = c.Arg(5) == 1;
        u8* iv = Ptr<u8>(c.Arg(6));
        for (u32 b = 0; b < blocks; b++)
        {
            u8* out = Ptr<u8>(c.Arg(2) + b * 8);
            const u8* in = Ptr<u8>(c.Arg(3) + b * 8);
            u8 block[8];
            if (encrypt)
            {
                for (int i = 0; i < 8; i++)
                    block[i] = in[i] ^ iv[i];
                CryptBlock(cipher, out, block, table, true);
                std::memcpy(iv, out, 8);
            }
            else
            {
                u8 next_iv[8];
                std::memcpy(next_iv, in, 8);
                CryptBlock(cipher, block, in, table, false);
                for (int i = 0; i < 8; i++)
                    out[i] = block[i] ^ iv[i];
                std::memcpy(iv, next_iv, 8);
            }
        }
        return 0;
    });

    reg("XcModExp", [](Call& c) -> u64 { // out*, base*, exponent*, modulus*, length in dwords
        const u32 bytes = c.Arg(4) * 4;
        const Big m = FromBytes(Ptr<u8>(c.Arg(3)), bytes);
        if (m.empty())
            return 0;
        const Big r = ModExp(FromBytes(Ptr<u8>(c.Arg(1)), bytes), FromBytes(Ptr<u8>(c.Arg(2)), bytes), m);
        ToBytes(r, Ptr<u8>(c.Arg(0)), bytes);
        return 1;
    });
    reg("XcPKGetKeyLen", [](Call& c) -> u64 { return Read<u32>(c.Arg(0) + 4); }); // the modulus buffer
    reg("XcPKEncPublic", [](Call& c) -> u64 { // key*, in*, out*: in^e mod n
        const u32 key = c.Arg(0);
        if (Read<u32>(key) != kRsa1)
            return 0;
        const u32 bytes = ModulusBytes(key);
        const Big n = FromBytes(Ptr<u8>(key + 0x14), bytes);
        if (n.empty())
            return 0;
        const Big r = ModExp(FromBytes(Ptr<u8>(c.Arg(1)), bytes), FromBytes(Ptr<u8>(key + 0x10), 4), n);
        ToBytes(r, Ptr<u8>(c.Arg(2)), bytes);
        return 1;
    });
    reg("XcPKDecPrivate", [](Call& c) -> u64 { // key*, in*, out*: with the Chinese remainder theorem
        const u32 key = c.Arg(0);
        if (Read<u32>(key) != kRsa2)
            return 0;
        const u32 bytes = ModulusBytes(key), half = Read<u32>(key + 8) / 16 + 4;
        const u32 at = key + 0x14 + bytes;
        const Big n = FromBytes(Ptr<u8>(key + 0x14), bytes), in = FromBytes(Ptr<u8>(c.Arg(1)), bytes);
        const Big p = FromBytes(Ptr<u8>(at), half), q = FromBytes(Ptr<u8>(at + half), half);
        const Big dp = FromBytes(Ptr<u8>(at + half * 2), half), dq = FromBytes(Ptr<u8>(at + half * 3), half);
        const Big qinv = FromBytes(Ptr<u8>(at + half * 4), half);
        Big r;
        if (!p.empty() && !q.empty())
        {
            const Big m1 = ModExp(in, dp, p), m2 = ModExp(in, dq, q);
            const Big diff = Sub(Add(m1, p), Mod(m2, p));
            r = Add(m2, Mul(Mod(Mul(qinv, Mod(diff, p)), p), q));
        }
        else if (!n.empty())
            r = ModExp(in, FromBytes(Ptr<u8>(at + half * 5), bytes), n);
        ToBytes(r, Ptr<u8>(c.Arg(2)), bytes);
        return 1;
    });
    // signature*, public key*, SHA-1 digest*: whether the signature is the digest's, signed with the
    // key. Signatures under the console's own key (XePublicKeyData, which isn't here) are taken as
    // valid: what it signs, a program can't check here otherwise.
    reg("XcVerifyPKCS1Signature", [](Call& c) -> u64 {
        const u32 key = c.Arg(1);
        if (Read<u32>(key) != kRsa1 || !Read<u32>(key + 0x14))
            return 1;
        const u32 bytes = ModulusBytes(key), length = Read<u32>(key + 8) / 8;
        const Big n = FromBytes(Ptr<u8>(key + 0x14), bytes);
        const Big r = ModExp(FromBytes(Ptr<u8>(c.Arg(0)), length), FromBytes(Ptr<u8>(key + 0x10), 4), n);
        std::vector<u8> got(length);
        ToBytes(r, got.data(), length);
        const std::vector<u8> want = SignedBlock(Ptr<u8>(c.Arg(2)), length);
        for (u32 i = 0; i < length && !want.empty(); i++)
            if (got[length - 1 - i] != want[i]) // the result is little-endian
                return 0;
        return want.empty() ? 0 : 1;
    });
}

} // namespace xb
