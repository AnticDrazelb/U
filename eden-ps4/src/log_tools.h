// The logs made readable on the console itself: the app's function names for the code addresses
// in crash.log, hang.log, the boot trace's callers and samples.log (from symbols.zst, written by
// build.sh from the build's ELF into the package), and samples.log summed up per thread. No ELF
// of the exact build is needed afterwards to read them.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Launcher {

class Symbols
{
public:
    // symbols.zst: zstd-compressed lines "<address> <size> <name>" (hex, relative to .text,
    // sorted); false if there is none
    bool Load(const std::string& path);
    bool Empty() const { return entries.empty(); }
    // "Name+0x1c" for an address relative to .text, "" outside every function
    std::string Name(uint64_t address) const;

private:
    struct Entry
    {
        uint64_t address;
        uint32_t size;
        uint32_t name; // offset into names, '\n' terminated
    };
    std::vector<Entry> entries;
    std::string names;
};

// The text of a log with the code addresses in it named: "elf 0x..." (crash.log, hang.log,
// samples.log), crash.log's "[rsp+0x..] 0x..." lines, and the address lists of hang.log's "stack:"
// lines and the boot trace's "callers", one address a line
std::string SymbolizeLog(std::string_view text, const Symbols& symbols);

// samples.log summed up: for each program run while it sampled ("## " lines) and each thread,
// where its time went: the game's code (JIT), waiting in system calls, and the app's busiest
// functions
std::string ProfileSummary(std::string_view text, const Symbols& symbols);

} // namespace Launcher
