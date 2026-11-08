// PS4: flexible memory at fixed addresses (executable from the system pool), amd64_set_fsbase for fs
// (looked up with sceKernelDlsym: no new import), FreeBSD's signal context. What xbox-bench measured
// on consoles: the 32-bit code segment is 0x33, the LDT is off, direct memory maps once, the app's
// executable is at 0x400000.
#include <cstdio>
#include <atomic>
#include <cstdlib>
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
int32_t sceKernelGetModuleInfo(int32_t handle, ModuleInfo* info);
int32_t sceKernelQueryMemoryProtection(void* address, void** start, void** end, uint32_t* protection);
int32_t sceKernelDlsym(int32_t handle, const char* symbol, void** address);
}

namespace xb::platform {

namespace {

constexpr uint32_t kPage = 16 * 1024;
constexpr int32_t kMapFixed = 0x10;
constexpr int kSigIll = 4, kSigTrap = 5, kSigFpe = 8, kSigBus = 10, kSigSegv = 11, kSaSiginfo = 0x40, kSaOnstack = 0x1;
constexpr int kMcontextOffset = 0x40;
// FreeBSD mcontext, in 8-byte units: rdi 1, rsi 2, rdx 3, rcx 4, rax 7, rbx 8, rbp 9, addr 17,
// rip 20, cs 21, rflags 22, rsp 23; mc_fsbase after the 512-byte FPU state (16-aligned at 28)
constexpr int kGprIndex[8] = {7, 4, 3, 8, 23, 9, 2, 1};
constexpr int kAddr = 17, kRip = 20, kCs = 21, kRflags = 22, kFsBase = 92;
constexpr int kSignals[5] = {kSigSegv, kSigBus, kSigIll, kSigFpe, kSigTrap};
constexpr uint16_t kUserFs32 = 0x13; // GUFS32: the fs selector whose base is the thread's fs base

using SetFsBase = int (*)(void*);
using GetFsBase = int (*)(void**);
SetFsBase g_set_fsbase = nullptr;
GetFsBase g_get_fsbase = nullptr;
bool g_load_fs_selector = false;
FaultHandler g_handler = nullptr;

// Signal handlers set with the kernel itself (sigaction through libkernel's syscall(), number 416),
// not libkernel's sigaction: that one installs its threading library's wrapper, which reads the
// thread's data through fs before calling the handler, and guest code runs with fs at its KPCR;
// so a signal in guest code (any fault: a register access, a privileged instruction, an exception)
// crashed inside the wrapper and the console killed the app with nothing logged. Ours put the
// app's fs back first; what isn't ours goes on to the handler that was there (the crash log's, the
// stack dump's: the wrapper, called directly with the app's fs).
struct BsdSigaction
{
    void (*handler)(int, siginfo_t*, void*);
    int32_t flags;
    uint32_t mask[4];
};
static_assert(sizeof(BsdSigaction) == 32);
using SyscallFn = int (*)(int, ...);
SyscallFn g_syscall = nullptr;
// libkernel's own raw sigaction stub (FreeBSD's: mov rax, 416; mov r10, rcx; syscall; ...), found in
// its code when no exported name reaches the kernel's sigaction
using SigactionFn = int (*)(int, const void*, void*);
SigactionFn g_raw_sigaction = nullptr;
std::string g_route = "not set";
// the last resort, when neither is there: libkernel's wrapper finds its thread at fs:[0x10]
// (struct tcb's tcb_thread); each KPCR gets the host thread's there (NT_TIB's FiberData and
// ArbitraryUserPointer, otherwise unused), so the wrapper works with the guest's fs too
bool g_kpcr_tcb = false;

// The raw stub for system call `number` in libkernel's readable code, or null (and why in `how`).
void* FindSyscallStub(uint32_t number, std::string& how)
{
    int32_t handles[256];
    size_t count = 0;
    if (sceKernelGetModuleList(handles, 256, &count) != 0)
    {
        how = "no module list";
        return nullptr;
    }
    std::string seen;
    for (size_t m = 0; m < count && m < 256; m++)
    {
        ModuleInfo info;
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(handles[m], &info) != 0 || !std::strstr(info.name, "libkernel"))
            continue;
        for (uint32_t g = 0; g < info.segment_count && g < 4; g++)
        {
            const ModuleSegment& s = info.segments[g];
            char note[160];
            if (!(s.prot & 4))
                continue;
            void* start = nullptr;
            void* end = nullptr;
            uint32_t protection = 0;
            const int32_t q = sceKernelQueryMemoryProtection(s.address, &start, &end, &protection);
            std::snprintf(note, sizeof(note), "%s%s code %p+%#x (protection %#x%s)", seen.empty() ? "" : "; ", info.name,
                          s.address, s.size, protection, q != 0 ? ", query failed" : "");
            seen += note;
            if (q != 0 || !(protection & 1)) // not readable: execute-only
                continue;
            const auto* p = static_cast<const uint8_t*>(s.address);
            const uint8_t tail[5] = {0x49, 0x89, 0xca, 0x0f, 0x05}; // mov r10, rcx; syscall
            for (uint32_t i = 0; i + 12 <= s.size; i++)
            {
                size_t at = 0;
                if (p[i] == 0x48 && p[i + 1] == 0xc7 && p[i + 2] == 0xc0) // mov rax, imm32
                    at = 3;
                else if (p[i] == 0xb8) // mov eax, imm32
                    at = 1;
                else
                    continue;
                uint32_t n;
                std::memcpy(&n, p + i + at, 4);
                if (n == number && std::memcmp(p + i + at + 4, tail, 5) == 0)
                {
                    char found[64];
                    std::snprintf(found, sizeof(found), " -> its stub at %p", static_cast<const void*>(p + i));
                    how = seen + found;
                    return const_cast<uint8_t*>(p + i);
                }
            }
        }
    }
    how = seen.empty() ? "no libkernel code segment found" : seen + " -> no stub found";
    return nullptr;
}
static_assert(sizeof(struct sigaction) == sizeof(BsdSigaction), "the sysroot's sigaction is FreeBSD's");
BsdSigaction g_previous[33]; // by signal: what was there before ours
bool g_have_previous[33] = {};

int RawSigaction(int sig, const BsdSigaction* action, BsdSigaction* old)
{
    if (g_raw_sigaction)
        return g_raw_sigaction(sig, action, old);
    if (g_syscall)
        return g_syscall(416, sig, action, old);
    // libkernel's (with its wrapper; g_kpcr_tcb makes that work in guest code)
    return sigaction(sig, reinterpret_cast<const struct sigaction*>(action), reinterpret_cast<struct sigaction*>(old));
}

// fs back to the app's when guest code was interrupted; returns the interrupted fs base
uint64_t HostFs(void* context, bool& guest_fs)
{
    auto* regs = reinterpret_cast<uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
    uint64_t fs = regs[kFsBase];
    if (g_get_fsbase)
    {
        void* base = nullptr;
        if (g_get_fsbase(&base) == 0)
            fs = reinterpret_cast<uint64_t>(base);
    }
    guest_fs = fs && fs < 0x100000000ull;
    if (guest_fs)
        g_set_fsbase(reinterpret_cast<void*>(*reinterpret_cast<const uint64_t*>(fs + 0x300)));
    return fs;
}

// the handler that was there before ours, called as the kernel would
bool CallPrevious(int sig, siginfo_t* info, void* context)
{
    if (sig <= 0 || sig > 32 || !g_have_previous[sig])
        return false;
    const auto h = g_previous[sig].handler;
    if (reinterpret_cast<uintptr_t>(h) <= 1) // SIG_DFL, SIG_IGN
        return false;
    h(sig, info, context);
    return true;
}

// a signal that isn't a fault of ours (the stack dump's, SIGSYS): on to its handler, with the app's fs
void Trampoline(int sig, siginfo_t* info, void* context)
{
    bool guest_fs;
    const uint64_t fs = HostFs(context, guest_fs);
    CallPrevious(sig, info, context);
    if (guest_fs)
        g_set_fsbase(reinterpret_cast<void*>(fs));
}

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

void OnSignal(int sig, siginfo_t* info, void* context)
{
    auto* regs = reinterpret_cast<uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
    // the interrupted fs base: a KPCR when guest code (or the switch) ran; the emulator's own first.
    // Asked of the kernel (the handler runs with the interrupted fs base); else the context's copy.
    bool guest_fs;
    const uint64_t fs = HostFs(context, guest_fs);
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
    // not ours: the crash log's handler, now (with the app's fs); if it returns, the default
    // action when the instruction runs again
    CallPrevious(sig, info, context);
    BsdSigaction fallback{};
    if (g_have_previous[sig])
        fallback = g_previous[sig];
    RawSigaction(sig, &fallback, nullptr);
}

} // namespace
void OnSignalEntry(int sig, siginfo_t* info, void* context) { OnSignal(sig, info, context); }
void TrampolineEntry(int sig, siginfo_t* info, void* context) { Trampoline(sig, info, context); }

} // namespace xb::platform

