// XBE loader: the Xbox's executable format. The headers go to the base address (games read them
// there: the certificate, the TLS directory, library versions), each section to its own address.
#include <cstdio>
#include <fstream>
#include <iterator>

#include "core.h"
#include "xiso.h"

namespace xb {

XbeInfo g_xbe;

namespace {
// The entry point and kernel thunk table addresses are XORed with a key: retail, debug or Chihiro.
struct Keys
{
    const char* kind;
    u32 entry, thunk;
};
constexpr Keys kKeys[] = {
    {"retail", 0xA8FC57AB, 0x5B6D40B6},
    {"debug", 0x94859D4B, 0xEFB1F152},
    {"chihiro", 0x40B5C16E, 0x2290059D},
};
u32 Get32(const std::vector<u8>& d, size_t at)
{
    u32 v = 0;
    if (at + 4 <= d.size())
        std::memcpy(&v, d.data() + at, 4);
    return v;
}
} // namespace

bool LoadXbe(const std::string& path, XbeInfo& info, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        error = "can't open " + path;
        return false;
    }
    const std::vector<u8> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return LoadXbeData(d, path, info, error);
}

bool ReadXbeRange(const XbeInfo& info, u32 offset, u32 n, void* out)
{
    if (info.path.rfind("xiso:", 0) == 0)
    {
        const IsoEntry* e = IsoFind(info.path.substr(5));
        return e && IsoRead(*e, offset, n, out) == n;
    }
    FILE* f = std::fopen(info.path.c_str(), "rb");
    if (!f)
        return false;
    const bool ok = std::fseek(f, long(offset), SEEK_SET) == 0 && std::fread(out, 1, n, f) == n;
    std::fclose(f);
    return ok;
}

bool LoadXbeData(const std::vector<u8>& d, const std::string& path, XbeInfo& info, std::string& error)
{
    if (d.size() < 0x178 || std::memcmp(d.data(), "XBEH", 4) != 0)
    {
        error = "not an XBE";
        return false;
    }
    info.path = path;
    info.base = Get32(d, 0x104);
    const u32 headers_size = Get32(d, 0x108);
    info.image_size = Get32(d, 0x10c);
    const u32 certificate = Get32(d, 0x118);
    const u32 section_count = Get32(d, 0x11c), section_headers = Get32(d, 0x120);
    info.init_flags = Get32(d, 0x124);
    const u32 entry = Get32(d, 0x128);
    info.tls_directory = Get32(d, 0x12c);
    const u32 thunk = Get32(d, 0x158);
    const char* kind = nullptr;
    for (const Keys& k : kKeys)
    {
        const u32 e = entry ^ k.entry, t = thunk ^ k.thunk;
        if (e >= info.base && e < info.base + info.image_size && t >= info.base && t < info.base + info.image_size)
        {
            info.entry = e, info.thunk_table = t, kind = k.kind;
            break;
        }
    }
    if (!kind)
    {
        error = "entry point / thunk table keys not recognized";
        return false;
    }
    if (info.base < kRamBase || u64(info.base) + info.image_size > kRamEnd || headers_size > d.size())
    {
        error = "image outside the RAM range";
        return false;
    }
    if (AllocVirtual(info.image_size, info.base) != info.base)
    {
        error = "image range taken";
        return false;
    }
    std::memcpy(Ptr(info.base), d.data(), headers_size);
    for (u32 i = 0; i < section_count; i++)
    {
        const size_t h = section_headers - info.base + size_t(i) * 56;
        const u32 va = Get32(d, h + 4), vsize = Get32(d, h + 8), raw = Get32(d, h + 12), rsize = Get32(d, h + 16);
        if (va < info.base || u64(va) + std::max(vsize, rsize) > u64(info.base) + info.image_size ||
            u64(raw) + rsize > d.size())
        {
            error = "section outside the image";
            return false;
        }
        std::memcpy(Ptr(va), d.data() + raw, std::min(rsize, vsize ? vsize : rsize));
        // the preloaded sections are loaded once, with their shared head and tail pages counted (the
        // others are in memory too, but count as unloaded until XeLoadSection)
        const u32 g = section_headers + i * 56;
        if (Read<u32>(g) & 0x2)
        {
            Write<u32>(g + 24, 1);
            for (u32 ref : {Read<u32>(g + 28), Read<u32>(g + 32)})
                if (ref)
                    Write<u16>(ref, u16(Read<u16>(ref) + 1));
        }
    }
    info.section_count = section_count;
    info.section_headers = section_headers;
    if (info.tls_directory)
    {
        info.tls_raw_start = Read<u32>(info.tls_directory);
        info.tls_raw_end = Read<u32>(info.tls_directory + 4);
        info.tls_index_address = Read<u32>(info.tls_directory + 8);
        info.tls_zero_fill = Read<u32>(info.tls_directory + 16);
    }
    // the title: UTF-16 in the certificate, 40 characters
    if (certificate >= info.base && certificate - info.base + 0x0c + 80 <= d.size())
        for (int i = 0; i < 40; i++)
        {
            const u16 c = u16(d[certificate - info.base + 0x0c + i * 2] | d[certificate - info.base + 0x0d + i * 2] << 8);
            if (!c)
                break;
            info.title += c < 0x80 ? char(c) : '?';
        }
    Log("xbe: \"%s\" (%s), base %#x, image %#x, entry %#x, kernel thunks at %#x, TLS %#x", info.title.c_str(), kind,
        info.base, info.image_size, info.entry, info.thunk_table, info.tls_directory);
    return true;
}

} // namespace xb
