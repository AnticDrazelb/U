#include "profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

#include "core.h"
#include "diagnostics.h"
#include "platform.h"

namespace xb::prof {

namespace {

constexpr int kSlots = 96;
constexpr uint32_t kRing = 4096;          // samples a slot keeps between two reports
constexpr uint64_t kGuestTag = 1ull << 63; // a sample of Xbox code: its eip

struct Slot
{
    std::atomic<int> state{0}; // 0 free, 1 live, 2 being signalled
    char name[40] = {};
    uint64_t host = 0;
    uint64_t low = 0, high = 0; // its host stack (the handler finds a host thread's slot by rsp)
    std::atomic<uint32_t> count{0};
    uint64_t ring[kRing] = {};
    uint64_t cpu_prev = 0;
    uint64_t total = 0;                      // samples over the run
    std::map<uint64_t, uint64_t> run_counts; // key -> samples over the run (the reporter's)
};
Slot g_slots[kSlots];
std::atomic<int> g_slot_count{0};
std::atomic<bool> g_active{false};

struct Symbol
{
    uint64_t address, size;
    std::string name;
};
std::vector<Symbol> g_symbols;
uint64_t g_code_base = 0;
std::string g_out;

void LoadSymbols(const std::string& path)
{
    FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "r");
    if (!f)
        return;
    char line[1024];
    while (std::fgets(line, sizeof(line), f))
    {
        char* end = nullptr;
        const uint64_t address = std::strtoull(line, &end, 16);
        if (end == line)
            continue;
        const uint64_t size = std::strtoull(end, &end, 16);
        while (*end == ' ')
            end++;
        std::string name = end;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
            name.pop_back();
        g_symbols.push_back({address, size, std::move(name)});
    }
    std::fclose(f);
    std::sort(g_symbols.begin(), g_symbols.end(), [](const Symbol& a, const Symbol& b) { return a.address < b.address; });
}

// A host address's function: its index + 1 in g_symbols, 0 when none covers it
size_t SymbolOf(uint64_t rip)
{
    if (g_symbols.empty() || rip < g_code_base)
        return 0;
    const uint64_t offset = rip - g_code_base;
    auto it = std::upper_bound(g_symbols.begin(), g_symbols.end(), offset,
                               [](uint64_t v, const Symbol& s) { return v < s.address; });
    if (it == g_symbols.begin())
        return 0;
    --it;
    if (offset >= it->address + std::max<uint64_t>(it->size, 1) && offset - it->address > 0x10000)
        return 0;
    return size_t(it - g_symbols.begin()) + 1;
}

// A function's name, shortened: no argument lists or templates (they make lines a page long)
std::string Short(const std::string& name)
{
    std::string out;
    int depth = 0;
    for (char c : name)
    {
        if (c == '(' || c == '<')
        {
            if (depth++ == 0)
                out += c == '(' ? "()" : "<>";
            continue;
        }
        if (c == ')' || c == '>')
        {
            if (depth > 0)
                depth--;
            continue;
        }
        if (depth == 0)
            out += c;
    }
    if (out.size() > 70)
        out = out.substr(0, 67) + "...";
    return out;
}

// The XBE section an Xbox address is in (its headers in guest memory), "" none
std::string SectionOf(uint32_t eip)
{
    const u32 count = g_xbe.section_count, headers = g_xbe.section_headers;
    for (u32 i = 0; i < count && i < 64 && headers; i++)
    {
        const u32 h = headers + i * 0x38;
        const u32 va = Read<u32>(h + 4), size = Read<u32>(h + 8), name = Read<u32>(h + 0x14);
        if (eip >= va && eip < va + size)
        {
            std::string s;
            for (u32 k = 0; k < 12 && name; k++)
            {
                const char c = char(Read<u8>(name + k));
                if (!c)
                    break;
                s += c;
            }
            return s;
        }
    }
    return "";
}

// A sample's key for counting: an Xbox address in 64-byte buckets (tagged), a host function (its
// symbol index | bit 62), or a host address outside the app's code in 4 KiB pages (the system's)
uint64_t KeyOf(uint64_t sample)
{
    if (sample & kGuestTag)
        return sample & ~uint64_t(63);
    if (const size_t s = SymbolOf(sample))
        return (1ull << 62) | s;
    return sample & ~uint64_t(0xfff);
}

std::string Describe(uint64_t key)
{
    char b[160];
    if (key & kGuestTag)
    {
        const uint32_t eip = uint32_t(key);
        const std::string section = SectionOf(eip);
        std::snprintf(b, sizeof(b), "xbox %#x%s%s", eip, section.empty() ? "" : " in ", section.c_str());
        return b;
    }
    if (key & (1ull << 62))
        return Short(g_symbols[size_t(key & 0xffffffff) - 1].name);
    if (key >= g_code_base && key - g_code_base < 0x40000000ull)
        std::snprintf(b, sizeof(b), "app code +%#llx", (unsigned long long)(key - g_code_base));
    else
    {
        // a system library: usually a thread waiting (blocked in a system call), its CPU share says
        const std::string module = platform::ModuleName(key);
        std::snprintf(b, sizeof(b), "%s %#llx", module.empty() ? "system code" : module.c_str(), (unsigned long long)key);
    }
    return b;
}

void Report(double seconds)
{
    const int n = std::min(g_slot_count.load(), kSlots);
    std::string all;
    for (int i = 0; i < n; i++)
    {
        Slot& s = g_slots[i];
        if (s.state.load() == 0)
            continue;
        // the window's samples
        const uint32_t got = std::min(s.count.exchange(0), kRing);
        std::map<uint64_t, uint32_t> counts;
        for (uint32_t k = 0; k < got; k++)
            counts[KeyOf(s.ring[k])]++;
        // CPU use since the last report
        const uint64_t cpu = platform::ThreadCpuNs(s.host);
        const double share = s.cpu_prev && cpu >= s.cpu_prev ? double(cpu - s.cpu_prev) / (seconds * 1e9) : -1;
        s.cpu_prev = cpu;
        for (auto& [k, c] : counts)
            s.run_counts[k] += c, s.total += c;
        if ((got < 5 && share < 0.05) || std::strcmp(s.name, "profiler") == 0)
            continue; // idle, or the profiler itself: not worth a line
        std::vector<std::pair<uint32_t, uint64_t>> top;
        for (auto& [k, c] : counts)
            top.push_back({c, k});
        std::sort(top.rbegin(), top.rend());
        std::string line;
        char b[64];
        for (size_t k = 0; k < top.size() && k < 8; k++)
        {
            std::snprintf(b, sizeof(b), "%s%.0f%% ", k ? ", " : "", 100.0 * top[k].first / std::max(got, 1u));
            line += b + Describe(top[k].second);
        }
        if (share >= 0)
            Log("profile: %s: CPU %.0f%%, %u samples: %s", s.name, 100 * share, got, line.c_str());
        else
            Log("profile: %s: %u samples: %s", s.name, got, line.c_str());
    }
    // the whole run so far, per thread: the top 40
    if (g_out.empty())
        return;
    for (int i = 0; i < n; i++)
    {
        Slot& s = g_slots[i];
        if (!s.total)
            continue;
        char b[160];
        std::snprintf(b, sizeof(b), "== %s: %llu samples\n", s.name, (unsigned long long)s.total);
        all += b;
        std::vector<std::pair<uint64_t, uint64_t>> top;
        for (auto& [k, c] : s.run_counts)
            top.push_back({c, k});
        std::sort(top.rbegin(), top.rend());
        for (size_t k = 0; k < top.size() && k < 40; k++)
        {
            std::snprintf(b, sizeof(b), "  %5.1f%% ", 100.0 * double(top[k].first) / double(s.total));
            all += b + Describe(top[k].second) + "\n";
        }
    }
    const std::string part = g_out + ".part";
    if (FILE* f = std::fopen(part.c_str(), "w"))
    {
        std::fprintf(f, "xbox-core profile: the whole run (%.0f s), the busiest functions of each thread\n%s",
                     diag::Seconds(), all.c_str());
        std::fclose(f);
        std::rename(part.c_str(), g_out.c_str());
    }
}

} // namespace