// The handlers as the kernel calls them: through an entry that aligns the stack to 16 bytes first
// (the kernel's own sigaction gives no promise of it, libkernel's wrapper did that; formatting a
// log line in the handler uses SSE stores that fault on a misaligned stack, and a fault in the
// handler ends the app), and notes the stack pointer it came with (the self-test logs it).
extern "C" {
__attribute__((used, visibility("hidden"))) uint64_t xb_signal_rsp = 0;
__attribute__((used, visibility("hidden"))) void xb_on_signal(int sig, siginfo_t* info, void* context)
{
    xb::platform::OnSignalEntry(sig, info, context);
}
__attribute__((used, visibility("hidden"))) void xb_on_trampoline(int sig, siginfo_t* info, void* context)
{
    xb::platform::TrampolineEntry(sig, info, context);
}
void xb_signal_entry(int, siginfo_t*, void*);
void xb_trampoline_entry(int, siginfo_t*, void*);
}
__asm__(R"(
    .text
    .p2align 4
    .globl xb_signal_entry
    .hidden xb_signal_entry
xb_signal_entry:
    movq %rsp, xb_signal_rsp(%rip)
    pushq %rbp
    movq %rsp, %rbp
    andq $-16, %rsp
    call xb_on_signal
    movq %rbp, %rsp
    popq %rbp
    ret
    .p2align 4
    .globl xb_trampoline_entry
    .hidden xb_trampoline_entry
xb_trampoline_entry:
    pushq %rbp
    movq %rsp, %rbp
    andq $-16, %rsp
    call xb_on_trampoline
    movq %rbp, %rsp
    popq %rbp
    ret
)");

