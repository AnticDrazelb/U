// What differs between the PS4 and a desktop host: where results go, how they are shown, and
// the memory-capacity probes (PS4 only).
#pragma once

#include <string>

namespace Platform {
void Init();
// A log line (stdout, and the results file on the PS4).
void Log(const std::string& line);
// A short message the player sees now (a system notification on the PS4).
void Notify(const std::string& message);
// What is running now (a status line at the bottom of the screen on the PS4; not saved).
void Progress(const std::string& status);
// Probes how much of the memory a Switch emulator needs the platform can give: appends a report.
std::string MemoryReport();
void Finish(const std::string& summary);
} // namespace Platform
