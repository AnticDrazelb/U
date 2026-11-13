#include "library.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

namespace fe {
namespace {

bool EndsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool Exists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

uint64_t FileSize(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;
}

uint32_t Get32(const std::vector<uint8_t>& d, size_t at)
{
    return at + 4 <= d.size() ? uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24 : 0;
}

bool ReadAt(FILE* f, uint64_t at, void* out, size_t n)
{
    return fseeko(f, off_t(at), SEEK_SET) == 0 && std::fread(out, 1, n, f) == n;
}

// An XBE's header (enough for the certificate): its title, title ID and region.
void ParseXbe(const std::vector<uint8_t>& d, Game& g)
{
    if (d.size() < 0x180 || std::memcmp(d.data(), "XBEH", 4) != 0)
        return;
    const uint32_t base = Get32(d, 0x104), cert = Get32(d, 0x118) - base;
    if (cert + 0xa4 > d.size())
        return;
    g.title_id = Get32(d, cert + 0x8);
    g.region = Get32(d, cert + 0xa0);
    std::string title;
    for (int i = 0; i < 40; i++)
    {
        const uint32_t c = uint32_t(d[cert + 0xc + i * 2]) | uint32_t(d[cert + 0xd + i * 2]) << 8;
        if (!c)
            break;
        // UTF-16 (its basic plane) to UTF-8
        if (c < 0x80)
            title += char(c);
        else if (c < 0x800)
            title += char(0xc0 | c >> 6), title += char(0x80 | (c & 63));
        else
            title += char(0xe0 | c >> 12), title += char(0x80 | ((c >> 6) & 63)), title += char(0x80 | (c & 63));
    }
    while (!title.empty() && title.back() == ' ')
        title.pop_back();
    if (!title.empty())
        g.title = title;
}

std::vector<uint8_t> ReadHead(FILE* f, uint64_t at, size_t limit)
{
    std::vector<uint8_t> d(limit);
    if (fseeko(f, off_t(at), SEEK_SET) != 0)
        return {};
    d.resize(std::fread(d.data(), 1, d.size(), f));
    return d;
}

// A disc image's default.xbe header: the XDVDFS volume (at the image's start, or after a full
// dump's video partition), its root directory searched by name.
std::vector<uint8_t> IsoXbeHead(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return {};
    std::vector<uint8_t> out;
    for (uint64_t base : {0ull, 0x18300000ull, 0xfd90000ull, 0x2080000ull})
    {
        uint8_t vd[0x20];
        if (!ReadAt(f, base + 0x10000, vd, sizeof(vd)) || std::memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20) != 0)
            continue;
        const uint32_t root_sector = uint32_t(vd[0x14]) | uint32_t(vd[0x15]) << 8 | uint32_t(vd[0x16]) << 16 | uint32_t(vd[0x17]) << 24;
        const uint32_t root_size = uint32_t(vd[0x18]) | uint32_t(vd[0x19]) << 8 | uint32_t(vd[0x1a]) << 16 | uint32_t(vd[0x1b]) << 24;
        std::vector<uint8_t> dir(std::min<uint32_t>(root_size, 1 << 20));
        if (!ReadAt(f, base + uint64_t(root_sector) * 2048, dir.data(), dir.size()))
            break;
        // entries: left, right (u16), sector, size (u32), attributes, name length, name; 4-byte
        // aligned; 0xffff pads to the next sector
        for (size_t at = 0; at + 14 <= dir.size();)
        {
            if (dir[at] == 0xff && dir[at + 1] == 0xff)
            {
                at = (at / 2048 + 1) * 2048;
                continue;
            }
            const uint32_t sector = Get32(dir, at + 4), size = Get32(dir, at + 8);
            const size_t len = dir[at + 13];
            if (at + 14 + len > dir.size())
                break;
            const std::string name(reinterpret_cast<const char*>(&dir[at + 14]), len);
            if (strcasecmp(name.c_str(), "default.xbe") == 0)
            {
                out = ReadHead(f, base + uint64_t(sector) * 2048, std::min<uint32_t>(size, 0x10000));
                break;
            }
            at = (at + 14 + len + 3) & ~size_t(3);
        }
        break;
    }
    std::fclose(f);
    return out;
}

} // namespace

std::string CoverKey(const Game& g)
{
    // its title ID, unless it's nxdk's (every homebrew's: FFFF0002) or none: then its file's name
    if (g.title_id && g.title_id != 0xFFFF0002u)
    {
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", g.title_id);
        return id;
    }
    std::string key;
    for (char c : g.file_name)
        key += (c == '/' || c == '\\' || c == ':') ? '_' : c;
    return key;
}

