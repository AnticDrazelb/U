// Desktop host: stdout only, no memory probes (the PS4's are what matter).
#include <cstdio>

#include "platform.h"

namespace Platform {
void Init() {}
void Log(const std::string& line)
{
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}
void Notify(const std::string&) {}
std::string MemoryReport()
{
    return "memory probes: PS4 only";
}
void Finish(const std::string&) {}
} // namespace Platform
