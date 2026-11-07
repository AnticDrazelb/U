// What differs between the PS4 and a desktop host (Linux): where results go and how they are
// shown, how memory is mapped at fixed addresses below 4 GiB, and the signal context layout.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Platform {
enum class Mode { All, CpuOnly, MemoryOnly };
// Opens the results file; on the PS4 first shows what an unfinished last run left behind and lets
// the player pick what to run.
Mode Init();
// A log line (stdout, and the results file on the PS4).
void Log(const std::string& line);
// What is running now (a status line at the bottom of the screen on the PS4; not saved).
void Progress(const std::string& status);
// Before a step that may crash the app (the boot trace on the PS4: its name stays behind).
void Trace(const std::string& step);
void Finish(const std::string& summary);

// ---- memory at fixed addresses

enum Access { Read = 1, Write = 2, Exec = 4 };
// Maps zeroed memory at exactly `addr` (nothing else may be there). Exec memory comes from where
// the platform lets code run (the PS4's system flexible memory). Returns an error text, or "".
std::string MapFixed(uint64_t addr, size_t size, int access);
// Keeps `addr` .. `addr + size` free of mappings, inaccessible (a fault on every access).
std::string ReserveFixed(uint64_t addr, size_t size);
void Unmap(uint64_t addr, size_t size);
// Maps the same zeroed memory at `a` and at `b` (an alias, as the Xbox's contiguous memory at
// 0x80000000 is the RAM at 0), with `access` (Access bits) at both. Returns an error text, or "".
std::string MapAlias(uint64_t a, uint64_t b, size_t size, int access);
// The smallest unit MapFixed and MapAlias work in.
size_t PageSize();
// Name of the memory pool MapFixed uses for each kind (for the log).
std::string PoolName(int access);

// ---- faults (SIGSEGV, SIGBUS, SIGILL)

// The registers a fault handler reads and changes. gpr[] in x86 encoding order: eax, ecx, edx,
// ebx, esp, ebp, esi, edi.
struct FaultContext
{
    uint64_t* gpr[8];
    uint64_t* rip;
    uint64_t address; // the faulting data address (0 if none)
    int signal;
    uint16_t cs;
    void* raw;
};
void SetCs(FaultContext& context, uint16_t cs);
// Called for every fault; returns true when handled (execution resumes from the context). Faults
// it doesn't handle go to the handlers that were there before (the crash log).
using FaultHandler = bool (*)(FaultContext& context);
void InstallFaultHandler(FaultHandler handler);
void RemoveFaultHandler();

} // namespace Platform
