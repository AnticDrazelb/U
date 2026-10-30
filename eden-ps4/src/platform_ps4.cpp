// The launcher on the PS4. A start page first: it shows what an unfinished last run left behind
// (the end of its log.txt, its crash report and boot trace) so that a photo of it is enough to see
// where it stopped, and lets the player pick a program from a list (D-pad, X): the package's
// (two games, a controller test, a GPU test, the test program hello.nro) and every program or
// game in /data/eden-ps4 (.nro, .nso, .nsp, .xci: commercial games need the console's keys in
// /data/eden-ps4/user/keys and its firmware in /data/eden-ps4/user/nand).
// Then the program runs. With Mesa's RADV linked, Eden's Vulkan renderer draws it on the PS4's GPU,
// straight to the screen (which the launcher gives up meanwhile); otherwise the null renderer's
// frames (drawn on the CPU) fill the screen. Until it draws, its text output and Eden's warnings
// and errors are shown, and they always go to /data/eden-ps4/log.txt. The controller is player 1's Pro Controller. When the program ends,
// or L3 + R3 held for a second stop it, the start page comes back. Nothing is written outside /data/eden-ps4; Eden's data folders are
// in /data/eden-ps4/user.
//
// The screen, controller and start page are switch-bench's (../../switch-bench/src/platform_ps4.cpp).
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>
#include <orbis/AudioOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <stb/stb_easy_font.h>

#include "common/orbis_memory.h"
#include "common/ps4_diagnostics.h"
#include "launcher.h"
#include "ui/canvas.h"
#include "ui/home_menu.h"
#include "ui/nro.h"
#include "usb_import.h"
#include "log_tools.h"
#include "web_server.h"

// Declared by hand: the toolchain headers give these the wrong (or no) prototypes
// (as in port/ps4_runtime.cpp).
extern "C" {
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int32_t type,
                                      off_t* physOut);
int32_t sceKernelMapDirectMemory(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys,
                                 size_t align);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t len);
int32_t sceKernelMunmap(void* addr, size_t len);
int32_t sceKernelCreateEqueue(OrbisKernelEqueue* queue, const char* name);
int32_t sceKernelWaitEqueue(OrbisKernelEqueue queue, void* events, int32_t count, int32_t* out, uint32_t* timeout_us);
int32_t sceSysmoduleLoadModuleInternal(uint32_t id);
void* scePthreadSelf(void);
int32_t scePthreadSetaffinity(void* thread, uint64_t cpus);
int32_t scePthreadSetprio(void* thread, int32_t priority);
// port/ps4_crashlog.cpp: a line in /data/eden-ps4/boot-trace.log
void ps4_boot_trace(const char* stage);
// Mesa's RADV (or ps4_stubs.cpp's stand-in, without it)
void* vk_icdGetInstanceProcAddr(void* instance, const char* name);
// libSceSystemService: "exit" closes the app (back to the PS4's home screen)
int sceSystemServiceLoadExec(const char* path, char* const argv[]);
// port/ps4_runtime.cpp: the cores new threads may run on
void ps4_set_app_cores(uint64_t mask);
// port/ps4_runtime.cpp: direct memory the C heap's overflow leaves for the GPU
void ps4_heap_set_overflow_gpu_reserve(size_t bytes);
// port/ps4_runtime.cpp: the C heap's bytes in use, and its size
void ps4_heap_usage(size_t* in_use, size_t* size);
int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
// port/ps4_sampler.cpp: every thread's stack, appended to a file
void ps4_dump_thread_stacks(const char* path, const char* reason);
// Mesa's PS4 winsys (port/mesa/ac_ps4_drm.c; ps4_stubs.cpp without it): the GPU buffers' memory
uint64_t ac_ps4_gpu_bytes();
// port/ps4_sampler.cpp: the profiler (samples.log), the threads it samples (by name), and a line
// in samples.log
void ps4_sampler_sample_threads(const char* names);
void ps4_sampler_set_enabled(int on);
void ps4_sampler_note(const char* text);
}

namespace {

constexpr size_t MB = 1024 * 1024;
constexpr int32_t kProtCpuGpuRw = 0x33;
constexpr const char* kDataDir = "/data/eden-ps4";
constexpr const char* kLogFile = "/data/eden-ps4/log.txt";
constexpr const char* kCrashFile = "/data/eden-ps4/crash.log";
constexpr const char* kHangFile = "/data/eden-ps4/hang.log";
// The app's function names (build.sh, from the ELF): the logs name the code they point at
constexpr const char* kSymbolsFile = "/app0/symbols.zst";
// The last run's boot trace: Crema's runtime renames boot-trace.log to this as the app starts.
constexpr const char* kLastTraceFile = "/data/eden-ps4/boot-trace-launcher.log";
constexpr const char* kTestProgram = "/app0/hello.nro";
// The last line of log.txt after a run that finished.
constexpr const char* kFinishedLine = "eden-ps4: run finished";

void Trace(const std::string& step)
{
    ps4_boot_trace(("eden-ps4: " + step).c_str());
}

double Seconds()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// The screen: 1920x1080 linear ARGB, three buffers, redrawn whenever a line is added.
class Screen
{
public:
    static constexpr int kWidth = 1920, kHeight = 1080;
    static constexpr int kCell = 7; // stb_easy_font's widest advance: drawn as a fixed-width font
    static constexpr int kMargin = 48;

    // Video out set up as Crema's launcher does it: the same calls in the same order, three linear
    // A8B8G8R8 buffers in write-combined direct memory, and a flip event queue to wait on.
    bool Open()
    {
        Trace("screen: sceVideoOutOpen");
        handle = sceVideoOutOpen(0xFF, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
        if (handle < 0)
            return Failed("sceVideoOutOpen", handle);
        Trace("screen: flip event queue");
        if (!flip_queue)
            sceKernelCreateEqueue(&flip_queue, "eden-ps4 flip");
        sceVideoOutAddFlipEvent(flip_queue, handle, nullptr);
        Trace("screen: direct memory for 3 frames");
        int32_t ret = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), kBufferSize * kFrames,
                                                    2 * MB, 3 /* write-combined, as Crema's VRAM */, &phys);
        if (ret != 0)
        {
            phys = -1;
            return Failed("sceKernelAllocateDirectMemory", ret);
        }
        Trace("screen: map it");
        ret = sceKernelMapDirectMemory(&base, kBufferSize * kFrames, kProtCpuGpuRw, 0, phys, 2 * MB);
        if (ret != 0)
            return Failed("sceKernelMapDirectMemory", ret);
        const size_t buffer_size = kBufferSize;
        for (int i = 0; i < kFrames; i++)
            buffers[i] = reinterpret_cast<uint32_t*>(static_cast<char*>(base) + buffer_size * i);
        Trace("screen: register buffers");
        OrbisVideoOutBufferAttribute attr;
        sceVideoOutSetBufferAttribute(&attr, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                      ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0, kWidth, kHeight, kWidth);
        ret = sceVideoOutRegisterBuffers(handle, 0, reinterpret_cast<void* const*>(buffers), kFrames, &attr);
        if (ret < 0)
            return Failed("sceVideoOutRegisterBuffers", ret);
        open = true;
        flips = 0;
        current = 0;
        return true;
    }

    // Gives the screen up (video out, and its frames' direct memory) for a program whose own
    // display takes it: Eden's Vulkan renderer. Open takes it back.
    void Release()
    {
        // (once the last flip is on screen: none left pending on buffers about to go. A flip shadPS4
        // still had queued after 100 ms took it down, "flip of an unregistered buffer")
        for (int tries = 0; handle >= 0 && flips > 0; tries++)
        {
            OrbisVideoOutFlipStatus flip_status = {};
            if (sceVideoOutGetFlipStatus(handle, &flip_status) < 0 ||
                (flip_status.flipArg >= int64_t(flips) && flip_status.numFlipPending == 0))
                break;
            if (tries == 1000)
            {
                Trace(fmt::format("screen: flips still pending after 2 s (flip {} of {}, {} pending)",
                                  flip_status.flipArg, flips, flip_status.numFlipPending));
                break;
            }
            usleep(2000);
        }
        if (handle >= 0)
            sceVideoOutClose(handle);
        handle = -1;
        if (base)
            sceKernelMunmap(base, kBufferSize * kFrames);
        if (phys >= 0)
            sceKernelReleaseDirectMemory(phys, kBufferSize * kFrames);
        base = nullptr;
        phys = -1;
        open = false;
        Trace("screen: given up");
    }