bool Active() { return g_active.load(std::memory_order_relaxed); }

void Start(const std::string& symbols, const std::string& out)
{
    static std::atomic<bool> started{false};
    if (started.exchange(true))
        return;
    const char* env = std::getenv("XB_PROFILE");
    if (env && env[0] == '0')
    {
        Log("profile: off (XB_PROFILE=0)");
        return;
    }
    int hz = 200;
    if (const char* r = std::getenv("XB_PROFILE_HZ"))
        hz = std::clamp(std::atoi(r), 10, 2000);
    g_code_base = platform::CodeBase();
    LoadSymbols(symbols);
    g_out = out;
    Log("profile: sampling the registered threads %d times a second (%zu functions named from %s)", hz,
        g_symbols.size(), symbols.empty() ? "nowhere" : symbols.c_str());
    g_active = true;
    std::thread([hz] {
        RegisterThread("profiler");
        const auto period = std::chrono::microseconds(1000000 / hz);
        auto next = std::chrono::steady_clock::now();
        auto last_report = next;
        for (;;)
        {
            next += period;
            std::this_thread::sleep_until(next);
            const int n = std::min(g_slot_count.load(), kSlots);
            for (int i = 0; i < n; i++)
            {
                Slot& s = g_slots[i];
                int live = 1;
                if (!s.state.compare_exchange_strong(live, 2))
                    continue; // free, or going away
                platform::SendSample(s.host);
                s.state.store(1);
            }
            const auto now = std::chrono::steady_clock::now();
            const double since = std::chrono::duration<double>(now - last_report).count();
            if (since >= 10)
            {
                Report(since);
                last_report = now;
            }
            if (now - next > std::chrono::seconds(1))
                next = now; // fell behind (a suspended app): no burst to catch up
        }
    }).detach();
}

