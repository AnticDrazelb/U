// PS4: results on the TV (a text console drawn into a video-out framebuffer), in
// /data/switch-bench/results.txt and as system notifications; the memory probes, which ask the
// system for what a Switch emulator would need; and the start page, which shows what an unfinished
// last run left behind (its results, crash.log and boot trace) so a photo of it is enough to see
// where it stopped, and lets the player pick what to run.
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <stb/stb_easy_font.h>

#include "platform.h"

// Declared by hand: the toolchain headers give these the wrong (or no) prototypes
// (as in port/ps4_runtime.cpp).
extern "C" {
int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
int32_t sceKernelReserveVirtualRange(void** addr, size_t len, int32_t flags, size_t alignment);
int32_t sceKernelMunmap(void* addr, size_t len);
int32_t sceKernelMapNamedSystemFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags,
                                              const char* name);
int32_t sceKernelMapNamedFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags, const char* name);
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int32_t type,
                                      off_t* physOut);
int32_t sceKernelMapDirectMemory(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys,
                                 size_t align);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t len);
int32_t sceKernelSendNotificationRequest(int32_t device, void* request, size_t size, int32_t blocking);
int32_t sceSystemServiceHideSplashScreen(void);
// port/ps4_runtime.cpp: the app heap behind malloc
const char* ps4rt_heap_source(void);
size_t ps4rt_heap_size(void);
size_t ps4rt_heap_footprint(void);
// port/ps4_crashlog.cpp: a line in /data/switch-bench/boot-trace.log
void ps4_boot_trace(const char* stage);
}

namespace {

constexpr size_t MB = 1024 * 1024;
constexpr size_t GB = 1024 * MB;
constexpr int32_t kProtCpuRw = 0x3, kProtCpuGpuRw = 0x33;
constexpr int32_t kMapFixed = 0x10;
constexpr const char* kResultsDir = "/data/switch-bench";
constexpr const char* kResultsFile = "/data/switch-bench/results.txt";
constexpr const char* kCrashFile = "/data/switch-bench/crash.log";
// The last run's boot trace: Crema's runtime renames boot-trace.log to this as the app starts.
constexpr const char* kLastTraceFile = "/data/switch-bench/boot-trace-launcher.log";

struct NotificationRequest
{
    int32_t type, reqId, priority, msgId, targetId, userId, unk1, unk2, appId, errorNum, unk3;
    unsigned char useIconImageUri;
    char message[1024];
    char iconUri[1024];
    char unk[1024];
};

double Seconds()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// The screen: 1920x1080 linear ARGB, two buffers, redrawn whenever a line is added.
class Screen
{
public:
    static constexpr int kWidth = 1920, kHeight = 1080;
    static constexpr int kCell = 7; // stb_easy_font's widest advance: drawn as a fixed-width font
    static constexpr int kMargin = 48;

