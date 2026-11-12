#include "tools.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace fe {
namespace {

bool IsDir(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsFile(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool IsIso(const std::string& n) { return IsDiscImageName(n); }

uint64_t TreeSize(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return uint64_t(st.st_size);
    uint64_t total = 0;
    if (DIR* d = opendir(path.c_str()))
    {
        while (dirent* e = readdir(d))
            if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, ".."))
                total += TreeSize(path + "/" + e->d_name);
        closedir(d);
    }
    return total;
}

void RemoveTree(const std::string& path)
{
    if (IsDir(path))
    {
        if (DIR* d = opendir(path.c_str()))
        {
            while (dirent* e = readdir(d))
                if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, ".."))
                    RemoveTree(path + "/" + e->d_name);
            closedir(d);
        }
        rmdir(path.c_str());
    }
    else
        unlink(path.c_str());
}

double Now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::vector<std::string> LastLines(const std::string& file, size_t n)
{
    std::deque<std::string> lines;
    if (FILE* f = std::fopen(file.c_str(), "r"))
    {
        char buf[1024];
        while (std::fgets(buf, sizeof(buf), f))
        {
            std::string l = buf;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
                l.pop_back();
            lines.push_back(l);
            if (lines.size() > n)
                lines.pop_front();
        }
        std::fclose(f);
    }
    return {lines.begin(), lines.end()};
}

} // namespace

std::vector<std::string> UsbRoots(const std::vector<std::string>& candidates)
{
    std::vector<std::string> out;
    for (const std::string& c : candidates)
        if (DIR* d = opendir(c.c_str()))
        {
            // a mount point with something in it (an empty folder is a drive that isn't there)
            bool any = false;
            while (dirent* e = readdir(d))
                if (e->d_name[0] != '.')
                {
                    any = true;
                    break;
                }
            closedir(d);
            if (any)
                out.push_back(c);
        }
    return out;
}

std::vector<UsbGame> FindUsbGames(const std::vector<std::string>& roots, const std::string& games_dir)
{
    std::vector<UsbGame> out;
    for (const std::string& root : roots)
        for (const char* sub : {"", "/games", "/xbox", "/xbox-core/games", "/Xbox"})
        {
            const std::string dir = root + sub;
            DIR* d = opendir(dir.c_str());
            if (!d)
                continue;
            while (dirent* e = readdir(d))
            {
                const std::string name = e->d_name, full = dir + "/" + name;
                if (name[0] == '.')
                    continue;
                UsbGame g;
                g.source = full, g.name = name;
                if (IsIso(name) && IsFile(full))
                    g.size = TreeSize(full);
                else if (IsFile(full + "/default.xbe"))
                    g.folder = true, g.size = TreeSize(full);
                else
                    continue;
                g.on_hdd = IsFile(games_dir + "/" + name) || IsDir(games_dir + "/" + name);
                if (std::none_of(out.begin(), out.end(), [&](const UsbGame& o) { return o.source == g.source; }))
                    out.push_back(g);
            }
            closedir(d);
        }
    std::sort(out.begin(), out.end(), [](const UsbGame& a, const UsbGame& b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

uint64_t FreeSpace(const std::string& path)
{
    struct statvfs s;
    if (statvfs(path.c_str(), &s) != 0)
        return 0;
    return uint64_t(s.f_bavail) * uint64_t(s.f_frsize ? s.f_frsize : s.f_bsize);
}

// ---- copying

Copier::~Copier()
{
    Cancel();
    if (worker.joinable())
        worker.join();
}

void Copier::Queue(const UsbGame& game, const std::string& games_dir)
{
    std::lock_guard lock(mutex);
    jobs.push_back({game, games_dir});
    if (!running)
    {
        if (worker.joinable())
            worker.join();
        running = true;
        cancel = false;
        worker = std::thread([this] { Run(); });
    }
}

void Copier::Cancel()
{
    std::lock_guard lock(mutex);
    jobs.clear();
    cancel = true;
}

Copier::Status Copier::Poll()
{
    std::lock_guard lock(mutex);
    Status s;
    s.busy = running;
    s.current = current;
    s.done = done, s.total = total;
    const double t = Now() - started;
    s.rate = t > 0.5 ? double(s.done) / t : 0;
    s.queued = jobs.size();
    s.finished.swap(finished);
    return s;
}

bool Copier::CopyFile(const std::string& from, const std::string& to, std::string& error)
{
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in)
        return error = "can't read " + from, false;
    FILE* out = std::fopen(to.c_str(), "wb");
    if (!out)
    {
        std::fclose(in);
        return error = "can't write " + to, false;
    }
    std::vector<char> buffer(4 << 20);
    bool ok = true;
    for (;;)
    {
        if (cancel)
        {
            ok = false, error = "cancelled";
            break;
        }
        const size_t n = std::fread(buffer.data(), 1, buffer.size(), in);
        if (n == 0)
        {
            if (std::ferror(in))
                ok = false, error = "reading " + from + " failed";
            break;
        }
        if (std::fwrite(buffer.data(), 1, n, out) != n)
        {
            ok = false, error = "writing failed (is the drive full?)";
            break;
        }
        done += n;
    }
    std::fclose(in);
    if (std::fclose(out) != 0 && ok)
        ok = false, error = "writing failed (is the drive full?)";
    return ok;
}

bool Copier::CopyTree(const std::string& from, const std::string& to, std::string& error)
{
    if (!IsDir(from))
        return CopyFile(from, to, error);
    mkdir(to.c_str(), 0777);
    DIR* d = opendir(from.c_str());
    if (!d)
        return error = "can't read " + from, false;
    bool ok = true;
    while (dirent* e = readdir(d))
        if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, ".."))
            if (!CopyTree(from + "/" + e->d_name, to + "/" + e->d_name, error))
            {
                ok = false;
                break;
            }
    closedir(d);
    return ok;
}