namespace {

// cover art: beside the game (cover.jpg in its folder, <image>.jpg beside an image) or in the
// covers folder by title ID (8 hex digits) or file name
std::string FindCover(const Game& g, const std::string& folder, const std::string& stem, const std::string& covers)
{
    std::vector<std::string> candidates;
    // first, a picture the player imported for it (the menu's Box Art): theirs over any other
    if (!covers.empty())
        for (const char* ext : {".jpg", ".png", ".jpeg"})
            candidates.push_back(covers + "/imported/" + CoverKey(g) + ext);
    for (const char* ext : {".jpg", ".png", ".jpeg"})
    {
        if (!folder.empty())
        {
            candidates.push_back(folder + "/cover" + ext);
            candidates.push_back(folder + "/default" + ext);
        }
        candidates.push_back(stem + ext);
        if (!covers.empty())
        {
            char id[16];
            std::snprintf(id, sizeof(id), "%08X", g.title_id);
            if (g.title_id)
                candidates.push_back(covers + "/" + id + ext);
            candidates.push_back(covers + "/" + g.file_name + ext);
        }
    }
    for (const std::string& c : candidates)
        if (Exists(c))
            return c;
    return "";
}

Game Describe(const std::string& path, const std::string& file_name, const std::string& folder, const std::string& kind,
              const std::string& covers)
{
    Game g;
    g.path = path, g.file_name = file_name, g.title = file_name, g.kind = kind, g.folder = folder;
    g.size = FileSize(path);
    if (IsDiscImageName(path))
        ParseXbe(IsoXbeHead(path), g);
    else if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        ParseXbe(ReadHead(f, 0, 0x10000), g);
        std::fclose(f);
    }
    const std::string stem = EndsWith(path, ".xiso") ? path.substr(0, path.size() - 5)
                             : EndsWith(path, ".iso") || EndsWith(path, ".xbe") ? path.substr(0, path.size() - 4) : path;
    g.cover = FindCover(g, folder, stem, covers);
    return g;
}

struct HistoryEntry
{
    int plays = 0;
    int64_t last = 0;
    std::string result;
};

std::map<std::string, HistoryEntry> LoadHistory(const std::string& file)
{
    std::map<std::string, HistoryEntry> out;
    FILE* f = std::fopen(file.c_str(), "r");
    if (!f)
        return out;
    char line[2048];
    while (std::fgets(line, sizeof(line), f))
    {
        // path \t plays \t last \t result
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        std::vector<std::string> parts;
        size_t at = 0;
        for (int i = 0; i < 3; i++)
        {
            const size_t tab = s.find('\t', at);
            if (tab == std::string::npos)
                break;
            parts.push_back(s.substr(at, tab - at));
            at = tab + 1;
        }
        if (parts.size() < 3)
            continue;
        out[parts[0]] = {std::atoi(parts[1].c_str()), std::atoll(parts[2].c_str()), s.substr(at)};
    }
    std::fclose(f);
    return out;
}

void WriteHistory(const std::string& file, const std::map<std::string, HistoryEntry>& history)
{
    FILE* f = std::fopen(file.c_str(), "w");
    if (!f)
        return;
    for (const auto& [path, e] : history)
        std::fprintf(f, "%s\t%d\t%lld\t%s\n", path.c_str(), e.plays, (long long)e.last, e.result.c_str());
    std::fclose(f);
}

} // namespace

bool IsDiscImageName(const std::string& name) { return EndsWith(name, ".iso") || EndsWith(name, ".xiso"); }