    bool Open()
    {
        handle = sceVideoOutOpen(0xFF, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
        if (handle < 0)
            return false;
        const size_t buffer_size = (size_t(kWidth) * kHeight * 4 + 2 * MB - 1) & ~(2 * MB - 1);
        off_t phys = 0;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), buffer_size * 2, 2 * MB, 3, &phys) != 0)
            return false;
        void* base = nullptr;
        if (sceKernelMapDirectMemory(&base, buffer_size * 2, kProtCpuGpuRw, 0, phys, 2 * MB) != 0)
            return false;
        buffers[0] = static_cast<uint32_t*>(base);
        buffers[1] = reinterpret_cast<uint32_t*>(static_cast<char*>(base) + buffer_size);
        OrbisVideoOutBufferAttribute attr;
        sceVideoOutSetBufferAttribute(&attr, 0x80000000 /* A8R8G8B8 sRGB */, 1 /* linear */, 0, kWidth, kHeight,
                                      kWidth);
        if (sceVideoOutRegisterBuffers(handle, 0, reinterpret_cast<void* const*>(buffers), 2, &attr) != 0)
            return false;
        sceVideoOutSetFlipRate(handle, 0);
        open = true;
        return true;
    }

    // Text size: 2 for the results (readable in a photo), 1.75 for the longer start page.
    void SetScale(float s)
    {
        scale = s;
        line_height = int(12 * s);
    }
    void Clear()
    {
        lines.clear();
        status.clear();
    }

    void Add(const std::string& line, bool draw = true)
    {
        // wrap at the screen's width (in columns), on spaces; continuation lines indented
        const size_t columns = (kWidth - 2 * kMargin) / int(kCell * scale);
        std::string rest = line;
        do
        {
            size_t cut = rest.size();
            if (cut > columns)
            {
                const size_t space = rest.rfind(' ', columns);
                cut = space == std::string::npos || space < columns / 2 ? columns : space;
            }
            lines.push_back(rest.substr(0, cut));
            const size_t next = rest.find_first_not_of(' ', cut);
            rest = next == std::string::npos ? "" : "      " + rest.substr(next);
        } while (!rest.empty());
        if (draw)
            Draw();
    }

    void SetStatus(const std::string& text)
    {
        status = text;
        Draw();
    }

    void Draw()
    {
        if (!open)
            return;
        // the other buffer was on screen until the last flip: give that flip two frames to happen
        const double now = Seconds();
        if (now - last_flip < 0.034)
            usleep(useconds_t((0.034 - (now - last_flip)) * 1e6));
        uint32_t* fb = buffers[current];
        std::fill(fb, fb + size_t(kWidth) * kHeight, 0xFF101418u);
        Fill(fb, 0, 0, kWidth, 6, 0xFF3A8EE6u);
        // the log, scrolled to its end, and below it the status line
        const int rows = (kHeight - 2 * kMargin) / line_height - 2;
        const size_t first = lines.size() > size_t(rows) ? lines.size() - rows : 0;
        int y = kMargin;
        for (size_t i = first; i < lines.size(); i++, y += line_height)
            Text(fb, kMargin, y, lines[i], Color(lines[i], i));
        if (!status.empty())
        {
            Fill(fb, 0, kHeight - kMargin - line_height - 12, kWidth, kHeight - kMargin - line_height - 10, 0xFF2A3445u);
            Text(fb, kMargin, kHeight - kMargin - line_height, status, 0xFF8FB4E8u);
        }
        sceVideoOutSubmitFlip(handle, current, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
        last_flip = Seconds();
        current ^= 1;
        if (!shown)
        {
            shown = true;
            sceSystemServiceHideSplashScreen();
        }
    }

private:
    int handle = -1;
    bool open = false, shown = false;
    uint32_t* buffers[2] = {};
    int current = 0;
    double last_flip = 0;
    float scale = 2.0f;
    int line_height = 24;
    std::vector<std::string> lines;
    std::string status;

    static uint32_t Color(const std::string& line, size_t index)
    {
        if (line.find("WRONG") != std::string::npos || line.find("PROBLEMS") != std::string::npos ||
            line.find("fault") != std::string::npos || line.find("failed") != std::string::npos ||
            line.find("DOESN'T") != std::string::npos || line.find("signal") != std::string::npos ||
            line.find("didn't finish") != std::string::npos)
            return 0xFFFF6B6Bu;
        if (index == 0 || line.rfind("Switch code runs", 0) == 0)
            return 0xFF7EE787u;
        if (line.rfind("kernel ", 0) == 0 || line.rfind("three cores", 0) == 0 || line.rfind("memory:", 0) == 0 ||
            line.rfind("its ", 0) == 0 || line.rfind("Press", 0) == 0)
            return 0xFFF2CC60u;
        return 0xFFE6E6E6u;
    }
    static void Fill(uint32_t* fb, int x0, int y0, int x1, int y1, uint32_t color)
    {
        x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, kWidth), y1 = std::min(y1, kHeight);
        for (int y = y0; y < y1; y++)
            std::fill(fb + size_t(y) * kWidth + x0, fb + size_t(y) * kWidth + std::max(x0, x1), color);
    }
    // stb_easy_font gives each character as quads; draw them as filled rectangles, scaled, one
    // character per fixed-width cell so the result table's columns line up.
    void Text(uint32_t* fb, int x, int y, const std::string& text, uint32_t color) const
    {
        struct Vertex
        {
            float x, y, z;
            unsigned char c[4];
        };
        Vertex quads[4 * 64];
        for (size_t col = 0; col < text.size(); col++)
        {
            char ch[2] = {text[col], 0};
            if (ch[0] == ' ' || ch[0] < 32 || ch[0] > 126)
                continue;
            const int advance = stb_easy_font_charinfo[ch[0] - 32].advance & 15;
            const float cx = float(col * kCell) + float(kCell - advance) / 2.0f;
            const int count = stb_easy_font_print(cx, 0, ch, nullptr, quads, sizeof(quads));
            for (int q = 0; q < count; q++)
            {
                const Vertex* v = &quads[q * 4];
                Fill(fb, x + int(v[0].x * scale + 0.5f), y + int(v[0].y * scale + 0.5f),
                     x + int(v[2].x * scale + 0.5f), y + int(v[2].y * scale + 0.5f), color);
            }
        }
    }
};

