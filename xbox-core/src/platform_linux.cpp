// Linux x86-64: mmap, wrfsbase (FSGSBASE, kernel 5.9+), the ucontext of a signal.
#include <cerrno>
#include <fcntl.h>
#include <atomic>
#include <csignal>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "platform.h"

extern "C" {
void xcs_linux_fs_guest(uint64_t kpcr);
void xcs_linux_fs_host(uint64_t base);
}

namespace xb {
int EnterSignalHandler(uint64_t lo, uint64_t hi); // core.h: the handler's threads, without thread-local storage
void LeaveSignalHandler(int slot);
}

namespace xb::platform {

namespace {
int Prot(int access)
{
    return ((access & Read) ? PROT_READ : 0) | ((access & Write) ? PROT_WRITE : 0) | ((access & Exec) ? PROT_EXEC : 0);
}
uint64_t RdFsBase()
{
    uint64_t v;
    __asm__ volatile("rdfsbase %0" : "=r"(v));
    return v;
}
void WrFsBase(uint64_t v)
{
    __asm__ volatile("wrfsbase %0" : : "r"(v));
}

FaultHandler g_handler = nullptr;
struct sigaction g_old[5];
// XB_TEST_FS_AS_PS4=1: the handler behaves as the PS4's does: the fs base isn't asked for (reported
// 0) or given back, so the handler runs with the guest's KPCR as fs, and anything fs-relative in its
// path (a thread_local, the C library's per-thread state) ends the run as it ends the app there
bool g_fs_as_ps4 = false;
KpcrFinder g_find_kpcr = nullptr;
int g_trail_die_at = -1;

// a system call without the C library (whose wrappers keep per-thread state behind fs)
long RawSyscall(long n, long a = 0, long b = 0, long c = 0, long d = 0)
{
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return r;
}
constexpr int kSignals[5] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};

void OnSignal(int sig, siginfo_t* info, void* raw)
{
    // The guest's fs may be current (its base below 4 GiB): the emulator's first, from the KPCR's
    // host block (C++ thread-locals live behind fs).
    // (in the PS4 test, the handler's fs is an unmapped page until it returns: anything fs-relative
    // in its path faults at once, where on the console it might read the KPCR and end later)
    const uint64_t poisoned_from = g_fs_as_ps4 ? RdFsBase() : 0;
    if (g_fs_as_ps4 && poisoned_from == 0x1000)
    {
        // a fault with the handler's poisoned fs: something fs-relative ran in the handler's path.
        // Where (rip, then the code addresses on its stack), written without anything fs-relative.
        auto* c = static_cast<ucontext_t*>(raw);
        char line[512];
        int n = 0;
        auto hex = [&](uint64_t v) {
            line[n++] = ' ', line[n++] = '0', line[n++] = 'x';
            for (int i = 60; i >= 0; i -= 4)
                line[n++] = "0123456789abcdef"[(v >> i) & 15];
        };
        static const char head[] = "fs-relative access in the fault handler's path at";
        for (const char ch : head)
            if (ch)
                line[n++] = ch;
        hex(uint64_t(c->uc_mcontext.gregs[REG_RIP]));
        line[n++] = ';';
        const auto* sp = reinterpret_cast<const uint64_t*>(c->uc_mcontext.gregs[REG_RSP]);
        for (int i = 0; i < 400 && n < 480; i++)
            if (sp[i] >= 0x500000000000ull && sp[i] < 0x700000000000ull && sp[i] < reinterpret_cast<uint64_t>(&RawSyscall) + 0x2000000 &&
                sp[i] > reinterpret_cast<uint64_t>(&RawSyscall) - 0x2000000)
                hex(sp[i]);
        line[n++] = '\n';
        RawSyscall(SYS_write, 2, reinterpret_cast<long>(line), n);
        RawSyscall(SYS_kill, RawSyscall(SYS_getpid), SIGKILL);
    }
    if (g_fs_as_ps4)
        WrFsBase(0x1000);
    // (the PS4 test: the system's answer is 0, as there; the KPCR from the finder, as there)
    auto* uc0 = static_cast<ucontext_t*>(raw);
    const uint64_t fs = !g_fs_as_ps4 ? RdFsBase()
                        : g_find_kpcr ? g_find_kpcr(0, 0, uint64_t(uc0->uc_mcontext.gregs[REG_RSP]),
                                                    uint16_t(uc0->uc_mcontext.gregs[REG_CSGSFS] & 0xffff))
                                      : 0;
    const bool guest_fs = fs && fs < 0x100000000ull;
    if (guest_fs)
    {
        uint64_t host;
        std::memcpy(&host, reinterpret_cast<const void*>(fs + 0x300), 8);
        WrFsBase(host);
    }
    auto* uc = static_cast<ucontext_t*>(raw);
    greg_t* g = uc->uc_mcontext.gregs;
    Trail(1, uint64_t(sig), uint64_t(g[REG_RIP]), uint64_t(g[REG_CSGSFS] & 0xffff), uint64_t(g[REG_RSP]));
    Trail(2, fs, uint64_t(reinterpret_cast<uintptr_t>(info->si_addr)), uint64_t(g[REG_TRAPNO]) | (uint64_t(g[REG_ERR]) << 32),
          uint64_t(g[REG_EFL]));
    Fault f{};
    const int order[8] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI};
    for (int i = 0; i < 8; i++)
        f.gpr[i] = reinterpret_cast<uint64_t*>(&g[order[i]]);
    f.rip = reinterpret_cast<uint64_t*>(&g[REG_RIP]);
    f.eflags = reinterpret_cast<uint64_t*>(&g[REG_EFL]);
    f.cs = uint16_t(g[REG_CSGSFS] & 0xffff);
    f.address = uint32_t(reinterpret_cast<uintptr_t>(info->si_addr));
    f.signal = sig == SIGSEGV ? 11 : sig;
    f.fs_base = fs;
    f.raw = raw;
    // the signal stack this runs on (the kernel's answer: the raw system call, no C library state)
    stack_t on{};
    const bool alt = RawSyscall(SYS_sigaltstack, 0, reinterpret_cast<long>(&on)) == 0 && (on.ss_flags & SS_ONSTACK);
    const uint64_t lo = alt ? reinterpret_cast<uint64_t>(on.ss_sp) : 0;
    const int handler_slot = EnterSignalHandler(lo, alt ? lo + on.ss_size : 0);
    const bool handled = g_handler && g_handler(f);
    LeaveSignalHandler(handler_slot);
    if (handled)
    {
        Trail(5, uint64_t(g[REG_RIP]), uint64_t(g[REG_CSGSFS] & 0xffff), guest_fs ? fs : 0, uint64_t(g[REG_RSP]));
        if (guest_fs)
            WrFsBase(fs);
        if (g_fs_as_ps4)
            WrFsBase(poisoned_from);
        return;
    }
    if (g_fs_as_ps4)
        WrFsBase(poisoned_from);
    for (int i = 0; i < 5; i++)
        if (kSignals[i] == sig)
            sigaction(sig, &g_old[i], nullptr);
}
} // namespace

