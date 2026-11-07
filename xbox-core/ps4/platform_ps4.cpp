// PS4: flexible memory at fixed addresses (executable from the system pool), amd64_set_fsbase for fs
// (looked up with sceKernelDlsym: no new import), FreeBSD's signal context. What xbox-bench measured
// on consoles: the 32-bit code segment is 0x33, the LDT is off, direct memory maps once, the app's
// executable is at 0x400000.
#include <cstdio>
#include <cstring>
#include <string>

#include <signal.h>

#include "../src/platform.h"

extern "C" {
int32_t sceKernelReserveVirtualRange(void** addr, size_t len, int32_t flags, size_t alignment);
int32_t sceKernelMunmap(void* addr, size_t len);
int32_t sceKernelMprotect(const void* addr, size_t len, int32_t prot);
int32_t sceKernelMapNamedSystemFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags, const char* name);
int32_t sceKernelMapNamedFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags, const char* name);
struct VirtualQueryInfo
{
    void* start;
    void* end;
    off_t offset;
    int32_t protection, memoryType;
    uint32_t isFlexible : 1, isDirect : 1, isStack : 1, isPooled : 1, isCommitted : 1;
    char name[32];
};
int32_t sceKernelVirtualQuery(const void* addr, int32_t flags, VirtualQueryInfo* info, size_t infoSize);
struct ModuleSegment
{
    void* address;
    uint32_t size;
    int32_t prot;
};
struct ModuleInfo
{
    size_t size;
    char name[256];
    ModuleSegment segments[4];
    uint32_t segment_count;
    uint8_t fingerprint[20];
};
int32_t sceKernelGetModuleList(int32_t* handles, size_t count, size_t* available);
int32_t sceKernelDlsym(int32_t handle, const char* symbol, void** address);
}