std::mutex g_mutex;
Screen g_screen;
FILE* g_file = nullptr;

void Trace(const std::string& step)
{
    ps4_boot_trace(("switch-bench: " + step).c_str());
}

void SendNotification(const std::string& message)
{
    static NotificationRequest request; // 3 KiB: not on the stack
    std::memset(&request, 0, sizeof(request));
    request.type = 0;
    request.targetId = -1;
    request.useIconImageUri = 1;
    std::snprintf(request.message, sizeof(request.message), "%s", message.c_str());
    std::snprintf(request.iconUri, sizeof(request.iconUri), "cxml://psnotification/tex_icon_system");
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

// ---- the start page: what the last run left behind, and what to run

// The lines of a file, from at most its last `max_bytes`.
std::vector<std::string> ReadLines(const char* path, long max_bytes = 64 * 1024)
{
    std::vector<std::string> lines;
    FILE* f = std::fopen(path, "r");
    if (!f)
        return lines;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, size > max_bytes ? size - max_bytes : 0, SEEK_SET);
    char line[1024];
    bool first = size > max_bytes; // starts mid-line
    while (std::fgets(line, sizeof(line), f))
    {
        line[std::strcspn(line, "\r\n")] = 0;
        if (!first)
            lines.push_back(line);
        first = false;
    }
    std::fclose(f);
    return lines;
}

std::vector<std::string> Last(const std::vector<std::string>& lines, size_t n)
{
    return std::vector<std::string>(lines.end() - std::min(n, lines.size()), lines.end());
}

// crash.log's last report, compacted: its header, rip and registers, the code addresses found on
// the stack (several to a line), and the threads named in the dump that follows it.
std::vector<std::string> LastCrash(const std::vector<std::string>& log)
{
    size_t start = log.size();
    for (size_t i = log.size(); i-- > 0;)
        if (log[i].rfind("=== ", 0) == 0)
        {
            start = i;
            break;
        }
    std::vector<std::string> out;
    std::string addresses;
    int address_count = 0, threads = 0;
    bool in_dump = false;
    for (size_t i = start; i < log.size(); i++)
    {
        const std::string& l = log[i];
        if (l.rfind("====", 0) == 0 && !in_dump)
        {
            in_dump = true;
            if (!addresses.empty())
                out.push_back("   stack: " + addresses);
            addresses.clear();
        }
        if (in_dump)
        {
            if (l.rfind("-- ", 0) == 0 && threads++ < 8)
                out.push_back("   thread " + l.substr(3));
            continue;
        }
        if (l.empty() || l.rfind("code addresses", 0) == 0)
            continue;
        const size_t bracket = l.find("] 0x");
        if (l.rfind("  [rsp+", 0) == 0 && bracket != std::string::npos)
        {
            if (address_count < 40)
                addresses += (address_count % 10 ? " " : "") + l.substr(bracket + 2);
            if (++address_count % 10 == 0 && address_count <= 40)
            {
                out.push_back("   stack: " + addresses);
                addresses.clear();
            }
            continue;
        }
        out.push_back("   " + l);
    }
    if (!addresses.empty())
        out.push_back("   stack: " + addresses);
    return out;
}

