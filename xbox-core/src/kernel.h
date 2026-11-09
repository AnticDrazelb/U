// What the kernel's parts share (kernel.cpp, files.cpp, devices.cpp).
#pragma once

#include <functional>
#include <string>

#include "core.h"

namespace xb {

// A kernel call's arguments: stdcall ones on the stack; fastcall ones in ecx, edx, then the stack.
struct Call
{
    GateFrame* frame;
    GuestThread* thread;
    bool fastcall;
    u32 Arg(int i) const;
};
using RegisterFn = void (*)(const char* name, std::function<u64(Call&)> handler);

s64 SystemTime(); // 100 ns units since 1601
std::string GuestString(u32 ansi_string);
std::string FormatGuest(u32 fmt, u32 args); // printf over the guest's arguments at `args`

// kernel_rtl.cpp: strings, memory, integers, printf, the pool, lists, crypto, debug and HAL stubs
void RegisterRtlExports(RegisterFn reg);
u32 PoolAlloc(u32 size);
void PoolFree(u32 p);

// kernel_crypto.cpp: DES and triple DES, RSA
void RegisterCryptoExports(RegisterFn reg);

// files.cpp
void RegisterFileExports(RegisterFn reg);
void CloseHostHandle(int kind, void* host);
void SetGameDirectory(const std::string& dir);
// Loads a game: a default.xbe (its folder is D:) or a disc image (.iso: its default.xbe; D: is the image).
bool LoadGame(const std::string& path, std::string& error);
void SetDataDirectory(const std::string& dir); // the hard disk's partitions (E:, T:, ...) live under it

// handles (kernel.cpp): kind 1 = file, 2 = directory
u32 NewHostHandle(int kind, void* host);
void* HostOf(u32 handle, int kind);
bool IsHandleKind(u32 handle, int kind); // kind 3 = the \?? object directory
u32 KernelObjectOf(u32 handle);          // a kernel object's handle: its object (0 if not one)
// An I/O request done (all of ours finish at once): its event set, its APC queued to the calling
// thread or a packet posted to the file's completion port (port: the I/O completion object, or 0).
void CompleteFileIo(u32 event, u32 apc_routine, u32 apc_context, u32 io_status_block, u32 status, u32 information,
                u32 port, u32 key);

// devices.cpp
void QueueDpc(u32 kdpc);
void SetDisplayMode(u32 mode, u32 format, u32 pitch, u32 framebuffer);
void SetTickCountVariable(u32 address);
void SetTimeVariable(bool system_time, u32 address); // KeSystemTime / KeInterruptTime: kept current
s64 InterruptTime();                                // 100 ns units since the start

} // namespace xb
