#include "diagnostics.h"
#include "gpu.h"
#include "devices.h"
#include "platform.h"

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

std::atomic<u64> g_raster_ns{0};
std::atomic<u64> g_frames{0}, g_draws{0}, g_kernel_calls{0}, g_methods{0}, g_exceptions{0};

namespace {

const auto g_start = std::chrono::steady_clock::now();
inline uint64_t Tsc()
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t(hi) << 32) | lo;
}
const uint64_t g_tsc_start = Tsc();
// seconds since the start from the time-stamp counter, with no C library (the fault handler's
// clock): the counter's rate measured against the clock since the start, out of the handler
std::atomic<double> g_tsc_per_second{0};
float TscSeconds(uint64_t tsc)
{
    const double rate = g_tsc_per_second.load(std::memory_order_relaxed);
    return rate > 0 ? float(double(tsc - g_tsc_start) / rate) : 0.0f;
}

std::mutex g_notes_mutex;
std::map<std::string, std::map<std::string, u64>> g_notes;

std::atomic<u64> g_per_ordinal[400];

// the guest threads, for reports read without locks: append-only
constexpr u32 kMaxThreads = 512;
GuestThread* g_threads[kMaxThreads];
std::atomic<u32> g_thread_count{0};

// the latest faults in guest code
struct FaultRecord
{
    float at;
    u32 thread, eip, address, regs[8];
    int signal;
    u8 code[16];
    const char* outcome;
};
constexpr u32 kFaultRing = 128;
FaultRecord g_fault_ring[kFaultRing];
std::atomic<u32> g_fault_next{0};
std::atomic<u64> g_faults{0};

// the GPU's latest methods
struct MethodRecord
{
    u32 cls, method, parameter;
};
constexpr u32 kMethodRing = 256;
MethodRecord g_method_ring[kMethodRing];
std::atomic<u32> g_method_next{0};

// to a file with write() (a crash report's: no locks, no allocation), or into a string
struct Out
{
    int fd = -1;
    std::string* text = nullptr;
};
void Put(Out out, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Put(Out out, const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    const size_t length = size_t(std::min(n, int(sizeof(line)) - 1));
    if (out.text)
        out.text->append(line, length);
    else
        (void)!write(out.fd, line, length);
}

std::atomic<bool> g_run{false};
std::atomic<u32> g_run_number{0};
std::atomic<u32> g_failures_logged{0};
// every kernel call, as it starts and as it ends, for the first ones of a run: the start-up's
// every step, and the last thing a program did before it stopped
constexpr u32 kTraceCalls = 5000;
std::atomic<u32> g_traced{0};

const char* Name(u32 ordinal)
{
    const char* n = KernelExportName(ordinal);
    return n ? n : "?";
}

} // namespace

double Seconds()
{
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    if (s > 0.2)
        g_tsc_per_second.store(double(Tsc() - g_tsc_start) / s, std::memory_order_relaxed);
    return s;
}

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
    if (g_traced++ < kTraceCalls)
        Log("call %u> %s(%#x, %#x, %#x, %#x) from %#x", t->id, Name(ordinal), a0, a1, a2, a3, return_eip);
}

void CallEnded(GuestThread* t, u32 result)
{
    if (!t)
        return;
    const u32 n = t->calls_next.load();
    if (n)
    {
        CallRecord& r = t->calls[(n - 1) % GuestThread::kCallRecords];
        r.result = result;
        if (g_traced < kTraceCalls)
            Log("call %u< %s = %#x", t->id, Name(r.ordinal), result);
        // an NTSTATUS error (nothing of the guest's is at 0xc0000000: no pointer looks like one)
        if (result >= 0xC0000000u && result < 0xC0010000u && g_failures_logged++ < 3000)
            Log("kernel: %s(%#x, %#x, %#x, %#x) from %#x failed: %#x", Name(r.ordinal), r.a0, r.a1, r.a2, r.a3, r.eip,
                result);
    }
    t->in_call = 0;
}

