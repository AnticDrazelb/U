// PS4: results on the TV (a text console drawn into a video-out framebuffer) and in
// /data/xbox-bench/results.txt; memory at fixed addresses (flexible memory, and direct memory for
// aliases); FreeBSD's signal context; and the start page, which shows what an unfinished last run
// left behind (its results, crash.log and boot trace) so a photo of it is enough to see where it
// stopped, and lets the player pick what to run. The screen and start page are switch-bench's.
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
int32_t sceKernelCreateEqueue(OrbisKernelEqueue* queue, const char* name);
int32_t sceKernelWaitEqueue(OrbisKernelEqueue queue, void* events, int32_t count, int32_t* out, uint32_t* timeout_us);
// port/ps4_runtime.cpp: the app heap behind malloc
const char* ps4rt_heap_source(void);
size_t ps4rt_heap_size(void);
size_t ps4rt_heap_footprint(void);
int32_t sceSysmoduleLoadModuleInternal(uint32_t id);
// port/ps4_crashlog.cpp: a line in /data/xbox-bench/boot-trace.log
void ps4_boot_trace(const char* stage);
}

namespace {

constexpr size_t MB = 1024 * 1024;
constexpr int32_t kProtCpuGpuRw = 0x33;
constexpr int32_t kMapFixed = 0x10;
constexpr const char* kResultsDir = "/data/xbox-bench";
constexpr const char* kResultsFile = "/data/xbox-bench/results.txt";
constexpr const char* kCrashFile = "/data/xbox-bench/crash.log";
// The last run's boot trace: Crema's runtime renames boot-trace.log to this as the app starts.
constexpr const char* kLastTraceFile = "/data/xbox-bench/boot-trace-launcher.log";

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

    // Video out set up as Crema's launcher does it on the same consoles (patches/cemu-ps4.patch,
    // Display::CreateFrames): the same calls in the same order, three linear A8B8G8R8 buffers in
    // write-combined direct memory, and a flip event queue to wait on. Version 01.01 also called
    // sceVideoOutSetFlipRate, which Crema never does, and the app was ended inside this function
    // on a console, before its first frame, without a crash report. Each call is traced.
    bool Open()
    {
        TraceStep("screen: sceVideoOutOpen");
        handle = sceVideoOutOpen(0xFF, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
        if (handle < 0)
            return Failed("sceVideoOutOpen", handle);
        TraceStep("screen: flip event queue");
        sceKernelCreateEqueue(&flip_queue, "xbox-bench flip");
        sceVideoOutAddFlipEvent(flip_queue, handle, nullptr);
        TraceStep("screen: direct memory for 3 frames");
        const size_t buffer_size = (size_t(kWidth) * kHeight * 4 + 2 * MB - 1) & ~(2 * MB - 1);
        off_t phys = 0;
        int32_t ret = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), buffer_size * kFrames,
                                                    2 * MB, 3 /* write-combined, as Crema's VRAM */, &phys);
        if (ret != 0)
            return Failed("sceKernelAllocateDirectMemory", ret);
        TraceStep("screen: map it");
        void* base = nullptr;
        ret = sceKernelMapDirectMemory(&base, buffer_size * kFrames, kProtCpuGpuRw, 0, phys, 2 * MB);
        if (ret != 0)
            return Failed("sceKernelMapDirectMemory", ret);
        for (int i = 0; i < kFrames; i++)
            buffers[i] = reinterpret_cast<uint32_t*>(static_cast<char*>(base) + buffer_size * i);
        TraceStep("screen: register buffers");
        OrbisVideoOutBufferAttribute attr;
        sceVideoOutSetBufferAttribute(&attr, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                      ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0, kWidth, kHeight, kWidth);
        ret = sceVideoOutRegisterBuffers(handle, 0, reinterpret_cast<void* const*>(buffers), kFrames, &attr);
        if (ret < 0)
            return Failed("sceVideoOutRegisterBuffers", ret);
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
        WaitForImageOffScreen();
        uint32_t* fb = buffers[current];
        std::fill(fb, fb + size_t(kWidth) * kHeight, Native(0xFF101418u));
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
        if (flips == 0)
            TraceStep("screen: first flip");
        sceVideoOutSubmitFlip(handle, current, ORBIS_VIDEO_OUT_FLIP_VSYNC, int64_t(++flips));
        if (flips == 1)
            TraceStep("screen: first flip submitted");
        current = (current + 1) % kFrames;
    }