    // Text size: 2 for the program's output (readable in a photo), 1.75 for the longer start page.
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

    void Add(const std::string& line, uint32_t color, bool draw = true)
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
            lines.push_back({rest.substr(0, cut), color});
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

    // A frame the program drew, scaled to fill the screen (nearest pixel). Its pixels are R, G, B,
    // A bytes: the buffers' own order. Each row is scaled into a row buffer first and copied from
    // there to the screen rows that show it: the frame buffers are write-combined memory, quick to
    // write in order but very slow for the CPU to read.
    void DrawFrame(std::span<const uint32_t> pixels, uint32_t width, uint32_t height)
    {
        if (!open || width == 0 || height == 0)
            return;
        if (map_width != width || map_height != height)
        {
            x_map.resize(kWidth);
            y_map.resize(kHeight);
            row.resize(kWidth);
            for (int x = 0; x < kWidth; x++)
                x_map[x] = uint32_t(uint64_t(x) * width / kWidth);
            for (int y = 0; y < kHeight; y++)
                y_map[y] = uint32_t(uint64_t(y) * height / kHeight);
            // a whole number of screen pixels for each of the frame's (640 wide: 3)
            x_factor = kWidth % width == 0 ? kWidth / width : 0;
            map_width = width;
            map_height = height;
        }
        const double wait_start = Seconds();
        WaitForImageOffScreen();
        last_wait = Seconds() - wait_start;
        uint32_t* fb = buffers[current];
        for (int y = 0; y < kHeight; y++)
        {
            if (y == 0 || y_map[y] != y_map[y - 1])
            {
                const uint32_t* src = pixels.data() + size_t(y_map[y]) * width;
                uint32_t* out = row.data();
                if (x_factor)
                {
                    for (uint32_t x = 0; x < width; x++)
                        for (uint32_t k = 0; k < x_factor; k++)
                            *out++ = src[x];
                }
                else
                {
                    for (int x = 0; x < kWidth; x++)
                        out[x] = src[x_map[x]];
                }
            }
            std::memcpy(fb + size_t(y) * kWidth, row.data(), kWidth * sizeof(uint32_t));
        }
        sceVideoOutSubmitFlip(handle, current, ORBIS_VIDEO_OUT_FLIP_VSYNC, int64_t(++flips));
        current = (current + 1) % kFrames;
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
            Text(fb, kMargin, y, lines[i].text, lines[i].color);
        if (!status.empty())
        {
            Fill(fb, 0, kHeight - kMargin - line_height - 12, kWidth, kHeight - kMargin - line_height - 10, 0xFF2A3445u);
            Text(fb, kMargin, kHeight - kMargin - line_height, status, 0xFF8FB4E8u);
        }
        if (flips == 0)
            Trace("screen: first flip");
        sceVideoOutSubmitFlip(handle, current, ORBIS_VIDEO_OUT_FLIP_VSYNC, int64_t(++flips));
        current = (current + 1) % kFrames;
    }

private:
    struct Line
    {
        std::string text;
        uint32_t color;
    };
    static constexpr int kFrames = 3;
    static constexpr size_t kBufferSize = (size_t(kWidth) * kHeight * 4 + 2 * MB - 1) & ~(2 * MB - 1);
    int handle = -1;
    off_t phys = -1; // the frames' direct memory
    void* base = nullptr;
    bool open = false;
    OrbisKernelEqueue flip_queue = 0;
    uint32_t* buffers[kFrames] = {};
    int current = 0;
    uint64_t flips = 0;
    float scale = 2.0f;
    int line_height = 24;
    std::vector<Line> lines;
    std::string status;
    std::vector<uint32_t> x_map, y_map, row;
    uint32_t map_width = 0, map_height = 0, x_factor = 0;

public:
    double last_wait = 0; // how long DrawFrame waited for a buffer off the screen, in seconds

private:
    static bool Failed(const char* call, int32_t ret)
    {
        Trace(fmt::format("screen: {} failed ({:#x}); output goes to log.txt only", call, uint32_t(ret)));
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
            OrbisVideoOutFlipStatus flip_status = {};
            if (sceVideoOutGetFlipStatus(handle, &flip_status) < 0 || flip_status.flipArg >= needed)
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
    static void Fill(uint32_t* fb, int x0, int y0, int x1, int y1, uint32_t argb)
    {
        const uint32_t color = Native(argb);
        x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, kWidth), y1 = std::min(y1, kHeight);
        for (int y = y0; y < y1; y++)
            std::fill(fb + size_t(y) * kWidth + x0, fb + size_t(y) * kWidth + std::max(x0, x1), color);
    }
    // stb_easy_font gives each character as quads; draw them as filled rectangles, scaled, one
    // character per fixed-width cell.
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

constexpr uint32_t kWhite = 0xFFE6E6E6u, kGreen = 0xFF7EE787u, kYellow = 0xFFF2CC60u, kRed = 0xFFFF6B6Bu,
                   kGrey = 0xFF9AA4B2u;

std::mutex g_mutex;
Screen g_screen;

// The system modules of every library this program calls, loaded before any call into them, as
// Crema's frontend does (LoadSystemModules in patches/cemu-ps4.patch): "calling into a system
// library whose module isn't loaded kills the app silently". (Net is for the runtime's resolver,
// port/ps4_net.cpp.)
void LoadSystemModules()
{
    // (AudioOut was missing until 03.51: every program that played sound, Breakout and Blocks,
    // was killed without a trace as it opened its audio output on a console; shadPS4 doesn't care)
    const std::pair<const char*, uint32_t> modules[] = {{"SystemService", 0x80000010},
                                                        {"UserService", 0x80000011},
                                                        {"VideoOut", 0x80000022},
                                                        {"AudioOut", 0x80000001},
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
// the stack (several to a line, or with symbols the first functions, one a line), and the threads
// named in the dump that follows it.
std::vector<std::string> LastCrash(const std::vector<std::string>& log, const Launcher::Symbols& symbols)
{
    size_t start = log.size();
    for (size_t i = log.size(); i-- > 0;)
        if (log[i].rfind("=== ", 0) == 0)
        {
            start = i;
            break;
        }
    std::vector<std::string> out;
    std::string addresses, last_function;
    int address_count = 0, threads = 0, named = 0;
    bool in_dump = false;
    const auto name_of = [&](const std::string& line) {
        return symbols.Empty() ? line : Launcher::SymbolizeLog(line, symbols);
    };
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
                out.push_back("   thread " + name_of(l.substr(3)));
            continue;
        }
        if (l.empty() || l.rfind("code addresses", 0) == 0)
            continue;
        const size_t bracket = l.find("] 0x");
        if (l.rfind("  [rsp+", 0) == 0 && bracket != std::string::npos && !symbols.Empty())
        {
            // "  [rsp+0x18] 0x1234  = Kernel::KThread::Run(...)+0x10": a function once in a row
            const std::string line = name_of(l);
            const size_t equals = line.find("  = ");
            if (equals == std::string::npos || named >= 12)
                continue;
            std::string function = line.substr(equals + 4);
            const std::string base = function.substr(0, function.rfind("+0x"));
            if (base == last_function)
                continue;
            last_function = base;
            if (function.size() > 110)
                function = function.substr(0, 107) + "...";
            out.push_back("   " + l.substr(bracket + 2) + "  " + function);
            named++;
            continue;
        }
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
        out.push_back("   " + name_of(l));
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

// Player 1's controller from the PS4 pad, by the PS4's conventions: Cross is A (confirm),
// Circle B, Square Y, Triangle X; L1/R1 L/R, L2/R2 ZL/ZR, OPTIONS +, the touchpad's click -.
Launcher::PadState ToSwitchPad(const OrbisPadData& pad)
{
    static const std::pair<uint32_t, uint32_t> map[] = {
        {ORBIS_PAD_BUTTON_CROSS, Launcher::ButtonA},       {ORBIS_PAD_BUTTON_CIRCLE, Launcher::ButtonB},
        {ORBIS_PAD_BUTTON_SQUARE, Launcher::ButtonY},      {ORBIS_PAD_BUTTON_TRIANGLE, Launcher::ButtonX},
        {ORBIS_PAD_BUTTON_L1, Launcher::ButtonL},          {ORBIS_PAD_BUTTON_R1, Launcher::ButtonR},
        {ORBIS_PAD_BUTTON_L2, Launcher::ButtonZL},         {ORBIS_PAD_BUTTON_R2, Launcher::ButtonZR},
        {ORBIS_PAD_BUTTON_OPTIONS, Launcher::ButtonPlus},  {ORBIS_PAD_BUTTON_TOUCH_PAD, Launcher::ButtonMinus},
        {ORBIS_PAD_BUTTON_L3, Launcher::ButtonLStick},     {ORBIS_PAD_BUTTON_R3, Launcher::ButtonRStick},
        {ORBIS_PAD_BUTTON_UP, Launcher::ButtonUp},         {ORBIS_PAD_BUTTON_DOWN, Launcher::ButtonDown},
        {ORBIS_PAD_BUTTON_LEFT, Launcher::ButtonLeft},     {ORBIS_PAD_BUTTON_RIGHT, Launcher::ButtonRight}};
    Launcher::PadState state;
    for (const auto& [ps4, nx] : map)
        if (pad.buttons & ps4)
            state.buttons |= nx;
    const auto axis = [](uint8_t v) { return std::clamp((int(v) - 128) / 127.0f, -1.0f, 1.0f); };
    state.lx = axis(pad.leftStick.x);
    state.ly = -axis(pad.leftStick.y);
    state.rx = axis(pad.rightStick.x);
    state.ry = -axis(pad.rightStick.y);
    return state;
}

// Sound: an AudioOut port for each stream Eden opens, the system user's main port (as Crema's).
class Ps4AudioOutput final : public Launcher::AudioOutput {
public:
    ~Ps4AudioOutput() override
    {
        if (port >= 0)
        {
            sceAudioOutOutput(port, nullptr); // the last buffer plays out
            sceAudioOutClose(port);
        }
    }

    bool Open(uint32_t frames, uint32_t channels) override
    {
        if (channels != 2)
            return false;
        // 0x8026000E: already initialized. (The step first: a console kills the app without a trace
        // if libSceAudioOut's module isn't loaded, as until 03.51)
        Trace(fmt::format("audio: opening ({} frames)", frames));
        static const int32_t init = sceAudioOutInit();
        if (init != 0 && init != int32_t(0x8026000E))
        {
            Trace(fmt::format("audio: sceAudioOutInit = {:#x}", uint32_t(init)));
            return false;
        }
        port = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, frames, 48000,
                               ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        Trace(fmt::format("audio: sceAudioOutOpen({} frames) = {:#x}", frames, uint32_t(port)));
        return port >= 0;
    }

    void Output(std::span<const int16_t> samples) override
    {
        if (!thread_set)
        {
            // Eden's audio thread was made by one of its own, whose cores and priority it has: any
            // of the app's cores, and a priority above the emulation's so that a busy frame doesn't
            // make the sound crackle (as Crema's audio thread).
            thread_set = true;
            void* self = scePthreadSelf();
            scePthreadSetaffinity(self, 0x3F);
            scePthreadSetprio(self, 400);
        }
        sceAudioOutOutput(port, samples.data()); // blocks until the previous buffer plays: the pace
    }

private:
    int32_t port = -1;
    bool thread_set = false;
};

std::unique_ptr<Launcher::AudioOutput> MakeAudioOutput()
{
    return std::make_unique<Ps4AudioOutput>();
}

// While a program runs: the pad, read every 4 ms, as player 1's controller. Both sticks pressed
// in (L3 + R3) for a second stop the program.
std::atomic<bool> g_running{false};
// The last program that didn't load, and why (the HOME menu says so when it comes back)
std::string g_load_failure_name, g_load_failure;
void* PadThread(void*)
{
    // above the emulation's busy threads (650): a poll is a moment's work, and with all seven
    // cores busy at a lower priority than theirs the controller would lag
    scePthreadSetprio(scePthreadSelf(), 600);
    constexpr uint32_t kStopButtons = ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3;
    double stop_held_since = -1;
    while (g_running)
    {
        union {
            OrbisPadData data;
            unsigned char room[512]; // in case the system's structure is larger than the header's
        } state{};
        if (g_pad >= 0 && scePadReadState(g_pad, &state.data) == 0 && state.data.connected)
        {
            Launcher::SetPad(ToSwitchPad(state.data));
            if ((state.data.buttons & kStopButtons) != kStopButtons)
                stop_held_since = -1;
            else if (stop_held_since < 0)
                stop_held_since = Seconds();
            else if (Seconds() - stop_held_since >= 1)
            {
                Trace("L3 + R3 held: stopping the program");
                Launcher::Stop();
                stop_held_since = 1e18; // once
            }
        }
        usleep(4000);
    }
    return nullptr;
}

// Frames from the program: once it draws, the screen shows its frames instead of the text. The
// boot trace gets their rate every 10 s, how long the launcher waited for a buffer off the screen
// (the display's pace) and how long scaling a frame onto it took.
std::atomic<bool> g_drawing{false};
// What draws the Switch's GPU work, for the start page (Run sets it)
std::string g_renderer_label = "the software renderer";
std::atomic<uint64_t> g_frames_shown{0}; // (for the hang watchdog)
void ShowFrame(std::span<const uint32_t> pixels, uint32_t width, uint32_t height)
{
    g_frames_shown.fetch_add(1, std::memory_order_relaxed);
    static double period_start = 0, drawing = 0, waiting = 0;
    static unsigned frames = 0;
    std::lock_guard lock{g_mutex};
    const double start = Seconds();
    if (!g_drawing.exchange(true))
    {
        Trace(fmt::format("first frame from the program: {}x{}", width, height));
        period_start = start;
        drawing = waiting = 0;
        frames = 0;
    }
    g_screen.DrawFrame(pixels, width, height);
    const double end = Seconds();
    drawing += end - start - g_screen.last_wait;
    waiting += g_screen.last_wait;
    frames++;
    if (end - period_start >= 10)
    {
        Trace(fmt::format("frames: {:.1f} a second; each waited {:.1f} ms for a buffer, took {:.1f} ms to scale",
                          frames / (end - period_start), waiting * 1000 / frames, drawing * 1000 / frames));
        period_start = end;
        drawing = waiting = 0;
        frames = 0;
    }
}

// The HOME menu's titles: the package's programs, then every program (.nro, .nso) and game (.nsp,
// .xci) in the data folder and at the top of a USB drive (/mnt/usb0 to /mnt/usb7), the most
// recently played first. A homebrew program's name and icon come from its assets; a game's from
// what Launcher::Boot kept the first time it booted (reading them takes the keys), else its cover
// shows its file's name.
constexpr const char* kRecentFile = "/data/eden-ps4/recent.txt";
constexpr const char* kMenuFile = "/data/eden-ps4/menu.txt";
constexpr const char* kKeysFile = "/data/eden-ps4/user/keys/prod.keys";
constexpr const char* kTitleKeysFile = "/data/eden-ps4/user/keys/title.keys";
constexpr const char* kFirmwareDir = "/data/eden-ps4/user/nand/system/Contents/registered";

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

bool IsGame(const std::string& path)
{
    const std::string extension = Lower(std::filesystem::path(path).extension().string());
    return extension == ".nsp" || extension == ".xci";
}

std::string SizeText(uintmax_t size)
{
    return size >= 1024 * MB ? fmt::format("{:.1f} GB", double(size) / (1024 * MB))
                             : fmt::format("{} MB", (size + MB - 1) / MB);
}

// "Some Game [0100ABCD00000000][v0] (USA).nsp" -> "Some Game"
std::string NameFromFile(const std::filesystem::path& path)
{
    std::string name = path.stem().string();
    std::string out;
    int depth = 0;
    for (const char c : name)
    {
        if (c == '[' || c == '(')
            depth++;
        else if ((c == ']' || c == ')') && depth > 0)
            depth--;
        else if (depth == 0)
            out += c == '_' ? ' ' : c;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    while (!out.empty() && out.front() == ' ')
        out.erase(out.begin());
    return out.empty() ? name : out;
}

std::vector<std::string> ReadRecent()
{
    return ReadLines(kRecentFile);
}

void NoteRecent(const std::string& program)
{
    std::vector<std::string> recent = ReadRecent();
    std::erase(recent, program);
    recent.insert(recent.begin(), program);
    if (recent.size() > 100)
        recent.resize(100);
    if (FILE* f = std::fopen(kRecentFile, "w"))
    {
        for (const std::string& path : recent)
            std::fprintf(f, "%s\n", path.c_str());
        std::fclose(f);
    }
}

std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::vector<uint8_t> data;
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        uint8_t buffer[16384];
        for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;)
            data.insert(data.end(), buffer, buffer + n);
        std::fclose(f);
    }
    return data;
}

std::vector<Ui::Title> LoadTitles()
{
    struct BuiltIn
    {
        const char* path;
        const char* name;
        Ui::Art art;
    };
    const BuiltIn built_in[] = {
        {"/app0/breakout.nro", "Breakout", Ui::Art::Breakout},
        {"/app0/blocks.nro", "Blocks", Ui::Art::Blocks},
        {"/app0/padtest.nro", "Controller test", Ui::Art::Controller},
        {"/app0/gputest.nro", "GPU test", Ui::Art::Gpu},
        {kTestProgram, "The test program", Ui::Art::Terminal},
    };
    std::vector<Ui::Title> titles;
    std::error_code ec;
    for (const BuiltIn& program : built_in)
    {
        Ui::Title title;
        title.path = program.path;
        title.name = program.name;
        title.publisher = "Eden for PS4";
        title.location = "in the package";
        title.kind = "NRO";
        title.art = program.art;
        const uintmax_t size = std::filesystem::file_size(program.path, ec);
        title.size = ec ? "" : SizeText(size);
        titles.push_back(std::move(title));
    }
    std::vector<std::pair<std::string, std::string>> folders = {{kDataDir, "data folder"}};
    for (int i = 0; i < 8; i++)
        folders.push_back({fmt::format("/mnt/usb{}", i), fmt::format("USB {}", i)});
    for (const auto& [folder, where] : folders)
    {
        std::vector<std::filesystem::path> found;
        for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string extension = Lower(it->path().extension().string());
            if ((extension == ".nro" || extension == ".nso" || extension == ".nsp" || extension == ".xci") &&
                it->is_regular_file(ec))
                found.push_back(it->path());
        }
        std::sort(found.begin(), found.end());
        for (const std::filesystem::path& path : found)
        {
            Ui::Title title;
            title.path = path.string();
            title.location = where;
            title.kind = Lower(path.extension().string()).substr(1);
            std::transform(title.kind.begin(), title.kind.end(), title.kind.begin(),
                           [](unsigned char c) { return char(std::toupper(c)); });
            title.game = IsGame(title.path);
            const uintmax_t size = std::filesystem::file_size(path, ec);
            title.size = ec ? "" : SizeText(size);
            // what its last boot learned, else (homebrew) its assets, else its file's name
            const std::vector<std::string> cached = ReadLines(Launcher::TitleCachePath(kDataDir, title.path, "txt").c_str());
            std::vector<uint8_t> icon;
            if (!cached.empty() && !cached[0].empty())
            {
                title.name = cached[0];
                title.publisher = cached.size() > 1 ? cached[1] : "";
                icon = ReadFile(Launcher::TitleCachePath(kDataDir, title.path, "jpg").string());
            }
            else if (title.kind == "NRO")
            {
                Ui::NroInfo info;
                if (Ui::ReadNroInfo(title.path, info))
                {
                    title.name = info.name;
                    title.publisher = info.publisher;
                    icon = std::move(info.icon);
                }
            }
            if (title.name.empty())
                title.name = NameFromFile(path);
            if (!icon.empty())
                title.icon = Ui::DecodeImage(icon.data(), icon.size());
            titles.push_back(std::move(title));
        }
    }
    // the most recently played first, in the order they were played; the rest as found
    const std::vector<std::string> recent = ReadRecent();
    std::stable_sort(titles.begin(), titles.end(), [&](const Ui::Title& a, const Ui::Title& b) {
        const auto rank = [&](const Ui::Title& t) {
            const auto it = std::find(recent.begin(), recent.end(), t.path);
            return it == recent.end() ? recent.size() : size_t(it - recent.begin());
        };
        return rank(a) < rank(b);
    });
    return titles;
}