int RegisterThread(const char* name)
{
    int slot = -1;
    const int n = std::min(g_slot_count.load(), kSlots);
    for (int i = 0; i < n && slot < 0; i++) // a free slot first (threads come and go)
    {
        int free_state = 0;
        if (g_slots[i].state.load() == 0 && g_slots[i].host == 0 && g_slots[i].state.compare_exchange_strong(free_state, 3))
            slot = i;
    }
    if (slot < 0)
    {
        slot = g_slot_count.fetch_add(1);
        if (slot >= kSlots)
            return -1;
        g_slots[slot].state.store(3);
    }
    Slot& s = g_slots[slot];
    std::snprintf(s.name, sizeof(s.name), "%s", name);
    s.host = platform::CurrentHostThread();
    if (!platform::ThreadStack(s.low, s.high))
    {
        // not known: about where it is (the threads' stacks are at least 2 MiB here)
        const uint64_t here = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
        s.low = here - (2u << 20) + 0x10000;
        s.high = here + 0x10000;
    }
    s.count.store(0);
    s.cpu_prev = 0;
    s.state.store(1);
    return slot;
}

void UnregisterThread(int slot)
{
    if (slot < 0 || slot >= kSlots)
        return;
    Slot& s = g_slots[slot];
    // wait out a signal being sent to it
    for (;;)
    {
        int live = 1;
        if (s.state.compare_exchange_strong(live, 0))
            break;
        if (s.state.load() == 0)
            break;
        std::this_thread::yield();
    }
    s.host = 0;
}

void NoteGuest(int slot, uint32_t eip)
{
    if (slot < 0 || slot >= kSlots)
        return;
    Slot& s = g_slots[slot];
    const uint32_t n = s.count.load(std::memory_order_relaxed);
    if (n < kRing)
    {
        s.ring[n] = kGuestTag | eip;
        s.count.store(n + 1, std::memory_order_release);
    }
}

void NoteHost(uint64_t rip, uint64_t rsp)
{
    const int n = std::min(g_slot_count.load(std::memory_order_relaxed), kSlots);
    for (int i = 0; i < n; i++)
    {
        Slot& s = g_slots[i];
        if (s.state.load(std::memory_order_relaxed) == 0 || rsp < s.low || rsp >= s.high)
            continue;
        const uint32_t c = s.count.load(std::memory_order_relaxed);
        if (c < kRing)
        {
            s.ring[c] = rip & ~kGuestTag;
            s.count.store(c + 1, std::memory_order_release);
        }
        return;
    }
}

} // namespace xb::prof

extern "C" int xb_prof_register_thread(const char* name) { return xb::prof::RegisterThread(name); }
extern "C" void xb_prof_unregister_thread(int slot) { xb::prof::UnregisterThread(slot); }
