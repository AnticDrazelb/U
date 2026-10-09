// Crash diagnostics for PS4 test builds.
//
// PS4 coredumps are encrypted and klog is shared with other tools, so the app reports its own
// crashes: a constructor that runs before all others (priority 101) starts
// /data/Crema/boot-trace.log and installs a handler for fatal signals that appends the
// signal, fault address, registers and the code addresses found on the stack to
// /data/Crema/crash.log. Addresses are printed relative to the start of .text, i.e. as ELF
// virtual addresses: symbolize with `llvm-symbolizer --obj=<elf> 0x...`.
//
// Handlers installed later (Dolphin's fastmem handler) chain to this one for faults they don't
// handle.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cxxabi.h>
#include <exception>
#include <typeinfo>

extern "C" char __text_start[];  // defined by the OpenOrbis link.x at the start of .text
extern "C" __attribute__((weak)) void ps4_crash_guest_info();  // the emulator's, if it has one
extern "C" __attribute__((weak)) void ps4_crash_guest_regs(int fd);  // lock-free, into crash.log
// the recompiled game function a host address is in, and its code (lock-free, into crash.log)
extern "C" __attribute__((weak)) void ps4_crash_jit_info(int fd, uint64_t rip, int brief);
// the emulator's note for its menu, and its restart into the menu (Crema: PS4WindowSystem.cpp)
extern "C" __attribute__((weak)) void ps4_crash_note();
extern "C" __attribute__((weak)) void ps4_crash_restart();

// Back into the emulator's menu if it has one, else the process ends.
[[noreturn]] static void endAfterCrash(int status) {
    if (ps4_crash_restart)
        ps4_crash_restart();
    _exit(status);
}

// FreeBSD's stack_t (the OpenOrbis musl header has Linux's field order, which the kernel misreads)
struct BsdSignalStack {
    void* sp;
    size_t size;
    int flags;
};
extern "C" int ps4_sigaltstack(const BsdSignalStack* stack, BsdSignalStack* old) __asm__("sigaltstack");

// The calling thread's own stack for the crash handler: a thread that runs out of stack faults on
// its guard page, and without another stack the handler can't run at all; the system then ends
// the app with error CE-34878-0 and nothing in crash.log. Every thread gets one
// (ps4_runtime.cpp: __wrap_pthread_create; the main thread: earlyInit below).
extern "C" void ps4_install_signal_stack() {
    constexpr size_t kSignalStackSize = 64 * 1024;
    void* stack = malloc(kSignalStackSize);
    if (!stack)
        return;
    const BsdSignalStack signalStack{stack, kSignalStackSize, 0};
    if (ps4_sigaltstack(&signalStack, nullptr) != 0)
        free(stack);
}
extern "C" void ps4_dump_thread_stacks(const char* path, const char* reason);  // ps4_sampler.cpp
// the system's (ps4_runtime.cpp wraps pthread_create with an allocation; a crash may hold the heap)
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