Ui::MenuSettings LoadMenuSettings()
{
    Ui::MenuSettings settings;
    for (const std::string& line : ReadLines(kMenuFile))
    {
        if (line == "theme=white")
            settings.white_theme = true;
        else if (line == "renderer=software")
            settings.software_renderer = true;
        else if (line == "mode=docked")
            settings.docked = true;
        else if (line == "resolution=75")
            settings.resolution = 75;
        else if (line == "resolution=50")
            settings.resolution = 50;
        else if (line == "shader_cache=off")
            settings.shader_cache = false;
        else if (line == "async_shaders=off")
            settings.async_shaders = false;
        else if (line == "cpu=unsafe")
            settings.cpu_unsafe = true;
        else if (line == "gpu=high")
            settings.gpu_high = true;
        else if (line == "cores=6")
            settings.seventh_core = false;
        else if (line == "profiler=on")
            settings.profiler = true;
        else if (line == "ram=reserved")
            settings.ram_on_demand = false;
    }
    return settings;
}

void SaveMenuSettings(const Ui::MenuSettings& settings)
{
    if (FILE* f = std::fopen(kMenuFile, "w"))
    {
        std::fprintf(f, "theme=%s\nrenderer=%s\nmode=%s\nresolution=%d\nshader_cache=%s\nasync_shaders=%s\ngpu=%s\n"
                        "cpu=%s\ncores=%d\nprofiler=%s\nram=%s\n",
                     settings.white_theme ? "white" : "black", settings.software_renderer ? "software" : "vulkan",
                     settings.docked ? "docked" : "handheld", settings.resolution, settings.shader_cache ? "on" : "off",
                     settings.async_shaders ? "on" : "off", settings.gpu_high ? "high" : "normal",
                     settings.cpu_unsafe ? "unsafe" : "auto", settings.seventh_core ? 7 : 6,
                     settings.profiler ? "on" : "off", settings.ram_on_demand ? "on-demand" : "reserved");
        std::fclose(f);
    }
}

