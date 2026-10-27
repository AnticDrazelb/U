#pragma once
// The launcher's core (main.cpp): boots a Switch program in Eden, headless. The platform files
// (platform_host.cpp, platform_ps4.cpp) pick the program and show what it prints.

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace Launcher {

enum class LineKind {
    Program, // the program's own output (svcOutputDebugString)
    Warning, // Eden's warnings
    Error,   // Eden's errors
    Status,  // the launcher's own lines
};

/// Shows one line of output (no newline). Called from Eden's threads.
using LineSink = void (*)(LineKind kind, std::string_view text);

struct BootResult {
    enum class Status {
        Exited,     // the program ended itself (svcExitProcess)
        LoadFailed, // Eden could not load it
        TimedOut,   // still running after the timeout
    } status;
    std::string detail;
};

/// Boots `program` and runs it until it exits or `timeout` passes. Eden's data folders (NAND, SD
/// card, keys) are in `data_dir`/user, and everything the sink is given also goes to
/// `data_dir`/log.txt.
BootResult Boot(const std::filesystem::path& program, const std::filesystem::path& data_dir,
                LineSink sink, std::chrono::milliseconds timeout);

} // namespace Launcher
