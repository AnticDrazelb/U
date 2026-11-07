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

// files.cpp
void RegisterFileExports(RegisterFn reg);
void CloseHostHandle(int kind, void* host);
void SetGameDirectory(const std::string& dir);
void SetDataDirectory(const std::string& dir); // the hard disk's partitions (E:, T:, ...) live under it

// handles (kernel.cpp): kind 1 = file, 2 = directory
u32 NewHostHandle(int kind, void* host);
void* HostOf(u32 handle, int kind);
bool IsHandleKind(u32 handle, int kind); // kind 3 = the \?? object directory

// devices.cpp
void QueueDpc(u32 kdpc);
void SetDisplayMode(u32 mode, u32 format, u32 pitch, u32 framebuffer);
void SetTickCountVariable(u32 address);

} // namespace xb