// The Games and Keys screen: the keys, the firmware and the games found
// What the app itself sees at the USB drives' mount points (/mnt/usb0-7): whether each is there for
// it, whether it can be opened (the system's reason if not), and what's at its top. The system's
// own package installer reading a drive doesn't mean an app may.
std::string UsbReport(bool* readable = nullptr)
{
    if (readable)
        *readable = false; // (set when a drive could be opened and has something on it)
    std::vector<std::string> seen;
    for (int i = 0; i < 8; i++)
    {
        const std::string path = fmt::format("/mnt/usb{}", i);
        struct stat status{};
        if (stat(path.c_str(), &status) != 0)
        {
            if (errno != ENOENT)
                seen.push_back(fmt::format("{}: can't be looked at ({})", path, std::strerror(errno)));
            continue;
        }
        DIR* folder = opendir(path.c_str());
        if (!folder)
        {
            seen.push_back(fmt::format("{}: there, but can't be opened ({})", path, std::strerror(errno)));
            continue;
        }
        size_t entries = 0, games = 0;
        bool keys = false;
        while (const dirent* entry = readdir(folder))
        {
            const std::string name = entry->d_name;
            if (name == "." || name == "..")
                continue;
            entries++;
            const std::string extension = Lower(std::filesystem::path(name).extension().string());
            games += extension == ".nsp" || extension == ".xci" || extension == ".nro" || extension == ".nso";
            keys |= name == "prod.keys";
        }
        closedir(folder);
        if (readable && entries > 0)
            *readable = true;
        seen.push_back(fmt::format("{}: {} entries at its top, {} games{}", path, entries, games,
                                   keys ? ", prod.keys" : ""));
    }
    if (seen.empty())
        return "none of /mnt/usb0 to /mnt/usb7 is there for the app";
    std::string text;
    for (const std::string& line : seen)
        text += (text.empty() ? "" : "; ") + line;
    return text;
}

std::vector<Ui::StatusRow> KeysAndFirmwareStatus()
{
    std::vector<Ui::StatusRow> rows;
    std::error_code ec;
    const bool keys = std::filesystem::exists(kKeysFile, ec);
    rows.push_back({"Keys (prod.keys)", keys ? fmt::format("Found: {}", kKeysFile)
                                             : fmt::format("Not found: Import from USB, or copy it to {}", kKeysFile),
                    keys});
    const bool title_keys = std::filesystem::exists(kTitleKeysFile, ec);
    rows.push_back({"Title keys (title.keys)",
                    title_keys ? fmt::format("Found: {}", kTitleKeysFile)
                               : fmt::format("Not found ({}): some games need it", kTitleKeysFile),
                    title_keys});
    size_t firmware = 0;
    for (std::filesystem::directory_iterator it(kFirmwareDir, ec), end; !ec && it != end; it.increment(ec))
        firmware++;
    rows.push_back({"Firmware", firmware ? fmt::format("{} files in {}", firmware, kFirmwareDir)
                                         : fmt::format("None in {}: Import from USB, or copy your Switch's firmware "
                                                       "(.nca files) there",
                                                       kFirmwareDir),
                    firmware > 0});
    bool readable = false;
    const std::string usb = UsbReport(&readable);
    rows.push_back({"USB drives, as the app sees them", usb, readable});
    return rows;
}