namespace xb::platform {

std::string Init()
{
    g_syscall = reinterpret_cast<SyscallFn>(Lookup("syscall"));
    if (g_syscall)
        g_route = "set with the kernel's own sigaction (libkernel's syscall())";
    else
    {
        std::string how;
        g_raw_sigaction = reinterpret_cast<SigactionFn>(FindSyscallStub(416, how));
        if (g_raw_sigaction)
            g_route = "set with the kernel's own sigaction (libkernel's raw stub; " + how + ")";
        else
        {
            g_kpcr_tcb = true;
            g_route = "set with libkernel's sigaction, each KPCR carrying the host thread at fs:[0x10] for its wrapper "
                      "(syscall() isn't exported; " + how + ")";
        }
    }
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
    BsdSigaction action{};
    action.handler = xb_signal_entry;
    action.flags = kSaSiginfo | kSaOnstack;
    for (int i = 0; i < 5; i++)
    {
        const int sig = kSignals[i];
        if (RawSigaction(sig, &action, &g_previous[sig]) == 0)
            g_have_previous[sig] = true;
    }
}

uint64_t LastSignalStack() { return ::xb_signal_rsp; }

const char* SignalRoute()
{
    return g_route.c_str();
}

void PrepareKpcr(uint32_t kpcr, uint64_t host_fs_base)
{
    if (!g_kpcr_tcb || !host_fs_base)
        return;
    uint64_t thread;
    std::memcpy(&thread, reinterpret_cast<const void*>(host_fs_base + 0x10), 8);
    std::memcpy(reinterpret_cast<void*>(uintptr_t(kpcr) + 0x10), &thread, 8);
}

std::string CheckFaultHandler()
{
    std::string replaced;
    for (int i = 0; i < 5; i++)
    {
        const int sig = kSignals[i];
        BsdSigaction current{};
        if (RawSigaction(sig, nullptr, &current) != 0)
            return "sigaction (the kernel's) failed";
        if (current.handler == xb_signal_entry)
            continue;
        replaced += (replaced.empty() ? "" : ", ") + std::to_string(sig);
        g_previous[sig] = current;
        g_have_previous[sig] = true;
        BsdSigaction action{};
        action.handler = xb_signal_entry;
        action.flags = kSaSiginfo | kSaOnstack;
        RawSigaction(sig, &action, nullptr);
    }
    return replaced.empty() ? "" : "signals " + replaced + " had another handler: ours put back";
}

std::string WrapSignal(int sig)
{
    if (sig <= 0 || sig > 32)
        return "no such signal";
    BsdSigaction current{};
    if (RawSigaction(sig, nullptr, &current) != 0)
        return "sigaction (the kernel's) failed";
    if (current.handler == xb_trampoline_entry || current.handler == xb_signal_entry)
        return "";
    BsdSigaction action = current;
    action.handler = xb_trampoline_entry;
    action.flags |= kSaSiginfo | kSaOnstack;
    g_previous[sig] = current;
    g_have_previous[sig] = true;
    if (RawSigaction(sig, &action, nullptr) != 0)
        return "sigaction (the kernel's) failed";
    return "";
}

// Crema's runtime gives every thread a 64 KiB signal stack (ps4_install_signal_stack), from the heap.
// The fault handler can need more (it emulates the hardware a register access reaches). Bigger
// ones come from a pool in the app's own .bss, and only once the self-test has seen one work (a
// 1 MiB one from the heap killed the app at the first fault). Threads past the pool keep Crema's.
struct BsdSignalStack
{
    void* sp;
    size_t size;
    int32_t flags;
};
extern "C" int ps4_sigaltstack_xb(const BsdSignalStack* stack, BsdSignalStack* old) __asm__("sigaltstack");
constexpr size_t kBigStack = 256 * 1024, kBigStacks = 48;
alignas(64) uint8_t g_stack_pool[kBigStacks][kBigStack];
std::atomic<uint32_t> g_stacks_used{0};
std::atomic<bool> g_big_stacks{false};

void UseBigSignalStacks(bool on) { g_big_stacks = on; }

void PrepareThread()
{
    thread_local bool installed = false;
    if (installed || !g_big_stacks)
        return;
    installed = true;
    const uint32_t slot = g_stacks_used++;
    if (slot >= kBigStacks)
        return;
    const BsdSignalStack s{g_stack_pool[slot], kBigStack, 0};
    ps4_sigaltstack_xb(&s, nullptr);
}

} // namespace xb::platform
