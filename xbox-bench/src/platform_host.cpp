// Desktop host (Linux x86-64): stdout, mmap and Linux's signal context.
// The 32-bit code segment is Linux's __USER32_CS, which the bench finds by itself.
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <asm/ldt.h>
#include <dlfcn.h>
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
std::vector<Mapping> Mappings(uint64_t limit)
{
    std::vector<Mapping> out;
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (!f)
        return out;
    char line[512];
    while (std::fgets(line, sizeof(line), f))
    {
        unsigned long long start = 0, end = 0;
        char perms[8] = {}, path[256] = {};
        if (std::sscanf(line, "%llx-%llx %7s %*s %*s %*s %255[^\n]", &start, &end, perms, path) < 3)
            continue;
        if (start >= limit)
            break;
        out.push_back({start, end, std::string(perms) + (path[0] ? " " + std::string(path) : "")});
    }
    std::fclose(f);
    return out;
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
    action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int i = 0; i < 3; i++)
        sigaction(kSignals[i], &action, &g_old[i]);
}
void PrepareThread()
{
    thread_local bool done = false;
    if (done)
        return;
    done = true;
    static constexpr size_t kSize = 256 * 1024;
    stack_t st{};
    st.ss_sp = std::malloc(kSize);
    st.ss_size = kSize;
    if (st.ss_sp && sigaltstack(&st, nullptr) != 0)
        std::free(st.ss_sp);
}
void RemoveFaultHandler()
{
    for (int i = 0; i < 3; i++)
        sigaction(kSignals[i], &g_old[i], nullptr);
    g_handler = nullptr;
}

// ---- system functions and segments

void* SystemFunction(const char* name, std::string& where)
{
    void* p = dlsym(RTLD_DEFAULT, name);
    where = p ? "libc" : "";
    return p;
}

std::string LdtRead()
{
    uint8_t buffer[16 * 8] = {};
    const long n = syscall(SYS_modify_ldt, 0, buffer, sizeof(buffer));
    return n < 0 ? Errno("modify_ldt (read)") : "modify_ldt (read): " + std::to_string(n / 8) + " entries";
}

// The desktop has no JIT calls; a memfd mapped twice stands in for them.
std::string JitAlias(uint64_t a, uint64_t b, size_t size, int access_a, int access_b, bool)
{
    const int fd = static_cast<int>(syscall(SYS_memfd_create, "xbox-bench jit", 0));
    if (fd < 0)
        return Errno("memfd_create");
    std::string error;
    if (ftruncate(fd, static_cast<off_t>(size)) != 0)
        error = Errno("ftruncate");
    auto prot = [](int access) {
        return ((access & Read) ? PROT_READ : 0) | ((access & Write) ? PROT_WRITE : 0) | ((access & Exec) ? PROT_EXEC : 0);
    };
    void* pa = MAP_FAILED;
    if (error.empty() &&
        (pa = mmap(reinterpret_cast<void*>(a), size, prot(access_a), MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0)) == MAP_FAILED)
        error = Errno("mmap (first view)");
    if (error.empty() &&
        mmap(reinterpret_cast<void*>(b), size, prot(access_b), MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0) == MAP_FAILED)
    {
        error = Errno("mmap (second view)");
        munmap(pa, size);
    }
    close(fd);
    return error;
}

uint16_t SetLdtEntry(int index, uint32_t base, bool code, std::string& error)
{
    user_desc d{};
    d.entry_number = static_cast<unsigned>(index);
    d.base_addr = base;
    d.limit = 0xFFFFF;
    d.seg_32bit = 1;
    d.contents = code ? 2 : 0; // MODIFY_LDT_CONTENTS_CODE / _DATA
    d.read_exec_only = 0;
    d.limit_in_pages = 1;
    d.seg_not_present = 0;
    d.useable = 1;
    if (syscall(SYS_modify_ldt, 1, &d, sizeof(d)) != 0)
    {
        error = Errno("modify_ldt");
        return 0;
    }
    return static_cast<uint16_t>((index << 3) | 4 | 3);
}

} // namespace Platform