Ui::StatusRow SoftwareStatus(const std::vector<Ui::Title>& titles)
{
    size_t games = 0, on_usb = 0, homebrew = 0;
    for (const Ui::Title& title : titles)
    {
        if (title.location == "in the package")
            continue;
        if (title.game)
        {
            games++;
            on_usb += title.location.starts_with("USB") ? 1 : 0;
        }
        else
        {
            homebrew++;
        }
    }
    return {"Software",
            fmt::format("{} game{} ({} on USB drives) and {} homebrew program{}", games, games == 1 ? "" : "s", on_usb,
                        homebrew, homebrew == 1 ? "" : "s"),
            games + homebrew > 0};
}

constexpr const char* kGamesHelp =
    "Games (.nsp, .xci) and homebrew (.nro) go in /data/eden-ps4 or at the top of a USB drive (exFAT or FAT32). A "
    "game needs the keys and firmware of the Switch it was dumped from: put prod.keys and title.keys at the top of a "
    "USB drive (or in its switch folder) and the firmware on it (its .zip, or its .nca files in a folder), then "
    "Import from USB. Or "
    "send all of them from a browser: the address is at the top of the HOME menu.";

// Import from USB (Games and Keys): the copy runs on its own thread, its progress in a message
struct UsbImportJob
{
    std::thread thread;
    std::atomic<bool> done{false};
    std::atomic<size_t> copied{0}, total{0};
    Launcher::UsbImport result;
};

std::string UsbImportReport(const Launcher::UsbImport& r)
{
    if (r.keys_folder.empty() && r.firmware_folder.empty())
        return "No prod.keys and no firmware on a USB drive (the app sees: " + UsbReport() + "). Put prod.keys and "
               "title.keys at the top of the drive (or "
               "in its switch folder), and the firmware on it: its .zip as it came, or its .nca files in a folder "
               "(Firmware 19.0.1, say). The drive must be exFAT or FAT32.";
    std::string text;
    if (!r.keys_folder.empty())
        text += fmt::format("prod.keys{} from {}.\n", r.title_keys ? " and title.keys" : "", r.keys_folder);
    else
        text += "No prod.keys found (only the firmware was copied).\n";
    if (!r.firmware_folder.empty())
        text += fmt::format("{} firmware files from {} (the firmware that was there was replaced).\n",
                            r.firmware_files, r.firmware_folder);
    else
        text += "No firmware found (a folder of .nca files).\n";
    if (!r.error.empty())
        text += "Stopped: " + r.error + ".";
    return text;
}

std::vector<std::pair<std::string, std::string>> News()
{
    return {
        {"Eden for PS4 " EDEN_PS4_VERSION,
         "More memory for games' graphics: the Switch's 4 GB of RAM no longer take 4 GB of the PS4's memory as a "
         "game starts, only what the game actually uses, 2 MB at a time. The rest goes to the PS4's GPU, which "
         "had about half a GB left for textures before; its caches now give memory back as the game's grows. "
         "System Settings, System, Switch Memory turns this off.\n"
         "Updates and DLC: .nsp files sent from the browser page (or put in /data/eden-ps4/updates) are applied "
         "as their game starts. Many games need their latest update.\n"
         "Every 30 seconds while a game runs, boot-trace.log notes its frame rate and where the memory went.\n"
         "03.71: firmware as it comes, a .zip: send it from the browser page as it is, or put it on a USB drive "
         "for Import from USB. The PS4 unpacks it, checks every file, and keeps the firmware it had if the zip "
         "turns out damaged. Fixed with it: firmware sent before was added to the old one instead of replacing "
         "it (the PS4 couldn't delete folders); send yours once more to be sure.\n"
         "03.72: Games and Keys shows what the app itself sees of each USB drive (whether it may open it at "
         "all, and what's at its top), as the boot trace does: the system installing packages from a drive "
         "doesn't mean an app is allowed to read it."},
        {"Before: 03.60",
         "From a PC or phone: the address at the top of the HOME menu, opened in a browser on the same network, "
         "takes your keys, firmware (the whole folder at once) and games, with progress, and gives back every log "
         "in one file: no FTP, no USB drive, no photos of the screen. Anyone on your network can reach it while "
         "the app is open.\n"
         "The logs name the code they point at: crash reports, hang reports and the error screen show the app's "
         "function names, read from the package itself.\n"
         "New in System Settings, CPU: Profiler. While a game runs, where the emulated CPU cores, the GPU thread "
         "and the Vulkan thread spend their time: the web page's Profile summary shows it per function. Send it "
         "when a game is slow."},
        {"Before: 03.51",
         "Sound: Breakout and Blocks were closed by the console as they started, because the app never "
         "loaded the PS4's audio module. Fixed.\n"
         "From 03.50: faster on the PS4's CPU: reads of memory the GPU caches no longer take a slow path (without "
         "fastmem every one could wait for the GPU), GPU macros are compiled instead of interpreted, the "
         "emulated CPU cores and the GPU thread now come before logging and shader compiling, and the "
         "code caches went to the cores games run on.\n"
         "New in System Settings, Graphics: GPU Accuracy. Normal (the default) is Eden's choice on "
         "phones; High reads back what the GPU drew whenever the game might look at it, for the few "
         "games whose effects need it.\n"
         "Fixed: a crash as a program exited (its GPU memory freed while still in use), memory written "
         "after it was freed at every Vulkan shutdown, and input reaching a stopped system."},
        {"Before: 03.40",
         "One program after another (each left about 66 MiB behind, and the GPU driver kept a device "
         "per program), and keys and firmware from a USB drive: Games and Keys, Import from USB "
         "(Square)."},
        {"Games",
         "Put games (.nsp, .xci) in /data/eden-ps4 or at the top of a USB drive, or send them from a browser (the "
         "address at the top of the HOME menu). They need the keys and firmware of the Switch they were dumped "
         "from: send them from the browser too, or put prod.keys and title.keys at the top of a USB drive (or in "
         "its switch folder) and the firmware (its .zip, or its .nca files in a folder) on it, then Import from USB "
         "in Games and "
         "Keys. A game's icon and name appear once it has booted once."},
        {"The HOME menu",
         "Your software is in the row, the most recently played first, and All Software at its end has everything. "
         "Below: News, Games and Keys (what's found, and where things go), Controllers, System Settings (theme, "
         "renderer, resolution, shader cache, CPU, console mode) and Power. OPTIONS on a title shows where it is."},
        {"The PS4's GPU",
         "Games are drawn with the PS4's GPU through Mesa's RADV. If Vulkan doesn't start, the software renderer "
         "takes over (System Settings, Graphics, says so) and log-vulkan.txt and gpu/radv-ps4.log in /data/eden-ps4 "
         "say why."},
        {"If something goes wrong",
         "In a game, L3 + R3 held for a second stops it. If it froze, hang.log has where its threads were; if it "
         "crashed, this menu shows its report when it comes back. Send boot-trace.log, log.txt, crash.log, hang.log "
         "and gpu/radv-ps4.log (radv-ps4-previous.log after a crash) from /data/eden-ps4."},
    };
}

// The pad's buttons for the menu, with the left stick as the D-pad (past a third of its travel)
uint32_t MenuPad()
{
    union {
        OrbisPadData data;
        unsigned char room[512];
    } state{};
    if (g_pad < 0 || scePadReadState(g_pad, &state.data) != 0 || !state.data.connected)
        return 0;
    uint32_t buttons = state.data.buttons;
    const int x = int(state.data.leftStick.x) - 128, y = int(state.data.leftStick.y) - 128;
    if (x < -80)
        buttons |= ORBIS_PAD_BUTTON_LEFT;
    else if (x > 80)
        buttons |= ORBIS_PAD_BUTTON_RIGHT;
    if (y < -80)
        buttons |= ORBIS_PAD_BUTTON_UP;
    else if (y > 80)
        buttons |= ORBIS_PAD_BUTTON_DOWN;
    return buttons;
}