std::string Init()
{
    if (!(getauxval(AT_HWCAP2) & 2))
        return "this kernel doesn't enable FSGSBASE (Linux 5.9 or newer on a CPU with it)";
    return "";
}

std::string Map(uint32_t base, uint32_t size, int access)
{
    void* want = reinterpret_cast<void*>(uintptr_t(base));
    void* p = mmap(want, size, Prot(access), MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED)
        return std::string("mmap: ") + std::strerror(errno);
    if (p != want)
    {
        munmap(p, size);
        return "mmap: placed elsewhere";
    }
    return "";
}

std::string Reserve(uint32_t base, uint32_t size)
{
    return Map(base, size, 0);
}

void Protect(uint32_t base, uint32_t size, int access)
{
    mprotect(reinterpret_cast<void*>(uintptr_t(base)), size, Prot(access));
}

std::vector<std::pair<uint32_t, uint32_t>> Occupied(uint32_t begin, uint32_t end)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (!f)
        return out;
    char line[512];
    while (std::fgets(line, sizeof(line), f))
    {
        unsigned long long s, e;
        if (std::sscanf(line, "%llx-%llx", &s, &e) == 2 && s < end && e > begin)
            out.push_back({uint32_t(std::max<unsigned long long>(s, begin)), uint32_t(std::min<unsigned long long>(e, end))});
    }
    std::fclose(f);
    return out;
}