// Faults counted by instruction (lock-free: this can run in the signal handler): the hottest ones are
// what backpatching or a faster path would save most on.
struct FaultSite
{
    std::atomic<u32> eip{0};
    std::atomic<u64> count{0};
    u8 code[8];
    u32 address;
};
constexpr u32 kFaultSites = 1024;
FaultSite g_fault_sites[kFaultSites];

void CountFaultSite(u32 eip, u32 address, const u8* code)
{
    for (u32 i = 0, h = (eip * 2654435761u) % kFaultSites; i < 16; i++, h = (h + 1) % kFaultSites)
    {
        FaultSite& s = g_fault_sites[h];
        u32 cur = s.eip.load(std::memory_order_relaxed);
        if (cur == 0)
        {
            if (s.eip.compare_exchange_strong(cur, eip))
            {
                std::memcpy(s.code, code, 8);
                s.address = address;
                cur = eip;
            }
        }
        if (cur == eip)
        {
            s.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void RecordFault(u32 thread, int signal, u32 eip, u32 address, const u8* code, const u32 regs[8], const char* outcome)
{
    g_faults++;
    CountFaultSite(eip, address, code);
    FaultRecord& r = g_fault_ring[g_fault_next.fetch_add(1) % kFaultRing];
    r.at = TscSeconds(Tsc()); // no clock call: this runs in the fault handler
    r.thread = thread, r.eip = eip, r.address = address, r.signal = signal, r.outcome = outcome;
    std::memcpy(r.regs, regs, sizeof(r.regs));
    std::memcpy(r.code, code, sizeof(r.code));
}

struct DeviceRegister
{
    std::atomic<u32> address{0};
    std::atomic<u64> reads{0}, writes{0};
    std::atomic<u32> last{0}, eip{0};
};
constexpr u32 kDeviceRegisters = 16384, kDeviceRing = 64;
DeviceRegister g_device_registers[kDeviceRegisters];
struct DeviceRecord
{
    float at;
    u32 address, value, eip;
    u8 size, write;
};
DeviceRecord g_device_ring[kDeviceRing];
std::atomic<u32> g_device_next{0};

void DeviceAccess(u32 address, u32 size, u32 value, bool write, u32 eip)
{
    for (u32 i = 0, h = (address * 2654435761u) % kDeviceRegisters; i < 64; i++, h = (h + 1) % kDeviceRegisters)
    {
        DeviceRegister& r = g_device_registers[h];
        u32 cur = r.address.load(std::memory_order_relaxed);
        if (cur == 0 && r.address.compare_exchange_strong(cur, address))
            cur = address;
        if (cur == address)
        {
            (write ? r.writes : r.reads).fetch_add(1, std::memory_order_relaxed);
            r.last.store(value, std::memory_order_relaxed);
            r.eip.store(eip, std::memory_order_relaxed);
            break;
        }
    }
    DeviceRecord& d = g_device_ring[g_device_next.fetch_add(1) % kDeviceRing];
    d.at = TscSeconds(Tsc());
    d.address = address, d.value = value, d.eip = eip, d.size = u8(size), d.write = write;
}

void Method(u32 cls, u32 method, u32 parameter)
{
    g_methods++;
    g_method_ring[g_method_next.fetch_add(1) % kMethodRing] = {cls, method, parameter};
}

GuestThread* ThreadByStack(u32 esp)
{
    const u32 n = std::min<u32>(g_thread_count.load(std::memory_order_acquire), kMaxThreads);
    for (u32 i = 0; i < n; i++)
        if (GuestThread* t = g_threads[i]; t && esp >= t->stack_limit && esp < t->stack_top)
            return t;
    return nullptr;
}

u32 ThreadCount() { return std::min<u32>(g_thread_count.load(std::memory_order_acquire), kMaxThreads); }
GuestThread* ThreadAt(u32 i) { return i < ThreadCount() ? g_threads[i] : nullptr; }

void RegisterThread(GuestThread* t)
{
    const u32 i = g_thread_count.load();
    if (i < kMaxThreads)
    {
        g_threads[i] = t;
        g_thread_count = i + 1;
    }
}

void WriteState(Out fd)
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
        // its last kernel call's stack, and its last fault's registers and stack (the guest's stacks
        // are always mapped: read without risk)
        auto dump_stack = [&](const char* what, u32 esp) {
            if (esp < kStackBase || esp + 64 * 4 > kStackBase + kStackAreaSize)
                return;
            Put(fd, "    %s, esp %#x:", what, esp);
            for (u32 w = 0; w < 64; w++)
                Put(fd, "%s%08x", w % 8 ? " " : "\n      ", Read<u32>(esp + w * 4));
            Put(fd, "\n");
        };
        dump_stack("the stack at its last kernel call", u32(Read<u64>(t->kpcr + kpcr::GuestEsp)));
        if (const u32 feip = Read<u32>(t->kpcr + kpcr::FaultEip))
        {
            Put(fd, "    its last fault: signal %u at %#x, address %#x; eax %#x ecx %#x edx %#x ebx %#x esp %#x ebp %#x "
                    "esi %#x edi %#x eflags %#x\n",
                Read<u32>(t->kpcr + kpcr::FaultSignal), feip, Read<u32>(t->kpcr + kpcr::FaultAddress),
                Read<u32>(t->kpcr + kpcr::FaultRegs), Read<u32>(t->kpcr + kpcr::FaultRegs + 4),
                Read<u32>(t->kpcr + kpcr::FaultRegs + 8), Read<u32>(t->kpcr + kpcr::FaultRegs + 12),
                Read<u32>(t->kpcr + kpcr::FaultRegs + 16), Read<u32>(t->kpcr + kpcr::FaultRegs + 20),
                Read<u32>(t->kpcr + kpcr::FaultRegs + 24), Read<u32>(t->kpcr + kpcr::FaultRegs + 28),
                Read<u32>(t->kpcr + kpcr::FaultEflags));
            dump_stack("the stack at its last fault", Read<u32>(t->kpcr + kpcr::FaultRegs + 16));
        }
        // where it was when sampled (the watchdog's samples of a thread running guest code): each
        // eip and how often, the latest sample's registers and code, and memory at its registers
        if (const u32 sn = std::min<u32>(t->samples.load(std::memory_order_acquire), GuestThread::kSamples))
        {
            std::map<u32, u32> at;
            for (u32 k = 0; k < sn; k++)
                at[t->sample_eip[k]]++;
            Put(fd, "    sampled %u times in guest code, at (eip x times):", sn);
            for (const auto& [eip, times] : at)
                Put(fd, " %#x x%u", eip, times);
            const u32* g = t->sample_regs;
            Put(fd, "\n    the latest sample: eax %#x ecx %#x edx %#x ebx %#x esp %#x ebp %#x esi %#x edi %#x\n"
                    "    its code from eip - 16:",
                g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
            for (u32 b = 0; b < 48; b++)
                Put(fd, "%s%02x", b == 16 ? " | " : " ", t->sample_code[b]);
            Put(fd, "\n");
            static const char* const kNames[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
            for (int r = 0; r < 8; r++)
            {
                const u32 a = g[r] & ~3u;
                if (a < 0x10000 || !GuestMapped(a, 32))
                    continue;
                Put(fd, "      [%s %#x]:", kNames[r], a);
                for (u32 w = 0; w < 8; w++)
                    Put(fd, " %08x", Read<u32>(a + w * 4));
                Put(fd, "\n");
            }
        }
        const u32 n = t->calls_next.load();
        const u32 shown = std::min<u32>(n, GuestThread::kCallRecords);
        for (u32 k = 0; k < shown; k++)
        {
            const CallRecord& c = t->calls[(n - shown + k) % GuestThread::kCallRecords];
            Put(fd, "    %9.3f %s(%#x, %#x, %#x, %#x) from %#x = %#x\n", double(c.at), Name(c.ordinal), c.a0, c.a1, c.a2, c.a3,
                c.eip, c.result);
        }
    }
    {
        // the instructions that fault most
        u32 order[kFaultSites], used = 0;
        for (u32 i = 0; i < kFaultSites; i++)
            if (g_fault_sites[i].eip.load())
                order[used++] = i;
        std::sort(order, order + used, [](u32 a, u32 b) { return g_fault_sites[a].count.load() > g_fault_sites[b].count.load(); });
        Put(fd, "faults by instruction (the most frequent of %u; backpatched hardware accesses so far: %llu at %u sites):\n",
            used, (unsigned long long)BackpatchedAccessCount(), BackpatchedSiteCount());
        for (u32 k = 0; k < used && k < 16; k++)
        {
            const FaultSite& f = g_fault_sites[order[k]];
            Put(fd, "    %10llu  eip %#x  address %#x  code %02x %02x %02x %02x %02x %02x %02x %02x\n",
                (unsigned long long)f.count.load(), f.eip.load(), f.address, f.code[0], f.code[1], f.code[2], f.code[3],
                f.code[4], f.code[5], f.code[6], f.code[7]);
        }
    }
    {
        // the device registers read most (a program waiting on hardware polls one), and the latest accesses
        static u32 order[kDeviceRegisters];
        u32 used = 0;
        for (u32 i = 0; i < kDeviceRegisters; i++)
            if (g_device_registers[i].address.load())
                order[used++] = i;
        std::sort(order, order + used, [](u32 a, u32 b) {
            return g_device_registers[a].reads.load() > g_device_registers[b].reads.load();
        });
        Put(fd, "device registers by reads (of %u; a thread waiting on hardware polls the first ones):\n", used);
        for (u32 k = 0; k < used && k < 16; k++)
        {
            const DeviceRegister& r = g_device_registers[order[k]];
            Put(fd, "    %10llu reads %8llu writes  %#010x (%s)  last %#x, by eip %#x\n", (unsigned long long)r.reads.load(),
                (unsigned long long)r.writes.load(), r.address.load(), DeviceName(r.address.load()), r.last.load(),
                r.eip.load());
        }
        const u32 dn = g_device_next.load(), dshown = std::min(dn, kDeviceRing);
        Put(fd, "the latest %u device register accesses (time, read/write, address, size, value, eip):\n", dshown);
        for (u32 k = 0; k < dshown; k++)
        {
            const DeviceRecord& r = g_device_ring[(dn - dshown + k) % kDeviceRing];
            Put(fd, "    %9.3f %s %#010x/%u %#x  eip %#x\n", double(r.at), r.write ? "write" : "read ", r.address, r.size,
                r.value, r.eip);
        }
    }
    const u32 fn = g_fault_next.load();
    const u32 fshown = std::min(fn, kFaultRing);
    Put(fd, "faults in guest code: %llu; the latest %u (time, thread, signal, eip, address, what became of it, "
            "registers eax..edi, code at eip):\n", (unsigned long long)g_faults.load(), fshown);
    for (u32 k = 0; k < fshown; k++)
    {
        const FaultRecord& r = g_fault_ring[(fn - fshown + k) % kFaultRing];
        Put(fd, "    %9.3f t%u sig %d eip %#x addr %#x %s\n      regs %x %x %x %x %x %x %x %x\n      code", double(r.at),
            r.thread, r.signal, r.eip, r.address, r.outcome ? r.outcome : "?", r.regs[0], r.regs[1], r.regs[2], r.regs[3],
            r.regs[4], r.regs[5], r.regs[6], r.regs[7]);
        for (u8 b : r.code)
            Put(fd, " %02x", b);
        Put(fd, "\n");
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

void WriteStateLockFree(int fd) { WriteState(Out{fd, nullptr}); }

void LogState(const char* title)
{
    Log("==== %s: the guest's state", title);
    std::string text;
    WriteState(Out{-1, &text});
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

void SetRunning(bool running)
{
    if (running)
    {
        g_run_number++;
        g_traced = 0;
        Log("trace: the first %u kernel calls, each as it starts (\"call <thread>>\") and ends (\"call <thread><\")",
            kTraceCalls);
    }
    g_run = running;
}

void StartMonitor(const std::string& hang_file, void (*host_stacks)(const char*, const char*), void (*after)())
{
    std::thread([hang_file, host_stacks, after] {
        u64 last_frames = 0, last_draws = 0, last_calls = 0, last_methods = 0;
        u64 last_ordinal[400] = {};
        u32 screen_changes = 0;
        double last_activity = Seconds(), last_stats = Seconds();
        bool any_activity = false, hung = false;
        double last_snapshot = 0;
        u32 run_number = 0;
        double run_start = Seconds();
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const double now = Seconds();
            if (!g_run) // no program running (not yet, or it ended): nothing to watch
                continue;
            if (run_number != g_run_number)
            {
                run_number = g_run_number;
                run_start = last_activity = last_stats = now;
                any_activity = hung = false;
                last_snapshot = 0;
            }
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
                    last_snapshot = 0;
                    Log("watchdog: frames again");
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
                static u64 last_raster = 0, last_faults = 0, last_patched = 0;
                const u64 raster = g_raster_ns, faults = g_faults, patched = BackpatchedAccessCount();
                Log("stats: %.1f frames/s, the picture changed in %u of the last %.0f s, %.0f draws/s, drawing %.0f%% of the "
                    "time, %.0f kernel calls/s (%s), %.0f GPU methods/s, %.0f faults/s, %.0f patched hardware accesses/s, "
                    "%u guest threads, %u MiB free",
                    double(frames - last_frames) / span, screen_changes, span, double(draws - last_draws) / span,
                    100.0 * double(raster - last_raster) / 1e9 / span, double(calls - last_calls) / span,
                    top.empty() ? "none" : top.c_str(), double(methods - last_methods) / span,
                    double(faults - last_faults) / span, double(patched - last_patched) / span, g_thread_count.load(),
                    FreePages() * 4096 / (1024 * 1024));
                if (gpu::Enabled())
                    if (const std::string declined = gpu::DeclinedSince(); !declined.empty())
                        Log("stats: draws the GPU left to software (slower), by reason: %s", declined.c_str());
                Log("stats: sound: %s", SoundStats(span).c_str());
                {
                    const DisplayInfo d = GetDisplay();
                    Log("stats: the picture: %s at %#x, %dx%d, format %#x, pitch %u%s", d.valid ? "shown" : "no mode set",
                        d.address, d.width, d.height, d.format, d.pitch,
                        d.overlay.on ? ", with the video overlay on" : "");
                }
                last_raster = raster, last_faults = faults, last_patched = patched;
                last_frames = frames, last_draws = draws, last_calls = calls, last_methods = methods;
                last_stats = now;
                screen_changes = 0;
            }
            // the watchdog: no picture 10 s into a program, or none for 10 s after there was one: a
            // snapshot of every thread (the guest's and the host's: where each is running), and again
            // every 30 s while it lasts (does it move?), each into the log bundle
            const double quiet = now - last_activity;
            if (quiet >= 10 && (last_snapshot == 0 || now - last_snapshot >= 30))
            {
                if (!hung)
                    hung = true;
                last_snapshot = now;
                const char* reason = any_activity ? "no frames, draws or picture changes for 10 s after they had started"
                                                  : "no frame, draw or picture yet";
                Log("watchdog: %s (%.0f s into the program): every thread's state in %s and below", reason, now - run_start,
                    hang_file.c_str());
                // where each thread running guest code is: 32 samples apiece, 2 ms apart (the fault
                // handler takes them: safe in guest code)
                {
                    const u32 count = std::min(g_thread_count.load(), kMaxThreads);
                    for (u32 i = 0; i < count; i++)
                        if (GuestThread* t = g_threads[i]; t && t->host.load())
                            t->samples.store(0);
                    for (int k = 0; k < 32; k++)
                    {
                        for (u32 i = 0; i < count; i++)
                            if (GuestThread* t = g_threads[i]; t && t->host.load() && !t->in_call.load())
                                platform::SendSample(t->host.load());
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                }
                const int fd = open(hang_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd >= 0)
                {
                    Put(Out{fd}, "watchdog at %.1f s (%.0f s into the program), xbox-core build %s: %s\n\n", now,
                        now - run_start, kBuildId, reason);
                    WriteStateLockFree(fd);
                    close(fd);
                }
                if (host_stacks)
                    host_stacks(hang_file.c_str(), "every host thread (a guest thread's rip is its EIP)");
                LogState("watchdog");
                if (after)
                    after();
            }
        }
    }).detach();
}

} // namespace xb::diag