uint32_t MenuButtons(uint32_t pad)
{
    uint32_t held = 0;
    const struct { uint32_t pad, menu; } map[] = {
        {ORBIS_PAD_BUTTON_UP, Ui::kUp},           {ORBIS_PAD_BUTTON_DOWN, Ui::kDown},
        {ORBIS_PAD_BUTTON_LEFT, Ui::kLeft},       {ORBIS_PAD_BUTTON_RIGHT, Ui::kRight},
        {ORBIS_PAD_BUTTON_CROSS, Ui::kCross},     {ORBIS_PAD_BUTTON_CIRCLE, Ui::kCircle},
        {ORBIS_PAD_BUTTON_TRIANGLE, Ui::kTriangle}, {ORBIS_PAD_BUTTON_SQUARE, Ui::kSquare},
        {ORBIS_PAD_BUTTON_OPTIONS, Ui::kOptions}, {ORBIS_PAD_BUTTON_L1, Ui::kL1},
        {ORBIS_PAD_BUTTON_R1, Ui::kR1},
    };
    for (const auto& m : map)
        held |= (pad & m.pad) ? m.menu : 0;
    return held;
}

std::string ClockText()
{
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    return fmt::format("{}:{:02}", local.tm_hour, local.tm_min);
}

// The web server (web_server.h): started once, its port; set when a browser added or removed files
bool g_web_started = false;
uint16_t g_web_port = 8080;
std::atomic<bool> g_files_changed{false};

// Where a browser reaches the web server ("" without a network, or if it didn't start)
std::string WebUrl()
{
    if (!g_web_started)
        return {};
    const std::string address = Launcher::LocalAddress();
    return address.empty() ? std::string() : fmt::format("http://{}:{}", address, g_web_port);
}

void StartWebServer()
{
    Launcher::WebServerConfig web;
    web.data_dir = kDataDir;
    if (const char* port = std::getenv("EDEN_PS4_WEB_PORT"); port && std::atoi(port) > 0)
        g_web_port = uint16_t(std::atoi(port));
    web.port = g_web_port;
    web.version = EDEN_PS4_VERSION;
    web.status = [] {
        std::string label;
        {
            std::lock_guard lock{g_mutex};
            label = g_renderer_label;
        }
        std::erase_if(label, [](char c) { return c == '"' || c == '\\' || uint8_t(c) < 0x20; });
        return "\"renderer\":\"" + label + "\"";
    };
    web.changed = [] { g_files_changed = true; };
    web.log = [](const std::string& line) { Trace(line); };
    web.symbols_path = kSymbolsFile;
    web.program_running = [] { return g_running.load(); };
    g_web_started = Launcher::StartWebServer(std::move(web));
    if (!g_web_started)
        Trace(fmt::format("web: port {} can't be opened", g_web_port));
}

// What the HOME menu said to do: boot `program`, with these settings
struct MenuChoice
{
    std::string program;
    Ui::MenuSettings settings;
};

// The HOME menu, until a title is chosen. On the first one, without a controller, the test
// program boots after a countdown. After a run that didn't finish, its report comes first.
MenuChoice ShowHomeMenu(bool first, bool radv, const Ui::MenuSettings& settings)
{
    const std::vector<std::string> log = ReadLines(kLogFile);
    const std::vector<std::string> crash = ReadLines(kCrashFile);
    const std::vector<std::string> trace = ReadLines(kLastTraceFile);
    const bool unfinished = first && (!crash.empty() || (!log.empty() && log.back() != kFinishedLine));
    static Ui::HomeMenu menu; // (kept between runs: its place in the row, its screen)
    static Ui::Canvas canvas(Screen::kWidth, Screen::kHeight, Screen::kWidth / 1280.0f);

    std::unique_lock lock{g_mutex};
    if (first)
        OpenPad();
    std::vector<Ui::Title> titles = LoadTitles();
    Ui::StatusRow software_status = SoftwareStatus(titles);
    std::string web_url = WebUrl();
    menu.SetWebAddress(web_url);
    const auto set_games_status = [&] {
        std::vector<Ui::StatusRow> rows = KeysAndFirmwareStatus();
        rows.push_back(software_status);
        size_t updates = 0;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(std::string(kDataDir) + "/updates", ec), end; !ec && it != end;
             it.increment(ec))
            updates += it->path().extension() == ".nsp";
        rows.push_back({"Updates and DLC",
                        updates ? fmt::format("{} in /data/eden-ps4/updates, applied as their game starts", updates)
                                : std::string("None: .nsp files in /data/eden-ps4/updates (or sent from a browser) "
                                              "are applied as their game starts"),
                        updates > 0});
        rows.push_back({"From a PC or phone",
                        web_url.empty() ? std::string("No network: connected, the PS4 takes keys, firmware and games "
                                                      "from a browser, and gives it every log")
                                        : fmt::format("Open {} in a browser on the same network: keys, firmware, "
                                                      "games, and every log in one file",
                                                      web_url),
                        !web_url.empty()});
        menu.SetGamesStatus(std::move(rows), kGamesHelp);
        menu.SetKeysPresent(std::filesystem::exists(kKeysFile, ec));
    };
    set_games_status();
    const size_t count = titles.size();
    menu.SetTitles(std::move(titles));
    menu.SetSettings(settings);
    menu.SetInfo(EDEN_PS4_VERSION, g_renderer_label, radv);
    menu.SetNews(News());
    menu.SetControllerTest("/app0/padtest.nro");
    if (unfinished)
    {
        Launcher::Symbols symbols; // (~11 MiB, for this only)
        symbols.Load(kSymbolsFile);
        menu.ShowReport({Last(log, 40), LastCrash(crash, symbols), Last(trace, 24)});
    }
    else if (!g_load_failure.empty())
    {
        std::string why = g_load_failure;
        if (!why.empty())
            why[0] = char(std::toupper(static_cast<unsigned char>(why[0])));
        if (!why.ends_with('.') && !why.ends_with(".)"))
            why += '.';
        menu.ShowMessage(fmt::format("{} didn't start.", g_load_failure_name), why);
        g_load_failure.clear();
    }
    Trace("usb: " + UsbReport());
    Trace(fmt::format("start page: last run {}, pad {}, {} programs", unfinished ? "unfinished" : "finished or none",
                      g_pad, count));

    static std::unique_ptr<UsbImportJob> import_job; // (none while a program runs: a boot waits for it)
    menu.SetHeld(MenuButtons(MenuPad())); // (a button still held from the game doesn't count)
    const double start = Seconds();
    const int countdown = 60;
    MenuChoice choice{kTestProgram, settings};
    double web_checked = start;
    for (;;)
    {
        const double now = Seconds();
        const uint32_t pad = MenuPad();
        menu.SetClock(ClockText());
        if (now - web_checked >= 3)
        {
            // (the network may come up after the app; a new address shows)
            web_checked = now;
            if (std::string url = WebUrl(); url != web_url)
            {
                web_url = std::move(url);
                menu.SetWebAddress(web_url);
                set_games_status();
            }
        }
        if (g_files_changed.exchange(false))
        {
            // a browser added or removed keys, firmware or games
            std::vector<Ui::Title> fresh = LoadTitles();
            software_status = SoftwareStatus(fresh);
            menu.SetTitles(std::move(fresh));
            set_games_status();
        }
        menu.SetController(g_pad >= 0);
        if (g_pad < 0)
        {
            const int left = countdown - int(now - start);
            menu.SetCountdown(first ? std::max(left, 0) : -1);
            if (first && left <= 0)
                break;
        }
        const Ui::MenuAction action = menu.Update(MenuButtons(pad), now);
        if (action.kind == Ui::MenuAction::Kind::ImportUsb && !import_job)
        {
            Trace("start page: importing keys and firmware from USB");
            menu.ShowMessage("Import from USB", "Looking for prod.keys, title.keys and firmware on the USB drives...");
            import_job = std::make_unique<UsbImportJob>();
            import_job->thread = std::thread([job = import_job.get()] {
                std::vector<std::string> drives;
                for (int i = 0; i < 8; i++)
                    drives.push_back(fmt::format("/mnt/usb{}", i));
                job->result = Launcher::ImportFromUsb(drives, std::string(kDataDir) + "/user",
                                                      [job](size_t copied, size_t total) {
                                                          job->total = total;
                                                          job->copied = copied;
                                                      });
                job->done = true;
            });
        }
        if (import_job && import_job->done)
        {
            import_job->thread.join();
            const std::string report = UsbImportReport(import_job->result);
            Trace("start page: USB import: " + report);
            if (!menu.UpdateMessage("Import from USB", report))
                menu.ShowMessage("Import from USB", report);
            set_games_status();
            import_job.reset();
        }
        else if (import_job && import_job->total)
        {
            menu.UpdateMessage("Import from USB", fmt::format("Copying the firmware: {} of {} files...",
                                                              import_job->copied.load(), import_job->total.load()));
        }
        if (action.kind == Ui::MenuAction::Kind::Boot && import_job)
            menu.ShowMessage("Import from USB", "Still copying: start it again once the import is done.");
        if (action.kind == Ui::MenuAction::Kind::Boot && !import_job)
        {
            choice.program = action.path;
            break;
        }
        if (action.kind == Ui::MenuAction::Kind::Quit)
        {
            Trace("start page: closing the app");
            sceSystemServiceLoadExec("exit", nullptr);
            _exit(0);
        }
        if (action.kind == Ui::MenuAction::Kind::SettingsChanged)
        {
            choice.settings = menu.Settings();
            SaveMenuSettings(choice.settings);
            Trace(fmt::format("start page: settings: theme {}, renderer {}, {}, resolution {}%, shader cache {}, "
                              "asynchronous shaders {}, GPU accuracy {}, CPU {}, {} cores, profiler {}",
                              choice.settings.white_theme ? "white" : "black",
                              choice.settings.software_renderer ? "software" : "vulkan",
                              choice.settings.docked ? "docked" : "handheld", choice.settings.resolution,
                              choice.settings.shader_cache ? "on" : "off", choice.settings.async_shaders ? "on" : "off",
                              choice.settings.gpu_high ? "high" : "normal", choice.settings.cpu_unsafe ? "unsafe" : "auto",
                              choice.settings.seventh_core ? 7 : 6, choice.settings.profiler ? "on" : "off"));
        }
        menu.Draw(canvas, now);
        g_screen.DrawFrame({canvas.Pixels(), size_t(canvas.Width()) * canvas.Height()}, uint32_t(canvas.Width()),
                           uint32_t(canvas.Height()));
        // about 30 frames a second
        lock.unlock();
        const double spent = Seconds() - now;
        if (spent < 1.0 / 30)
            usleep(useconds_t((1.0 / 30 - spent) * 1e6));
        lock.lock();
    }
    menu.SetCountdown(-1);
    choice.settings = menu.Settings();
    Trace("start page: booting " + choice.program);

    // the last run's files are kept, under other names
    rename(kLogFile, "/data/eden-ps4/log-previous.txt");
    rename(kCrashFile, "/data/eden-ps4/crash-previous.log");
    rename(kHangFile, "/data/eden-ps4/hang-previous.log");
    g_screen.Clear();
    g_screen.SetScale(2.0f);
    return choice;
}