private:
    static constexpr int kFrames = 3;
    int handle = -1;
    bool open = false;
    OrbisKernelEqueue flip_queue = 0;
    uint32_t* buffers[kFrames] = {};
    int current = 0;
    uint64_t flips = 0;
    float scale = 2.0f;
    int line_height = 24;
    std::vector<std::string> lines;
    std::string status;

    static void TraceStep(const char* step) { ps4_boot_trace((std::string("xbox-bench: ") + step).c_str()); }
    static bool Failed(const char* call, int32_t ret)
    {
        ps4_boot_trace(fmt::format("xbox-bench: screen: {} failed ({:#x}); results go to the file only", call,
                                   uint32_t(ret)).c_str());
        return false;
    }
    // The image drawn next must be off screen: with 3 in rotation it was shown by flip n-2 and is
    // replaced on screen by flip n-1 (n = flips submitted). As Crema's launcher waits.
    void WaitForImageOffScreen()
    {
        if (flips < 2)
            return;
        const int64_t needed = int64_t(flips - 1);
        for (int tries = 0; tries < 40; tries++)
        {
            OrbisVideoOutFlipStatus status = {};
            if (sceVideoOutGetFlipStatus(handle, &status) < 0 || status.flipArg >= needed)
                return;
            unsigned char event[64];
            int32_t count = 0;
            uint32_t timeout = 5000;
            sceKernelWaitEqueue(flip_queue, event, 1, &count, &timeout);
        }
    }
    // Colors are written as 0xAARRGGBB; the buffers are A8B8G8R8 (bytes R, G, B, A), as Crema's.
    static uint32_t Native(uint32_t argb)
    {
        return (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFFu) | ((argb & 0xFFu) << 16);
    }
    static uint32_t Color(const std::string& line, size_t index)
    {
        if (line.find("WRONG") != std::string::npos || line.find("PROBLEMS") != std::string::npos ||
            line.find("fault") != std::string::npos || line.find("failed") != std::string::npos ||
            line.find("DOESN'T") != std::string::npos || line.find("signal") != std::string::npos ||
            line.find("didn't finish") != std::string::npos ||
            line.find(": NO") != std::string::npos || line.find("FAILED") != std::string::npos)
            return 0xFFFF6B6Bu;
        if (index == 0 || line.rfind("Xbox code runs", 0) == 0)
            return 0xFF7EE787u;
        if (line.rfind("kernel ", 0) == 0 || line.rfind("three cores", 0) == 0 || line.rfind("memory", 0) == 0 ||
            line.rfind("cpu:", 0) == 0 || line.rfind("segments:", 0) == 0 || line.rfind("what an", 0) == 0 ||
            line.rfind("its ", 0) == 0 || line.rfind("Press", 0) == 0)
            return 0xFFF2CC60u;
        return 0xFFE6E6E6u;
    }
    static void Fill(uint32_t* fb, int x0, int y0, int x1, int y1, uint32_t argb)
    {
        const uint32_t color = Native(argb);
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
    ps4_boot_trace(("xbox-bench: " + step).c_str());
}

