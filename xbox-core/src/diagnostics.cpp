#include "diagnostics.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace xb::diag {

std::atomic<u64> g_frames{0}, g_draws{0}, g_kernel_calls{0}, g_methods{0}, g_exceptions{0};

namespace {

const auto g_start = std::chrono::steady_clock::now();

std::mutex g_notes_mutex;
std::map<std::string, std::map<std::string, u64>> g_notes;

std::atomic<u64> g_per_ordinal[400];

// the guest threads, for reports read without locks: append-only
constexpr u32 kMaxThreads = 512;
GuestThread* g_threads[kMaxThreads];
std::atomic<u32> g_thread_count{0};

// the GPU's latest methods
struct MethodRecord
{
    u32 cls, method, parameter;
};
constexpr u32 kMethodRing = 256;
MethodRecord g_method_ring[kMethodRing];
std::atomic<u32> g_method_next{0};

void Put(int fd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Put(int fd, const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0)
        (void)!write(fd, line, size_t(std::min(n, int(sizeof(line)) - 1)));
}

const char* Name(u32 ordinal)
{
    const char* n = KernelExportName(ordinal);
    return n ? n : "?";
}

} // namespace

double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(); }

void Note(const char* category, const std::string& what)
{
    bool first;
    {
        std::lock_guard lock(g_notes_mutex);
        first = g_notes[category][what]++ == 0;
    }
    if (first)
        Log("not emulated (%s): %s", category, what.c_str());
}

std::string NotesSummary()
{
    std::lock_guard lock(g_notes_mutex);
    std::string out;
    if (g_notes.empty())
        return "nothing noted\n";
    for (const auto& [category, items] : g_notes)
    {
        out += category + ":\n";
        std::vector<std::pair<u64, std::string>> sorted;
        for (const auto& [what, count] : items)
            sorted.push_back({count, what});
        std::sort(sorted.rbegin(), sorted.rend());
        for (const auto& [count, what] : sorted)
            out += "  " + what + " (" + std::to_string(count) + "x)\n";
    }
    return out;
}

void CallStarted(GuestThread* t, u32 ordinal, u32 a0, u32 a1, u32 a2, u32 a3, u32 return_eip)
{
    g_kernel_calls++;
    if (ordinal < 400)
        g_per_ordinal[ordinal]++;
    if (!t)
        return;
    const u32 i = t->calls_next.fetch_add(1) % GuestThread::kCallRecords;
    t->calls[i] = {ordinal, a0, a1, a2, a3, return_eip, 0, float(Seconds())};
    t->in_call = ordinal + 1;
    t->call_started = float(Seconds());
}

void CallEnded(GuestThread* t, u32 result)
{
    if (!t)
        return;
    const u32 n = t->calls_next.load();
    if (n)
        t->calls[(n - 1) % GuestThread::kCallRecords].result = result;
    t->in_call = 0;
}

void Method(u32 cls, u32 method, u32 parameter)
{
    g_methods++;
    g_method_ring[g_method_next.fetch_add(1) % kMethodRing] = {cls, method, parameter};
}

void RegisterThread(GuestThread* t)
{
    const u32 i = g_thread_count.load();
    if (i < kMaxThreads)
    {
        g_threads[i] = t;
        g_thread_count = i + 1;
    }
}

void WriteStateLockFree(int fd)
{
    const float now = float(Seconds());
    Put(fd, "guest state at %.3f s: %llu kernel calls, %llu frames, %llu draws, %llu GPU methods, %llu exceptions\n", now,
        (unsigned long long)g_kernel_calls.load(), (unsigned long long)g_frames.load(), (unsigned long long)g_draws.load(),
        (unsigned long long)g_methods.load(), (unsigned long long)g_exceptions.load());
    const u32 count = std::min(g_thread_count.load(), kMaxThreads);
    for (u32 i = 0; i < count; i++)
    {
        GuestThread* t = g_threads[i];
        if (!t)
            continue;
        const u32 in = t->in_call;
        Put(fd, "thread %u (%s), KPCR %#x, stack %#x-%#x: %s", t->id, t->name.c_str(), t->kpcr, t->stack_limit, t->stack_top,
            in ? "in the kernel call " : "running guest code (or not started)\n");
        if (in)
            Put(fd, "%s for %.1f s\n", Name(in - 1), double(now - t->call_started));
        const u32 n = t->calls_next.load();
        const u32 shown = std::min<u32>(n, GuestThread::kCallRecords);
        for (u32 k = 0; k < shown; k++)
        {
            const CallRecord& c = t->calls[(n - shown + k) % GuestThread::kCallRecords];
            Put(fd, "    %9.3f %s(%#x, %#x, %#x, %#x) from %#x = %#x\n", double(c.at), Name(c.ordinal), c.a0, c.a1, c.a2, c.a3,
                c.eip, c.result);
        }
    }
    const u32 m = g_method_next.load();
    const u32 shown = std::min(m, kMethodRing);
    Put(fd, "the GPU's latest %u methods (class method = parameter):\n", shown);
    for (u32 k = 0; k < shown; k++)
    {
        const MethodRecord& r = g_method_ring[(m - shown + k) % kMethodRing];
        Put(fd, "    %#04x %#06x = %#x\n", r.cls, r.method, r.parameter);
    }
}

