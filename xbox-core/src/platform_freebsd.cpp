// FreeBSD x86-64: the PS4's system, as close as a desktop gets. The PS4's kernel and libkernel come
// from FreeBSD, so this is how the console's signal and fs paths are tried without a console
// (tests/freebsd-vm/): amd64_set_fsbase (sysarch) for fs, the kernel's own sigaction (system call
// 416, as the PS4 build finds in libkernel) because libthr's sigaction puts its wrapper in front of
// the handler and that wrapper reads its thread through fs:[0x10] (the PS4 crash), 16 KiB pages, and
// the app's executable at 0x400000 (linked there, as PS4 apps are loaded).
//
// XB_SIGNAL_ROUTE picks how the handlers are set, to try each of the PS4 build's routes:
//   raw (default)  the kernel's sigaction                       (PS4: libkernel's raw stub)
//   libthr         libthr's sigaction, its wrapper in front     (what crashed the console)
//   libthr-kpcr    libthr's, each KPCR carrying fs:[0x10]       (PS4: the last resort)
#include <cerrno>
#include <fcntl.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <machine/sysarch.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/user.h>
#include <libutil.h>
#include <ucontext.h>
#include <unistd.h>

#include "platform.h"

namespace xb {
extern thread_local constinit bool tl_in_signal_handler; // core.h
}

namespace xb::platform {

namespace {

constexpr uint32_t kPage = 16 * 1024; // the PS4's
constexpr int kSignals[5] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
constexpr uint16_t kUserFs32 = 0x13; // GUFS32, as on the PS4

FaultHandler g_handler = nullptr;
bool g_load_fs_selector = false;
enum class Route { Raw, Libthr, LibthrKpcr } g_route = Route::Raw;
std::string g_route_text = "not set";
struct sigaction g_previous[33];
bool g_have_previous[33] = {};

int Prot(int access)
{
    return ((access & Read) ? PROT_READ : 0) | ((access & Write) ? PROT_WRITE : 0) | ((access & Exec) ? PROT_EXEC : 0);
}

int SetSignal(int sig, const struct sigaction* action, struct sigaction* old)
{
    if (g_route == Route::Raw)
        return int(::syscall(SYS_sigaction, sig, action, old)); // past libthr: no wrapper
    return ::sigaction(sig, action, old);
}

uint64_t GetFs()
{
    void* base = nullptr;
    amd64_get_fsbase(&base);
    return reinterpret_cast<uint64_t>(base);
}

uint64_t HostFs(bool& guest_fs)
{
    const uint64_t fs = GetFs();
    guest_fs = fs && fs < 0x100000000ull;
    if (guest_fs)
        amd64_set_fsbase(reinterpret_cast<void*>(*reinterpret_cast<const uint64_t*>(fs + 0x300)));
    return fs;
}

bool CallPrevious(int sig, siginfo_t* info, void* context)
{
    if (sig <= 0 || sig > 32 || !g_have_previous[sig])
        return false;
    const struct sigaction& p = g_previous[sig];
    if (p.sa_handler == SIG_DFL || p.sa_handler == SIG_IGN)
        return false;
    if (p.sa_flags & SA_SIGINFO)
        p.sa_sigaction(sig, info, context);
    else
        p.sa_handler(sig);
    return true;
}

void OnSignal(int sig, siginfo_t* info, void* raw)
{
    bool guest_fs;
    const uint64_t fs = HostFs(guest_fs);
    auto* uc = static_cast<ucontext_t*>(raw);
    mcontext_t& m = uc->uc_mcontext;
    Fault f{};
    __register_t* order[8] = {&m.mc_rax, &m.mc_rcx, &m.mc_rdx, &m.mc_rbx, &m.mc_rsp, &m.mc_rbp, &m.mc_rsi, &m.mc_rdi};
    for (int i = 0; i < 8; i++)
        f.gpr[i] = reinterpret_cast<uint64_t*>(order[i]);
    f.rip = reinterpret_cast<uint64_t*>(&m.mc_rip);
    f.eflags = reinterpret_cast<uint64_t*>(&m.mc_rflags);
    f.cs = uint16_t(m.mc_cs);
    f.address = uint32_t(m.mc_addr);
    f.signal = sig;
    f.fs_base = fs;
    f.raw = raw;
    tl_in_signal_handler = true;
    const bool handled = g_handler && g_handler(f);
    tl_in_signal_handler = false;
    if (handled)
    {
        if (guest_fs)
            amd64_set_fsbase(reinterpret_cast<void*>(fs));
        return;
    }
    CallPrevious(sig, info, raw);
    struct sigaction fallback{};
    if (g_have_previous[sig])
        fallback = g_previous[sig];
    SetSignal(sig, &fallback, nullptr);
}

void Trampoline(int sig, siginfo_t* info, void* raw)
{
    bool guest_fs;
    const uint64_t fs = HostFs(guest_fs);
    CallPrevious(sig, info, raw);
    if (guest_fs)
        amd64_set_fsbase(reinterpret_cast<void*>(fs));
}

void FsGuestImpl(uint64_t kpcr)
{
    if (g_load_fs_selector)
        __asm__ volatile("mov %0, %%fs" : : "r"(uint32_t(kUserFs32)));
    amd64_set_fsbase(reinterpret_cast<void*>(kpcr));
}
void FsHostImpl(uint64_t base)
{
    amd64_set_fsbase(reinterpret_cast<void*>(base));
}

struct sigaction Action(void (*handler)(int, siginfo_t*, void*))
{
    struct sigaction a{};
    a.sa_sigaction = handler;
    a.sa_flags = SA_SIGINFO | SA_ONSTACK; // as the PS4 build: no SA_NODEFER
    sigemptyset(&a.sa_mask);
    return a;
}

} // namespace

std::string Init()
{
    const char* r = std::getenv("XB_SIGNAL_ROUTE");
    if (r && !std::strcmp(r, "libthr"))
    {
        g_route = Route::Libthr;
        g_route_text = "set with libthr's sigaction (its wrapper in front: the PS4's crash)";
    }
    else if (r && !std::strcmp(r, "libthr-kpcr"))
    {
        g_route = Route::LibthrKpcr;
        g_route_text = "set with libthr's sigaction, each KPCR carrying the host thread at fs:[0x10] for its wrapper";
    }
    else
        g_route_text = "set with the kernel's own sigaction (system call 416)";
    uint16_t fs;
    __asm__ volatile("mov %%fs, %0" : "=r"(fs));
    g_load_fs_selector = fs != kUserFs32;
    void* base = nullptr;
    if (amd64_get_fsbase(&base) != 0)
        return "amd64_get_fsbase failed";
    return "";
}

std::string Map(uint32_t base, uint32_t size, int access)
{
    if (base % kPage || size % kPage)
        return "not in whole 16 KiB pages";
    void* want = reinterpret_cast<void*>(uintptr_t(base));
    void* p = mmap(want, size, Prot(access), MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_EXCL, -1, 0);
    if (p == MAP_FAILED)
        return std::string("mmap: ") + std::strerror(errno);
    return "";
}

std::string Reserve(uint32_t base, uint32_t size)
{
    return Map(base, size, 0);
}

void Protect(uint32_t base, uint32_t size, int access)
{
    const uint32_t start = base & ~(kPage - 1), end = (base + size + kPage - 1) & ~(kPage - 1);
    mprotect(reinterpret_cast<void*>(uintptr_t(start)), end - start, Prot(access));
}

std::vector<std::pair<uint32_t, uint32_t>> Occupied(uint32_t begin, uint32_t end)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    int count = 0;
    kinfo_vmentry* entries = kinfo_getvmmap(getpid(), &count);
    if (!entries)
        return out;
    for (int i = 0; i < count; i++)
    {
        const uint64_t s = entries[i].kve_start, e = entries[i].kve_end;
        if (s < end && e > begin)
            out.push_back({uint32_t(std::max<uint64_t>(s, begin)), uint32_t(std::min<uint64_t>(e, end))});
    }
    std::free(entries);
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
    __asm__ volatile("mov %%fs:0, %0" : "=r"(v)); // the thread control block points to itself
    return v;
}