std::string UsbDrive(int i)
{
    const std::string mnt = "/mnt/usb" + std::to_string(i), root = "/usb" + std::to_string(i);
    struct stat st;
    if (stat(mnt.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        return mnt;
    if (stat(root.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        return root;
    return "";
}

std::vector<Game> ScanLibrary(const LibraryPaths& paths)
{
    std::vector<Game> out;
    for (const std::string& dir : paths.sample_dirs)
        if (DIR* d = opendir(dir.c_str()))
        {
            while (dirent* e = readdir(d))
            {
                const std::string name = e->d_name;
                if (EndsWith(name, ".xbe") || IsDiscImageName(name))
                {
                    Game g = Describe(dir + "/" + name, name.substr(0, name.find_last_of('.')), "", "Sample", paths.covers);
                    g.sample = true;
                    out.push_back(g);
                }
            }
            closedir(d);
        }
    // the games folder, then the USB drives (their tops, and their games, xbox and xbox-core/games
    // folders): disc images, folders with a default.xbe, folders holding a disc image
    auto scan = [&](const std::string& dir, const std::string& usb) {
        DIR* d = opendir(dir.c_str());
        if (!d)
            return;
        while (dirent* e = readdir(d))
        {
            const std::string name = e->d_name, full = dir + "/" + name;
            if (name[0] == '.')
                continue;
            // a drive's games folders are scanned themselves, not taken for a game holding a disc image
            if (!usb.empty() && dir == usb &&
                (name == "games" || name == "xbox" || name == "Xbox" || name == "xbox-core"))
                continue;
            Game g;
            if (IsDiscImageName(name))
                g = Describe(full, name.substr(0, name.find_last_of('.')), "", "Disc image", paths.covers);
            else if (Exists(full + "/default.xbe"))
                g = Describe(full + "/default.xbe", name, full, "Game folder", paths.covers);
            else if (DIR* inner = opendir(full.c_str()))
            {
                while (dirent* f = readdir(inner))
                    if (IsDiscImageName(f->d_name))
                    {
                        g = Describe(full + "/" + f->d_name, name, full, "Disc image", paths.covers);
                        break;
                    }
                closedir(inner);
            }
            if (g.path.empty())
                continue;
            g.usb = usb;
            if (std::none_of(out.begin(), out.end(), [&](const Game& o) { return o.path == g.path; }))
                out.push_back(g);
        }
        closedir(d);
    };
    scan(paths.games, "");
    for (const std::string& drive : paths.usb_drives)
        for (const char* sub : {"", "/games", "/xbox", "/Xbox", "/xbox-core/games"})
            scan(drive + sub, drive);
    const auto history = LoadHistory(paths.history);
    for (Game& g : out)
        if (auto it = history.find(g.path); it != history.end())
            g.plays = it->second.plays, g.last_played = it->second.last, g.last_result = it->second.result;
    return out;
}

void RecordRun(const std::string& file, const std::string& path, const std::string& result)
{
    auto history = LoadHistory(file);
    HistoryEntry& e = history[path];
    if (result.empty())
    {
        e.plays++;
        e.last = int64_t(std::time(nullptr));
        e.result = "Started";
    }
    else
        e.result = result;
    // tabs and line breaks would break the file
    for (char& c : e.result)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    WriteHistory(file, history);
}

void SaveHistory(const std::string& file, const std::vector<Game>& games)
{
    auto history = LoadHistory(file);
    for (const Game& g : games)
        if (g.plays)
            history[g.path] = {g.plays, g.last_played, g.last_result};
    WriteHistory(file, history);
}

std::string RegionName(uint32_t region)
{
    std::string out;
    auto add = [&](const char* s) { out += out.empty() ? s : std::string(", ") + s; };
    if (region & 1) add("North America");
    if (region & 2) add("Japan");
    if (region & 4) add("Europe and elsewhere");
    if (region & 0x80000000u) add("Debug");
    return out.empty() ? "Unknown" : out;
}

std::string FormatSize(uint64_t bytes)
{
    char buf[32];
    if (bytes >= (1ull << 30))
        std::snprintf(buf, sizeof(buf), "%.2f GB", double(bytes) / double(1ull << 30));
    else if (bytes >= (1ull << 20))
        std::snprintf(buf, sizeof(buf), "%.1f MB", double(bytes) / double(1ull << 20));
    else
        std::snprintf(buf, sizeof(buf), "%.0f KB", double(bytes) / 1024.0);
    return buf;
}

std::string FormatDate(int64_t when)
{
    if (!when)
        return "Never";
    const time_t t = time_t(when);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%b %d, %Y %H:%M", std::localtime(&t));
    return buf;
}

bool Settings::Load(const std::string& file)
{
    FILE* f = std::fopen(file.c_str(), "r");
    if (!f)
        return false;
    // "key value" lines; the value runs to the end of the line (a nickname can have spaces)
    char line[256];
    while (std::fgets(line, sizeof(line), f))
    {
        std::string text = line;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        const size_t space = text.find(' ');
        if (space == std::string::npos)
            continue;
        const std::string key = text.substr(0, space), word = text.substr(space + 1);
        const int value = std::atoi(word.c_str());
        if (key == "show_log") show_log = value != 0;
        else if (key == "picture") picture = std::clamp(value, 0, 2);
        else if (key == "sort") sort = std::clamp(value, 0, 1);
        // the GPU's emulation, under a key of its own each time a choice of before shouldn't hold:
        // "nv2a" before xemu's renderer fit the PS4's GPU memory; "renderer" before the HOME menu
        // put it out of the way (it was the first item of Graphics, where a second press of Cross
        // changes it unseen: Midway's own NV2A chosen by accident). Unset, it's xemu's, and a start
        // that dies in its start-up is followed by one with Midway's own.
        else if (key == "gpu") nv2a = std::clamp(value, 0, 1);
        else if (key == "dsp") dsp = std::clamp(value, 0, 1);
        else if (key == "theme") theme = std::clamp(value, 0, 2);
        else if (key == "logs") logs = value != 0;
        else if (key == "nickname") nickname = word.substr(0, 40);
        else if (key == "avatar") avatar = std::max(0, value);
        else if (key == "avatar_color") avatar_color = std::max(0, value);
    }
    std::fclose(f);
    return true;
}

void Settings::Save(const std::string& file) const
{
    if (FILE* f = std::fopen(file.c_str(), "w"))
    {
        std::fprintf(f, "show_log %d\npicture %d\nsort %d\ngpu %d\ndsp %d\ntheme %d\nlogs %d\navatar %d\navatar_color %d\n",
                     show_log ? 1 : 0, picture, sort, nv2a, dsp, theme, logs ? 1 : 0, avatar, avatar_color);
        if (!nickname.empty())
            std::fprintf(f, "nickname %s\n", nickname.c_str());
        std::fclose(f);
    }
}

} // namespace fe
