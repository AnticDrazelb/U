// What the core records about a run, to read afterwards (the logs) or after a crash or a hang:
// counters, what wasn't emulated (counted by kind), each guest thread's latest kernel calls, the
// GPU's latest methods, a line of figures every 10 s, and a watchdog that writes every thread's
// state to hang.log when frames stop.
#pragma once

#include <atomic>
#include <string>

#include "core.h"

namespace xb::diag {

// counters the stats line and the reports read
extern std::atomic<u64> g_raster_ns; // time the rasterizer's threads were busy (all draws)
extern std::atomic<u64> g_frames;      // flips the guest asked for (PCRTC start, the flip counter)
extern std::atomic<u64> g_draws;       // primitives drawn
extern std::atomic<u64> g_kernel_calls;
extern std::atomic<u64> g_methods;     // GPU methods
extern std::atomic<u64> g_exceptions;  // guest exceptions dispatched

// Something not emulated, or worth a count: by category ("kernel export", "GPU method", "device
// register", "file not found", ...) and what. The first of each is logged.
void Note(const char* category, const std::string& what);
std::string NotesSummary();

// a kernel call starting and ending on the calling guest thread (KernelCall)
void CallStarted(GuestThread* t, u32 ordinal, u32 a0, u32 a1, u32 a2, u32 a3, u32 return_eip);
void CallEnded(GuestThread* t, u32 result);
// a fault in guest code and what became of it (into a ring of the last 128, with the registers and
// the code at eip: every report shows them)
void RecordFault(u32 thread, int signal, u32 eip, u32 address, const u8* code, const u32 regs[8], const char* outcome);
// a GPU method (its class, method, parameter)
void Method(u32 cls, u32 method, u32 parameter);
// a guest thread, kept for the reports (never freed, so a crash report can read them)
void RegisterThread(GuestThread* t);
GuestThread* ThreadByStack(u32 esp); // the guest thread whose stack holds esp (lock-free: the fault handler's)

// The guest's state as text, without locks or allocation (a crash report's): each thread with
// the kernel call it's in and its latest ones, the GPU's latest methods. To `fd` with write().
void WriteStateLockFree(int fd);
// More, with locks allowed: the same plus the notes and counters, into the log.
void LogState(const char* title);

// The stats line every 10 s, and the hang watchdog: frames (or draws) stop for 20 s after some
// came, or none 90 s after the start: the guest's state to hang.log (once, until they come again),
// with the host's thread stacks if `host_stacks` is given (the PS4's crash log can).
// `after`: called after each snapshot (the PS4 writes the log bundle then).
void StartMonitor(const std::string& hang_file, void (*host_stacks)(const char* file, const char* title),
                  void (*after)());

// A program running (RunXbe) or not: the watchdog watches only while one is, from its start.
void SetRunning(bool running);

double Seconds(); // since the process started

} // namespace xb::diag