void SetCs(Fault& fault, uint16_t cs)
{
    static_cast<ucontext_t*>(fault.raw)->uc_mcontext.mc_cs = cs;
    fault.cs = cs;
}

void InstallFaultHandler(FaultHandler handler)
{
    g_handler = handler;
    const struct sigaction a = Action(OnSignal);
    for (int sig : kSignals)
        if (SetSignal(sig, &a, &g_previous[sig]) == 0)
            g_have_previous[sig] = true;
}

std::string WrapSignal(int sig)
{
    if (sig <= 0 || sig > 32)
        return "no such signal";
    struct sigaction current{};
    if (SetSignal(sig, nullptr, &current) != 0)
        return "sigaction failed";
    if (current.sa_sigaction == Trampoline || current.sa_sigaction == OnSignal)
        return "";
    g_previous[sig] = current;
    g_have_previous[sig] = true;
    struct sigaction a = Action(Trampoline);
    return SetSignal(sig, &a, nullptr) == 0 ? "" : "sigaction failed";
}

std::string CheckFaultHandler()
{
    std::string replaced;
    for (int sig : kSignals)
    {
        struct sigaction current{};
        if (SetSignal(sig, nullptr, &current) != 0)
            return "sigaction failed";
        if (current.sa_sigaction == OnSignal)
            continue;
        // libthr reports the handler the program gave it, so a match there means ours too
        replaced += (replaced.empty() ? "" : ", ") + std::to_string(sig);
        g_previous[sig] = current;
        g_have_previous[sig] = true;
        const struct sigaction a = Action(OnSignal);
        SetSignal(sig, &a, nullptr);
    }
    return replaced.empty() ? "" : "signals " + replaced + " had another handler: ours put back";
}

const char* SignalRoute()
{
    return g_route_text.c_str();
}

uint64_t LastSignalStack() { return 0; }

void PrepareKpcr(uint32_t kpcr, uint64_t host_fs_base)
{
    if (g_route != Route::LibthrKpcr || !host_fs_base)
        return;
    uint64_t thread;
    std::memcpy(&thread, reinterpret_cast<const void*>(host_fs_base + 0x10), 8);
    std::memcpy(reinterpret_cast<void*>(uintptr_t(kpcr) + 0x10), &thread, 8);
}

// The PS4 build's runtime gives each thread a 64 KiB signal stack; the same here, and 256 KiB ones
// once the self-test turns them on.
bool g_big = false;
void UseBigSignalStacks(bool on) { g_big = on; }

void PrepareThread()
{
    thread_local size_t have = 0;
    const size_t want = g_big ? 256 * 1024 : 64 * 1024;
    if (have >= want)
        return;
    have = want;
    stack_t st{};
    st.ss_size = want;
    st.ss_sp = std::malloc(want);
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
}

} // namespace xb::platform