namespace xb::platform {

namespace {

constexpr uint32_t kPage = 16 * 1024;
constexpr int32_t kMapFixed = 0x10;
constexpr int kSigIll = 4, kSigBus = 10, kSigSegv = 11, kSaSiginfo = 0x40, kSaOnstack = 0x1;
constexpr int kMcontextOffset = 0x40;
// FreeBSD mcontext, in 8-byte units: rdi 1, rsi 2, rdx 3, rcx 4, rax 7, rbx 8, rbp 9, addr 17,
// rip 20, cs 21, rflags 22, rsp 23; mc_fsbase after the 512-byte FPU state (16-aligned at 28)
constexpr int kGprIndex[8] = {7, 4, 3, 8, 23, 9, 2, 1};
constexpr int kAddr = 17, kRip = 20, kCs = 21, kRflags = 22, kFsBase = 92;
constexpr int kSignals[3] = {kSigSegv, kSigBus, kSigIll};
constexpr uint16_t kUserFs32 = 0x13; // GUFS32: the fs selector whose base is the thread's fs base

using SetFsBase = int (*)(void*);
using GetFsBase = int (*)(void**);
SetFsBase g_set_fsbase = nullptr;
GetFsBase g_get_fsbase = nullptr;
bool g_load_fs_selector = false;
FaultHandler g_handler = nullptr;
struct sigaction g_old[3];

int32_t Prot(int access)
{
    return ((access & Read) ? 0x1 : 0) | ((access & Write) ? 0x2 : 0) | ((access & Exec) ? 0x4 : 0);
}

void* Lookup(const char* name)
{
    int32_t handles[256];
    size_t count = 0;
    if (sceKernelGetModuleList(handles, 256, &count) != 0)
        return nullptr;
    for (size_t m = 0; m < count && m < 256; m++)
    {
        void* address = nullptr;
        if (sceKernelDlsym(handles[m], name, &address) == 0 && address)
            return address;
    }
    return nullptr;
}

std::string ReserveAt(uint32_t base, uint32_t size)
{
    if (base % kPage || size % kPage)
        return "not in whole 16 KiB pages";
    void* at = reinterpret_cast<void*>(uintptr_t(base));
    const int32_t r = sceKernelReserveVirtualRange(&at, size, 0, kPage);
    if (r != 0)
    {
        char e[64];
        std::snprintf(e, sizeof(e), "sceKernelReserveVirtualRange %#x", uint32_t(r));
        return e;
    }
    if (reinterpret_cast<uintptr_t>(at) != base)
    {
        sceKernelMunmap(at, size);
        return "in use";
    }
    return "";
}

// The guest's fs: on FreeBSD the fs selector is GUFS32 (its base is the thread's fs base, also in
// compatibility mode); if this thread's isn't, it is loaded first (a null fs faults in 32-bit code).
void FsGuestImpl(uint64_t kpcr)
{
    if (g_load_fs_selector)
        __asm__ volatile("mov %0, %%fs" : : "r"(uint32_t(kUserFs32)));
    g_set_fsbase(reinterpret_cast<void*>(kpcr));
}
void FsHostImpl(uint64_t base)
{
    g_set_fsbase(reinterpret_cast<void*>(base));
}

void OnSignal(int sig, siginfo_t*, void* context)
{
    auto* regs = reinterpret_cast<uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
    // the interrupted fs base: a KPCR when guest code (or the switch) ran; the emulator's own first.
    // Asked of the kernel (the handler runs with the interrupted fs base); else the context's copy.
    uint64_t fs = regs[kFsBase];
    if (g_get_fsbase)
    {
        void* base = nullptr;
        if (g_get_fsbase(&base) == 0)
            fs = reinterpret_cast<uint64_t>(base);
    }
    const bool guest_fs = fs && fs < 0x100000000ull;
    if (guest_fs)
        g_set_fsbase(reinterpret_cast<void*>(*reinterpret_cast<const uint64_t*>(fs + 0x300)));
    Fault f{};
    for (int i = 0; i < 8; i++)
        f.gpr[i] = &regs[kGprIndex[i]];
    f.rip = &regs[kRip];
    f.eflags = &regs[kRflags];
    f.cs = uint16_t(regs[kCs]);
    f.address = uint32_t(regs[kAddr]);
    f.signal = sig;
    f.fs_base = fs;
    f.raw = context;
    if (g_handler && g_handler(f))
    {
        if (guest_fs)
            g_set_fsbase(reinterpret_cast<void*>(fs));
        return;
    }
    // not ours: the crash log's handlers get it when the instruction runs again
    for (int i = 0; i < 3; i++)
        if (kSignals[i] == sig)
            sigaction(sig, &g_old[i], nullptr);
}

} // namespace

std::string Init()
{
    g_set_fsbase = reinterpret_cast<SetFsBase>(Lookup("amd64_set_fsbase"));
    g_get_fsbase = reinterpret_cast<GetFsBase>(Lookup("amd64_get_fsbase"));
    if (!g_set_fsbase)
        return "amd64_set_fsbase isn't exported by a loaded module";
    uint16_t fs;
    __asm__ volatile("mov %%fs, %0" : "=r"(fs));
    g_load_fs_selector = fs != kUserFs32;
    return "";
}

std::string Map(uint32_t base, uint32_t size, int access)
{
    std::string e = ReserveAt(base, size);
    if (!e.empty())
        return e;
    void* at = reinterpret_cast<void*>(uintptr_t(base));
    // code runs only from the system pool's flexible memory; data comes from regular flexible memory
    const int32_t r = (access & Exec)
                          ? sceKernelMapNamedSystemFlexibleMemory(&at, size, Prot(access), kMapFixed, "xbox code")
                          : sceKernelMapNamedFlexibleMemory(&at, size, Prot(access), kMapFixed, "xbox data");
    if (r != 0)
    {
        sceKernelMunmap(reinterpret_cast<void*>(uintptr_t(base)), size);
        char m[96];
        std::snprintf(m, sizeof(m), "%s %#x", (access & Exec) ? "sceKernelMapNamedSystemFlexibleMemory"
                                                             : "sceKernelMapNamedFlexibleMemory", uint32_t(r));
        return m;
    }
    return "";
}

std::string Reserve(uint32_t base, uint32_t size)
{
    return ReserveAt(base, size);
}

void Protect(uint32_t base, uint32_t size, int access)
{
    const uint32_t start = base & ~(kPage - 1), end = (base + size + kPage - 1) & ~(kPage - 1);
    sceKernelMprotect(reinterpret_cast<void*>(uintptr_t(start)), end - start, Prot(access));
}

std::vector<std::pair<uint32_t, uint32_t>> Occupied(uint32_t begin, uint32_t end)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    uint64_t at = begin;
    while (at < end && out.size() < 256)
    {
        VirtualQueryInfo info{};
        if (sceKernelVirtualQuery(reinterpret_cast<const void*>(at), 1 /* find next */, &info, sizeof(info)) != 0)
            break;
        const uint64_t s = reinterpret_cast<uint64_t>(info.start), e = reinterpret_cast<uint64_t>(info.end);
        if (s >= end || e <= s)
            break;
        out.push_back({uint32_t(std::max<uint64_t>(s, begin)), uint32_t(std::min<uint64_t>(e, end))});
        at = e;
    }
    return out;
}

uint32_t PageSize()
{
    return kPage;
}

FsFunction FsGuest()
{
    return FsGuestImpl;
}
FsFunction FsHost()
{
    return FsHostImpl;
}
uint64_t HostFsBase()
{
    uint64_t v;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(v)); // FreeBSD's thread control block points to itself
    return v;
}

void SetCs(Fault& fault, uint16_t cs)
{
    reinterpret_cast<uint64_t*>(static_cast<char*>(fault.raw) + kMcontextOffset)[kCs] = cs;
    fault.cs = cs;
}

void InstallFaultHandler(FaultHandler handler)
{
    g_handler = handler;
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    void (*function)(int, siginfo_t*, void*) = OnSignal;
    std::memcpy(&action, &function, sizeof(function)); // the handler union is the first member
    action.sa_flags = kSaSiginfo | kSaOnstack;
    sigemptyset(&action.sa_mask);
    for (int i = 0; i < 3; i++)
        sigaction(kSignals[i], &action, &g_old[i]);
}

// Crema's runtime gives every thread a signal stack (ps4_install_signal_stack).
void PrepareThread() {}

} // namespace xb::platform
