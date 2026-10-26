// Desktop host: stdout only, no memory probes (the PS4's are what matter).
#include <cstdio>
#include <cstdlib>

#include "platform.h"

namespace Platform {
Mode Init()
{
    // BENCH_MODE=cpu|memory: part of the run, as the PS4's start page offers
    const char* mode = std::getenv("BENCH_MODE");
    if (mode && std::string(mode) == "cpu")
        return Mode::CpuOnly;
    if (mode && std::string(mode) == "memory")
        return Mode::MemoryOnly;
    return Mode::All;
}
void Log(const std::string& line)
{
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}
void Notify(const std::string&) {}
void Progress(const std::string& status)
{
    if (std::getenv("BENCH_PROGRESS"))
        std::fprintf(stderr, "%s\n", status.c_str());
}
void MemoryChecks()
{
    Log("memory probes: PS4 only");
}
void Finish(const std::string&) {}
} // namespace Platform
