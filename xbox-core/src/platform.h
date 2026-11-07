// What differs between the hosts xbox-core runs on (Linux x86-64, the PS4): memory at fixed
// addresses, switching fs, the signal context, the screen.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xb::platform {

enum Access { Read = 1, Write = 2, Exec = 4 };

// Checks what the core needs (Linux: FSGSBASE; PS4: amd64_set_fsbase). Returns an error, or "".
std::string Init();
// Maps zeroed memory at exactly `base` (nothing replaced). Returns an error, or "".
std::string Map(uint32_t base, uint32_t size, int access);
// Keeps a range inaccessible (every access faults).
std::string Reserve(uint32_t base, uint32_t size);
void Protect(uint32_t base, uint32_t size, int access);
// The parts of [begin, end) that the system has already mapped (the PS4: the app's own executable).
std::vector<std::pair<uint32_t, uint32_t>> Occupied(uint32_t begin, uint32_t end);
// The smallest unit Map works in.
uint32_t PageSize();

// fs: the switch calls these (from 64-bit code, without C++ thread-locals until FsHost has run).
using FsFunction = void (*)(uint64_t base);
FsFunction FsGuest(); // fs base = a KPCR (below 4 GiB), usable from compatibility mode
FsFunction FsHost();  // fs base = the emulator's thread pointer
uint64_t HostFsBase(); // the calling thread's

// A fault, for the core's handler. The platform has already put the emulator's fs back; if the
// handler returns true, execution resumes from the (changed) context with the interrupted fs.
struct Fault
{
    uint64_t* gpr[8]; // eax ecx edx ebx esp ebp esi edi (64-bit slots)
    uint64_t* rip;
    uint64_t* eflags;
    uint16_t cs;
    uint32_t address; // the faulting data address
    int signal;       // 11: SIGSEGV-like (page fault, general protection), others as the system names them
    uint64_t fs_base; // the interrupted code's fs base
    void* raw;
};
void SetCs(Fault& fault, uint16_t cs);
using FaultHandler = bool (*)(Fault& fault);
void InstallFaultHandler(FaultHandler handler);
// Each host thread that runs guest code: its signal stack (a fault in guest code has no usable stack).
void PrepareThread();

} // namespace xb::platform
