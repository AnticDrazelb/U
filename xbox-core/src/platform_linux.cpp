// Linux x86-64: mmap, wrfsbase (FSGSBASE, kernel 5.9+), the ucontext of a signal.
#include <cerrno>
#include <fcntl.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/auxv.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "platform.h"

extern "C" {
void xcs_linux_fs_guest(uint64_t kpcr);
void xcs_linux_fs_host(uint64_t base);
}

namespace xb {
extern thread_local constinit bool tl_in_signal_handler; // core.h
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
constexpr int kSignals[5] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};

void OnSignal(int sig, siginfo_t* info, void* raw)
{
    // The guest's fs may be current (its base below 4 GiB): the emulator's first, from the KPCR's
    // host block (C++ thread-locals live behind fs).
    const uint64_t fs = RdFsBase();
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
    tl_in_signal_handler = true;
    const bool handled = g_handler && g_handler(f);
    tl_in_signal_handler = false;
    if (handled)
    {
        Trail(5, uint64_t(g[REG_RIP]), uint64_t(g[REG_CSGSFS] & 0xffff), guest_fs ? fs : 0, uint64_t(g[REG_RSP]));
        if (guest_fs)
            WrFsBase(fs);
        return;
    }
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

void SetCs(Fault& fault, uint16_t cs)
{
    greg_t* g = static_cast<ucontext_t*>(fault.raw)->uc_mcontext.gregs;
    g[REG_CSGSFS] = (g[REG_CSGSFS] & ~greg_t(0xffff)) | cs;
    fault.cs = cs;
}

void InstallFaultHandler(FaultHandler handler)
{
    g_handler = handler;
    struct sigaction action{};
    action.sa_sigaction = OnSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    for (int i = 0; i < 5; i++)
        sigaction(kSignals[i], &action, &g_old[i]);
}

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
    if (pwrite(g_trail_fd, &r, sizeof(r), off_t((r.seq - 1) % kTrailRecords) * off_t(sizeof(r))) < 0)
        return;
    // tests of the trail itself: killed outright at a stage, as the console kills an app
    static const int die_at = std::getenv("XB_TRAIL_DIE_AT") ? std::atoi(std::getenv("XB_TRAIL_DIE_AT")) : -1;
    if (int(stage) == die_at)
        kill(getpid(), SIGKILL);
}

} // namespace xb::platform

