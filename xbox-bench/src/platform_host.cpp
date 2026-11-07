// Desktop host (Linux x86-64): stdout, mmap, Linux's signal context, modify_ldt and arch_prctl.
// The 32-bit code segment is Linux's __USER32_CS, which the bench finds by itself.
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <asm/ldt.h>
#include <asm/prctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "platform.h"

namespace Platform {
Mode Init()
{
    // BENCH_MODE=cpu|memory: part of the run, as the PS4's start page offers
    const char* mode = std::getenv("BENCH_MODE");
    if (mode && std::string(mode) == "cpu")
        return Mode::CpuOnly;
    if (mode && std::string(mode) == "memory")
        return Mode::MemoryOnly;
    return Mode::All;
}
void Log(const std::string& line)
{
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}
void Progress(const std::string& status)
{
    if (std::getenv("BENCH_PROGRESS"))
        std::fprintf(stderr, "%s\n", status.c_str());
}
void Trace(const std::string& step)
{
    if (std::getenv("BENCH_PROGRESS"))
        std::fprintf(stderr, "trace: %s\n", step.c_str());
}
void Finish(const std::string&) {}

namespace {
std::string Errno(const char* what)
{
    return std::string(what) + ": " + std::strerror(errno);
}
} // namespace

std::string MapFixed(uint64_t addr, size_t size, int access)
{
    const int prot = ((access & Read) ? PROT_READ : 0) | ((access & Write) ? PROT_WRITE : 0) |
                     ((access & Exec) ? PROT_EXEC : 0);
    void* p = mmap(reinterpret_cast<void*>(addr), size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED)
        return Errno("mmap");
    if (reinterpret_cast<uint64_t>(p) != addr)
    {
        munmap(p, size);
        return "mmap: placed elsewhere (old kernel without MAP_FIXED_NOREPLACE)";
    }
    return "";
}
std::string ReserveFixed(uint64_t addr, size_t size)
{
    return MapFixed(addr, size, 0);
}
void Unmap(uint64_t addr, size_t size)
{
    munmap(reinterpret_cast<void*>(addr), size);
}
std::string MapAlias(uint64_t a, uint64_t b, size_t size, int access)
{
    const int prot = ((access & Read) ? PROT_READ : 0) | ((access & Write) ? PROT_WRITE : 0) |
                     ((access & Exec) ? PROT_EXEC : 0);
    const int fd = static_cast<int>(syscall(SYS_memfd_create, "xbox-bench ram", 0));
    if (fd < 0)
        return Errno("memfd_create");
    std::string error;
    if (ftruncate(fd, static_cast<off_t>(size)) != 0)
        error = Errno("ftruncate");
    void* pa = MAP_FAILED;
    if (error.empty())
    {
        pa = mmap(reinterpret_cast<void*>(a), size, prot, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        if (pa == MAP_FAILED)
            error = Errno("mmap (first view)");
    }
    if (error.empty())
    {
        void* pb = mmap(reinterpret_cast<void*>(b), size, prot, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        if (pb == MAP_FAILED)
        {
            error = Errno("mmap (second view)");
            munmap(pa, size);
        }
    }
    close(fd);
    return error;
}
size_t PageSize()
{
    return static_cast<size_t>(sysconf(_SC_PAGESIZE));
}
std::string PoolName(int access)
{
    return (access & Exec) ? "anonymous RWX" : "anonymous";
}

// ---- faults

namespace {
FaultHandler g_handler = nullptr;
struct sigaction g_old[3];
constexpr int kSignals[3] = {SIGSEGV, SIGBUS, SIGILL};

void OnFault(int sig, siginfo_t* info, void* raw)
{
    auto* uc = static_cast<ucontext_t*>(raw);
    greg_t* g = uc->uc_mcontext.gregs;
    FaultContext c{};
    const int order[8] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI};
    for (int i = 0; i < 8; i++)
        c.gpr[i] = reinterpret_cast<uint64_t*>(&g[order[i]]);
    c.rip = reinterpret_cast<uint64_t*>(&g[REG_RIP]);
    c.address = reinterpret_cast<uint64_t>(info->si_addr);
    c.signal = sig;
    c.cs = static_cast<uint16_t>(g[REG_CSGSFS] & 0xFFFF);
    c.raw = raw;
    if (g_handler && g_handler(c))
        return;
    // not ours: the previous handler (or the default action) when the instruction runs again
    for (int i = 0; i < 3; i++)
        if (kSignals[i] == sig)
            sigaction(sig, &g_old[i], nullptr);
}
} // namespace

void SetCs(FaultContext& context, uint16_t cs)
{
    greg_t* g = static_cast<ucontext_t*>(context.raw)->uc_mcontext.gregs;
    g[REG_CSGSFS] = (g[REG_CSGSFS] & ~greg_t(0xFFFF)) | cs;
    context.cs = cs;
}
void InstallFaultHandler(FaultHandler handler)
{
    g_handler = handler;
    struct sigaction action{};
    action.sa_sigaction = OnFault;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    for (int i = 0; i < 3; i++)
        sigaction(kSignals[i], &action, &g_old[i]);
}
void RemoveFaultHandler()
{
    for (int i = 0; i < 3; i++)
        sigaction(kSignals[i], &g_old[i], nullptr);
    g_handler = nullptr;
}

// ---- fs

uint16_t SetLdtEntry(int index, uint32_t base, uint32_t limit, std::string& error)
{
    user_desc d{};
    d.entry_number = static_cast<unsigned>(index);
    d.base_addr = base;
    d.limit = limit >> 12;
    d.seg_32bit = 1;
    d.contents = 0; // data
    d.limit_in_pages = 1;
    d.useable = 1;
    if (syscall(SYS_modify_ldt, 1, &d, sizeof(d)) != 0)
    {
        error = Errno("modify_ldt");
        return 0;
    }
    return static_cast<uint16_t>((index << 3) | 4 | 3);
}
void FsBaseRestoreCall(uint64_t base, uint64_t* slot, uint64_t out[3])
{
    *slot = base;
    out[0] = SYS_arch_prctl;
    out[1] = ARCH_SET_FS;
    out[2] = base;
}
} // namespace Platform