void ShowLine(Launcher::LineKind kind, std::string_view text)
{
    std::lock_guard lock{g_mutex};
    uint32_t color = kWhite;
    switch (kind)
    {
    case Launcher::LineKind::Program:
        break;
    case Launcher::LineKind::Warning:
        color = kGrey;
        break;
    case Launcher::LineKind::Error:
        color = kRed;
        Trace(std::string(text));
        break;
    case Launcher::LineKind::Status:
        color = kYellow;
        Trace(std::string(text));
        break;
    case Launcher::LineKind::Info:
        return;
    }
    // while the program draws, its frames have the screen: the lines are kept for later
    g_screen.Add(std::string(text), color, !g_drawing);
}

// Settings for the console, where there's no shell to set environment variables: each line of
// /data/eden-ps4/settings.txt that reads NAME=value is set as one (EDEN_SW_THREADS=5, say).
void ReadSettings()
{
    FILE* file = std::fopen((std::string(kDataDir) + "/settings.txt").c_str(), "r");
    if (!file)
    {
        return;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), file))
    {
        std::string text{line};
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        {
            text.pop_back();
        }
        const size_t equals = text.find('=');
        if (text.empty() || text[0] == '#' || equals == std::string::npos || equals == 0)
        {
            continue;
        }
        setenv(text.substr(0, equals).c_str(), text.substr(equals + 1).c_str(), 1);
        Trace(fmt::format("setting {}", text));
    }
    std::fclose(file);
}

// The hang watchdog (after Eden4's): while a program runs, if its frames stop for 20 s after they
// had started, or none has come 90 s after it booted, every thread's stack goes to hang.log, once
// until frames move again: what to send when a game freezes on the console. It allocates nothing
// before the dump, which then still happens when the hang is in the heap. EDEN_PS4_HANG_SECONDS
// in settings.txt changes the 20 (and the 90 with it, 4.5 times as long).
// Where the memory is: the Switch's RAM (direct memory taken as the game touches it), the GPU's
// buffers, the C heap, and what's left
std::string MemoryLine()
{
    size_t in_use = 0, size = 0, flexible = 0;
    ps4_heap_usage(&in_use, &size);
    sceKernelAvailableFlexibleMemorySize(&flexible);
    return fmt::format("the Switch's RAM {} MiB (at most {}), the GPU's buffers {} MiB, C heap {} of {} MiB in use, "
                       "{} MiB of flexible memory free, largest free block of direct memory {} MiB",
                       Common::Orbis::OnDemandBytes() / MB, Common::Orbis::OnDemandPeakBytes() / MB,
                       ac_ps4_gpu_bytes() / MB, in_use / MB, size / MB, flexible / MB,
                       Common::PS4Diagnostics::LargestFreeDirect() / MB);
}

class HangWatchdog
{
public:
    void Start(const std::string& program)
    {
        name = program;
        const char* seconds = std::getenv("EDEN_PS4_HANG_SECONDS");
        stall_limit = seconds ? std::max(1, std::atoi(seconds)) : 20;
        boot_limit = stall_limit * 9 / 2;
        stop = false;
        thread = std::thread([this] { Watch(); });
    }
    void Stop()
    {
        stop = true;
        if (thread.joinable())
            thread.join();
    }

private:
    static uint64_t Frames()
    {
        return Common::PS4Diagnostics::composites.load(std::memory_order_relaxed) +
               g_frames_shown.load(std::memory_order_relaxed);
    }
    void Watch()
    {
        scePthreadSetprio(scePthreadSelf(), 600); // (it sleeps; when it looks, it must get to)
        const uint64_t base = Frames();
        uint64_t last = base;
        int seconds = 0, stale = 0;
        bool dumped = false;
        uint64_t reported = base;
        while (!stop)
        {
            for (int i = 0; i < 10 && !stop; i++)
                usleep(100 * 1000);
            seconds++;
            const uint64_t frames = Frames();
            if (seconds % 30 == 0)
            {
                // how it runs: the frame rate, and the memory (what a game needs, on the console)
                Trace(fmt::format("running {} s: {:.1f} frames a second; {}", seconds,
                                  double(frames - reported) / 30.0, MemoryLine()));
                reported = frames;
            }
            if (frames != last)
            {
                last = frames;
                stale = 0;
                dumped = false;
                continue;
            }
            stale++;
            const bool started = frames != base;
            if (dumped || (started ? stale < stall_limit : seconds < boot_limit))
                continue;
            dumped = true;
            char reason[256];
            const char* stage = Common::PS4Diagnostics::stage.load();
            if (started)
                snprintf(reason, sizeof(reason), "%s: no new frame for %d s, %d s after booting, %llu frames so far "
                         "(the GPU display: %s)", name.c_str(), stale, seconds, (unsigned long long)(frames - base),
                         stage);
            else
                snprintf(reason, sizeof(reason), "%s: no frame %d s after booting (the GPU display: %s)",
                         name.c_str(), seconds, stage);
            ps4_dump_thread_stacks(kHangFile, reason);
            Trace(std::string("hang watchdog: ") + reason + ": thread stacks in hang.log");
        }
    }

    std::string name;
    int stall_limit = 20, boot_limit = 90;
    std::atomic<bool> stop{false};
    std::thread thread;
};

// The C heap, flexible and direct memory, in the boot trace: what a program left behind shows
// up as the next one starts (the first console run ran out after two programs)
void MemoryTrace(const char* when)
{
    Trace(fmt::format("memory {}: {}", when, MemoryLine()));
}

// EDEN_PS4_HEAP_TEST=<MiB> in settings.txt: with 4 GiB of direct memory taken first (as the
// Switch's RAM will be) and RADV's reserve set, the C heap grows by that much (8 MiB blocks), to
// see where its overflow heaps come from (boot-trace.log); everything is then given back.
void HeapTest(size_t megabytes)
{
    off_t ram = 0;
    const bool have_ram = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), 4096 * MB, 2 * MB, 0,
                                                        &ram) == 0;
    Trace(fmt::format("heap test: {} MiB; 4 GiB of direct memory for the Switch's RAM {}", megabytes,
                      have_ram ? "taken" : "NOT taken"));
    ps4_heap_set_overflow_gpu_reserve(256 * MB);
    std::vector<void*> blocks;
    blocks.reserve(megabytes / 8 + 1);
    size_t got = 0;
    while (got < megabytes * MB)
    {
        void* block = std::malloc(8 * MB);
        if (!block)
            break;
        blocks.push_back(block);
        got += 8 * MB;
    }
    Trace(fmt::format("heap test: {} of {} MiB allocated", got / MB, megabytes));
    for (void* block : blocks)
        std::free(block);
    if (have_ram)
        sceKernelReleaseDirectMemory(ram, 4096 * MB);
}

