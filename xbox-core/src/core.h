// Xbox core: runs original Xbox programs (XBEs) on the host's own CPU in 32-bit compatibility mode,
// with a high-level kernel instead of the Xbox's (no BIOS, no recompiler). Shared declarations.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xb {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// ---- log

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Set while this thread is in the fault handler (a signal handler): Log drops lines there (on the PS4
// the C library can't be used in it; XB_STRICT_SIGNAL=1 makes it abort instead, to find them).
// the fault handler's threads (no thread-local storage: fs isn't the app's in there on the PS4)
int EnterSignalHandler(u64 lo, u64 hi); // at the handler's entry, the signal stack it's on (0, 0: none): its slot
void LeaveSignalHandler(int slot); // as it returns
bool InSignalHandler();            // the calling thread is inside the fault handler
extern std::atomic<u64> g_lines_dropped_in_handler;
extern void (*g_log_sink)(const char* line); // also gets every log line (the PS4's screen and log file)
// A line that must be on the disk before what comes next (a step that may kill the app): logged,
// then the log files synced (g_log_sync).
void LogSync(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern void (*g_log_sync)();
// Where the CPU self-test notes the step it's in (empty: nowhere): a step the last run died in is
// left out of this one, with what it would switch on.
extern std::string g_selftest_file;
extern std::string g_fault_trail_file; // the fault path's forensic trail (platform.h), when set
void ReportFaultTrail(const std::string& path);
extern const char* const kBuildId; // this build's identity: its time and a random tag (new each build) // a trail file a run left: decoded to <name>-previous.txt and the log
extern void (*g_fatal_hook)(); // runs before Fatal ends the process
[[noreturn]] void Fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern int g_verbose; // 0 quiet, 1 kernel calls not yet seen, 2 every kernel call

// ---- guest memory: 32-bit guest addresses are host addresses (flat segments)

template <typename T = void>
inline T* Ptr(u32 guest)
{
    return reinterpret_cast<T*>(static_cast<uintptr_t>(guest));
}
inline u32 Guest(const void* host)
{
    return static_cast<u32>(reinterpret_cast<uintptr_t>(host));
}
template <typename T>
inline T Read(u32 guest)
{
    T v;
    std::memcpy(&v, Ptr(guest), sizeof(T));
    return v;
}
template <typename T>
inline void Write(u32 guest, T v)
{
    std::memcpy(Ptr(guest), &v, sizeof(T));
}

// The Xbox's address space as the core lays it out:
//   0x00010000-0x03ffffff  RAM: the XBE, then the program's own allocations (NtAllocateVirtualMemory)
//   0x80000000-0x83ffffff  contiguous memory (MmAllocateContiguousMemory): what the GPU reads; its
//                          "physical" address is the offset from 0x80000000. Not an alias of the RAM
//                          above (the PS4 can't map memory twice).
//   0x8c000000-0x8cffffff  the core's kernel area: KPCRs, threads, objects, export stubs, data exports
//   0xd0000000-0xd3ffffff  thread stacks
//   0xf0000000-0xf7ffffff  physical memory again (write-combined): emulated accesses
//   0xfd000000-0xffffffff  devices (NV2A at 0xfd000000), kept unmapped: accesses fault and are emulated
constexpr u32 kRamBase = 0x00010000, kRamEnd = 0x04000000;
constexpr u32 kContiguousBase = 0x80000000, kContiguousSize = 0x04000000;
constexpr u32 kKernelBase = 0x8c000000, kKernelSize = 0x01000000;
constexpr u32 kStackBase = 0xd0000000, kStackAreaSize = 0x04000000;
constexpr u32 kDeviceBase = 0xfd000000;
// The GPU's write-combined view of memory: 0xf0000000 + a physical address. Kept unmapped like the
// devices; accesses are emulated (the PS4 can't map memory twice).
constexpr u32 kAliasBase = 0xf0000000, kAliasSize = 0x08000000;
// The RAM's "physical" addresses (what MmGetPhysicalAddress gives and the GPU uses): above the
// contiguous memory's 64 MiB, so that a physical address names one place.
constexpr u32 kRamPhysical = 0x04000000;
// The GPU's instance memory (RAMIN): the last 1 MiB of the contiguous memory, as the last 1 MiB of
// the Xbox's RAM. Never handed out.
constexpr u32 kGpuInstanceMemory = kContiguousBase + kContiguousSize - 0x100000;

bool MapGuest(u32 base, u32 size, bool exec, std::string& error);
void InitMemory();
// The program's memory (NtAllocateVirtualMemory): page-granular, first fit in the RAM range above
// the XBE. Returns 0 if out of memory.
u32 AllocVirtual(u32 size, u32 at = 0);
bool FreeVirtual(u32 base);
u32 AllocContiguous(u32 size, u32 alignment, u32 lowest, u32 highest);
bool FreeContiguous(u32 base);
u32 AllocKernel(u32 size, u32 alignment = 16); // never freed
u32 AllocStack(u32 size);                      // returns the top
u32 AllocSystem(u32 size);                     // MmAllocateSystemMemory: pages in the stack area
u32 FreeSystem(u32 base);                      // the pages freed
u32 PhysicalAddress(u32 guest);
// The allocation (program or contiguous memory) containing `at`.
bool QueryAllocation(u32 at, u32& start, u32& size);
bool GuestMapped(u32 address, u32 length); // the whole range is mapped guest memory (lock-free)
u32 FreePages();

// ---- XBE

struct XbeInfo
{
    u32 base = 0, image_size = 0, entry = 0, thunk_table = 0;
    u32 section_count = 0, section_headers = 0; // the headers, in guest memory
    u32 tls_directory = 0, tls_raw_start = 0, tls_raw_end = 0, tls_index_address = 0, tls_zero_fill = 0;
    u32 init_flags = 0;
    std::string title;
    std::string path; // the file it was loaded from (XeLoadSection reads sections again from it)
};
bool LoadXbe(const std::string& path, XbeInfo& info, std::string& error);
// the same from the file's bytes; `path` is where they came from ("xiso:<path>" inside the disc image)
bool LoadXbeData(const std::vector<u8>& data, const std::string& path, XbeInfo& info, std::string& error);
bool ReadXbeRange(const XbeInfo& info, u32 offset, u32 n, void* out); // its file's bytes, again
extern XbeInfo g_xbe;

// ---- CPU: guest threads

// Offsets in the guest structures (nxdk's xboxkrnl.h; the KPCR as the Xbox kernel has it)
namespace kpcr {
constexpr u32 NtTibStackBase = 0x04, NtTibStackLimit = 0x08, NtTibSelf = 0x18, SelfPcr = 0x1c, Prcb = 0x20,
              Irql = 0x24, CurrentThread = 0x28, Size = 0x300;
constexpr u32 HostFsBase = 0x300, HostThread = 0x308, HostRsp = 0x310, HostSegs = 0x318, Self = 0x320,
              GuestEsp = 0x328, GuestEax = 0x330, GuestEbx = 0x334, GuestEbp = 0x338, GuestEsi = 0x33c,
              GuestEdi = 0x340, GuestEflags = 0x344,
              // a fault in guest code, saved by the signal handler for its handling outside it
              // (xcs_fault64): eip, eflags, eax ecx edx ebx esp ebp esi edi, the signal, the address
              FaultEip = 0x348, FaultEflags = 0x34c, FaultRegs = 0x350, FaultSignal = 0x370, FaultAddress = 0x374,
              BlockSize = 0x380;
// the PRCB's DPC fields, in the KPCR
constexpr u32 DpcInterruptRequested = 0x4c, DpcRoutineActive = 0x58;
} // namespace kpcr
namespace kthread {
constexpr u32 StackBase = 0x1c, StackLimit = 0x20, KernelStack = 0x24, TlsData = 0x28, Priority = 0x32,
              KernelApcDisable = 0x68, SuspendCount = 0x75, HasTerminated = 0x77, Size = 0x110;
constexpr u32 EthreadUniqueThread = 0x12c, EthreadStartAddress = 0x130, EthreadSize = 0x140;
} // namespace kthread

// a kernel call, as a thread's report shows it (diagnostics.h)
struct CallRecord
{
    u32 ordinal, a0, a1, a2, a3, eip, result;
    float at; // seconds since the start
};

struct GuestThread
{
    static constexpr u32 kCallRecords = 24;
    CallRecord calls[kCallRecords] = {}; // its latest kernel calls, a ring
    std::atomic<u32> calls_next{0};
    std::atomic<u32> in_call{0}; // the kernel call it's in (ordinal + 1), 0 none
    float call_started = 0;
    u32 kpcr = 0;     // KPCR block (with the host block)
    u32 ethread = 0;  // ETHREAD (its KTHREAD header is a waitable object: signaled when it ends)
    u32 stack_top = 0, stack_limit = 0;
    u32 tls_area = 0; // the TLS block below the stack's top (0: none); the stack proper is below it
    u32 id = 0;
    std::string name;
    // where it runs, sampled (the watchdog's, through the fault handler: platform::SendSample)
    std::atomic<u64> host{0}; // its host thread (platform::CurrentHostThread), 0 before it starts
    static constexpr u32 kSamples = 64;
    std::atomic<u32> samples{0};
    u32 sample_eip[kSamples] = {};
    u32 sample_regs[8] = {}; // the latest sample's eax..edi
    u8 sample_code[48] = {}; // the latest sample's code from eip - 16
    // the Xbox's single processor (StopGuestThreads): 1 while it runs guest code (not the emulator's:
    // a kernel call, a fault's handling), and 1 while it is held in the fault handler meanwhile
    std::atomic<u8> running_guest{0};
    std::atomic<u8> held{0};
};
GuestThread* CurrentThread(); // the calling host thread's guest thread, or nullptr
const char* KernelExportName(u32 ordinal); // kernel.cpp: an export's name, or nullptr

void InitCpu();
// Creates a guest thread (KPCR, ETHREAD, stack, TLS from the XBE) without starting it.
GuestThread* NewGuestThread(u32 stack_size, u32 tls_size, const char* name);
// Calls guest function fn on the current host thread as guest thread t; returns edx:eax.
u64 CallGuest(GuestThread* t, u32 fn, std::initializer_list<u32> args);
// From inside a kernel call: calls back into the guest below its current stack.
u64 CallGuestNested(u32 fn, std::initializer_list<u32> args);
// Starts a host thread that runs `body` as guest thread t.
void StartHostThread(GuestThread* t, std::function<void()> body);
// The Xbox's single processor: while an interrupt service routine or a DPC runs, no other code does
// (it ran between two instructions of whatever thread it interrupted). The interrupt thread holds
// every other guest thread that runs guest code where it is (at its next instruction, through the
// sample signal), and the others before they go back into guest code, until ResumeGuestThreads.
void StopGuestThreads(GuestThread* self);
extern std::atomic<int> g_sampling; // the watchdog samples where threads are (the sample signal notes it)
void ResumeGuestThreads();
std::string SingleProcessorStats(double seconds); // for the stats line
// The guest address of the stub that calls kernel export `ordinal`.
u32 ExportStub(u32 ordinal);
// kernel.cpp: an object body with its OBJECT_HEADER; `type` names the exported OBJECT_TYPE
u32 NewObjectBody(u32 size, const char* type);
u32 ExceptionDispatcher(); // xcs_raise32's guest address (switch.S)
u16 GuestCodeSelector();   // the guest's cs and ss (RtlCaptureContext)
u16 GuestDataSelector();

// ---- kernel

struct GateFrame // on the guest stack (see switch.S)
{
    u64 rdi, rsi;
    u32 eax, ecx, edx, ret;
    u32 args[16];
};
void InitKernel();
void ResolveThunks(); // fills the XBE's kernel thunk table
void KernelCall(GateFrame* frame, GuestThread* thread);
// A hardware access by a backpatched instruction (cpu.cpp), through the gate's stub 399.
void BackpatchedAccess(GateFrame* frame);
u64 BackpatchedAccessCount();
u32 BackpatchedSiteCount();
int RunXbe(); // the start-up thread; returns when the program ends (or the time limit passes)
extern std::atomic<bool> g_quit;
extern double g_time_limit;

// waitable objects in guest memory (DISPATCHER_HEADER)
void SignalObject(u32 object); // wakes waiters after a change of SignalState
void WakeAll();

// ---- devices

void InitDevices();
void SetNv2aChoice(bool own); // before InitDevices: xbox-core's own NV2A instead of xemu's (where built)
// An access to device memory, from the fault handler: returns false if not emulated.
bool DeviceRead(u32 address, u32 size, u32& value, u32 eip = 0); // eip: the guest instruction, for the reports
bool DeviceWrite(u32 address, u32 size, u32 value, u32 eip = 0);
// Interrupts: the vblank thread calls the connected service routines.
void ConnectInterrupt(u32 vector, u32 kinterrupt);
void DisconnectInterrupt(u32 kinterrupt);
// The current thread's IRQL, with the Xbox's single processor's exclusion: at dispatch level (2) and
// above no DPC runs (nor does another thread at dispatch level), above it (a device's level) no
// interrupt service routine runs either. Every IRQL change goes through here.
void SetIrql(u8 irql);
// KeSynchronizeExecution: the routine with the interrupt's service routine held off
u32 SynchronizeWithInterrupt(u32 kinterrupt, u32 routine, u32 context);
// The picture the guest shows: where it is and how to read it.
struct DisplayInfo
{
    bool valid = false; // a mode has been set
    u32 address = 0, format = 0, pitch = 0, bpp = 4;
    int width = 640, height = 480;
    // the video overlay (PVIDEO: what movies are shown through), over the picture where it is on
    struct Overlay
    {
        bool on = false, color_key = false;
        u32 address = 0, pitch = 0, key = 0; // its YUY2 (Cr8Yb8Cb8Ya8) frame, in guest memory
        int in_w = 0, in_h = 0, out_x = 0, out_y = 0, out_w = 0, out_h = 0;
        float in_x = 0, in_y = 0, scale_x = 1, scale_y = 1;
    } overlay;
};
u32 Nv2aPvideo(u32 reg); // a PVIDEO register (offset within PVIDEO), whichever NV2A runs
DisplayInfo GetDisplay();
std::string LastDrawsNote(); // draw.cpp: the last draws of xbox-core's NV2A (where they land), for the log
u32 DisplayPixel(const DisplayInfo& d, int x, int y); // 0xRRGGBB
bool SaveFramebuffer(const std::string& path);
// Input: the Xbox controller on USB port 1 (usb.cpp). Buttons (digital): 0x01 up, 0x02 down,
// 0x04 left, 0x08 right, 0x10 start, 0x20 back, 0x40 left stick, 0x80 right stick. Analog buttons
// (0-255): A, B, X, Y, black, white, left trigger, right trigger.
struct PadState
{
    u16 buttons = 0;
    u8 analog[8] = {};
    s16 lx = 0, ly = 0, rx = 0, ry = 0;
};
void SetPad(const PadState& pad);
void GetRumble(u16& left, u16& right);

// Sound: the AC'97's PCM output, 48 kHz 16-bit stereo, as it plays (audio.cpp)
extern void (*g_audio_sink)(const s16* samples, size_t frames);
// the frames the sink holds, not yet played (a sound device's queue); null: a sink with no queue
extern size_t (*g_audio_queued)();
extern std::atomic<u64> g_audio_dry; // times the sink's device ran out of sound (it counts them)
std::string SoundStats(double seconds); // for the stats line
void StartVblank();

} // namespace xb