// The system modules of every library this program calls, loaded before any call into them, as
// Crema's frontend does (LoadSystemModules in patches/cemu-ps4.patch): "calling into a system
// library whose module isn't loaded kills the app silently". 01.00 to 01.02 didn't, and were
// ended in their first sceVideoOutOpen without a crash report. (Crema also loads AudioOut, which
// this program doesn't use; Net is for the runtime's resolver, port/ps4_net.cpp.)
void LoadSystemModules()
{
    const std::pair<const char*, uint32_t> modules[] = {{"SystemService", 0x80000010},
                                                        {"UserService", 0x80000011},
                                                        {"VideoOut", 0x80000022},
                                                        {"Pad", 0x80000024},
                                                        {"Net", 0x8000001C}};
    for (const auto& [name, id] : modules)
    {
        const int32_t result = sceSysmoduleLoadModuleInternal(id);
        Trace(fmt::format("system module {}: {}", name,
                          result == 0 ? std::string("loaded") : fmt::format("FAILED ({:#x})", uint32_t(result))));
    }
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
    Trace("pad: sceUserServiceInitialize");
    sceUserServiceInitialize(nullptr);
    int32_t user = -1;
    if (sceUserServiceGetInitialUser(&user) != 0)
        return Trace("pad: no initial user");
    Trace("pad: scePadInit");
    scePadInit();
    g_pad = scePadOpen(user, 0, 0, nullptr);
    Trace(fmt::format("pad: scePadOpen = {:#x}", uint32_t(g_pad)));
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
    g_screen.Add("Xbox CPU Bench " BENCH_VERSION, false);
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
    g_screen.Add("Press  X: run everything (about a minute)    SQUARE: CPU tests only    TRIANGLE: memory checks only",
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
    rename(kResultsFile, "/data/xbox-bench/results-previous.txt");
    rename(kCrashFile, "/data/xbox-bench/crash-previous.log");
    g_screen.Clear();
    g_screen.SetScale(2.0f);
    return mode;
}

// ---- memory at fixed addresses

constexpr int32_t kProtCpuRead = 0x1, kProtCpuWrite = 0x2, kProtCpuExec = 0x4;
constexpr size_t kPage = 16 * 1024; // the PS4 maps in 16 KiB pages

int32_t Prot(int access)
{
    return ((access & Platform::Read) ? kProtCpuRead : 0) | ((access & Platform::Write) ? kProtCpuWrite : 0) |
           ((access & Platform::Exec) ? kProtCpuExec : 0);
}

// Reserves exactly [addr, addr + size) without replacing anything there: asked as a hint (no
// fixed flag) and given back if it lands elsewhere.
std::string ReserveAt(uint64_t addr, size_t size)
{
    if (addr % kPage || size % kPage)
        return fmt::format("not in whole {} KiB pages", kPage / 1024);
    void* at = reinterpret_cast<void*>(addr);
    const int32_t r = sceKernelReserveVirtualRange(&at, size, 0, kPage);
    if (r != 0)
        return fmt::format("sceKernelReserveVirtualRange {:#x}", uint32_t(r));
    if (reinterpret_cast<uint64_t>(at) != addr)
    {
        sceKernelMunmap(at, size);
        return fmt::format("in use (the system offered {})", at);
    }
    return "";
}

// Direct memory behind an alias, released when its second view is unmapped.
struct AliasRecord
{
    uint64_t b;
    off_t phys;
    size_t size;
};
std::vector<AliasRecord> g_aliases;

// ---- faults (FreeBSD's signal numbers and context: kMcontextOffset, the register order of
// port/ps4_crashlog.cpp)

constexpr int kSigIllFreeBsd = 4, kSigBusFreeBsd = 10, kSigSegvFreeBsd = 11, kSaSiginfoFreeBsd = 0x40;
constexpr int kMcontextOffset = 0x40;
constexpr int kGprIndex[8] = {7, 4, 3, 8, 23, 9, 2, 1}; // rax rcx rdx rbx rsp rbp rsi rdi
constexpr int kRipIndex = 20, kCsIndex = 21, kAddrIndex = 17;
constexpr int kSignals[3] = {kSigSegvFreeBsd, kSigBusFreeBsd, kSigIllFreeBsd};
Platform::FaultHandler g_handler = nullptr;
struct sigaction g_old[3];

uint64_t* Regs(void* context)
{
    return reinterpret_cast<uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
}

void OnFault(int sig, siginfo_t*, void* context)
{
    uint64_t* regs = Regs(context);
    Platform::FaultContext c{};
    for (int i = 0; i < 8; i++)
        c.gpr[i] = &regs[kGprIndex[i]];
    c.rip = &regs[kRipIndex];
    c.address = regs[kAddrIndex];
    c.signal = sig;
    c.cs = uint16_t(regs[kCsIndex]);
    c.raw = context;
    if (g_handler && g_handler(c))
        return;
    // not ours: the crash log's handlers get it when the instruction runs again
    for (int i = 0; i < 3; i++)
        if (kSignals[i] == sig)
            sigaction(sig, &g_old[i], nullptr);
}

} // namespace