void* Run(void*)
{
    // Eden finds its data under $HOME when no folder is given, and a few paths it always takes
    // from there: keep them in the app's folder.
    setenv("HOME", kDataDir, 1);
    ReadSettings();
    if (const char* test = std::getenv("EDEN_PS4_HEAP_TEST"))
        HeapTest(std::strtoul(test, nullptr, 10));
    // the null renderer's software draws log what they cost (every 2 seconds, in log.txt), and
    // rasterize on 4 threads (the GPU thread and 3 more) of the app's 6 cores, leaving the
    // emulated CPU's thread one of its own
    setenv("EDEN_SW_STATS", "1", 0);
    setenv("EDEN_SW_THREADS", "4", 0);
    Launcher::SetAudio(MakeAudioOutput);
    // RADV's own log and its GPU hang and fault reports (port/mesa/ac_ps4_drm.c), here (the last
    // session's log kept: it starts a new one, and a crash restarts the app)
    setenv("RADV_PS4_DIR", (std::string(kDataDir) + "/gpu").c_str(), 0);
    rename((std::string(kDataDir) + "/gpu/radv-ps4.log").c_str(),
           (std::string(kDataDir) + "/gpu/radv-ps4-previous.log").c_str());
    // Eden's Vulkan renderer on the PS4's GPU when Mesa's RADV is linked, unless System Settings
    // (or settings.txt, EDEN_PS4_RENDERER=null) chose the software renderer, or Vulkan didn't
    // start this session
    const char* renderer = std::getenv("EDEN_PS4_RENDERER");
    const bool radv = vk_icdGetInstanceProcAddr(nullptr, "vkCreateInstance") != nullptr;
    const bool forced_null = renderer && std::string_view{renderer} == "null";
    bool vulkan_failed = false;
    Ui::MenuSettings settings = LoadMenuSettings();
    StartWebServer();
    // the threads the profiler samples (System Settings, CPU, Profiler): the emulated CPU cores, the
    // GPU thread (Maxwell commands) and the Vulkan thread (recording and submitting)
    ps4_sampler_sample_threads("CPUCore_,CPUThread,GPU,VulkanWorker");
    const auto choose_renderer = [&] {
        const bool use_gpu = radv && !forced_null && !settings.software_renderer && !vulkan_failed;
        g_renderer_label = use_gpu        ? "Vulkan on the PS4's GPU (Mesa's RADV)"
                           : vulkan_failed ? "the software renderer (Vulkan didn't start: see log-vulkan.txt)"
                           : !radv         ? "the software renderer (no RADV in this build)"
                           : forced_null   ? "the software renderer (EDEN_PS4_RENDERER=null)"
                                           : "the software renderer (chosen in System Settings)";
        return use_gpu;
    };
    bool gpu = choose_renderer();
    Trace("renderer: " + g_renderer_label);
    for (bool first = true;; first = false)
    {
        const MenuChoice choice = ShowHomeMenu(first, radv, settings);
        const std::string& program = choice.program;
        settings = choice.settings;
        gpu = choose_renderer();
        Launcher::SetGpuDisplay(gpu);
        Launcher::SetDocked(settings.docked);
        Launcher::SetResolution(settings.resolution);
        Launcher::SetShaderCache(settings.shader_cache);
        Launcher::SetAsyncShaders(settings.async_shaders);
        Launcher::SetCpuUnsafe(settings.cpu_unsafe);
        Launcher::SetGpuAccuracyHigh(settings.gpu_high);
        ps4_set_app_cores(settings.seventh_core ? 0x7F : 0x3F);
        // read by Eden's HostMemory as it maps the Switch's RAM (common/host_memory.cpp)
        setenv("EDEN_PS4_DRAM_ON_DEMAND", settings.ram_on_demand ? "1" : "0", 1);
        NoteRecent(program);
        const std::string setup = fmt::format(
            "renderer: {}; console mode: {}; resolution {}%; shader cache {}, asynchronous shaders {}, "
            "GPU accuracy {}; CPU {}, {} cores; the Switch's RAM {}{}",
            g_renderer_label, settings.docked ? "docked" : "handheld", settings.resolution,
            settings.shader_cache ? "on" : "off", settings.async_shaders ? "on" : "off",
            settings.gpu_high ? "high" : "normal", settings.cpu_unsafe ? "unsafe" : "auto",
            settings.seventh_core ? 7 : 6, settings.ram_on_demand ? "on demand" : "all up front",
            settings.profiler ? "; profiler on" : "");
        Trace(setup);
        if (settings.profiler)
        {
            ps4_sampler_set_enabled(1);
            ps4_sampler_note(fmt::format("{} ({})", program, setup).c_str());
        }
        {
            std::lock_guard lock{g_mutex};
            g_screen.SetStatus(fmt::format("booting {}", program));
        }
        g_drawing = false;
        g_running = true;
        pthread_t pad_thread;
        pthread_create(&pad_thread, nullptr, PadThread, nullptr);
        if (gpu)
        {
            // the program's display takes the screen: what it prints still goes to log.txt
            std::lock_guard lock{g_mutex};
            g_screen.Release();
        }
        // The C heap's overflow (once its 384 MiB are full) leaves direct memory to the GPU: the
        // Switch's 4 GiB leave about 480 MiB of it, and RADV needs most of what's left
        ps4_heap_set_overflow_gpu_reserve((gpu ? 256 : 32) * MB);
        MemoryTrace("before booting");
        HangWatchdog watchdog;
        watchdog.Start(program);
        auto result = Launcher::Boot(program, kDataDir, ShowLine, ShowFrame, std::chrono::hours(24));
        watchdog.Stop();
        if (gpu)
        {
            std::lock_guard lock{g_mutex};
            Trace(g_screen.Open() ? "screen open again" : "no screen (video out failed)");
            g_screen.Draw();
        }
        if (gpu && result.status == Launcher::BootResult::Status::LoadFailed &&
            result.detail == "the video core did not start")
        {
            // Vulkan didn't start (RADV couldn't open the GPU: this boot's log, kept as
            // log-vulkan.txt, and gpu/radv-ps4.log say why): the software renderer, from now on
            vulkan_failed = true;
            gpu = choose_renderer();
            Launcher::SetGpuDisplay(false);
            rename(kLogFile, "/data/eden-ps4/log-vulkan.txt");
            Trace("renderer: Vulkan did not start; the null renderer from now on");
            ShowLine(Launcher::LineKind::Status,
                     "Vulkan did not start (see log-vulkan.txt): booting again with the software renderer");
            ps4_heap_set_overflow_gpu_reserve(32 * MB);
            watchdog.Start(program);
            result = Launcher::Boot(program, kDataDir, ShowLine, ShowFrame, std::chrono::hours(24));
            watchdog.Stop();
        }
        MemoryTrace("after the program");
        ps4_sampler_set_enabled(0);
        if (result.status == Launcher::BootResult::Status::LoadFailed)
        {
            g_load_failure_name = std::filesystem::path(program).stem().string();
            g_load_failure = result.detail;
        }
        g_running = false;
        pthread_join(pad_thread, nullptr);
        g_drawing = false;
        Trace(fmt::format("boot returned: status {}", int(result.status)));
        if (FILE* f = std::fopen(kLogFile, "a"))
        {
            std::fprintf(f, "%s\n", kFinishedLine);
            std::fclose(f);
        }
    }
    return nullptr;
}

} // namespace

int main()
{
    mkdir(kDataDir, 0777);
    Trace("started");
    // a browser that goes away mid-transfer would otherwise end the app (the web server)
    std::signal(SIGPIPE, SIG_IGN);
    LoadSystemModules();
    Trace(g_screen.Open() ? "screen open" : "no screen (video out failed)");

    // Eden on a thread with a large stack (the main thread's is small).
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * MB);
    pthread_t thread;
    if (pthread_create(&thread, &attr, Run, nullptr) != 0)
    {
        Trace("could not start the launcher thread");
        return 1;
    }
    for (;;)
        sleep(1);
}