void Copier::Run()
{
    for (;;)
    {
        Job job;
        {
            std::lock_guard lock(mutex);
            if (jobs.empty() || cancel)
            {
                jobs.clear();
                running = false;
                current.clear();
                return;
            }
            job = jobs.front();
            jobs.pop_front();
            current = job.game.name;
            done = 0, total = job.game.size;
            started = Now();
        }
        mkdir(job.games_dir.c_str(), 0777);
        const std::string target = job.games_dir + "/" + job.game.name, part = target + ".part";
        std::string error;
        bool ok;
        const uint64_t free = FreeSpace(job.games_dir);
        if (free && free < job.game.size + (64ull << 20))
            ok = false, error = "not enough space: " + FormatSize(job.game.size) + " needed, " + FormatSize(free) + " free";
        else if (IsFile(target) || IsDir(target))
            ok = false, error = "the hard drive has " + job.game.name + " already";
        else
        {
            RemoveTree(part);
            ok = CopyTree(job.game.source, part, error) && std::rename(part.c_str(), target.c_str()) == 0;
            if (!ok && error.empty())
                error = "couldn't rename the copy";
            if (!ok)
                RemoveTree(part);
        }
        std::lock_guard lock(mutex);
        finished.push_back(ok ? job.game.name : job.game.name + ": " + error);
    }
}

// ---- diagnostics

namespace {

// The app's functions by address (relative to .text), for crash reports' code addresses.
struct Symbols
{
    struct Entry
    {
        uint64_t address, size;
        std::string name;
    };
    std::vector<Entry> entries;

    void Load(const std::string& file)
    {
        FILE* f = std::fopen(file.c_str(), "r");
        if (!f)
            return;
        char line[1024];
        while (std::fgets(line, sizeof(line), f))
        {
            char* end = nullptr;
            const uint64_t address = std::strtoull(line, &end, 16);
            const uint64_t size = std::strtoull(end, &end, 16);
            while (*end == ' ')
                end++;
            std::string name = end;
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
                name.pop_back();
            if (!name.empty())
                entries.push_back({address, size, name});
        }
        std::fclose(f);
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.address < b.address; });
    }
    std::string Name(uint64_t address) const
    {
        auto it = std::upper_bound(entries.begin(), entries.end(), address,
                                   [](uint64_t a, const Entry& e) { return a < e.address; });
        if (it == entries.begin())
            return "";
        --it;
        if (it->size && address >= it->address + it->size)
            return "";
        char off[32];
        std::snprintf(off, sizeof(off), "+0x%llx", (unsigned long long)(address - it->address));
        return it->name + off;
    }
};

