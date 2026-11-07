// The logs as one file (log_bundle.h): every log in the data folder, one after another, code
// addresses named, samples.log summed up. (04.87: what remains of the web server, removed: the
// bundle goes to the Game Folder's USB drive, eden-ps4-logs.txt.)

#include "log_bundle.h"
#include "log_tools.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Launcher {

namespace {

namespace fs = std::filesystem;

constexpr size_t kLogLimit = size_t(8) << 20; // per file in the log bundle (its end)

std::string Lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

bool EndsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && Lower(text.substr(text.size() - suffix.size())) == suffix;
}

bool IsLog(std::string_view name)
{
    return EndsWith(name, ".log") || EndsWith(name, ".txt");
}

// The data folder's logs, and gpu/'s (relative paths), and their sizes
std::vector<std::pair<std::string, uintmax_t>> ListLogs(const fs::path& data)
{
    std::vector<std::pair<std::string, uintmax_t>> logs;
    for (const char* folder : {"", "gpu", "gpu/flight"})
    {
        std::error_code ec;
        for (fs::directory_iterator it(data / folder, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (it->is_regular_file(ec) && IsLog(name))
                logs.emplace_back((*folder ? std::string(folder) + "/" : std::string()) + name, it->file_size(ec));
        }
    }
    std::sort(logs.begin(), logs.end());
    return logs;
}

// The end of a file, at most `limit` bytes
std::string ReadEnd(const fs::path& path, size_t limit)
{
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> f{std::fopen(path.c_str(), "rb"), std::fclose};
    if (!f)
        return {};
    std::fseek(f.get(), 0, SEEK_END);
    const long size = std::ftell(f.get());
    const long start = size > long(limit) ? size - long(limit) : 0;
    std::fseek(f.get(), start, SEEK_SET);
    std::string text(size_t(size - start), '\0');
    text.resize(std::fread(text.data(), 1, text.size(), f.get()));
    return text;
}

// Every log, one after another, each read and handed to `write` on its own, with the app's function
// names for the code addresses in them (unless `symbols` is empty) and samples.log summed up
bool WriteBundle(const fs::path& data, const std::string& version, const Symbols& symbols,
                 const std::function<bool(std::string_view)>& write)
{
    const size_t limit = kLogLimit;
    std::string head = "New Eden " + version + ": every log in " + data.string() + "\n";
    if (symbols.Empty())
        head += "(no symbols.zst in this build: code addresses without names)\n";
    if (!write(head))
        return false;
    for (const auto& [name, size] : ListLogs(data))
    {
        std::string text = ReadEnd(data / name, limit);
        std::string part = "\n===== " + name + " (" + std::to_string(size) + " bytes" +
                           (size > limit ? ", its last " + std::to_string(limit >> 20) + " MiB" : "") + ") =====\n";
        // samples.log stays until the profiler runs again, perhaps on an older build: its addresses
        // are named only with the symbols of the build that recorded it ("[build 03.89]")
        // (04.82: samples-previous*.log too, one a game)
        const bool is_samples = name.rfind("samples", 0) == 0 && name.size() > 4 && name.ends_with(".log");
        const bool other_build = is_samples && text.find("[build " + version + "]") == std::string::npos;
        if (is_samples)
            part += (other_build ? ProfileSummary(text, Symbols{},
                                                  "(recorded by an earlier build, not this one: its app addresses "
                                                  "aren't named, this build's names wouldn't fit them)")
                                 : ProfileSummary(text, symbols)) +
                    "\n----- " + name + " itself -----\n";
        part += symbols.Empty() || other_build ? std::move(text) : SymbolizeLog(text, symbols);
        if (!part.empty() && part.back() != '\n')
            part += '\n';
        if (!write(part))
            return false;
    }
    return true;
}

} // namespace

bool WriteLogBundle(const std::string& data_dir, const std::string& version, const std::string& symbols_path,
                   const std::filesystem::path& file)
{
    Symbols symbols; // (no program runs: the names are loaded, ~11 MiB, and go again)
    if (!symbols_path.empty())
        symbols.Load(symbols_path);
    const fs::path partial = file.string() + ".part";
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> out{std::fopen(partial.c_str(), "wb"), std::fclose};
    if (!out)
        return false;
    const bool written = WriteBundle(data_dir, version, symbols, [&](std::string_view part) {
        return std::fwrite(part.data(), 1, part.size(), out.get()) == part.size();
    });
    std::error_code ec;
    if (std::fclose(out.release()) != 0 || !written)
    {
        fs::remove(partial, ec);
        return false;
    }
    fs::rename(partial, file, ec);
    return !ec;
}

} // namespace Launcher