namespace Platform {

Mode Init()
{
    mkdir(kResultsDir, 0777);
    Trace("started");
    LoadSystemModules();
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

void Progress(const std::string& status)
{
    std::lock_guard lock{g_mutex};
    ::Trace("step: " + status);
    g_screen.SetStatus(status);
}

void Trace(const std::string& step)
{
    ::Trace(step);
}

void Finish(const std::string&)
{
    Progress("finished");
    Log(fmt::format("Done. Saved to {}. Take a photo of this screen, then close the app (PS button).", kResultsFile));
    for (;;)
        sleep(1);
}

std::string MapFixed(uint64_t addr, size_t size, int access)
{
    std::string e = ReserveAt(addr, size);
    if (!e.empty())
        return e;
    void* at = reinterpret_cast<void*>(addr);
    // code only runs from system flexible memory; data comes from regular flexible memory
    const int32_t r = (access & Exec)
                          ? sceKernelMapNamedSystemFlexibleMemory(&at, size, Prot(access), kMapFixed, "xbox-bench code")
                          : sceKernelMapNamedFlexibleMemory(&at, size, Prot(access), kMapFixed, "xbox-bench data");
    if (r != 0)
    {
        sceKernelMunmap(reinterpret_cast<void*>(addr), size);
        return fmt::format("{} {:#x}", (access & Exec) ? "sceKernelMapNamedSystemFlexibleMemory"
                                                       : "sceKernelMapNamedFlexibleMemory",
                           uint32_t(r));
    }
    return "";
}

std::string ReserveFixed(uint64_t addr, size_t size)
{
    return ReserveAt(addr, size);
}

void Unmap(uint64_t addr, size_t size)
{
    sceKernelMunmap(reinterpret_cast<void*>(addr), size);
    for (size_t i = 0; i < g_aliases.size(); i++)
        if (g_aliases[i].b == addr)
        {
            sceKernelReleaseDirectMemory(g_aliases[i].phys, g_aliases[i].size);
            g_aliases.erase(g_aliases.begin() + long(i));
            break;
        }
}

std::string MapAlias(uint64_t a, uint64_t b, size_t size, int access)
{
    if (size % kPage)
        return fmt::format("direct memory maps in {} KiB pages only", kPage / 1024);
    off_t phys = 0;
    int32_t r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, kPage, 0, &phys);
    if (r != 0)
        return fmt::format("sceKernelAllocateDirectMemory {:#x}", uint32_t(r));
    std::string e;
    bool a_mapped = false;
    for (uint64_t view : {a, b})
    {
        e = ReserveAt(view, size);
        if (!e.empty())
            break;
        void* at = reinterpret_cast<void*>(view);
        r = sceKernelMapDirectMemory(&at, size, Prot(access), kMapFixed, phys, kPage);
        if (r != 0)
        {
            sceKernelMunmap(reinterpret_cast<void*>(view), size);
            e = fmt::format("sceKernelMapDirectMemory ({} view) {:#x}", view == a ? "first" : "second", uint32_t(r));
            break;
        }
        a_mapped = true;
    }
    if (!e.empty())
    {
        if (a_mapped)
            sceKernelMunmap(reinterpret_cast<void*>(a), size);
        sceKernelReleaseDirectMemory(phys, size);
        return e;
    }
    g_aliases.push_back({b, phys, size});
    return "";
}

size_t PageSize()
{
    return kPage;
}

std::string PoolName(int access)
{
    return (access & Exec) ? "system flexible memory, RWX" : "flexible memory";
}

void SetCs(FaultContext& context, uint16_t cs)
{
    Regs(context.raw)[kCsIndex] = cs;
    context.cs = cs;
}

void InstallFaultHandler(FaultHandler handler)
{
    g_handler = handler;
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    void (*function)(int, siginfo_t*, void*) = OnFault;
    std::memcpy(&action, &function, sizeof(function)); // the handler union is the first member
    action.sa_flags = kSaSiginfoFreeBsd;
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

} // namespace Platform