// A log's text with the app's code addresses named: "elf 0x..." and the stack lines
// "[rsp+0x..] 0x..." of crash.log and hang.log.
std::string Symbolize(const std::string& text, const Symbols& symbols)
{
    if (symbols.entries.empty())
        return text;
    std::string out;
    size_t at = 0;
    while (at < text.size())
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(at, end - at);
        std::string names;
        auto name_at = [&](size_t pos) {
            const uint64_t a = std::strtoull(line.c_str() + pos, nullptr, 16);
            const std::string n = symbols.Name(a);
            if (!n.empty())
                names += "  <" + n + ">";
        };
        for (size_t p = line.find("elf 0x"); p != std::string::npos; p = line.find("elf 0x", p + 1))
            name_at(p + 4);
        if (line.find("[rsp+0x") != std::string::npos)
            if (const size_t p = line.rfind(" 0x"); p != std::string::npos)
                name_at(p + 1);
        out += line + names + "\n";
        at = end + 1;
    }
    return out;
}

std::string ReadEnd(const std::string& path, size_t limit, uint64_t& size)
{
    std::string text;
    size = 0;
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        fseeko(f, 0, SEEK_END);
        size = uint64_t(ftello(f));
        const uint64_t start = size > limit ? size - limit : 0;
        fseeko(f, off_t(start), SEEK_SET);
        text.resize(size_t(size - start));
        text.resize(std::fread(text.data(), 1, text.size(), f));
        std::fclose(f);
    }
    return text;
}

} // namespace