uint32_t PageSize()
{
    return uint32_t(sysconf(_SC_PAGESIZE));
}

FsFunction FsGuest()
{
    return xcs_linux_fs_guest;
}
FsFunction FsHost()
{
    return xcs_linux_fs_host;
}
uint64_t HostFsBase()
{
    uint64_t v;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(v)); // the thread control block's self pointer
    return v;
}

void SetSwitchArg(Fault& fault, uint64_t value)
{
    static_cast<ucontext_t*>(fault.raw)->uc_mcontext.gregs[REG_R12] = greg_t(value);
}

void SetKpcrFinder(KpcrFinder finder) { g_find_kpcr = finder; }

void SetCs(Fault& fault, uint16_t cs)
{
    greg_t* g = static_cast<ucontext_t*>(fault.raw)->uc_mcontext.gregs;
    g[REG_CSGSFS] = (g[REG_CSGSFS] & ~greg_t(0xffff)) | cs;
    fault.cs = cs;
}

void InstallFaultHandler(FaultHandler handler)
{
    g_handler = handler;
    g_fs_as_ps4 = std::getenv("XB_TEST_FS_AS_PS4") != nullptr;
    struct sigaction action{};
    action.sa_sigaction = OnSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR2); // a sample waits for a fault's handling to end
    for (int i = 0; i < 5; i++)
        sigaction(kSignals[i], &action, &g_old[i]);
    // the sample signal also holds a thread while an interrupt routine runs (StopGuestThreads): one
    // that has just gone into a system call has it restarted, not failed with EINTR
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigaction(SIGUSR2, &action, nullptr);
}

int SampleSignal() { return SIGUSR2; }
uint64_t CurrentHostThread() { return uint64_t(pthread_self()); }
bool SendSample(uint64_t host_thread) { return host_thread && pthread_kill(pthread_t(host_thread), SIGUSR2) == 0; }

std::string WrapSignal(int) { return ""; }
std::string CheckFaultHandler() { return ""; }
const char* SignalRoute() { return "set with sigaction"; }
uint64_t LastSignalStack() { return 0; }
void PrepareKpcr(uint32_t, uint64_t) {}

void UseBigSignalStacks(bool) {}

void PrepareThread()
{
    thread_local bool done = false;
    if (done)
        return;
    done = true;
    stack_t st{};
    st.ss_size = 256 * 1024;
    st.ss_sp = std::malloc(st.ss_size);
    sigaltstack(&st, nullptr);
}

// the fault trail: pwrite (async-signal-safe here) into a ring of records
namespace {
int g_trail_fd = -1;
std::atomic<uint32_t> g_trail_seq{0};
}
bool OpenFaultTrail(const std::string& path, std::string& how)
{
    g_trail_fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    how = g_trail_fd >= 0 ? "pwrite" : "can't open " + path;
    if (const char* at = std::getenv("XB_TRAIL_DIE_AT"))
        g_trail_die_at = std::atoi(at);
    return g_trail_fd >= 0;
}

void Trail(uint32_t stage, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    if (g_trail_fd < 0)
        return;
    TrailRecord r;
    r.magic = 0x52544258;
    r.seq = g_trail_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    r.stage = stage;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    r.tsc = (uint64_t(hi) << 32) | lo;
    r.thread = uint32_t(reinterpret_cast<uintptr_t>(&r) >> 16);
    r.a = a, r.b = b, r.c = c, r.d = d;
    if (RawSyscall(SYS_pwrite64, g_trail_fd, reinterpret_cast<long>(&r), sizeof(r),
                   long((r.seq - 1) % kTrailRecords) * long(sizeof(r))) < 0)
        return;
    // tests of the trail itself: killed outright at a stage, as the console kills an app
    if (int(stage) == g_trail_die_at)
        RawSyscall(SYS_kill, RawSyscall(SYS_getpid), SIGKILL);
}

} // namespace xb::platform