// The pad of the user who started the app, or none.
int g_pad = -1;
void OpenPad()
{
    sceUserServiceInitialize(nullptr);
    int32_t user = -1;
    if (sceUserServiceGetInitialUser(&user) != 0)
        return;
    scePadInit();
    g_pad = scePadOpen(user, 0, 0, nullptr);
}
uint32_t PadButtons()
{
    union {
        OrbisPadData data;
        unsigned char room[512]; // in case the system's structure is larger than the header's
    } state{};
    if (g_pad < 0 || scePadReadState(g_pad, &state.data) != 0 || !state.data.connected)
        return 0;
    return state.data.buttons;
}

// Shows the start page and waits for a choice. Without a controller, or after a finished run,
// everything runs after a countdown.
Platform::Mode StartPage()
{
    const std::vector<std::string> results = ReadLines(kResultsFile);
    const std::vector<std::string> crash = ReadLines(kCrashFile);
    const std::vector<std::string> trace = ReadLines(kLastTraceFile);
    const bool finished = std::any_of(results.begin(), results.end(),
                                      [](const std::string& l) { return l.rfind("Done.", 0) == 0; });
    const bool unfinished = !crash.empty() || (!results.empty() && !finished);

    g_screen.SetScale(1.75f);
    g_screen.Add("Switch CPU Bench " BENCH_VERSION, false);
    if (unfinished)
    {
        g_screen.Add("The last run didn't finish. What it left behind is below: take a photo of this screen first.",
                     false);
        g_screen.Add("", false);
        g_screen.Add("its results so far (results.txt):", false);
        if (results.empty())
            g_screen.Add("   (none: it stopped before showing anything)", false);
        for (const std::string& l : Last(results, 14))
            g_screen.Add("   " + l, false);
        g_screen.Add("its crash report (crash.log):", false);
        const std::vector<std::string> report = LastCrash(crash);
        if (report.empty())
            g_screen.Add("   (none)", false);
        for (const std::string& l : report)
            g_screen.Add(l, false);
        g_screen.Add("its last steps (boot-trace):", false);
        if (trace.empty())
            g_screen.Add("   (none)", false);
        for (const std::string& l : Last(trace, 12))
            g_screen.Add("   " + l, false);
        g_screen.Add("", false);
    }
    g_screen.Add("Press  X: run everything (3-5 minutes)    SQUARE: CPU tests only    TRIANGLE: memory checks only",
                 false);
    OpenPad();
    Trace(fmt::format("start page: last run {}, pad {}", unfinished ? "unfinished" : "finished or none", g_pad));

    // with a controller after an unfinished run: wait for the choice (the photo comes first)
    const bool wait = unfinished && g_pad >= 0;
    const int countdown = g_pad < 0 ? 90 : 20;
    const double start = Seconds();
    uint32_t previous = PadButtons(); // a button still held from starting the app doesn't count
    Platform::Mode mode = Platform::Mode::All;
    for (int last_shown = -1;;)
    {
        const uint32_t buttons = PadButtons();
        const uint32_t pressed = buttons & ~previous;
        previous = buttons;
        if (pressed & ORBIS_PAD_BUTTON_CROSS)
            break;
        if (pressed & ORBIS_PAD_BUTTON_SQUARE)
        {
            mode = Platform::Mode::CpuOnly;
            break;
        }
        if (pressed & ORBIS_PAD_BUTTON_TRIANGLE)
        {
            mode = Platform::Mode::MemoryOnly;
            break;
        }
        const int left = countdown - int(Seconds() - start);
        if (!wait && left <= 0)
            break;
        if (!wait && left != last_shown)
        {
            last_shown = left;
            g_screen.SetStatus(fmt::format("{}everything runs in {} s", g_pad < 0 ? "no controller found: " : "",
                                           left));
        }
        else if (wait && last_shown != 0)
        {
            last_shown = 0;
            g_screen.SetStatus("waiting for a button");
        }
        usleep(50 * 1000);
    }
    Trace(fmt::format("start page: mode {}", int(mode)));

    // the last run's files are kept, under other names
    rename(kResultsFile, "/data/switch-bench/results-previous.txt");
    rename(kCrashFile, "/data/switch-bench/crash-previous.log");
    g_screen.Clear();
    g_screen.SetScale(2.0f);
    return mode;
}