std::vector<std::string> WriteDiagnostics(const DiagnosticsInput& in, const std::vector<std::string>& outputs)
{
    std::string r;
    auto line = [&r](const std::string& s) { r += s + "\n"; };
    const time_t t = time(nullptr);
    char when[64];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    line("xbox-core diagnostics report");
    line(std::string("written ") + when);
    line("");
    line("== system");
    for (const std::string& s : in.system)
        line(s);
    const uint64_t free = FreeSpace(in.data_dir);
    line("free space in " + in.data_dir + ": " + (free ? FormatSize(free) : "unknown"));
    line("");
    if (in.settings)
    {
        static const char* kPicture[3] = {"sharp", "fit 4:3", "fill"};
        line("== settings");
        line(std::string("show log: ") + (in.settings->show_log ? "on" : "off") + ", picture: " + kPicture[in.settings->picture] +
             ", order: " + (in.settings->sort ? "recently played" : "title"));
        line("");
    }
    line("== library (" + std::to_string(in.games.size()) + " entries)");
    for (const Game& g : in.games)
    {
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", g.title_id);
        line("- " + g.title + "  [" + g.kind + "]");
        line("    title ID " + std::string(id) + ", region " + RegionName(g.region) + ", " + FormatSize(g.size));
        line("    " + g.path + (g.cover.empty() ? "" : "  (cover " + g.cover + ")"));
        if (!g.title_id)
            line("    note: no XBE certificate read (not an Xbox image, or an unusual layout)");
        if (g.plays)
            line("    runs: " + std::to_string(g.plays) + ", last " + FormatDate(g.last_played) + ": " + g.last_result);
    }
    line("");
    line("== USB drives");
    if (in.usb_roots.empty())
        line("none found");
    for (const std::string& u : in.usb_roots)
    {
        const uint64_t f = FreeSpace(u);
        line(u + (f ? ", " + FormatSize(f) + " free" : ""));
    }
    if (in.all_logs)
    {
        // every log in the data folder, as New Eden bundles them
        Symbols symbols;
        if (!in.symbols.empty())
            symbols.Load(in.symbols);
        if (symbols.entries.empty())
            line("(no symbols in this build: code addresses without names)");
        std::vector<std::string> names;
        if (DIR* d = opendir(in.data_dir.c_str()))
        {
            while (dirent* e = readdir(d))
            {
                const std::string n = e->d_name;
                const bool log = n.size() > 4 && (strcasecmp(n.c_str() + n.size() - 4, ".log") == 0 ||
                                                  strcasecmp(n.c_str() + n.size() - 4, ".txt") == 0);
                if (log && n != "diagnostics.txt")
                    names.push_back(n);
            }
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        // the pictures shown (picture-N.png: the app saves one every 10 s while a game runs), as
        // base64, so that the one text file carries them
        std::vector<std::string> pictures;
        if (DIR* d = opendir(in.data_dir.c_str()))
        {
            while (dirent* e = readdir(d))
            {
                const std::string n = e->d_name;
                if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".png") == 0)
                    pictures.push_back(n);
            }
            closedir(d);
        }
        std::sort(pictures.begin(), pictures.end());
        for (const std::string& n : pictures)
        {
            std::string bytes;
            if (FILE* f = std::fopen((in.data_dir + "/" + n).c_str(), "rb"))
            {
                char buf[65536];
                size_t got;
                while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0 && bytes.size() < (size_t(4) << 20))
                    bytes.append(buf, got);
                std::fclose(f);
            }
            std::string when;
            struct stat st;
            if (stat((in.data_dir + "/" + n).c_str(), &st) == 0)
            {
                char b[40];
                const time_t t = st.st_mtime;
                std::strftime(b, sizeof(b), ", written %Y-%m-%d %H:%M:%S", std::localtime(&t));
                when = b;
            }
            line("");
            line("===== " + n + " (" + std::to_string(bytes.size()) + " bytes, base64" + when + ") =====");
            static const char* const kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            for (size_t i = 0; i < bytes.size(); i += 3)
            {
                const uint32_t v = uint32_t(uint8_t(bytes[i])) << 16 |
                                   (i + 1 < bytes.size() ? uint32_t(uint8_t(bytes[i + 1])) << 8 : 0) |
                                   (i + 2 < bytes.size() ? uint32_t(uint8_t(bytes[i + 2])) : 0);
                out += kB64[v >> 18 & 63];
                out += kB64[v >> 12 & 63];
                out += i + 1 < bytes.size() ? kB64[v >> 6 & 63] : '=';
                out += i + 2 < bytes.size() ? kB64[v & 63] : '=';
                if (out.size() >= 76)
                {
                    line(out);
                    out.clear();
                }
            }
            if (!out.empty())
                line(out);
        }
        constexpr size_t kLimit = size_t(8) << 20;
        for (const std::string& n : names)
        {
            uint64_t size = 0;
            const std::string text = ReadEnd(in.data_dir + "/" + n, kLimit, size);
            line("");
            std::string when;
            struct stat st;
            if (stat((in.data_dir + "/" + n).c_str(), &st) == 0)
            {
                char b[40];
                const time_t t = st.st_mtime;
                std::strftime(b, sizeof(b), ", written %Y-%m-%d %H:%M:%S", std::localtime(&t));
                when = b;
            }
            line("===== " + n + " (" + std::to_string(size) + " bytes" + (size > kLimit ? ", its last 8 MiB" : "") + when + ") =====");
            r += Symbolize(text, symbols);
            if (!r.empty() && r.back() != '\n')
                r += '\n';
        }
    }
    for (const auto& [file, n] : in.all_logs ? std::vector<std::pair<std::string, size_t>>{} : in.logs)
    {
        line("");
        line("== " + file + " (last " + std::to_string(n) + " lines)");
        const auto lines = LastLines(file, n);
        if (lines.empty())
            line("(not there, or empty)");
        for (const std::string& l : lines)
            line(l);
    }
    // each written whole beside, then put in place: never half a file where the last one was
    std::vector<std::string> written;
    for (const std::string& out : outputs)
    {
        const std::string part = out + ".part";
        if (FILE* f = std::fopen(part.c_str(), "w"))
        {
            bool ok = std::fwrite(r.data(), 1, r.size(), f) == r.size();
            // on the disk before it replaces the last one (a USB stick keeps nothing of a write the
            // console didn't sync before it went down)
            ok = std::fflush(f) == 0 && ok;
            fsync(fileno(f));
            if (std::fclose(f) == 0 && ok && std::rename(part.c_str(), out.c_str()) == 0)
                written.push_back(out);
            else
                std::remove(part.c_str());
        }
    }
    return written;
}

} // namespace fe
