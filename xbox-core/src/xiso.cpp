// XDVDFS: 2048-byte sectors; the volume descriptor at sector 32 of the game partition says
// "MICROSOFT*XBOX*MEDIA" and where the root directory is. A directory is a binary tree of entries
// (left and right subtree offsets in 4-byte units, the data's sector and size, attributes, the
// name); 0xffff marks padding. Full dumps hold the game partition after a video partition, at an
// offset that depends on the disc's generation.
#include "xiso.h"

#include <cstring>
#include <fcntl.h>
#include <set>
#include <unistd.h>

namespace xb {

namespace {

constexpr u32 kSector = 2048;
const char kMagic[] = "MICROSOFT*XBOX*MEDIA";

int g_fd = -1;
u64 g_base = 0; // the game partition's offset in the image
s64 g_time = 0;
std::vector<IsoEntry> g_entries; // [0] is the root

std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ReadAt(u64 at, void* out, size_t n)
{
    return pread(g_fd, out, n, off_t(at)) == ssize_t(n);
}

// Adds the entries of the tree at `node` (a byte offset in the directory's data) to `dir`.
void Walk(const std::vector<u8>& data, u32 node, u32 dir, std::set<u32>& seen, int depth)
{
    if (depth > 64 || node + 14 > data.size() || !seen.insert(node).second)
        return;
    u16 left, right;
    std::memcpy(&left, &data[node], 2);
    std::memcpy(&right, &data[node + 2], 2);
    if (left == 0xffff)
        return; // padding: no entry here
    u32 sector, size;
    std::memcpy(&sector, &data[node + 4], 4);
    std::memcpy(&size, &data[node + 8], 4);
    const u8 attributes = data[node + 12], length = data[node + 13];
    if (node + 14 + length > data.size())
        return;
    IsoEntry e;
    e.name.assign(reinterpret_cast<const char*>(&data[node + 14]), length);
    e.offset = g_base + u64(sector) * kSector;
    e.size = size;
    e.directory = attributes & 0x10;
    const u32 index = u32(g_entries.size());
    g_entries.push_back(e);
    g_entries[dir].children.push_back(index);
    if (left)
        Walk(data, u32(left) * 4, dir, seen, depth + 1);
    if (right)
        Walk(data, u32(right) * 4, dir, seen, depth + 1);
}

void LoadDirectory(u32 index, int depth)
{
    if (depth > 32)
        return;
    const IsoEntry dir = g_entries[index];
    if (dir.size == 0)
        return;
    std::vector<u8> data(dir.size);
    if (!ReadAt(dir.offset, data.data(), data.size()))
        return;
    std::set<u32> seen;
    Walk(data, 0, index, seen, 0);
    const std::vector<u32> children = g_entries[index].children;
    for (u32 child : children)
        if (g_entries[child].directory)
            LoadDirectory(child, depth + 1);
}

} // namespace

bool MountIso(const std::string& path, std::string& error)
{
    if (g_fd >= 0)
        close(g_fd);
    g_entries.clear();
    g_fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (g_fd < 0)
    {
        error = "can't open " + path;
        return false;
    }
    // XISO (0), then full dumps of the three disc generations
    for (u64 base : {0ull, 0x18300000ull, 0xfd90000ull, 0x2080000ull})
    {
        u8 descriptor[kSector];
        if (!ReadAt(base + 32 * kSector, descriptor, sizeof(descriptor)) || std::memcmp(descriptor, kMagic, 20) != 0)
            continue;
        g_base = base;
        u32 root_sector, root_size;
        std::memcpy(&root_sector, descriptor + 20, 4);
        std::memcpy(&root_size, descriptor + 24, 4);
        std::memcpy(&g_time, descriptor + 28, 8);
        IsoEntry root;
        root.directory = true;
        root.offset = base + u64(root_sector) * kSector;
        root.size = root_size;
        g_entries.push_back(root);
        LoadDirectory(0, 0);
        Log("xiso: %s: %zu files and folders (game partition at %#llx)", path.c_str(), g_entries.size() - 1,
            (unsigned long long)base);
        return true;
    }
    error = path + " isn't an Xbox disc image (no XDVDFS volume found)";
    close(g_fd);
    g_fd = -1;
    return false;
}

bool IsoMounted()
{
    return g_fd >= 0;
}

const IsoEntry* IsoFind(const std::string& path)
{
    if (g_entries.empty())
        return nullptr;
    u32 at = 0;
    size_t i = 0;
    while (i < path.size())
    {
        size_t end = path.find('/', i);
        if (end == std::string::npos)
            end = path.size();
        const std::string part = Lower(path.substr(i, end - i));
        i = end + 1;
        if (part.empty() || part == ".")
            continue;
        if (!g_entries[at].directory)
            return nullptr;
        bool found = false;
        for (u32 child : g_entries[at].children)
            if (Lower(g_entries[child].name) == part)
            {
                at = child, found = true;
                break;
            }
        if (!found)
            return nullptr;
    }
    return &g_entries[at];
}

const IsoEntry& IsoEntryAt(u32 index)
{
    return g_entries[index];
}

size_t IsoRead(const IsoEntry& e, u64 at, size_t n, void* out)
{
    if (at >= e.size)
        return 0;
    n = std::min<u64>(n, e.size - at);
    const ssize_t got = pread(g_fd, out, n, off_t(e.offset + at));
    return got < 0 ? 0 : size_t(got);
}

s64 IsoTime()
{
    return g_time;
}

} // namespace xb