namespace {

// FreeBSD values (the PS4 kernel's); the OpenOrbis musl headers may carry Linux ones.
constexpr int kSigIll = 4, kSigAbrt = 6, kSigFpe = 8, kSigBus = 10, kSigSegv = 11;
constexpr int kSaSiginfo = 0x40, kSaOnstack = 0x1;
constexpr int kMcontextOffset = 0x40;  // measured on hardware
// Registers in FreeBSD mcontext order (after mc_onstack).
constexpr const char* kRegNames[] = {"rdi", "rsi", "rdx", "rcx", "r8",  "r9",  "rax", "rbx",
                                     "rbp", "r10", "r11", "r12", "r13", "r14", "r15"};
constexpr int kRipIndex = 20, kRspIndex = 23;
constexpr uintptr_t kTextWindow = 0x4000000;  // 64 MiB covers the eboot's code

int g_traceFd = -1;

// The app's folder was /data/CemuPS4 before the app was renamed Crema (v01.34): moved once, before
// anything opens a file there, and the paths kept in its files changed to the new place.
void migrateDataFolder() {
    struct stat st;
    if (stat("/data/Crema", &st) == 0 || stat("/data/CemuPS4", &st) != 0)
        return;
    if (rename("/data/CemuPS4", "/data/Crema") != 0)
        return;
    const char* const files[] = {"/data/Crema/launcher.ini", "/data/Crema/launch.txt", "/data/Crema/played.ini",
                                 "/data/Crema/settings.xml"};
    const char kOld[] = "/data/CemuPS4", kNew[] = "/data/Crema";
    for (const char* path : files) {
        const int in = open(path, O_RDONLY);
        if (in < 0)
            continue;
        const off_t size = lseek(in, 0, SEEK_END);
        lseek(in, 0, SEEK_SET);
        // no malloc: this can run while the heap is being set up (it logs here)
        static char text[1 << 20];
        const bool read_all = size > 0 && size < static_cast<off_t>(sizeof(text)) && read(in, text, size) == size;
        close(in);
        if (!read_all)
            continue;
        text[size] = 0;
        const int out = open(path, O_WRONLY | O_TRUNC);
        if (out >= 0) {
            for (const char* p = text; *p;) {
                const char* found = strstr(p, kOld);
                const size_t keep = found ? static_cast<size_t>(found - p) : strlen(p);
                write(out, p, keep);
                if (!found)
                    break;
                write(out, kNew, sizeof(kNew) - 1);
                p = found + sizeof(kOld) - 1;
            }
            close(out);
        }
    }
}

// Opened on first use: the heap may log before this file's constructor runs.
int traceFd() {
    if (g_traceFd < 0) {
        migrateDataFolder();
        mkdir("/data/Crema", 0777);
        // After a game stopped, the launcher starts: the game's trace is kept as
        // boot-trace-game.log (the note for the launcher is there until it shows it).
        struct stat st;
        // ... and after any game run (its trace names the game early on), so a game that ended
        // by itself - the HOME button, its own exit - keeps its trace too
        static char head[2048];
        bool gameRun = false;
        const int previous = open("/data/Crema/boot-trace.log", O_RDONLY);
        if (previous >= 0) {
            const ssize_t n = read(previous, head, sizeof(head) - 1);
            close(previous);
            head[n > 0 ? n : 0] = 0;
            gameRun = strstr(head, "] game: ") != nullptr;
        }
        if (gameRun || stat("/data/Crema/last-stop.txt", &st) == 0)
            rename("/data/Crema/boot-trace.log", "/data/Crema/boot-trace-game.log");
        g_traceFd = open("/data/Crema/boot-trace.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    return g_traceFd;
}

void writeAll(int fd, const char* text) {
    size_t length = strlen(text);
    while (length > 0) {
        ssize_t written = write(fd, text, length);
        if (written <= 0)
            return;
        text += written;
        length -= static_cast<size_t>(written);
    }
}

__attribute__((format(printf, 2, 3))) void writeFormat(int fd, const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    writeAll(fd, line);
}

bool isCode(uint64_t address) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(__text_start);
    return address >= base && address < base + kTextWindow;
}

uint64_t toElf(uint64_t address) { return address - reinterpret_cast<uintptr_t>(__text_start); }

void setHandler(int sig, void (*handler)(int, siginfo_t*, void*), int flags) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    // The handler union is the first member; its member names differ between headers.
    memcpy(&action, &handler, sizeof(handler));
    action.sa_flags = flags;
    sigemptyset(&action.sa_mask);
    sigaction(sig, &action, nullptr);
}

void crashHandler(int sig, siginfo_t* info, void* context) {
    static volatile int s_depth = 0;
    // First, before anything that could fault: one line in the boot trace with where it happened
    // (static buffer, write() only). Minecraft (1.93) crashed, the report itself faulted before its
    // first line, and the nested exit ended the app with nothing recorded.
    {
        const auto* regs = reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        const uint64_t rip = regs[kRipIndex];
        const uint64_t address = *reinterpret_cast<const uint64_t*>(reinterpret_cast<char*>(info) + 24);
        static char early[200];
        if (g_traceFd >= 0) {
            const int n = snprintf(early, sizeof(early), "fatal signal %d (depth %d): rip 0x%llx%s elf 0x%llx, address 0x%llx\n",
                                   sig, s_depth, static_cast<unsigned long long>(rip), isCode(rip) ? "" : " (not eboot code)",
                                   static_cast<unsigned long long>(toElf(rip)), static_cast<unsigned long long>(address));
            if (n > 0)
                write(g_traceFd, early, static_cast<size_t>(n));
        }
    }
    // Crashed while reporting (typically the stack scan running off the end of a small thread
    // stack): end the process now. Returning re-ran the faulting read forever, leaving the crashed
    // thread spinning and the app hanging on a black screen. (The PS4 answers this _exit with
    // SIGSYS: the system's crash report, error CE-34878-0.)
    if (s_depth++ > 0)
        _exit(128 + sig);

    int fd = open("/data/Crema/crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    const bool ownFile = fd >= 0;
    if (!ownFile && traceFd() >= 0) {  // (1.69: a crash left no report; opening files was failing)
        // the report then goes to the boot trace, which is already open (1.73: the PS4's open file
        // limit was reached and the crash left no report)
        writeFormat(traceFd(), "signal %d: crash.log cannot be opened (errno %d), the report follows here\n", sig,
                    errno);
        fd = traceFd();
    }
    if (fd >= 0) {
        const auto* regs = reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        const uint64_t rip = regs[kRipIndex];
        const uint64_t rsp = regs[kRspIndex];
        const uint64_t address = *reinterpret_cast<const uint64_t*>(reinterpret_cast<char*>(info) + 24);
        writeFormat(fd, "\n=== signal %d (code %d), fault address 0x%llx\n", sig,
                    *reinterpret_cast<const int*>(reinterpret_cast<char*>(info) + 8),
                    static_cast<unsigned long long>(address));
        writeFormat(fd, "rip 0x%llx%s elf 0x%llx, rsp 0x%llx, text at %p\n",
                    static_cast<unsigned long long>(rip), isCode(rip) ? "" : " (not eboot code)",
                    static_cast<unsigned long long>(toElf(rip)), static_cast<unsigned long long>(rsp),
                    static_cast<void*>(__text_start));
        for (int i = 0; i < 15; i++)
            writeFormat(fd, "%s 0x%llx%s", kRegNames[i], static_cast<unsigned long long>(regs[1 + i]),
                        i % 4 == 3 ? "\n" : "  ");
        if (address < rsp && rsp - address < 0x10000)
            writeAll(fd, "\nthe fault is just below the stack pointer: this thread ran out of stack\n");
        if (ps4_crash_guest_regs) {
            writeAll(fd, "\n");
            ps4_crash_guest_regs(fd);
        }
        if (ps4_crash_jit_info && !isCode(rip))
            ps4_crash_jit_info(fd, rip, 0);
        writeAll(fd, "\ncode addresses on the stack (elf):\n");
        const auto* stack = reinterpret_cast<const uint64_t*>(rsp);
        int found = 0;
        for (int i = 0; i < 2048 && found < 48; i++) {
            if (isCode(stack[i])) {
                writeFormat(fd, "  [rsp+0x%x] 0x%llx\n", i * 8, static_cast<unsigned long long>(toElf(stack[i])));
                found++;
            }
        }
        if (ownFile)
            close(fd);
    }
    if (traceFd() >= 0)
        writeAll(traceFd(), ownFile ? "crashed (see crash.log)\n" : "crashed\n");
    if (ps4_crash_note)
        ps4_crash_note();

    // What else to record takes locks and allocates, which a crash may have left held: a timer
    // thread ends the process (or restarts into the menu) if it doesn't finish.
    pthread_t timer;
    __real_pthread_create(&timer, nullptr, [](void*) -> void* {
        sleep(15);
        endAfterCrash(126);
    }, nullptr);
    // Where every other thread was (stacks, and the lock each one waits on), after the crash.
    ps4_dump_thread_stacks("/data/Crema/crash.log", "every thread at the crash");
    // The emulator's view of the crash (Cemu: the guest CPU state, stack trace and threads, into
    // log.txt).
    if (ps4_crash_guest_info) {
        ps4_crash_guest_info();
        if (traceFd() >= 0)
            writeAll(traceFd(), "guest state written to log.txt\n");
    }

    // End the process here (or restart into the menu) rather than with the system's crash report:
    // on FW 13.52 that coredump of a homebrew app never finished and hung the console until a
    // hard power-off (DolphinPS4), and the report itself is encrypted.
    endAfterCrash(128 + sig);
}

// std::terminate (e.g. an uncaught exception): record what was thrown, then abort as usual.
[[noreturn]] void terminateHandler() {
    char message[512] = "std::terminate called";
    if (const std::type_info* type = abi::__cxa_current_exception_type()) {
        int status = 0;
        char* name = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
        const char* what = "";
        try {
            if (std::exception_ptr current = std::current_exception())
                std::rethrow_exception(current);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        snprintf(message, sizeof(message), "uncaught exception %s: %s", name ? name : type->name(), what);
    }
    if (traceFd() >= 0)
        writeFormat(traceFd(), "%s\n", message);
    const int fd = open("/data/Crema/crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0 && traceFd() >= 0)
        writeFormat(traceFd(), "crash.log cannot be opened (errno %d)\n", errno);
    if (fd >= 0) {
        writeFormat(fd, "\n=== %s\n", message);
        close(fd);
    }
    if (traceFd() >= 0)
        writeFormat(traceFd(), "%s\n", message);
    abort();
}

__attribute__((constructor(101))) void earlyInit() {
    if (traceFd() >= 0)
        writeFormat(traceFd(), "static init (text at %p)\n", static_cast<void*>(__text_start));
    std::set_terminate(terminateHandler);
    ps4_install_signal_stack();
    const int signals[] = {kSigIll, kSigAbrt, kSigFpe, kSigBus, kSigSegv};
    for (int sig : signals)
        setHandler(sig, crashHandler, kSaSiginfo | kSaOnstack);
}

}  // namespace

// Appends "<what>: callers 0x.. 0x.." to the boot trace: code addresses found on the calling
// thread's stack (ELF addresses, symbolize like crash.log). A cheap "who calls this" without
// frame pointers; may include stale return addresses.
extern "C" void ps4_boot_trace(const char* stage);
extern "C" __attribute__((noinline)) void ps4_trace_callers(const char* what) {
    // Read from the stack pointer itself: walking past a local variable is undefined behaviour
    // the optimizer is free to drop (an earlier version printed nothing).
    const uint64_t* stack;
    asm volatile("mov %%rsp, %0" : "=r"(stack));
    char line[320];
    int len = snprintf(line, sizeof(line), "%s: callers", what);
    int found = 0;
    for (int i = 0; i < 512 && found < 10 && len < 280; i++) {
        if (isCode(stack[i])) {
            len += snprintf(line + len, sizeof(line) - len, " 0x%llx",
                            static_cast<unsigned long long>(toElf(stack[i])));
            found++;
        }
    }
    ps4_boot_trace(line);
}

// Appends a line to /data/Crema/boot-trace.log (startup milestones), prefixed with the
// seconds since the first trace line.
extern "C" void ps4_boot_trace(const char* stage) {
    const int fd = traceFd();
    if (fd < 0)
        return;
    static timespec s_start;
    static bool s_started = false;
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!s_started) {
        s_start = now;
        s_started = true;
    }
    const double seconds = static_cast<double>(now.tv_sec - s_start.tv_sec) +
                           static_cast<double>(now.tv_nsec - s_start.tv_nsec) / 1e9;
    // One write per line, so lines from different threads don't interleave.
    // Room for the monitor's long lines; a cut line still ends with its newline (a truncated
    // monitor line used to run into the next one).
    char line[2048];
    const size_t length = strlen(stage);
    const bool newline = length == 0 || stage[length - 1] != '\n';
    const int written =
        snprintf(line, sizeof(line), "[%8.3f] %s%s", seconds, stage, newline ? "\n" : "");
    if (written >= static_cast<int>(sizeof(line)))
        line[sizeof(line) - 2] = '\n';
    writeAll(fd, line);
}

// Every open() and close() of the app's own code (Cemu, Mesa, libc++, and the C library's fopen,
// which calls open; linker --wrap, toolchain/ps4-love-style.cmake): the path, when, and the code
// addresses that opened it, so files left open can be named with their opener. The PS4 refuses
// new files at ~250 distinct opens, and Tekken Tag Tournament 2 leaked one every ~7 s until its
// own file loads failed and it stopped itself (1.75). Sony's libraries open files directly
// through the kernel and aren't seen here.
namespace {

constexpr int kTrackedFds = 4096;
struct OpenRecord {
    char path[104];
    uint32_t callers[5];  // elf addresses of code on the stack at the open, innermost first
    float seconds;
    bool used;
};
OpenRecord g_openRecords[kTrackedFds];
uint64_t g_trackStart = 0;

float secondsSinceStart() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t ns = static_cast<uint64_t>(now.tv_sec) * 1000000000ull + now.tv_nsec;
    if (g_trackStart == 0)
        g_trackStart = ns;
    return static_cast<float>((ns - g_trackStart) / 1e9);
}

}  // namespace

extern "C" int __real_open(const char* path, int flags, ...);
extern "C" int __real_close(int fd);

extern "C" __attribute__((noinline)) int __wrap_open(const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const int mode = va_arg(args, int);  // always passed on: harmless when the call had none
    va_end(args);
    const int fd = __real_open(path, flags, mode);
    if (fd >= 0 && fd < kTrackedFds) {
        OpenRecord& record = g_openRecords[fd];
        // the end of the path is the telling part
        const size_t length = path ? strlen(path) : 0;
        const char* tail = length >= sizeof(record.path) ? path + length - (sizeof(record.path) - 1) : path;
        memset(record.callers, 0, sizeof(record.callers));
        const auto* stack = static_cast<const uint64_t*>(__builtin_frame_address(0));
        for (int i = 0, found = 0; i < 400 && found < 5; i++) {
            if (isCode(stack[i])) {
                record.callers[found++] = static_cast<uint32_t>(toElf(stack[i]));
            }
        }
        snprintf(record.path, sizeof(record.path), "%s", tail ? tail : "(null)");
        record.seconds = secondsSinceStart();
        record.used = true;
    }
    return fd;
}

extern "C" int __wrap_close(int fd) {
    if (fd >= 0 && fd < kTrackedFds)
        g_openRecords[fd].used = false;
    return __real_close(fd);
}

// The files open now, except the descriptors in `skip` (a byte per descriptor, nonzero to skip:
// Cemu's FileStreams, which it reports itself): grouped by folder and opener, then the newest.
extern "C" void ps4_open_calls_report(const unsigned char* skip) {
    struct Group {
        char folder[104];
        uint32_t caller;
        int count;
    };
    static Group groups[64];
    int groupCount = 0, total = 0;
    const float now = secondsSinceStart();
    for (int fd = 0; fd < kTrackedFds; fd++) {
        const OpenRecord& record = g_openRecords[fd];
        if (!record.used || (skip && skip[fd]))
            continue;
        total++;
        char folder[104];
        snprintf(folder, sizeof(folder), "%s", record.path);
        if (char* slash = strrchr(folder, '/'))
            *slash = 0;
        // the opener: the first caller outside the C library's own open paths is not known here,
        // so the group key is the folder and the outermost of the first three callers
        const uint32_t caller = record.callers[2] ? record.callers[2] : record.callers[0];
        int g = 0;
        while (g < groupCount && (groups[g].caller != caller || strcmp(groups[g].folder, folder) != 0))
            g++;
        if (g == groupCount) {
            if (groupCount == 64)
                continue;
            snprintf(groups[g].folder, sizeof(groups[g].folder), "%s", folder);
            groups[g].caller = caller;
            groups[g].count = 0;
            groupCount++;
        }
        groups[g].count++;
    }
    char line[300];
    snprintf(line, sizeof(line), "open() files still open (not file streams): %d in %d groups", total, groupCount);
    ps4_boot_trace(line);
    for (int shown = 0; shown < 12; shown++) {
        int best = -1;
        for (int g = 0; g < groupCount; g++)
            if (groups[g].count > 0 && (best < 0 || groups[g].count > groups[best].count))
                best = g;
        if (best < 0)
            break;
        snprintf(line, sizeof(line), "  %dx %s (opener elf 0x%x)", groups[best].count, groups[best].folder,
                 groups[best].caller);
        ps4_boot_trace(line);
        groups[best].count = -groups[best].count;  // shown
    }
    // the six opened last that are still open
    int newest[6] = {-1, -1, -1, -1, -1, -1};
    for (int fd = 0; fd < kTrackedFds; fd++) {
        const OpenRecord& record = g_openRecords[fd];
        if (!record.used || (skip && skip[fd]))
            continue;
        // replace the oldest kept one (an empty slot first) if this one is newer
        int oldest = 0;
        for (int i = 1; i < 6 && newest[oldest] >= 0; i++)
            if (newest[i] < 0 || g_openRecords[newest[i]].seconds < g_openRecords[newest[oldest]].seconds)
                oldest = i;
        if (newest[oldest] < 0 || g_openRecords[newest[oldest]].seconds < record.seconds)
            newest[oldest] = fd;
    }
    for (int i = 0; i < 6; i++) {
        if (newest[i] < 0)
            continue;
        const OpenRecord& record = g_openRecords[newest[i]];
        snprintf(line, sizeof(line), "  newest: fd %d at %.0f s (%.0f s ago) %s, callers elf 0x%x 0x%x 0x%x 0x%x 0x%x",
                 newest[i], record.seconds, now - record.seconds, record.path, record.callers[0], record.callers[1],
                 record.callers[2], record.callers[3], record.callers[4]);
        ps4_boot_trace(line);
    }
}
