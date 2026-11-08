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
    for (const auto& [file, n] : in.logs)
    {
        line("");
        line("== " + file + " (last " + std::to_string(n) + " lines)");
        const auto lines = LastLines(file, n);
        if (lines.empty())
            line("(not there, or empty)");
        for (const std::string& l : lines)
            line(l);
    }
    std::vector<std::string> written;
    for (const std::string& out : outputs)
        if (FILE* f = std::fopen(out.c_str(), "w"))
        {
            const bool ok = std::fwrite(r.data(), 1, r.size(), f) == r.size();
            if (std::fclose(f) == 0 && ok)
                written.push_back(out);
        }
    return written;
}

} // namespace fe
