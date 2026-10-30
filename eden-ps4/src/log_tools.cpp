// Function names for the logs' code addresses and samples.log's summary (log_tools.h).

#include "log_tools.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <span>

#include <fmt/format.h>
#include <zstd.h>

namespace Launcher {

namespace {

bool IsHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// A "0x..." number at `pos` (moved past it); false if there is none
bool ParseHex(std::string_view text, size_t& pos, uint64_t& value)
{
    if (pos + 2 >= text.size() || text[pos] != '0' || (text[pos + 1] != 'x' && text[pos + 1] != 'X') ||
        !IsHex(text[pos + 2]))
        return false;
    pos += 2;
    value = 0;
    while (pos < text.size() && IsHex(text[pos]))
    {
        const char c = text[pos++];
        value = value * 16 + uint64_t(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return true;
}

// A function's name without its parameters ("Kernel::KThread::Run(Kernel::KernelCore&)" ->
// "Kernel::KThread::Run"), at most 110 characters: what the summary lists
std::string ShortName(std::string_view name)
{
    int depth = 0;
    size_t end = name.size();
    for (size_t i = 0; i < name.size(); i++)
    {
        const char c = name[i];
        if (c == '<' || c == '{')
            depth++;
        else if ((c == '>' || c == '}') && depth > 0)
            depth--;
        else if (c == '(' && depth == 0)
        {
            if (name.substr(0, i).ends_with("operator") || name.substr(i).starts_with("(anonymous namespace)"))
            {
                const size_t close = name.find(')', i);
                if (close == std::string_view::npos)
                    break;
                i = close;
                continue;
            }
            end = i;
            break;
        }
    }
    std::string out{name.substr(0, end)};
    if (out.size() > 110)
        out = out.substr(0, 107) + "...";
    return out;
}

} // namespace

bool Symbols::Load(const std::string& path)
{
    entries.clear();
    names.clear();
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> f{std::fopen(path.c_str(), "rb"), std::fclose};
    if (!f)
        return false;
    // streamed (the file's frame doesn't say its size: build.sh compresses from a pipe), at most
    // 64 MiB, and nothing thrown: a damaged file leaves no names, not a stopped app
    std::unique_ptr<ZSTD_DCtx, size_t (*)(ZSTD_DCtx*)> context{ZSTD_createDCtx(), ZSTD_freeDCtx};
    if (!context)
        return false;
    std::vector<char> in(ZSTD_DStreamInSize()), out(ZSTD_DStreamOutSize());
    for (size_t n; (n = std::fread(in.data(), 1, in.size(), f.get())) > 0;)
    {
        ZSTD_inBuffer input{in.data(), n, 0};
        while (input.pos < input.size)
        {
            ZSTD_outBuffer output{out.data(), out.size(), 0};
            if (ZSTD_isError(ZSTD_decompressStream(context.get(), &output, &input)) ||
                names.size() + output.pos > (size_t(64) << 20))
            {
                names.clear();
                return false;
            }
            names.append(out.data(), output.pos);
        }
    }
    f.reset();
    const std::string_view all{names};
    for (size_t pos = 0; pos < all.size();)
    {
        size_t end = all.find('\n', pos);
        if (end == std::string_view::npos)
            end = all.size();
        uint64_t address = 0, size = 0;
        size_t p = pos;
        // "<address> <size> <name>", both in hex without 0x
        char* stop = nullptr;
        address = std::strtoull(names.c_str() + p, &stop, 16);
        p = size_t(stop - names.c_str());
        size = std::strtoull(names.c_str() + p, &stop, 16);
        p = size_t(stop - names.c_str()) + 1;
        if (p < end && size > 0)
            entries.push_back({address, uint32_t(std::min<uint64_t>(size, UINT32_MAX)), uint32_t(p)});
        pos = end + 1;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.address < b.address; });
    return !entries.empty();
}

std::string Symbols::Name(uint64_t address) const
{
    auto it = std::upper_bound(entries.begin(), entries.end(), address,
                               [](uint64_t a, const Entry& e) { return a < e.address; });
    if (it == entries.begin())
        return {};
    --it;
    if (address >= it->address + it->size)
        return {};
    const size_t end = names.find('\n', it->name);
    std::string name = names.substr(it->name, (end == std::string::npos ? names.size() : end) - it->name);
    if (address != it->address)
        name += fmt::format("+0x{:x}", address - it->address);
    return name;
}

std::string SymbolizeLog(std::string_view text, const Symbols& symbols)
{
    if (symbols.Empty())
        return std::string(text);
    std::string out;
    out.reserve(text.size() + text.size() / 2);
    for (size_t pos = 0; pos < text.size();)
    {
        size_t end = text.find('\n', pos);
        const bool last = end == std::string_view::npos;
        if (last)
            end = text.size();
        const std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        out += line;

        // a list of addresses: one a line, named
        size_t list = line.find("stack:");
        size_t skip = 6;
        if (list == std::string_view::npos)
        {
            list = line.find(": callers ");
            skip = 9;
        }
        if (list != std::string_view::npos)
        {
            for (size_t p = list + skip; p < line.size();)
            {
                uint64_t address = 0;
                const size_t start = p;
                if (!ParseHex(line, p, address))
                {
                    p++;
                    continue;
                }
                const std::string name = symbols.Name(address);
                out += "\n      " + std::string(line.substr(start, p - start)) + "  " + (name.empty() ? "?" : name);
            }
        }
        else
        {
            // "elf 0x..." (any number of them), or crash.log's "  [rsp+0x..] 0x..."
            std::vector<std::string> found;
            for (size_t p = line.find("elf 0x"); p != std::string_view::npos; p = line.find("elf 0x", p))
            {
                p += 4;
                uint64_t address = 0;
                if (ParseHex(line, p, address))
                {
                    const std::string name = symbols.Name(address);
                    if (!name.empty())
                        found.push_back(name);
                }
            }
            if (line.starts_with("  [rsp+0x"))
            {
                size_t p = line.find("] ");
                uint64_t address = 0;
                if (p != std::string_view::npos && ParseHex(line, p += 2, address))
                {
                    const std::string name = symbols.Name(address);
                    if (!name.empty())
                        found.push_back(name);
                }
            }
            for (size_t i = 0; i < found.size(); i++)
                out += (i == 0 ? "  = " : " | ") + found[i];
        }
        if (!last)
            out += '\n';
    }
    return out;
}

std::string ProfileSummary(std::string_view text, const Symbols& symbols)
{
    struct Thread
    {
        double samples = 0, syscalls = 0, listed = 0;
        std::map<std::string, double> keys;
    };
    struct Section
    {
        std::string title;
        std::map<std::string, Thread> threads;
    };
    std::vector<Section> sections(1);
    sections[0].title = "(before the first program)";
    Thread* thread = nullptr;
    double window = 0;
    for (size_t pos = 0; pos < text.size();)
    {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.starts_with("## "))
        {
            sections.push_back({std::string(line.substr(3)), {}});
            thread = nullptr;
            continue;
        }
        if (line.starts_with("== "))
        {
            // "== 20 s, CPUCore_0: 9921 samples, 1200 in eboot code (12%), 3000 in system calls (30%)"
            const size_t comma = line.find(" s, ");
            const size_t colon = comma == std::string_view::npos ? comma : line.find(": ", comma + 4);
            if (colon == std::string_view::npos)
            {
                thread = nullptr;
                continue;
            }
            thread = &sections.back().threads[std::string(line.substr(comma + 4, colon - comma - 4))];
            window = std::atof(std::string(line.substr(colon + 2)).c_str());
            thread->samples += window;
            const size_t calls = line.find(" in system calls");
            if (calls != std::string_view::npos)
            {
                const size_t before = line.rfind(", ", calls);
                if (before != std::string_view::npos)
                    thread->syscalls += std::atof(std::string(line.substr(before + 2, calls - before - 2)).c_str());
            }
            continue;
        }
        if (!thread)
            continue;
        // "   12.3% elf 0x1234" (also jit, sys)
        const size_t first = line.find_first_not_of(' ');
        if (first == std::string_view::npos)
            continue;
        line.remove_prefix(first);
        const size_t percent_end = line.find("% ");
        if (percent_end == std::string_view::npos || line.size() < percent_end + 6)
            continue;
        const double count = std::atof(std::string(line.substr(0, percent_end)).c_str()) * window / 100.0;
        const std::string_view kind = line.substr(percent_end + 2, 3);
        size_t p = percent_end + 6;
        uint64_t address = 0;
        if (!ParseHex(line, p, address))
            continue;
        std::string key;
        if (kind == "jit")
            key = "[the game's code, translated (JIT)]";
        else if (kind == "sys")
            key = "[system libraries]";
        else if (kind == "elf")
        {
            const std::string name = symbols.Name(address);
            key = name.empty() ? fmt::format("elf 0x{:x}", address & ~uint64_t(0xFF)) : ShortName(name);
        }
        else
            continue;
        thread->keys[key] += count;
        thread->listed += count;
    }

