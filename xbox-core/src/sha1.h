// SHA-1, for XcSHA* and XcHMAC (files.cpp, kernel_rtl.cpp).
#pragma once

#include "core.h"

namespace xb {

// The state is kept in the guest's context (0x74 bytes on the Xbox)
struct Sha1
{
    u32 h[5];
    u64 length;
    u8 buffer[64];
    u32 used;

    void Init()
    {
        h[0] = 0x67452301, h[1] = 0xEFCDAB89, h[2] = 0x98BADCFE, h[3] = 0x10325476, h[4] = 0xC3D2E1F0;
        length = 0, used = 0;
    }
    static u32 Rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }
    void Block(const u8* p)
    {
        u32 w[80];
        for (int i = 0; i < 16; i++)
            w[i] = u32(p[i * 4]) << 24 | u32(p[i * 4 + 1]) << 16 | u32(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; i++)
            w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++)
        {
            u32 f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            const u32 t = Rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = Rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    void Update(const u8* p, u32 n)
    {
        length += n;
        while (n--)
        {
            buffer[used++] = *p++;
            if (used == 64)
                Block(buffer), used = 0;
        }
    }
    void Final(u8* out)
    {
        const u64 bits = length * 8;
        const u8 one = 0x80, zero = 0;
        Update(&one, 1);
        while (used != 56)
            Update(&zero, 1);
        for (int i = 7; i >= 0; i--)
        {
            const u8 b = u8(bits >> (i * 8));
            Update(&b, 1);
        }
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                out[i * 4 + j] = u8(h[i] >> (24 - j * 8));
    }
};
static_assert(sizeof(Sha1) <= 0x74, "SHA-1 state must fit the Xbox's context");

} // namespace xb
