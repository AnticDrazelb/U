// NRO assets (nro.h): the header's total size at 0x18 ("NRO0" at 0x10); after that many bytes,
// "ASET", a version, and the icon's, NACP's and RomFS's offsets and sizes (from the asset header).
// The NACP starts with 16 languages' names (0x200 bytes) and publishers (0x100): the first that
// isn't empty is used.

#include "ui/nro.h"

#include <cstdio>
#include <cstring>

namespace Ui {

namespace {

template <typename T>
T Get(const unsigned char* p)
{
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

std::string CString(const char* text, size_t max)
{
    return std::string(text, strnlen(text, max));
}

} // namespace

bool ReadNroInfo(const std::string& path, NroInfo& info)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    bool ok = false;
    unsigned char header[0x20];
    if (std::fread(header, 1, sizeof(header), f) == sizeof(header) && std::memcmp(header + 0x10, "NRO0", 4) == 0)
    {
        const uint32_t size = Get<uint32_t>(header + 0x18);
        unsigned char assets[0x38];
        if (std::fseek(f, long(size), SEEK_SET) == 0 && std::fread(assets, 1, sizeof(assets), f) == sizeof(assets) &&
            std::memcmp(assets, "ASET", 4) == 0)
        {
            const uint64_t icon_offset = Get<uint64_t>(assets + 0x08), icon_size = Get<uint64_t>(assets + 0x10);
            const uint64_t nacp_offset = Get<uint64_t>(assets + 0x18), nacp_size = Get<uint64_t>(assets + 0x20);
            if (nacp_size >= 0x3000 && std::fseek(f, long(size + nacp_offset), SEEK_SET) == 0)
            {
                std::vector<char> nacp(0x3000);
                if (std::fread(nacp.data(), 1, nacp.size(), f) == nacp.size())
                {
                    for (int language = 0; language < 16 && info.name.empty(); language++)
                    {
                        info.name = CString(&nacp[language * 0x300], 0x200);
                        info.publisher = CString(&nacp[language * 0x300 + 0x200], 0x100);
                    }
                }
            }
            if (icon_size > 0 && icon_size < 4 * 1024 * 1024 && std::fseek(f, long(size + icon_offset), SEEK_SET) == 0)
            {
                info.icon.resize(size_t(icon_size));
                if (std::fread(info.icon.data(), 1, info.icon.size(), f) != info.icon.size())
                    info.icon.clear();
            }
            ok = true;
        }
    }
    std::fclose(f);
    return ok;
}

} // namespace Ui