    std::string out = "Profile: where each sampled thread's time went (the sampler's 1000 looks a second)\n";
    if (symbols.Empty())
        out += "(no symbols.zst: the app's functions by address only)\n";
    for (const Section& section : sections)
    {
        if (section.threads.empty())
            continue;
        out += "\n## " + section.title + "\n";
        std::vector<std::pair<std::string, const Thread*>> threads;
        for (const auto& [name, t] : section.threads)
            threads.emplace_back(name, &t);
        std::sort(threads.begin(), threads.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [name, t] : threads)
        {
            if (t->samples <= 0)
                continue;
            const auto share = [&](double n) { return 100.0 * n / t->samples; };
            const auto jit = t->keys.find("[the game's code, translated (JIT)]");
            const auto sys = t->keys.find("[system libraries]");
            const double jit_n = jit == t->keys.end() ? 0 : jit->second;
            const double sys_n = sys == t->keys.end() ? 0 : sys->second;
            const double waiting = std::min(t->syscalls, sys_n);
            out += fmt::format("\n{}: {:.0f} samples (~{:.0f} s): {:.0f}% the game's code (JIT), {:.0f}% waiting in "
                               "system calls, {:.0f}% other system library code, {:.0f}% the app's own code\n",
                               name, t->samples, t->samples / 1000.0, share(jit_n), share(waiting),
                               share(sys_n - waiting), share(t->listed - jit_n - sys_n));
            std::vector<std::pair<double, std::string>> keys;
            for (const auto& [key, n] : t->keys)
                keys.emplace_back(n, key);
            std::sort(keys.rbegin(), keys.rend());
            for (size_t i = 0; i < keys.size() && i < 25 && share(keys[i].first) >= 0.3; i++)
                out += fmt::format("  {:5.1f}%  {}\n", share(keys[i].first), keys[i].second);
            if (t->samples > t->listed)
                out += fmt::format("  {:5.1f}%  (addresses sampled only once in a window, not listed)\n",
                                   share(t->samples - t->listed));
        }
    }
    return out;
}

} // namespace Launcher