// ---- memory probes

std::string Mib(size_t bytes)
{
    return bytes >= 10 * GB / 10 ? fmt::format("{:.2f} GiB", double(bytes) / GB) : fmt::format("{} MiB", bytes / MB);
}

// Whether code at `code` (mapped RWX) runs: a "ret" is called, and a fault on it is caught and
// skipped. As port/ps4_runtime.cpp's runsCode, which found consoles (firmware 11.00) where RWX
// mappings succeed but don't execute. FreeBSD's signal numbers and context layout.
constexpr int kSigBusFreeBsd = 10, kSigSegvFreeBsd = 11, kSaSiginfoFreeBsd = 0x40;
constexpr int kMcontextOffset = 0x40, kRipIndex = 20, kRspIndex = 23;
volatile uintptr_t g_probeCode = 0;
volatile int g_probeFaulted = 0;
struct sigaction g_oldSegv, g_oldBus;

void ExecProbeHandler(int, siginfo_t*, void* context)
{
    auto* regs = reinterpret_cast<uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
    if (g_probeCode && regs[kRipIndex] == g_probeCode)
    {
        // as if the ret ran: back to the caller
        regs[kRipIndex] = *reinterpret_cast<const uint64_t*>(regs[kRspIndex]);
        regs[kRspIndex] += 8;
        g_probeFaulted = 1;
        return;
    }
    // another fault: the previous handlers get it when the instruction runs again
    sigaction(kSigSegvFreeBsd, &g_oldSegv, nullptr);
    sigaction(kSigBusFreeBsd, &g_oldBus, nullptr);
}

bool RunsCode(void* code)
{
    static_cast<volatile uint8_t*>(code)[0] = 0xC3; // ret
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    void (*handler)(int, siginfo_t*, void*) = ExecProbeHandler;
    std::memcpy(&action, &handler, sizeof(handler)); // the handler union is the first member
    action.sa_flags = kSaSiginfoFreeBsd;
    sigemptyset(&action.sa_mask);
    g_probeFaulted = 0;
    g_probeCode = reinterpret_cast<uintptr_t>(code);
    sigaction(kSigSegvFreeBsd, &action, &g_oldSegv);
    sigaction(kSigBusFreeBsd, &action, &g_oldBus);
    void (*volatile function)() = reinterpret_cast<void (*)()>(code);
    function();
    sigaction(kSigSegvFreeBsd, &g_oldSegv, nullptr);
    sigaction(kSigBusFreeBsd, &g_oldBus, nullptr);
    g_probeCode = 0;
    return !g_probeFaulted;
}


// Each check is traced before it runs and logged when it's done, so a check that crashes leaves
// the earlier ones' results and its own name behind.
void Check(const char* name)
{
    Trace(fmt::format("memory check: {}", name));
    Platform::Progress(fmt::format("memory checks: {}", name));
}