void LogState(const char* title)
{
    Log("==== %s: the guest's state", title);
    // the lock-free text, through a pipe-less temporary file, into the log line by line
    char path[] = "/tmp/xbox-core-state-XXXXXX";
    int fd = mkstemp(path);
    std::string text;
    if (fd >= 0)
    {
        WriteStateLockFree(fd);
        lseek(fd, 0, SEEK_SET);
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            text.append(buf, size_t(n));
        close(fd);
        unlink(path);
    }
    size_t at = 0;
    while (at < text.size())
    {
        const size_t end = text.find('\n', at);
        Log("%s", text.substr(at, end == std::string::npos ? std::string::npos : end - at).c_str());
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    Log("what wasn't emulated:");
    const std::string notes = NotesSummary();
    at = 0;
    while (at < notes.size())
    {
        const size_t end = notes.find('\n', at);
        Log("%s", notes.substr(at, end - at).c_str());
        at = end + 1;
    }
}

void StartMonitor(const std::string& hang_file, void (*host_stacks)(const char*, const char*))
{
    std::thread([hang_file, host_stacks] {
        u64 last_frames = 0, last_draws = 0, last_calls = 0, last_methods = 0;
        u64 last_ordinal[400] = {};
        u32 screen_changes = 0;
        double last_activity = Seconds(), last_stats = Seconds();
        bool any_activity = false, hung = false;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const double now = Seconds();
            const u64 frames = g_frames, draws = g_draws;
            static u64 seen_frames = 0, seen_draws = 0;
            // the picture itself, sampled (a program drawing into one framebuffer flips nothing)
            u64 sum = 0;
            const DisplayInfo d = GetDisplay();
            if (d.valid)
                for (int y = 0; y < d.height; y += std::max(1, d.height / 32))
                    for (int x = 0; x < d.width; x += std::max(1, d.width / 32))
                        sum = sum * 1099511628211ull + DisplayPixel(d, x, y);
            static u64 seen_sum = 0;
            const bool picture_changed = d.valid && sum != seen_sum;
            seen_sum = sum;
            if (picture_changed)
                screen_changes++;
            if (frames != seen_frames || draws != seen_draws || picture_changed)
            {
                seen_frames = frames, seen_draws = draws;
                last_activity = now, any_activity = true;
                if (hung)
                {
                    hung = false;
                    Log("hang watchdog: frames again");
                }
            }
            // the figures, every 10 s
            if (now - last_stats >= 10)
            {
                const double span = now - last_stats;
                const u64 calls = g_kernel_calls, methods = g_methods;
                std::vector<std::pair<u64, u32>> busiest;
                for (u32 o = 0; o < 400; o++)
                {
                    const u64 c = g_per_ordinal[o].load();
                    if (c > last_ordinal[o])
                        busiest.push_back({c - last_ordinal[o], o});
                    last_ordinal[o] = c;
                }
                std::sort(busiest.rbegin(), busiest.rend());
                std::string top;
                for (size_t i = 0; i < busiest.size() && i < 5; i++)
                    top += std::string(i ? ", " : "") + Name(busiest[i].second) + " " + std::to_string(busiest[i].first);
                Log("stats: %.1f frames/s, the picture changed in %u of the last %.0f s, %.0f draws/s, %.0f kernel calls/s (%s), "
                    "%.0f GPU methods/s, %u guest threads, %u MiB free",
                    double(frames - last_frames) / span, screen_changes, span, double(draws - last_draws) / span,
                    double(calls - last_calls) / span,
                    top.empty() ? "none" : top.c_str(), double(methods - last_methods) / span, g_thread_count.load(),
                    FreePages() * 4096 / (1024 * 1024));
                last_frames = frames, last_draws = draws, last_calls = calls, last_methods = methods;
                last_stats = now;
                screen_changes = 0;
            }
            // the watchdog
            const double quiet = now - last_activity;
            if (!hung && ((any_activity && quiet >= 20) || (!any_activity && now >= 90)))
            {
                hung = true;
                const char* reason = any_activity ? "no frames, draws or picture changes for 20 s after they had started"
                                                  : "no frame, draw or picture 90 s after the start";
                Log("hang watchdog: %s: every thread's state in %s and below", reason, hang_file.c_str());
                const int fd = open(hang_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd >= 0)
                {
                    Put(fd, "hang watchdog at %.1f s: %s\n\n", now, reason);
                    WriteStateLockFree(fd);
                    close(fd);
                }
                if (host_stacks)
                    host_stacks(hang_file.c_str(), "every host thread at the hang");
                LogState("hang");
            }
        }
    }).detach();
}

} // namespace xb::diag