// Flexible memory (system and regular pools): the app heap, page tables, code caches come from here.
void CheckFlexible()
{
    Check("flexible memory");
    size_t regular = 0;
    sceKernelAvailableFlexibleMemorySize(&regular);
    Platform::Log(fmt::format("flexible memory: {} free; the app heap (malloc) is {} of {}, {} in use", Mib(regular),
                              Mib(ps4rt_heap_size()), ps4rt_heap_source(), Mib(ps4rt_heap_footprint())));

    Check("system flexible memory, 64 MiB at a time");
    std::vector<void*> blocks;
    for (int i = 0; i < 64; i++)
    {
        void* at = nullptr;
        if (sceKernelReserveVirtualRange(&at, 64 * MB, 0, 2 * MB) != 0)
            break;
        if (sceKernelMapNamedSystemFlexibleMemory(&at, 64 * MB, kProtCpuRw, kMapFixed, "switch-bench probe") != 0)
        {
            sceKernelMunmap(at, 64 * MB);
            break;
        }
        blocks.push_back(at);
    }
    for (void* b : blocks)
        sceKernelMunmap(b, 64 * MB);
    Platform::Log(fmt::format("  system flexible memory still free: {}", Mib(blocks.size() * 64 * MB)));

    // Eden's page table for a 39-bit address space: 2^27 entries of 8 bytes, from either pool
    auto map1g = [](bool system_pool) {
        void* at = nullptr;
        if (sceKernelReserveVirtualRange(&at, GB, 0, 2 * MB) != 0)
            return false;
        const int32_t ret = system_pool
                                ? sceKernelMapNamedSystemFlexibleMemory(&at, GB, kProtCpuRw, kMapFixed, "switch-bench probe")
                                : sceKernelMapNamedFlexibleMemory(&at, GB, kProtCpuRw, kMapFixed, "switch-bench probe");
        sceKernelMunmap(at, GB);
        return ret == 0;
    };
    Check("a 1 GiB page table, system pool");
    const bool table_system = map1g(true);
    Check("a 1 GiB page table, regular flexible memory");
    const bool table_regular = map1g(false);
    Platform::Log(fmt::format("  Eden's page table (1 GiB for a 39-bit address space): {}",
                              table_system && table_regular ? "fits in either pool"
                              : table_system               ? "fits in the system pool only"
                              : table_regular              ? "fits in regular flexible memory only"
                                                           : "DOESN'T FIT (Eden's 39-bit page table needs rework)"));
}

// Direct memory (the GPU's pool, 4.5 GiB on a retail console): how much can be allocated, and can
// 4 GiB of it be mapped as one CPU buffer (the Switch's DRAM, as Eden's DeviceMemory needs)?
void CheckDirect()
{
    Check("direct memory");
    struct Chunk
    {
        off_t phys;
        size_t size;
    };
    std::vector<Chunk> chunks;
    chunks.reserve(512); // no allocation while all of it is taken
    size_t total = 0;
    for (size_t size : {256 * MB, 64 * MB, 16 * MB})
        for (;;)
        {
            off_t phys = 0;
            if (chunks.size() == chunks.capacity() ||
                sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 2 * MB, 0, &phys) != 0)
                break;
            chunks.push_back({phys, size});
            total += size;
        }

    // map up to 4 GiB of it into one reserved range, back to back
    Check("4 GiB of direct memory as one buffer");
    void* range = nullptr;
    size_t mapped = 0;
    const bool reserved = sceKernelReserveVirtualRange(&range, 4 * GB, 0, 2 * MB) == 0;
    if (reserved)
    {
        for (const Chunk& c : chunks)
        {
            if (mapped + c.size > 4 * GB)
                break;
            void* at = static_cast<char*>(range) + mapped;
            if (sceKernelMapDirectMemory(&at, c.size, kProtCpuRw, kMapFixed, c.phys, 2 * MB) != 0 ||
                at != static_cast<char*>(range) + mapped)
                break;
            static_cast<volatile char*>(at)[0] = 1; // really there
            static_cast<volatile char*>(at)[c.size - 1] = 1;
            mapped += c.size;
        }
        sceKernelMunmap(range, 4 * GB);
    }
    for (const Chunk& c : chunks)
        sceKernelReleaseDirectMemory(c.phys, c.size);
    Platform::Log(fmt::format("direct memory: {} in all, {} of it free for this app",
                              Mib(sceKernelGetDirectMemorySize()), Mib(total)));
    Platform::Log(fmt::format("  Switch DRAM (4 GiB) as one CPU buffer: {}{}",
                              mapped >= 4 * GB ? "yes" : fmt::format("no, {} mapped", Mib(mapped)),
                              reserved ? "" : " (no 4 GiB address range)"));
    Platform::Log(fmt::format("  left for the GPU after that: {}", Mib(total > 4 * GB ? total - 4 * GB : 0)));
}

// Executable memory for the recompilers' code caches (Eden: 512 MiB per core), asked of each pool
// directly (the runtime's mmap would fall back to memory that may not run code), up to 1.5 GiB.
void CheckExecutable()
{
    constexpr int32_t kProtCpuRwx = 0x7;
    constexpr size_t kChunk = 64 * MB, kMaxChunks = 24;
    auto runs = [](bool any, bool ok) { return !any ? "" : ok ? " (runs code)" : " (maps, but DOESN'T RUN CODE)"; };

    Check("executable system flexible memory");
    std::vector<void*> system;
    bool system_runs = false;
    for (size_t i = 0; i < kMaxChunks; i++)
    {
        void* at = nullptr;
        if (sceKernelReserveVirtualRange(&at, kChunk, 0, 2 * MB) != 0)
            break;
        if (sceKernelMapNamedSystemFlexibleMemory(&at, kChunk, kProtCpuRwx, kMapFixed, "switch-bench code probe") != 0)
        {
            sceKernelMunmap(at, kChunk);
            break;
        }
        if (system.empty())
        {
            Check("does executable system flexible memory run code");
            system_runs = RunsCode(at);
        }
        system.push_back(at);
    }
    for (void* b : system)
        sceKernelMunmap(b, kChunk);
    Platform::Log(fmt::format("executable memory (code caches; Eden asks 3 x 512 MiB), probed up to 1.5 GiB:"));
    Platform::Log(fmt::format("  system flexible memory: {}{}", Mib(system.size() * kChunk),
                              runs(!system.empty(), system_runs)));

    Check("executable direct memory");
    struct Chunk
    {
        void* at;
        off_t phys;
    };
    std::vector<Chunk> direct;
    direct.reserve(kMaxChunks);
    bool direct_runs = false;
    for (size_t i = 0; i < kMaxChunks; i++)
    {
        off_t phys = 0;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), kChunk, 2 * MB, 0, &phys) != 0)
            break;
        void* at = nullptr;
        if (sceKernelMapDirectMemory(&at, kChunk, kProtCpuRwx, 0, phys, 2 * MB) != 0)
        {
            sceKernelReleaseDirectMemory(phys, kChunk);
            break;
        }
        if (direct.empty())
        {
            Check("does executable direct memory run code");
            direct_runs = RunsCode(at);
        }
        direct.push_back({at, phys});
    }
    for (const Chunk& c : direct)
    {
        sceKernelMunmap(c.at, kChunk);
        sceKernelReleaseDirectMemory(c.phys, kChunk);
    }
    Platform::Log(fmt::format("  direct memory: {}{}", Mib(direct.size() * kChunk), runs(!direct.empty(), direct_runs)));
}

} // namespace

namespace Platform {

Mode Init()
{
    mkdir(kResultsDir, 0777);
    Trace("started");
    Trace(g_screen.Open() ? "screen open" : "no screen (video out failed)");
    const Mode mode = StartPage();
    g_file = std::fopen(kResultsFile, "w");
    return mode;
}

void Log(const std::string& text)
{
    std::lock_guard lock{g_mutex};
    if (g_file)
    {
        std::fprintf(g_file, "%s\n", text.c_str());
        std::fflush(g_file);
    }
    size_t start = 0;
    do
    {
        const size_t end = std::min(text.find('\n', start), text.size());
        g_screen.Add(text.substr(start, end - start));
        start = end + 1;
    } while (start < text.size());
}

void Notify(const std::string& message)
{
    SendNotification(message);
}

void Progress(const std::string& status)
{
    std::lock_guard lock{g_mutex};
    Trace("step: " + status);
    g_screen.SetStatus(status);
}

void MemoryChecks()
{
    Log("memory:");
    CheckFlexible();
    CheckDirect();
    CheckExecutable();
}

void Finish(const std::string& summary)
{
    Progress("finished");
    Log(fmt::format("Done. Saved to {}. Take a photo of this screen, then close the app (PS button).", kResultsFile));
    SendNotification(summary.substr(0, 1000));
    for (;;)
        sleep(1);
}

} // namespace Platform
