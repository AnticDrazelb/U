// The launcher on the PS4. A start page first: it shows what an unfinished last run left behind
// (the end of its log.txt, its crash report and boot trace) so that a photo of it is enough to see
// where it stopped, and lets the player pick a program from a list (D-pad, X): the package's
// (from 04.21 only the player's: the package's own test programs aren't listed) every program or
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
#include <setjmp.h>
#include <signal.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <set>
#include <tuple>
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

#include "common/cpu_features.h"
#include "common/orbis_memory.h"
#include "common/ps4_diagnostics.h"
#include "common/ps4_perf.h"
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
// Mesa's PS4 winsys (port/mesa/ac_ps4_drm.c; ps4_stubs.cpp without it): the GPU buffers' memory,
// and the GPU's statistics since it started (busy and span in GPU clock ticks, submissions, and
// the CPU's waits for the GPU's work: how many, and how long in all)
uint64_t ac_ps4_gpu_bytes();
void ac_ps4_get_gpu_stats(uint64_t* busy_ticks, uint64_t* span_ticks, uint64_t* submits, uint64_t* waits,
                          uint64_t* wait_ns);
// port/ps4_sampler.cpp: the profiler (samples.log), the threads it samples (by name), and a line
// in samples.log
void ps4_sampler_sample_threads(const char* names);
void ps4_sampler_set_enabled(int on);
void ps4_sampler_note(const char* text);
// Eden's dynarmic (backend/x64/ps4_block_map.cpp): its map of recompiled blocks, which the profiler
// names JIT samples with (the guest block) and dumps the hottest of (hotblocks.log)
void dynarmic_ps4_block_map_enable(int on);
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

// A long line in pieces at its "; " (the boot trace keeps 2 KiB of a line), each piece after the
// first marked as the rest of it
void TraceLong(const std::string& head, const std::string& text)
{
    constexpr size_t kPiece = 1800;
    size_t at = 0;
    for (int part = 0; at < text.size(); part++)
    {
        size_t end = text.size();
        if (end - at > kPiece)
        {
            const size_t cut = text.rfind("; ", at + kPiece);
            end = cut != std::string::npos && cut > at ? cut + 2 : at + kPiece;
        }
        Trace((part ? head + "(continued) " : head) + text.substr(at, end - at));
        at = end;
    }
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
// .xci) in the data folder and at the top of a USB drive (UsbDrive: /mnt/usb0-7 or /usb0-7), the
// most recently played first. A homebrew program's name and icon come from its assets; a game's from
// what Launcher::Boot kept the first time it booted (reading them takes the keys), else its cover
// shows its file's name.
constexpr const char* kRecentFile = "/data/eden-ps4/recent.txt";
constexpr const char* kMenuFile = "/data/eden-ps4/menu.txt";
constexpr const char* kKeysFile = "/data/eden-ps4/user/keys/prod.keys";
constexpr const char* kTitleKeysFile = "/data/eden-ps4/user/keys/title.keys";
constexpr const char* kFirmwareDir = "/data/eden-ps4/user/nand/system/Contents/registered";
// Games and Keys, Game Folder (Ui::MenuSettings::games_folder): searched for games, keys and firmware
// in all its folders, as Crema's game folder is (empty: none)
std::string g_games_folder;
constexpr int kGameFolderDepth = 6;

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

bool IsGameFileName(const std::string& name)
{
    const std::string extension = Lower(std::filesystem::path(name).extension().string());
    return extension == ".nro" || extension == ".nso" || extension == ".nsp" || extension == ".xci";
}

// The game files in `folder` and, `depth` folders down, in its folders (a USB drive's switch or
// games folder), sorted: the system's own folders and hidden ones left out
std::vector<std::filesystem::path> GameFiles(const std::string& folder, int depth)
{
    std::vector<std::filesystem::path> found;
    std::vector<std::string> folders;
    DIR* directory = opendir(folder.c_str());
    if (!directory)
        return found;
    while (const dirent* entry = readdir(directory))
    {
        const std::string name = entry->d_name;
        if (name.empty() || name[0] == '.' || name[0] == '$' || name == "System Volume Information")
            continue;
        const std::string path = folder + "/" + name;
        struct stat status{};
        if (stat(path.c_str(), &status) != 0)
            continue;
        if (S_ISREG(status.st_mode) && IsGameFileName(name))
            found.push_back(path);
        else if (S_ISDIR(status.st_mode) && depth > 0)
            folders.push_back(path);
    }
    closedir(directory);
    std::sort(found.begin(), found.end());
    std::sort(folders.begin(), folders.end());
    for (const std::string& inner : folders)
        for (std::filesystem::path& path : GameFiles(inner, depth - 1))
            found.push_back(std::move(path));
    return found;
}

// USB drive `i` (0-7) where the app sees it: /mnt/usbN, or /usbN, where some consoles show an
// app its drives instead (a console's log: nothing at /mnt/usb0, the drive at /usb0)
std::string UsbDrive(int i)
{
    const std::string mnt = fmt::format("/mnt/usb{}", i);
    struct stat status{};
    if (stat(mnt.c_str(), &status) == 0 && S_ISDIR(status.st_mode))
        return mnt;
    const std::string root = fmt::format("/usb{}", i);
    return stat(root.c_str(), &status) == 0 && S_ISDIR(status.st_mode) ? root : mnt;
}

// Which USB drives are there and what's at their tops, in short: the game list is made again when
// it changes (a drive plugged in, or mounted after the app started)
std::string UsbSignature()
{
    std::string signature;
    for (int i = 0; i < 8; i++)
    {
        const std::string path = UsbDrive(i);
        DIR* directory = opendir(path.c_str());
        if (!directory)
            continue;
        size_t entries = 0;
        while (readdir(directory))
            entries++;
        closedir(directory);
        signature += fmt::format("{}:{};", i, entries);
    }
    if (!g_games_folder.empty())
    {
        size_t entries = 0;
        if (DIR* directory = opendir(g_games_folder.c_str()))
        {
            while (readdir(directory))
                entries++;
            closedir(directory);
        }
        signature += fmt::format("folder:{};", entries);
    }
    return signature;
}

std::vector<Ui::Title> LoadTitles()
{
    // (04.21) The package's own programs (Breakout, Blocks, the controller and GPU tests, the test
    // program) are no longer listed: only the player's games and programs. Their files stay in the
    // package: the Controllers screen still starts the controller test (X), the start page the test
    // program when no controller is found, and test.sh runs them all.
    std::vector<Ui::Title> titles;
    std::error_code ec;
    // the data folder's top (its folders are the app's own), and each USB drive's top and folders
    std::vector<std::tuple<std::string, std::string, int>> folders = {{kDataDir, "data folder", 0}};
    for (int i = 0; i < 8; i++)
        folders.push_back({UsbDrive(i), fmt::format("USB {}", i), 1});
    // the Game Folder first: a drive's game found there and on the drive shows once, as the folder's
    if (!g_games_folder.empty())
        folders.insert(folders.begin(), {g_games_folder, "Game Folder", kGameFolderDepth});
    std::set<std::string> seen;
    // a game copied to the PS4 (Copy to the PS4) starts from there: its copy on a drive isn't listed
    std::set<std::string> in_data_folder;
    for (const std::filesystem::path& path : GameFiles(kDataDir, 0))
        in_data_folder.insert(path.filename().string());
    for (const auto& [folder, where, depth] : folders)
    {
        const std::vector<std::filesystem::path> found = GameFiles(folder, depth);
        for (const std::filesystem::path& path : found)
        {
            const bool in_data = path.lexically_normal().parent_path() == std::filesystem::path(kDataDir);
            if (!in_data && in_data_folder.count(path.filename().string()))
                continue;
            if (!seen.insert(path.lexically_normal().string()).second)
                continue;
            Ui::Title title;
            title.path = path.string();
            title.location = in_data ? "data folder" : where;
            // on the PS4's own storage (the data folder, or a Game Folder there): Delete offered
            title.on_ps4 = path.lexically_normal().string().starts_with("/data/");
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
        else if (line == "jit_cache=off")
            settings.jit_cache = false;
        else if (line == "draw_shortcuts=off")
            settings.draw_shortcuts = false;
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
        else if (line == "fps=on")
            settings.fps_overlay = true;
        else if (line == "ram=reserved")
            settings.ram_on_demand = false;
        else if (line == "threads=free")
            settings.pin_threads = false;
        else if (line == "pacing=unlocked")
            settings.unlocked_frames = true;
        else if (line == "fastmem=off")
            settings.fastmem = false;
        else if (line == "speed=150")
            settings.speed = 150;
        else if (line == "speed=200")
            settings.speed = 200;
        else if (line.starts_with("games_folder="))
            settings.games_folder = line.substr(13);
    }
    return settings;
}

void SaveMenuSettings(const Ui::MenuSettings& settings)
{
    if (FILE* f = std::fopen(kMenuFile, "w"))
    {
        std::fprintf(f, "theme=%s\nrenderer=%s\nmode=%s\nresolution=%d\nshader_cache=%s\nasync_shaders=%s\ngpu=%s\n"
                        "cpu=%s\ncores=%d\nprofiler=%s\nram=%s\ngames_folder=%s\nfps=%s\nthreads=%s\npacing=%s\nspeed=%d\nfastmem=%s\n"
                        "jit_cache=%s\ndraw_shortcuts=%s\n",
                     settings.white_theme ? "white" : "black", settings.software_renderer ? "software" : "vulkan",
                     settings.docked ? "docked" : "handheld", settings.resolution, settings.shader_cache ? "on" : "off",
                     settings.async_shaders ? "on" : "off", settings.gpu_high ? "high" : "normal",
                     settings.cpu_unsafe ? "unsafe" : "auto", settings.seventh_core ? 7 : 6,
                     settings.profiler ? "on" : "off", settings.ram_on_demand ? "on-demand" : "reserved",
                     settings.games_folder.c_str(), settings.fps_overlay ? "on" : "off",
                     settings.pin_threads ? "pinned" : "free", settings.unlocked_frames ? "unlocked" : "60hz", settings.speed,
                     settings.fastmem ? "on" : "off", settings.jit_cache ? "on" : "off",
                     settings.draw_shortcuts ? "on" : "off");
        std::fclose(f);
    }
}

// The Games and Keys screen: the keys, the firmware and the games found
// What the app itself sees at the USB drives' mount points (/mnt/usb0-7, or /usb0-7: UsbDrive):
// whether each is there for it, whether it can be opened (the system's reason if not), and what's
// at its top. The system's own package installer reading a drive doesn't mean an app may.
std::string UsbReport(bool* readable = nullptr)
{
    if (readable)
        *readable = false; // (set when a drive could be opened and has something on it)
    std::vector<std::string> seen;
    for (int i = 0; i < 8; i++)
    {
        const std::string path = UsbDrive(i);
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
        size_t entries = 0;
        std::string names; // the first few, as the app reads them
        while (const dirent* entry = readdir(folder))
        {
            const std::string name = entry->d_name;
            if (name == "." || name == "..")
                continue;
            if (++entries <= 8)
                names += (names.empty() ? "" : ", ") + name;
        }
        closedir(folder);
        if (readable && entries > 0)
            *readable = true;
        const size_t games = GameFiles(path, 1).size();
        seen.push_back(fmt::format("{}: {} games (top and one folder down); at its top: {}{}", path, games,
                                   names.empty() ? std::string("nothing") : names, entries > 8 ? ", ..." : ""));
    }
    if (seen.empty())
        return "none of /mnt/usb0 to /mnt/usb7 (or /usb0 to /usb7) is there for the app";
    std::string text;
    for (const std::string& line : seen)
        text += (text.empty() ? "" : "; ") + line;
    return text;
}

// Keys and firmware in the Game Folder or on a USB drive (UsbDrive), which games use from there,
// before the ones in the user folder (Launcher::SetKeysAndFirmware)
Launcher::UsbKeysAndFirmware KeysAndFirmwareOnUsb()
{
    std::vector<std::string> drives;
    if (!g_games_folder.empty())
        drives.push_back(g_games_folder);
    for (int i = 0; i < 8; i++)
        drives.push_back(UsbDrive(i));
    return Launcher::FindKeysAndFirmware(drives);
}

std::vector<Ui::StatusRow> KeysAndFirmwareStatus()
{
    std::vector<Ui::StatusRow> rows;
    std::error_code ec;
    const Launcher::UsbKeysAndFirmware on_usb = KeysAndFirmwareOnUsb();
    const bool keys = !on_usb.keys_folder.empty() || std::filesystem::exists(kKeysFile, ec);
    rows.push_back({"Keys (prod.keys)",
                    !on_usb.keys_folder.empty()
                        ? fmt::format("Used from where it is: {}/prod.keys{}", on_usb.keys_folder,
                                      std::filesystem::exists(kKeysFile, ec)
                                          ? " (a copy is on the PS4 too)"
                                          : ". Square copies it to the PS4, then the drive isn't needed")
                    : keys ? fmt::format("On the PS4: {}", kKeysFile)
                           : fmt::format("Not found: put it on a USB drive (at its top, or in a folder there such as "
                                         "switch; Square then copies it to the PS4), or send it from a browser"),
                    keys});
    const bool title_keys = on_usb.title_keys || (on_usb.keys_folder.empty() && std::filesystem::exists(kTitleKeysFile, ec));
    rows.push_back({"Title keys (title.keys)",
                    on_usb.title_keys ? fmt::format("Used from where it is: {}/title.keys", on_usb.keys_folder)
                    : !on_usb.keys_folder.empty()
                        ? fmt::format("Not next to prod.keys ({}): some games need it", on_usb.keys_folder)
                    : title_keys ? fmt::format("On the PS4: {}", kTitleKeysFile)
                                 : fmt::format("Not found ({}): some games need it", kTitleKeysFile),
                    title_keys});
    size_t firmware = 0;
    for (std::filesystem::directory_iterator it(kFirmwareDir, ec), end; !ec && it != end; it.increment(ec))
        firmware++;
    rows.push_back({"Firmware",
                    !on_usb.firmware_folder.empty()
                        ? fmt::format("Used from where it is: {} files in {}{}", on_usb.firmware_files,
                                      on_usb.firmware_folder,
                                      firmware ? fmt::format(" ({} files on the PS4 too)", firmware)
                                               : std::string(". Square copies it to the PS4, then the drive isn't "
                                                             "needed"))
                    : firmware ? fmt::format("On the PS4: {} files in {}", firmware, kFirmwareDir)
                               : fmt::format("None in {}: put your Switch's firmware (its folder of .nca files) on a "
                                             "USB drive, or its .zip (Square copies either to the PS4), or send "
                                             "it from a browser",
                                             kFirmwareDir),
                    !on_usb.firmware_folder.empty() || firmware > 0});
    if (!g_games_folder.empty())
    {
        struct stat status{};
        const bool there = stat(g_games_folder.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
        rows.push_back({"Game Folder",
                        there ? fmt::format("{}: games, keys and firmware are looked for in it and its folders "
                                            "(Triangle to change it)",
                                            g_games_folder)
                              : fmt::format("{} isn't there now (a USB drive not plugged in?): Triangle to choose "
                                            "another",
                                            g_games_folder),
                        there});
    }
    else
    {
        rows.push_back({"Game Folder",
                        "None: Triangle to choose one, on a USB drive or anywhere (games, keys and firmware are "
                        "looked for in all its folders)",
                        true});
    }
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
    "Games (.nsp, .xci) and homebrew (.nro) go in /data/eden-ps4, on a USB drive, or in a Game Folder (Triangle: "
    "any folder, on a drive or elsewhere, searched through all its folders). prod.keys, title.keys and the "
    "firmware (its folder of .nca files) on a USB drive are used from there: the keys at the drive's top or in a "
    "folder there (switch), the firmware in a folder. Square copies them (or the firmware's .zip, unpacked) from "
    "the drive to the PS4, after which the drive isn't needed; it also takes them from /data/eden-ps4/import, "
    "where a file manager such as PS4-Xplorer can put them. Or send all of them from a browser: the address is at "
    "the top of the HOME menu. OPTIONS on a game on the PS4 deletes it.";

constexpr const char* kCopyKeysTitle = "Copy keys and firmware to the PS4";

// Import from USB (Games and Keys): the copy runs on its own thread, its progress in a message
struct UsbImportJob
{
    std::thread thread;
    std::atomic<bool> done{false};
    std::atomic<size_t> copied{0}, total{0};
    Launcher::UsbImport result;
};

// Copy to the PS4 (OPTIONS on a game on a drive): the game copied into the data folder, on its own
// thread, its progress in a message
struct CopyJob
{
    std::thread thread;
    std::atomic<bool> done{false};
    std::atomic<uint64_t> copied{0}, total{0};
    std::string name, error;
    bool ok = false;
};

std::string GigabytesText(uint64_t bytes)
{
    return fmt::format("{:.2f} GB", double(bytes) / 1e9);
}

// OPTIONS, Delete, on a title on the PS4's storage (confirmed): the file and what the launcher
// keeps about it (its name and icon). Only a game or homebrew file under /data; its saves stay.
std::string DeleteFromPs4(const std::string& path, const std::string& copying)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path file = fs::path(path).lexically_normal();
    const std::string name = file.filename().string();
    if (!file.string().starts_with("/data/") || !IsGameFileName(name) || !fs::is_regular_file(file, ec))
        return name + " wasn't deleted: only a game or homebrew file on the PS4's storage can be.";
    if (!copying.empty() && copying == name)
        return name + " is being copied: delete it once the copy is done.";
    const uintmax_t size = fs::file_size(file, ec);
    if (!fs::remove(file, ec) || ec)
        return name + " couldn't be deleted: " + (ec ? ec.message() : std::string("it's gone already"));
    // (what the launcher learned about it: its name, its icon)
    fs::remove(Launcher::TitleCachePath(kDataDir, path, "txt"), ec);
    fs::remove(Launcher::TitleCachePath(kDataDir, path, "jpg"), ec);
    const fs::space_info space = fs::space(kDataDir, ec);
    return fmt::format("{} is deleted ({} freed{}).", name, GigabytesText(size),
                       ec ? std::string() : ", " + GigabytesText(space.available) + " free now");
}

std::string UsbImportReport(const Launcher::UsbImport& r)
{
    if (r.keys_folder.empty() && r.firmware_folder.empty())
        return "No prod.keys and no firmware on a USB drive or in /data/eden-ps4/import (the app sees: " +
               UsbReport() + "). Put prod.keys and title.keys at the drive's top (or in a folder there, such as "
               "switch) and the firmware (its .zip as it came, or its folder of .nca files) on it, then Square "
               "again.";
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
    else
        text += "They're on the PS4 now (/data/eden-ps4/user): games start without the USB drive.";
    return text;
}

std::vector<std::pair<std::string, std::string>> News()
{
    return {
        {"Eden for PS4 " EDEN_PS4_VERSION,
         "04.28: a fix for 04.23-04.27: the CPU translator could, rarely, wrongly drop the saving of a "
         "result's flags before a branch (its look-ahead read the branch's target a few bytes off): a 64-bit "
         "game could then take a wrong turn. Install this one over them. Also aimed at Unity games "
         "(Silksong, Bluey): their null checks no longer cut the CPU code into "
         "blocks of two or three instructions (a check that fails leaves from the middle instead), and float "
         "values are stored straight from the vector registers. A Unity-shaped test: 22% fewer block "
         "changes, 8% fewer instructions with the fix.\n"
         "04.27: 64-bit games: float constants in one load, and calls and returns cheaper (a deeper return "
         "stack, stepped without a mask): the test game 38% leaner than in 04.22.\n"
         "04.26: 64-bit games load floats straight into the PS4's vector registers and sign-extending "
         "values (LDRSW and friends) in one instruction each: the test game is now 36% leaner than in 04.22.\n"
         "04.25: 64-bit games: loads and stores through the same base no longer go through a shared "
         "page-table lookup when the direct path works (8% fewer instructions on the test game, 33% fewer "
         "than 04.22), and atomic loads (spin locks, job counters: Unity games use many) are plain loads "
         "instead of locked operations that bounced the memory between cores.\n"
         "04.24: more of the same for 64-bit games: a CSEL or CSET after a compare picks its value on the "
         "PS4's own flags (cmp; cmov) instead of saving the Switch's flags and reading them back, and a "
         "function's return checks the return address as it is (8 instructions instead of 14). The 64-bit "
         "test game: 27% fewer instructions than in 04.22 with the options Eden uses.\n"
         "04.23: 64-bit games' CPU code (Zelda, Silksong, Bluey, Animal Crossing) runs leaner again: a "
         "conditional branch tests the compare just before it on the PS4's own flags instead of saving the "
         "Switch's flags and reading them back; flags that no later code reads aren't saved at all (the "
         "translator reads ahead into the code a block leads to, to tell); floats stay in vector registers "
         "instead of moving through general ones; an access at a fixed address skips its range check. A "
         "64-bit test game runs 21% fewer instructions than in 04.22. And the 30-second line now says what "
         "the game's writes to memory the GPU caches cost (Echoes of Wisdom makes 50-84 thousand a second).\n"
         "04.22: 64-bit games (Zelda, Silksong, Bluey, Animal Crossing) get three of the CPU translator's "
         "savings that only 32-bit games such as Mario Kart had: an address's constant taken into the memory "
         "access itself (no add before it), no second zero-extension after loading a byte or halfword, and the "
         "rarely run code kept apart from the code that runs. The FPS counter is at the top centre now.\n"
         "04.21: the settings lists scroll (Graphics had rows below the screen), and the package's own "
         "programs (Breakout, Blocks, the controller and GPU tests, the test program) are no longer listed "
         "with your games. And the CPU translator's code "
         "space, sized for Mario Kart (48 MiB for the main core), is now as large as the PS4's memory for "
         "generated code allows, up to 160 MiB: Silksong filled the main core's 48 MiB every 30-40 s and "
         "translated everything again each time (half of every second, the long freezes), Zelda's third core "
         "5 times in 6 minutes. The boot trace says which sizes it got.\n"
         "04.20: Zelda's crashes 30-50 s in (two runs in a row): the CPU translator keeps, per core, a table of the "
         "places to link to each block, and in Zelda it grew to 57 MB, made as one piece of memory; Eden for PS4 "
         "added a heap for it, but another thread could take some of that heap first, and the allocation failed. "
         "The table now takes a fifth of that, and a new heap serves the request it was made for first. Also: "
         "Settings, Graphics, Draw Shortcuts (04.15 and 04.16's savings on every draw): Zelda froze twice at the "
         "same spot (5.5-6.5 minutes in, the GPU stopped), which it hadn't in 04.15; turn them off and play to "
         "that spot to tell whether they are the cause.\n"
         "04.19: the translation cache was never used: Eden put a game's code at a random address each boot, "
         "and the cache finds code by its address, so 04.17's and 04.18's caches matched nothing (0 of 163,801 "
         "pieces in Mario Kart, 0 of 60,902 in Zelda). Each game's code now has the same address every boot. "
         "Made safer for its first real use: a cache whose last run crashed, hung or was closed from the PS4's "
         "menu is deleted instead of loaded, and the saved code's pointers are found more strictly. Zelda's "
         "slow second and third runs were the GPU, not the cache: after the first run hung the GPU, it started "
         "Eden's work 0.25-2 s late in the next runs; the log now says so (restart the PS4 if it does).\n"
         "04.18: the translation cache for 64-bit games too (Animal Crossing, the Zelda games: they translate "
         "even more code as they run than Mario Kart, in every new area). The same on/off setting. 04.17's "
         "caches are made again once (a new file format).\n"
         "04.17: the first race after starting a game stutters because Eden translates the game's CPU code "
         "for it the first time it runs (its shaders were already built during the black screen). Now each "
         "core's translated code is kept when you leave the game (/data/eden-ps4/cache/jit) and used again "
         "from the start next time, each piece checked against the game's code first (Settings: Translation "
         "Cache; off to compare). Leaving takes a second or two longer; the first start after this version "
         "builds the cache. Also: the shader cache is no longer deleted whole when a crash cut its last entry, "
         "and the log measures shader and pipeline work at boot and in play.\n"
         "04.16: the next idea from Ryujinx, textures. For every texture of every draw Eden looked up its "
         "descriptor and its sampler's in the GPU's memory, translating each address through the GPU's page "
         "table, and did the same for each texture handle. Now the descriptors are read straight from where "
         "they sit (worked out again when the GPU's memory map changes), and a draw's handles from one page "
         "lookup. And Eden no longer looks up the draw's pipeline twice.\n"
         "04.15: before each draw Eden worked out the graphics pipeline's key again from ~410 of the GPU's "
         "registers and compared it with the last one (~2-3% of its GPU thread). Now, when none of those "
         "registers took a new value since (and the shaders and the kind of primitive are the same), it "
         "uses the last pipeline. Which registers: found by a test that flips every bit of every register "
         "against Eden's own code. Every 64th time it checks anyway; if the key ever differs, it stops "
         "doing it for the session and the log says so.\n"
         "04.14: Eden's GPU thread does less per draw, from 04.12's samples of it in races. Eden for PS4's "
         "own counters cost it ~1%: two locked adds per uniform buffer, in a cache line the timing thread "
         "also writes; its counters now have their own, added without a lock. A macro's parameters, when "
         "they come in one piece, are no longer copied before it runs (0.4%). 04.12's comparison of "
         "constant buffer data with memory is gone (answered). A few % together on the race start's limit.\n"
         "04.13: a crash fixed. Booting Mario Kart once crashed while it loaded (as one did in 04.09): when "
         "the game gave memory back, Eden's graphics service thread, deleting the textures in it, also wrote "
         "into Eden's list of GPU commands, which only the GPU thread may do, at the same time as the GPU "
         "thread. Now it leaves that part to the GPU thread, which does it before it next draws. And what "
         "04.12's counts showed: at a race's start the game waits for Eden's GPU thread (up to 30% of the "
         "main thread's time on Baby Park), and that thread copies about four of the game's constant buffers "
         "for every draw, ~1 MiB a frame, half of it unchanged: the next thing to make cheaper.\n"
         "04.12: counting the uniform buffers. Comparing with Ryujinx (another Switch emulator, built "
         "differently) showed one big difference in the work done on every draw: Eden copies each of a draw's "
         "small uniform buffers (the game's constants) into a fresh buffer every draw unless almost all of "
         "them came unchanged from its cache, and a game rewriting the same constants every frame looks to it "
         "like a change; Ryujinx skips identical rewrites and copies only what changed. Before changing "
         "anything, the log now counts it: the constant data the game writes and how much was already there, "
         "the uniform buffers copied (every 10 s too) and those taken from the cache. Also 04.11's report of "
         "what the game waits for.\n"
         "04.11: what the game waits for at a race's start. In 04.10's races the game's main thread, at the "
         "start, spent 10-19% of its time waiting on something (17-21 waits a second, 5-9 ms each, three "
         "times what it waits later), while Eden's GPU thread was only ~72% busy: so it isn't only the GPU "
         "thread's work it waits for. Eden's events now keep the names their services gave them (a GPU fence, "
         "the display's buffer queue, audio...), and the log says, for each core, what ended the game's waits "
         "and how long they took (every 30 s; the main thread's two longest every 10 s). No other change: "
         "a race or two, each from a fresh boot, says where to go next.\n"
         "04.10: what slows a race's start, and a crash guarded. 04.09's counts show that a race's start "
         "isn't waiting for anything new (its shaders and textures came while the track loaded): it simply "
         "draws twice as much, ~800-880 draws a frame against ~400 later, with all the karts together, and "
         "Eden's GPU thread spends about 28 microseconds on each. This release makes the commonest part of "
         "that cheaper: the game's register writes to the 3D engine now go straight into its registers "
         "instead of a queue that was read back later. Also: a crash in the PS4's graphics driver while a "
         "game loaded (ending a render pass that wasn't begun) is now skipped and counted instead, and the "
         "log bundle keeps the last five sessions instead of two, so a test of several boots is all in it.\n"
         "04.09: no more frozen exits, and a closer look at a race's start. 04.08 ran your slowest track at 34.0 "
         "fps on average, its lowest 30.7 after the start; but once, leaving the game for Eden's menu froze "
         "everything. Now, if leaving a game ever stalls for 30 seconds, the app saves its logs and starts again "
         "on its start page (where you were going) instead of staying frozen, and the log says which step of "
         "the shutdown it stalled in, so it can be fixed. The first race's start still dips (27.6 once): the log "
         "now says, every 10 seconds, how many draws the game made and what was new to Eden then (graphics "
         "pipelines not in the shader cache, draws skipped while they built, new textures and buffers), to see "
         "what that start waits on.\n"
         "04.08: the start of a race. 04.07 ran your slowest track at 32.1 fps on average, but the first 20 "
         "seconds of a race dipped to 25-28. There, with all twelve karts on screen, Eden's GPU thread (which "
         "turns the game's graphics commands into work for the PS4's GPU) was busy 85-90% of the time and the "
         "game waited for it. This release trims that thread: the game's register writes, most of its "
         "graphics commands, are taken in one loop instead of one call each; only the vertex buffers that "
         "changed are looked at on each draw (not all 32); vertex formats are remembered instead of looked up "
         "on every draw; and a setting read on every buffer lookup is read once a frame. The 30-second log "
         "line now also counts the GPU thread's draws and commands a second.\n"
         "04.07: a batch of small savings on the game's main thread, the one that sets Mario Kart's frame "
         "rate. Each is small on its own (a fraction of a percent to about one percent), so they come "
         "together: a function's return no longer reads back the address it just wrote (1% fewer host "
         "instructions in game-like code); each piece of translated code keeps its rarely run parts (fault "
         "and slow paths) apart from the code that runs, so more of the running code fits the CPU's caches; "
         "finding the current thread on every system call is a plain read instead of a call; removing a "
         "timer event no longer locks every other queued event; and the services' threads, which sleep "
         "between requests, stay awake longer when the game's requests come close together. The log's "
         "30-second line now also says how the services' threads were woken.\n"
         "04.06: less sleeping on locks. 04.05 ran your slowest track at 30.5 fps on average (29.5-31.5 over "
         "30 seconds, dips to 28 and to 25 at the start of a race). The game's main thread is now the only "
         "limit, and almost a tenth of its core's time went to the PS4's kernel: whenever an emulated core found "
         "one of Eden's own locks taken (its scheduler's, its timer's, a core's context, the timer events' queue), "
         "the PS4 put it to sleep, and waking it again costs far longer than the locks are held. Now a core that "
         "finds a lock taken tries again for up to 20 microseconds before it sleeps, and the timer thread is only "
         "woken for a new event that comes before all the others. The 30-second line in the log now counts, for "
         "each of those locks, how often it was found taken and how it was got, to show what this saved.\n"
         "04.05: three more cuts to the game's translated code. 04.04 ran your slowest track at 28-31 fps "
         "(29.6 on average), faster than 04.01 on your usual one; its busiest code no longer saves flags it "
         "doesn't need. This release goes after what's left: a load or store at a register plus a constant "
         "([r1, #8], a third of the game's busiest instructions) is now one access instead of an add and the "
         "access; a float compare and the branch on it (vcmp, vmrs, bgt) is two host instructions instead of "
         "about thirteen; and byte and halfword loads and stores lose a zero extension they didn't need. In "
         "tests: 13% fewer instructions in Mario Kart's busiest loops and in game-like code, 9% in zlib, on top "
         "of 04.04, with the same results as 04.01 in more than 5,000 random programs and zlib built every way.\n"
         "04.04: 04.02's speed-up, where Mario Kart spends its time. 04.03 ran the races at 26-30 fps at 100% "
         "resolution, as 04.01 did at 50%, but in two thirds of the busiest code the flags were still saved and "
         "read back: the code there leads to a function's return or a call within a few instructions, and the "
         "translator took those as places that might read the flags. The ARM calling convention leaves the flags "
         "undefined when a function is called and when it returns, so now a call or a return counts as setting "
         "them, and where the flags must still be saved, the branch tests the compare directly instead of "
         "reading them back. In tests: 20% fewer instructions in zlib, 8% in game-like code, and the same "
         "results as 04.01 in 2,200 random programs and zlib built every way.\n"
         "04.03: 04.02 fixed. 04.02 never got Mario Kart to its first frame: compiling the game's code got "
         "slower with every piece compiled (1.3 ms each and rising, against 0.1 ms in 04.01). To be safe if "
         "code changed, each piece remembered all the code it had looked ahead at as one span from the lowest "
         "address to the highest, and after a call that span covers megabytes, so every piece overlapped "
         "thousands of others in the list that tracks them. Now it remembers only the few instructions it read. "
         "Compiling is back to 04.01's speed (a test of 3,000 functions: 18 s in 04.02, 0.4 s now), and the "
         "faster code from 04.02 stays.\n"
         "04.02: no flags saved that nothing reads. 04.01 ran Mario Kart's races at 26-30 fps (Profiler on), "
         "about where 04.00 was. Nearly every piece of the game's translated code ended the same wasteful way: "
         "after a compare, the ARM flags were packed and saved to memory, then read back for the branch right "
         "after it. The code a branch leads to almost always sets the flags again before reading them, so the "
         "translator now reads a few instructions ahead into it; when the flags are dead there, they're neither "
         "saved nor read back, and the branch tests the compare directly. In tests: 29% fewer instructions in "
         "Mario Kart's busiest loops, 7% in game-like code, and 3,200 random programs giving the same results "
         "as before.\n"
         "04.01: leaner translated code everywhere. 04.00 ran Mario Kart's races at 24-29 fps (Profiler on) "
         "with no freezes. Its main thread still spends three quarters of a frame in the game's translated "
         "code, spread over thousands of places, so this release makes all of that code shorter: reading a "
         "field of a structure ([r3, #8]) or an array element ([r1, r2, lsl #2]) no longer copies the base "
         "register first, and a conditional branch right after a compare takes the flags from the register "
         "instead of reading them back from memory. In tests: 11-19% fewer instructions in loops, calls, "
         "copies and structure accesses, with 2,350 random programs giving the same results as before.\n"
         "04.00: no more 20-second freezes. 03.99 ran Mario Kart's races at 25-29 fps with the profiler on "
         "(23-26 before): the game's globals now use fastmem (declines down from 2,000 a second to none). But "
         "five minutes in the picture froze for 20 seconds. The hang dump showed the GPU driver's and the "
         "audio service's threads both waiting on a lock of the 30-second statistics, which spun instead of "
         "sleeping: the thread holding it, of a lower priority, couldn't get a core while they spun, and the "
         "game waited on the GPU driver until the hang watchdog's dump shook things loose. Those locks sleep "
         "now (an earlier freeze in Captain Toad was likely the same). Also, the idle cores no longer bump a "
         "shared counter each time they read the clock.\n"
         "03.99: the game's own globals on the fast path. 03.98's profile and thread list showed Mario Kart's "
         "main thread running three quarters of the time and waiting on two helper threads the rest, all of "
         "them in the game's translated code, its time spread thinly over thousands of places. Looking at that "
         "code: every read of the game's global variables, its constants and its function tables took the slow "
         "way (a page table look-up), about 135,000 places in a session. The cause: Eden places the game's code "
         "at a random address in 4 KiB steps, and three times in four that put all of it out of line with the "
         "PS4's 16 KiB pages, so fastmem couldn't reach any of it. Now the step is 16 KiB (still random), and "
         "the last odd pages of a large allocation come as one lined-up piece too.\n"
         "03.98: a look at which of the game's threads hold a frame up. 03.97's log showed Mario Kart's "
         "main thread (core 0) waiting for other threads about 28% of the time, and a lock it and a "
         "thread on core 2 take turns at about 1,100 times a second. The 30-second lines now list the "
         "game's busiest threads: each one's time running and waiting, and on what (condition variables, "
         "locks, objects, sleeps, service calls), to find the slowest step a frame goes through. Long "
         "lines are kept whole now (03.97's were cut at 2 KiB). For the next log: turn Profiler on (System "
         "Settings, CPU) and race the same track as before.\n"
         "03.97: faster vector loads and stores, and quicker hand-offs between the game's threads. 03.96 ran "
         "Mario Kart's races at 25-29 fps. Its NEON loads and stores (VLD1, VST1: audio mixing, vector "
         "maths, copies) were translated an element at a time, sixteen loads for one 16-byte register; "
         "now one access per 8 bytes, a whole register at once (a copy loop 6 times faster in a test). "
         "The three cores the game runs on now wait for work a millisecond before sleeping (they have a "
         "PS4 core each), so a thread handed work rarely pays a sleeping core's wake-up. And the 30-second "
         "lines say what each core's threads wait on, and which GPU driver calls take the time.\n"
         "03.96: games on the PS4 can be deleted, and the keys and firmware copied to it. OPTIONS on a game "
         "(or homebrew) on the PS4's own storage has Delete, which asks first; its saves stay. In Games and Keys, "
         "Square copies prod.keys, title.keys and the firmware from a USB drive to the PS4, after which the drive "
         "isn't needed: games start with the PS4's copies when it's not plugged in. Games and Keys says which "
         "copies are used, and whether the PS4 has its own.\n"
         "03.95: thread stacks back on the fast path. 03.94 cut the stutters (far fewer translations in "
         "races) but walked every stack access the slow way from the start, and races ran at 20-23 fps, "
         "about 10% more work a frame on the busiest core than 03.93. Now the cause is gone instead: a "
         "game's thread stacks are memory it maps a second time at an address it picks, which didn't line "
         "up with the PS4's 16 KiB pages, so fastmem couldn't reach them. Eden's kernel now puts each such "
         "stack on a copy of its memory that lines up (with spare pages at its ends), and copies it back "
         "when the stack goes; the game's main stack lines up from the start. Stack accesses use fastmem "
         "again, and the slow way is only used if stacks keep failing. Core 0, where Mario Kart runs its "
         "main thread, also gets more room for translated code (48 MiB, from 40; it had 5 MiB left after "
         "a few races). The 30-second lines say how many stacks were lined up.\n"
         "03.94: fewer stutters. 03.93 ran Mario Kart races at 22-24 fps. Its stutters came at the start "
         "of races and while loading: each time new game code ran, every stack access in it first failed "
         "the fast way and had the code translated again (up to 5,000 times a second while loading, "
         "hundreds at a race's start), and the old copies filled the translator's memory until it had "
         "to throw everything away and start over (twice in that session). Now, once the first stack "
         "access fails the fast way, all code that touches the stack (or a thread's own storage) is "
         "translated the right way from the start. In a test of 50 functions: 1,000 failed accesses "
         "before, 20 now. The 30-second lines also show each core's translator memory, to see whether "
         "it still runs out.\n"
         "03.93: function calls up to 2.6 times cheaper in Mario Kart. 03.92's profile showed where the "
         "rest of the time goes: two thirds of the memory accesses in Mario Kart's busiest code went the "
         "slow way, mostly each function's saving and restoring of registers on its stack. The PS4 maps "
         "memory in 16 KiB pieces and the Switch in 4 KiB ones, and the game's stacks don't line up "
         "with the PS4's pieces, so the fast way (fastmem) can't reach them; each of those accesses then "
         "looked its page up on its own, nine look-ups to save nine registers. Now the accesses that can't "
         "use fastmem share one look-up, as a function's whole save or restore. In a test of nothing but "
         "calls on such a stack: 55 ns a call before, 21 now (18 where fastmem works). And the services that "
         "run on threads of their own (audio, the GPU driver, files) now outrank the other threads: in "
         "03.92 a call to one waited 130 us before the service even took it. The Profiler's summary also "
         "names the app's code again and counts the game's code properly.\n"
         "03.92: measurements for the next step. 03.91 ran Mario Kart races at 21 fps (03.88: 16-19). "
         "To find the rest: the log's 30-second lines now say how long the game's threads wait for each "
         "call to a system service, split into waiting for the service, the service's work and getting "
         "back to the game (in 03.91 a call took 200 us on average, of which the work was about 50), and "
         "how much time the translator spends on new code each second (bursts of it are slow frames). "
         "And the log of the game before the last one is kept too, so playing another game after Mario "
         "Kart no longer replaces its numbers.\n"
         "03.91: fixes 03.89 and 03.90 closing Mario Kart as it started (\"bad mem size\"). The new "
         "return jumps were written in a form that the code generator rejects when its strictest "
         "checks are on. On a PC they are off, so testing there never saw it. On the PS4 they "
         "were on, because the GPU's own small code generator brought its strict copy of the same "
         "functions into the app. Both are fixed: the jumps use the accepted form, and the whole app "
         "now uses the code generator's lenient settings, as the CPU translator always meant to. "
         "Everything else from 03.89 and 03.90 is in.\n"
         "03.90: more of the same. 32-bit ARM code can make almost any instruction conditional (\"move if "
         "not equal\"); each such instruction used to split the code around it into three pieces with "
         "handovers in between. Now most stay in place as a choice between the old and new value, with "
         "no jump at all. And a float comparison followed by a branch on its result (very common in "
         "game physics) skips a round trip through memory and two conversions. Calls through pointers "
         "(C++ virtual calls, everywhere in game engines) now each have their own jump the CPU can "
         "predict, as returns got in 03.89: in a test, mispredicted jumps fell from 106,000 to 6,000 "
         "per 100,000 calls, each one about 14 wasted cycles on the PS4's CPU. Together with 03.89: "
         "Mario Kart-style 32-bit code runs 29% fewer instructions than in 03.88 (on a PC set up like "
         "the PS4). Not yet tried on a console.\n"
         "03.89: leaner translated code. Mario Kart 8 Deluxe is a 32-bit game, and the translator split every "
         "loop's closing branch into a block of its own, with a handover between the two each time round; "
         "now the branch ends the block it is in. Each block also stopped saving and restoring a register "
         "that only a PC debugger uses, values that once went to memory to free a register now move to a "
         "spare one, forward jumps skip a check that loops still make on their way back, and each "
         "function return has its own jump the CPU can predict. On a PC set up like the PS4's CPU, "
         "Mario Kart-style 32-bit code now runs 25% fewer instructions, 64-bit code 13% fewer. The "
         "Profiler (System Settings) also works again: its report was an old session's, named with "
         "a newer build's functions, so it made no sense. Now it says which game code the time goes "
         "to, and hotblocks.log holds that code. A Mario Kart race with the Profiler on would show "
         "where the next gains are.\n"
         "03.88: a fresh start for every game. 03.87's log: Mario Kart races up to 18-19 fps with fastmem, its "
         "slow memory accesses down from 280,000 to under 30,000 a second. But the app never gave back what "
         "Mario Kart took: Captain Toad, started after it, had 1.8 GB of the PS4's 4.6 GB of memory and almost "
         "no system memory, and was closed by the PS4 while loading a level. Now the app restarts itself "
         "after each game (back to this page, all memory free). And if a crash ever happens inside the "
         "emulator's memory handling, the log now says where instead of the app just vanishing; the log "
         "also says why fastmem turns memory away, which Captain Toad did 1,500 times a second.\n"
         "03.87: fastmem without its catch. In 03.86 it worked (gigabytes mapped) but races weren't faster: "
         "every atomic operation that once touched memory fastmem couldn't map went back to the slowest path "
         "for good, 280,000 times a second, eating the gain. Those now keep the fast inline path. And memory "
         "the GPU is caching can now be read through fastmem too (only writes to it take the slow path, so "
         "the GPU hears about them). On a PC with the PS4's setup, Mario Kart-style code runs 4-30% faster "
         "with fastmem than without.\n"
         "03.86: fastmem, for real this time. 03.85 no longer crashed, but its log said fastmem mapped nothing: "
         "with CPU Accuracy Unsafe, Eden switches fastmem off unless a 'host MMU' option is on, and that option "
         "defaults to off on systems Eden doesn't know, like the PS4. Now it's on. Also: fastmem's memory "
         "range goes where it can be its full size (512 GiB) without crowding the GPU; addresses past it are "
         "looked up inline instead of through the slow path; the check on each access is one instruction "
         "fewer; and a run of loads and stores through one register (a function's start, an object's "
         "fields) still shares one lookup. On a PC with the PS4's setup, chains of loads run twice as fast.\n"
         "03.85: fastmem fixed. In 03.84 it closed games at their first picture: its address range (512 GiB, "
         "only ever used by the CPU) was taken low in the PS4's address space, which pushed the GPU's own "
         "ranges up to where the PS4's GPU can't reach, and the first thing the GPU drew killed the app. "
         "The range now goes high up, above the GPU's part, which stays where it always was. Fastmem is "
         "on again by default; System Settings, CPU, Fastmem still turns it off.\n"
         "03.84: FASTMEM. 03.83's log said yes: after sceKernelEnableDmemAliasing the PS4 lets this app map "
         "the same memory twice. So the Switch's memory now also sits at its own addresses, and the game's "
         "loads and stores are single instructions instead of a table lookup each (about 10), as on a PC: "
         "the biggest speedup there is for the game's own code, which is where Mario Kart's and Captain "
         "Toad's frames go. Pages appear there as the game first touches them; one the GPU is caching, or "
         "that isn't plain memory, keeps the lookup. System Settings, CPU, Fastmem turns it off if a game "
         "crashes or misbehaves with it. The log's 30-second line says how much is mapped and how many "
         "accesses were turned away.\n"
         "03.83: every memory access of the game's code is shorter. The table the recompiled code looks up "
         "for each load and store now holds the PS4 address whole, so the lookup is one instruction instead "
         "of three; a chain of loads (following pointers, as games do all the time) runs about 20% faster on "
         "a PC with the PS4's setup. And the start of the log now says whether the PS4 lets this app map the "
         "same memory twice, using two kernel calls found in the PS4's system library that allow it "
         "(sceKernelEnableDmemAliasing). If it does, the next version can use fastmem: the game's loads and "
         "stores as single instructions, the biggest speedup left. Just opening the app and sending the log "
         "answers it.\n"
         "03.82: System Settings, Graphics, Game Speed: 1.0x, 1.5x or 2.0x. The Switch's vsyncs come that much "
         "more often, so a game that has room to spare runs faster (up to 90 or 120 frames a second): one "
         "that moves by the frame speeds up, one that moves by the clock looks smoother. It raises the "
         "limit, not the PS4's power: a scene the PS4 can only run at 15 fps stays at 15. Above 1.0x the "
         "display doesn't wait for the TV's refresh (the picture may tear), and sound may lag behind.\n"
         "03.81: faster 64-bit games. 03.80's log of Captain Toad: Treasure Tracker showed its main thread "
         "busy all the time at 15 fps (the vsync wasn't holding it back; Frame Pacing won't change it): the "
         "game's own code is the limit. Most Switch games are 64-bit, and their every load and store walked "
         "the memory table here (about 12 instructions); now the loads and stores of a function's start and "
         "end, and of an object's fields, share one walk, as 03.79 did for 32-bit games. On a PC with the PS4's "
         "setup, a call that saves registers and reads and writes a few fields takes 12 ns instead of 27. "
         "Also fixed: the log's idle times (a core asleep for minutes showed 0%, then hundreds of percent) "
         "and its list of busiest services.\n"
         "03.80: atomic operations run inline. 03.79's log still had 370,000 memory accesses a second leaving "
         "the recompiled code, half of them writes: the game's atomic operations (LDREX/STREX, behind every "
         "lock and reference count), each a call into the emulator and its exclusive monitor. They now go "
         "through the same inline table walk as ordinary loads and stores (32- and 64-bit games): on a PC with "
         "the PS4's setup, an atomic increment and decrement takes a quarter of the time or less. A memory "
         "barrier (DMB) costs half what it did. The 30-second line in the log now says how long each emulated "
         "core sits idle and how long all of them are idle at once, how long a finished frame waits for the "
         "next vsync, how long the game waits for a buffer to draw in, and which services the game's service "
         "calls spend their time in. New: System Settings, Graphics, Frame Pacing. At 60 Hz (as before) a frame "
         "that misses a vsync waits for the next one, so 50 ms of work shows as 15 fps; Unlocked shows each frame "
         "the moment it's done. A race with each, the Profiler off, and the log, shows what's next.\n"
         "03.79: the game's own code runs faster. Every memory access of the Switch's CPU walks a table here "
         "(the PS4 can't do it the way PCs do), and a function's start and end save and restore up to 17 "
         "registers, each with its own walk: now one walk does for all of them, and for a run of loads and "
         "stores through the same register (an object's fields, the stack). On a PC, with the PS4's setup, "
         "calls take a third to a quarter of the time they did. And the emulated CPU cores and the GPU thread "
         "now have a core each on the PS4's first module, sharing its cache (System Settings, CPU, Thread "
         "Placement: Free puts them back). The boot trace says whether the PS4 can map memory twice, which "
         "the next big step would need.\n"
         "03.78: faster games. An emulated CPU core with nothing to run waits a moment before it sleeps, "
         "so a game's threads hand work to each other without waking a sleeping thread each time; the "
         "Switch's timers fire on time; and the game's ARM code is translated faster (loading, new areas). "
         "Drives at /usb0 are found without a Game Folder. Turn the Profiler off for playing: it interrupts "
         "the emulated cores 1000 times a second. On a PS4 Pro, Boost Mode (Settings, System) makes the CPU "
         "a third faster.\n"
         "03.77: an FPS counter in the top left corner while a game runs (System Settings, Graphics, FPS "
         "Counter): the frames the game itself drew in the last second.\n"
         "Copy to the PS4: in a game's Options from a USB drive (or another folder), \"Copy to the PS4\" copies "
         "it to the PS4's own drive (/data/eden-ps4), with its progress; it then starts from there, faster "
         "than from the USB drive, and the list shows it once.\n"
         "03.76: with a Game Folder chosen, every log goes there as one file, eden-ps4-logs.txt, as the app "
         "starts and after each game: take it away on the USB drive, no network needed. While a game runs, the "
         "boot trace notes its speed every 10 seconds (its frames a second, and how close to full speed).\n"
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
         "doesn't mean an app is allowed to read it.\n"
         "03.73: USB drives: games are found in any folder on the drive too (switch, games...), the list is "
         "made again when a drive is plugged in or mounted after the app started (it was only made as the app "
         "started), and Import from USB finds Prod.keys in any case.\n"
         "03.74: keys and firmware on a USB drive are used from there, without importing: prod.keys and "
         "title.keys at the drive's top or in a folder there (switch), the firmware's .nca files in a folder. "
         "Games and Keys says which keys and firmware games will use. A firmware .zip still needs Import.\n"
         "03.75: Game Folder (Games and Keys, Triangle), as in Crema: choose any folder, on a USB drive or "
         "anywhere the console shows (/mnt/usb0-7, /mnt/ext0, /mnt/ext1, /mnt, or from /), and its games, keys "
         "and firmware are found in it and its folders, six deep."},
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
         "Put games (.nsp, .xci) in /data/eden-ps4, or send them from a browser (the address at the top of the "
         "HOME menu). They need the keys and firmware of the Switch they were dumped from: send them from the "
         "browser too, or copy prod.keys, title.keys and the firmware (its .zip, or its folder of .nca files) from "
         "a USB drive into /data/eden-ps4/import with a file manager such as PS4-Xplorer, then Import in Games and "
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

// The logs as one file in the Game Folder, eden-ps4-logs.txt (what the web page's "Download all logs"
// gives, code addresses named): taken away on its USB drive, without a network. As the app starts
// (the last session's, a crash's included) and when a program has ended.
void ExportLogs(const char* when)
{
    struct stat status{};
    if (g_games_folder.empty() || stat(g_games_folder.c_str(), &status) != 0 || !S_ISDIR(status.st_mode))
        return;
    const std::string file = g_games_folder + "/eden-ps4-logs.txt";
    const bool written = Launcher::WriteLogBundle(kDataDir, EDEN_PS4_VERSION, kSymbolsFile, file);
    Trace(fmt::format("logs {}: {} {}", when, written ? "written to" : "could not be written to", file));
}

// A run's file kept as <stem>-previous<ext>, the ones before it as <stem>-previous-2<ext> to
// <stem>-previous-<keep><ext> (04.10): every kind shifts each run, present or not, so that the
// files with one number are one run's
void KeepPrevious(const std::string& stem, const std::string& ext, int keep = 4)
{
    const auto previous = [&](int i) {
        return stem + "-previous" + (i > 1 ? "-" + std::to_string(i) : std::string()) + ext;
    };
    std::remove(previous(keep).c_str());
    for (int i = keep - 1; i >= 1; i--)
        rename(previous(i).c_str(), previous(i + 1).c_str());
    rename((stem + ext).c_str(), previous(1).c_str());
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
    g_games_folder = settings.games_folder;
    ExportLogs(first ? "of the last session" : "after the program");
    {
        // Game Folder's places, as Crema's: the data folder, the USB drives, extended storage, /mnt
        std::vector<std::string> places{kDataDir};
        for (int i = 0; i < 8; i++)
            places.push_back(UsbDrive(i));
        places.insert(places.end(), {"/mnt/ext0", "/mnt/ext1", "/mnt"});
        menu.SetFolderPlaces(std::move(places));
    }
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
        menu.SetKeysPresent(std::filesystem::exists(kKeysFile, ec) || !KeysAndFirmwareOnUsb().keys_folder.empty());
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
    static std::unique_ptr<CopyJob> copy_job;
    menu.SetHeld(MenuButtons(MenuPad())); // (a button still held from the game doesn't count)
    const double start = Seconds();
    const int countdown = 60;
    MenuChoice choice{kTestProgram, settings};
    double web_checked = start;
    std::string usb_signature = UsbSignature();
    for (;;)
    {
        const double now = Seconds();
        const uint32_t pad = MenuPad();
        menu.SetClock(ClockText());
        if (now - web_checked >= 3)
        {
            // (the network may come up after the app; a new address shows; a USB drive may be
            // plugged in, or mounted after the app started: its games show)
            web_checked = now;
            if (std::string usb = UsbSignature(); usb != usb_signature)
            {
                usb_signature = std::move(usb);
                Trace("usb: " + UsbReport());
                g_files_changed = true;
            }
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
        if (action.kind == Ui::MenuAction::Kind::CopyToPs4 && copy_job)
            menu.ShowMessage("Copy to the PS4", "Still copying the last one: wait for it to finish.");
        else if (action.kind == Ui::MenuAction::Kind::CopyToPs4)
        {
            const std::filesystem::path from{action.path};
            const std::string to = std::string(kDataDir) + "/" + from.filename().string();
            std::error_code ec;
            const uint64_t size = std::filesystem::file_size(from, ec);
            const std::filesystem::space_info space = std::filesystem::space(kDataDir, ec);
            if (std::filesystem::exists(to, ec))
                menu.ShowMessage("Copy to the PS4", from.filename().string() + " is already in /data/eden-ps4.");
            else if (!ec && space.available < size + (uint64_t(1) << 30))
                menu.ShowMessage("Copy to the PS4",
                                 fmt::format("Not enough room on the PS4: {} is {}, {} is free (and 1 GB is kept spare).",
                                             from.filename().string(), GigabytesText(size),
                                             GigabytesText(space.available)));
            else
            {
                Trace("start page: copying " + action.path + " to " + to);
                copy_job = std::make_unique<CopyJob>();
                copy_job->name = from.filename().string();
                copy_job->total = size;
                menu.ShowMessage("Copy to the PS4", "Copying " + copy_job->name + "...");
                copy_job->thread = std::thread([job = copy_job.get(), path = action.path, to] {
                    job->ok = Launcher::CopyWithProgress(path, to, job->error, [job](uint64_t copied, uint64_t total) {
                        job->copied = copied;
                        job->total = total;
                    });
                    job->done = true;
                });
            }
        }
        if (copy_job && copy_job->done)
        {
            copy_job->thread.join();
            const std::string report =
                copy_job->ok ? copy_job->name + " is on the PS4 now, in /data/eden-ps4: it starts from there (the "
                                                "copy on the drive can go)."
                             : "Copying " + copy_job->name + " stopped: " + copy_job->error;
            Trace("start page: copy: " + report);
            if (!menu.UpdateMessage("Copy to the PS4", report))
                menu.ShowMessage("Copy to the PS4", report);
            copy_job.reset();
            g_files_changed = true;
        }
        else if (copy_job && copy_job->total)
        {
            menu.UpdateMessage("Copy to the PS4",
                               fmt::format("Copying {}: {} of {}...", copy_job->name,
                                           GigabytesText(copy_job->copied.load()), GigabytesText(copy_job->total.load())));
        }
        if (action.kind == Ui::MenuAction::Kind::Boot && copy_job)
            menu.ShowMessage("Copy to the PS4", "Still copying: start it again once the copy is done.");
        if (action.kind == Ui::MenuAction::Kind::Delete)
        {
            const std::string report = DeleteFromPs4(action.path, copy_job ? copy_job->name : std::string());
            Trace("start page: delete: " + report);
            menu.ShowMessage("Delete", report);
            g_files_changed = true;
        }
        if (action.kind == Ui::MenuAction::Kind::ImportUsb && !import_job)
        {
            Trace("start page: importing keys and firmware from USB");
            menu.ShowMessage(kCopyKeysTitle, "Looking for prod.keys, title.keys and firmware on the USB drives and "
                                             "in /data/eden-ps4/import...");
            import_job = std::make_unique<UsbImportJob>();
            import_job->thread = std::thread([job = import_job.get()] {
                // the import folder (filled from a USB drive with a file manager such as PS4-Xplorer),
                // then the drives themselves
                std::vector<std::string> drives{std::string(kDataDir) + "/import"};
                for (int i = 0; i < 8; i++)
                    drives.push_back(UsbDrive(i));
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
            if (!menu.UpdateMessage(kCopyKeysTitle, report))
                menu.ShowMessage(kCopyKeysTitle, report);
            set_games_status();
            import_job.reset();
        }
        else if (import_job && import_job->total)
        {
            menu.UpdateMessage(kCopyKeysTitle, fmt::format("Copying the firmware: {} of {} files...",
                                                              import_job->copied.load(), import_job->total.load()));
        }
        if (action.kind == Ui::MenuAction::Kind::Boot && import_job)
            menu.ShowMessage(kCopyKeysTitle, "Still copying: start it again once the copy is done.");
        if (action.kind == Ui::MenuAction::Kind::Boot && !import_job && !copy_job)
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
            if (choice.settings.games_folder != g_games_folder)
            {
                g_games_folder = choice.settings.games_folder;
                Trace("start page: game folder " + (g_games_folder.empty() ? std::string("none") : g_games_folder));
                usb_signature = UsbSignature();
                g_files_changed = true;
            }
            Trace(fmt::format("start page: settings: theme {}, renderer {}, {}, resolution {}%, shader cache {}, "
                              "asynchronous shaders {}, GPU accuracy {}, frame pacing {}, speed {}%, CPU {}, {} cores, "
                              "profiler {}",
                              choice.settings.white_theme ? "white" : "black",
                              choice.settings.software_renderer ? "software" : "vulkan",
                              choice.settings.docked ? "docked" : "handheld", choice.settings.resolution,
                              choice.settings.shader_cache ? "on" : "off", choice.settings.async_shaders ? "on" : "off",
                              choice.settings.gpu_high ? "high" : "normal",
                              choice.settings.unlocked_frames ? "unlocked" : "60 Hz", choice.settings.speed,
                              choice.settings.cpu_unsafe ? "unsafe" : "auto",
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

    // the last runs' files are kept, under other names (04.10: four runs back, so that a test of
    // several boots, say a track each, is all in the log bundle)
    KeepPrevious("/data/eden-ps4/log", ".txt");
    KeepPrevious("/data/eden-ps4/crash", ".log");
    KeepPrevious("/data/eden-ps4/hang", ".log");
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

// What the GPU did since the last call: how busy it was, its submissions a second, and how often
// and how long the CPU waited for it. A slow game whose GPU is busy most of the time is held up by
// the GPU; one whose GPU is mostly idle, with the CPU waiting for each submission, by the waits.
std::string GpuLine(double seconds)
{
    static uint64_t last_busy, last_span, last_submits, last_waits, last_wait_ns;
    uint64_t busy = 0, span = 0, submits = 0, waits = 0, wait_ns = 0;
    ac_ps4_get_gpu_stats(&busy, &span, &submits, &waits, &wait_ns);
    // (a new boot's device starts the counts again)
    if (submits < last_submits || span < last_span)
        last_busy = last_span = last_submits = 0;
    if (waits < last_waits)
        last_waits = last_wait_ns = 0;
    const uint64_t d_busy = busy - last_busy, d_span = span - last_span, d_waits = waits - last_waits;
    const double d_wait_ms = double(wait_ns - last_wait_ns) / 1e6;
    std::string line = fmt::format("the PS4's GPU busy {:.0f}% of the time, {:.0f} submissions a second; the CPU "
                                   "waited for it {:.0f} times a second, {:.1f} ms each, {:.0f}% of the time",
                                   d_span ? 100.0 * double(d_busy) / double(d_span) : 0.0,
                                   double(submits - last_submits) / seconds, double(d_waits) / seconds,
                                   d_waits ? d_wait_ms / double(d_waits) : 0.0, d_wait_ms / (seconds * 10.0));
    // (04.19) Waits of a tenth of a second and more on a GPU busy under 10% of the time: the GPU
    // started this program's work late rather than took long doing it. Seen in the two sessions
    // after one in which the GPU stopped finishing work (Zelda, 04.18): 0.25-0.5 s, then 2 s a
    // wait, 0-2% busy, from the first frames on, translation cache or not
    if (d_waits && d_wait_ms / double(d_waits) >= 100.0 && d_span && double(d_busy) < 0.1 * double(d_span))
        line += " (the GPU started this program's work that late, it didn't take that long doing it: it is "
                "probably still held by an earlier session in which it hung; restart the PS4, not from rest mode)";
    last_busy = busy;
    last_span = span;
    last_submits = submits;
    last_waits = waits;
    last_wait_ns = wait_ns;
    return line;
}

// Where the emulated cores' time went since the last call (common/ps4_perf.h): idle cores woken
// while they spun or after sleeping (and how long that wake-up took), the game's service calls,
// how late the timing thread's events fired, the recompiler's translations and cache flushes, and
// memory accesses that left its inline page table walk
std::string PerfLine(double seconds)
{
    namespace P = Common::PS4Perf;
    struct Totals
    {
        uint64_t spins, sleeps, sleep_ns, ipcs, ipc_ns, events, late_ns, blocks, full, ranges, slow, slow_writes;
        uint64_t gpu_same, gpu_collected, gpu_invalidated;
        uint64_t idle_ns[4], all_idle_ns, presents, present_ns, dequeues, dequeue_ns;
        uint64_t ipc_queue_ns, ipc_handle_ns, ipc_resume_ns, ipc_split, compile_ns;
    };
    static Totals last{};
    uint64_t idle_ns[4], all_idle_ns;
    P::IdleSnapshot(static_cast<uint64_t>(Common::g_wall_clock.GetTimeNS().count()), idle_ns, all_idle_ns);
    const Totals now{P::idle_spin_wakes.load(std::memory_order_relaxed),
                     P::idle_sleep_wakes.count.load(std::memory_order_relaxed),
                     P::idle_sleep_wakes.total_ns.load(std::memory_order_relaxed),
                     P::ipc.count.load(std::memory_order_relaxed),
                     P::ipc.total_ns.load(std::memory_order_relaxed),
                     P::timer_late.count.load(std::memory_order_relaxed),
                     P::timer_late.total_ns.load(std::memory_order_relaxed),
                     P::jit_blocks.load(std::memory_order_relaxed),
                     P::jit_full_flushes.load(std::memory_order_relaxed),
                     P::jit_range_flushes.load(std::memory_order_relaxed),
                     P::SlowMemoryAccesses(),
                     P::Total(P::slow_memory_writes),
                     P::gpu_cached_writes_same.load(std::memory_order_relaxed),
                     P::gpu_cached_writes_collected.load(std::memory_order_relaxed),
                     P::gpu_cached_writes_invalidated.load(std::memory_order_relaxed),
                     {idle_ns[0], idle_ns[1], idle_ns[2], idle_ns[3]},
                     all_idle_ns,
                     P::present_wait.count.load(std::memory_order_relaxed),
                     P::present_wait.total_ns.load(std::memory_order_relaxed),
                     P::dequeue_wait.count.load(std::memory_order_relaxed),
                     P::dequeue_wait.total_ns.load(std::memory_order_relaxed),
                     P::ipc_queue.total_ns.load(std::memory_order_relaxed),
                     P::ipc_handle.total_ns.load(std::memory_order_relaxed),
                     P::ipc_resume.total_ns.load(std::memory_order_relaxed),
                     P::ipc_queue.count.load(std::memory_order_relaxed),
                     P::jit_compile_ns.load(std::memory_order_relaxed)};
    // (the maxima since the last call)
    const uint64_t sleep_max = P::idle_sleep_wakes.max_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t ipc_max = P::ipc.max_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t late_max = P::timer_late.max_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t dequeue_max = P::dequeue_wait.max_ns.exchange(0, std::memory_order_relaxed);
    P::present_wait.max_ns.store(0, std::memory_order_relaxed);
    const auto rate = [&](uint64_t a, uint64_t b) { return double(a - b) / seconds; };
    const auto average_us = [](uint64_t ns, uint64_t count) { return count ? double(ns) / double(count) / 1000.0 : 0.0; };
    std::string line = fmt::format(
        "idle emulated cores woken {:.0f} times a second while spinning, {:.0f} after sleeping ({:.0f} us after the "
        "signal, at most {:.0f}); service calls {:.0f} a second, {:.0f} us each (at most {:.0f}; of that {:.0f} us "
        "until the service's thread took the call, {:.0f} us its handling, {:.0f} us from its answer until the "
        "caller ran again); timer events {:.0f} "
        "a second, {:.0f} us late (at most {:.0f}; timed waits overshoot {:.0f} us); the recompiler translated {:.0f} "
        "blocks a second ({:.1f} ms of the emulated cores' time a second), flushed its whole cache {} times and "
        "parts of it {} times; {:.0f} memory accesses a second "
        "left the inline page table walk ({:.0f} of them writes); writes to memory the GPU caches, a second: "
        "{:.0f} to the page the last one went to, {:.0f} collected for the GPU's own changes, {:.0f} "
        "invalidating its caches",
        rate(now.spins, last.spins), rate(now.sleeps, last.sleeps),
        average_us(now.sleep_ns - last.sleep_ns, now.sleeps - last.sleeps), double(sleep_max) / 1000.0,
        rate(now.ipcs, last.ipcs), average_us(now.ipc_ns - last.ipc_ns, now.ipcs - last.ipcs), double(ipc_max) / 1000.0,
        average_us(now.ipc_queue_ns - last.ipc_queue_ns, now.ipc_split - last.ipc_split),
        average_us(now.ipc_handle_ns - last.ipc_handle_ns, now.ipc_split - last.ipc_split),
        average_us(now.ipc_resume_ns - last.ipc_resume_ns, now.ipc_split - last.ipc_split),
        rate(now.events, last.events), average_us(now.late_ns - last.late_ns, now.events - last.events),
        double(late_max) / 1000.0, double(P::timer_overshoot_ns.load(std::memory_order_relaxed)) / 1000.0,
        rate(now.blocks, last.blocks), double(now.compile_ns - last.compile_ns) / seconds / 1e6,
        now.full - last.full, now.ranges - last.ranges, rate(now.slow, last.slow),
        rate(now.slow_writes, last.slow_writes), rate(now.gpu_same, last.gpu_same),
        rate(now.gpu_collected, last.gpu_collected), rate(now.gpu_invalidated, last.gpu_invalidated));
    // Each emulated core's code cache: what's left of it (a full one is flushed and its code
    // translated again, a stutter), and its whole flushes so far
    {
        std::string caches;
        for (int c = 0; c < 4; c++) {
            const uint32_t kib = P::jit_cache_free_kib[c].load(std::memory_order_relaxed);
            caches += fmt::format("{}{}", c ? ", " : "", kib ? fmt::format("{:.1f}", double(kib) / 1024.0) : std::string("-"));
        }
        line += fmt::format("; the emulated cores' code caches, MiB free: {} (whole flushes so far: {}, {}, {}, {})", caches,
                            P::jit_core_full_flushes[0].load(std::memory_order_relaxed),
                            P::jit_core_full_flushes[1].load(std::memory_order_relaxed),
                            P::jit_core_full_flushes[2].load(std::memory_order_relaxed),
                            P::jit_core_full_flushes[3].load(std::memory_order_relaxed));
        // (04.17) the recompiler per core (the time translating, a second, and the blocks), and
        // the translation cache loaded at boot: blocks used from it so far, and found stale
        static uint64_t last_core_ns[4] = {}, last_core_blocks[4] = {};
        std::string cores;
        for (int c = 0; c < 4; c++) {
            const uint64_t ns = P::jit_core_compile_ns[c].load(std::memory_order_relaxed);
            const uint64_t blocks = P::jit_core_blocks[c].load(std::memory_order_relaxed);
            cores += fmt::format("{}{:.1f} ms ({:.0f} blocks)", c ? ", " : "", double(ns - last_core_ns[c]) / seconds / 1e6,
                                 double(blocks - last_core_blocks[c]) / seconds);
            last_core_ns[c] = ns;
            last_core_blocks[c] = blocks;
        }
        line += fmt::format("; translating by core, a second: {}", cores);
        uint64_t loaded = 0;
        for (int c = 0; c < 4; c++)
            loaded += P::jit_cache_loaded[c].load(std::memory_order_relaxed);
        if (loaded != 0) {
            std::string use;
            for (int c = 0; c < 4; c++)
                use += fmt::format("{}{} of {}, {} stale", c ? "; " : "", P::jit_cache_used[c].load(std::memory_order_relaxed),
                                   P::jit_cache_loaded[c].load(std::memory_order_relaxed),
                                   P::jit_cache_stale[c].load(std::memory_order_relaxed));
            line += fmt::format("; translation cache blocks used so far, by core: {}", use);
        }
        // (04.22) the accesses fastmem was declined for, each compiled to walk the page table
        // from then on (in Bluey a third of the hottest code's accesses)
        std::string walked;
        for (int c = 0; c < 4; c++)
            walked += fmt::format("{}{} ({} through the stack)", c ? ", " : "",
                                  P::jit_walked_sites[c].load(std::memory_order_relaxed),
                                  P::jit_walked_stack_sites[c].load(std::memory_order_relaxed));
        line += fmt::format("; guest accesses walking the page table since fastmem was declined for them, by core: {}",
                            walked);
    }
    // Where the game's time goes besides running: each emulated core idle (no thread of the game
    // to run), all at once (every thread waiting: on the GPU, a vsync, a service), a finished
    // frame waiting for the next vsync to be shown, the game waiting for a buffer to draw in
    const auto percent = [&](uint64_t a, uint64_t b) { return double(a - b) / (seconds * 1e9) * 100.0; };
    line += fmt::format(
        "; emulated cores idle {:.0f}%, {:.0f}%, {:.0f}%, {:.0f}% of the time, all four at once {:.0f}%; a finished "
        "frame waited {:.1f} ms for the next vsync ({:.0f} frames a second); the game asked for a buffer to draw in "
        "{:.0f} times a second, waiting {:.2f} ms each (at most {:.1f})",
        percent(now.idle_ns[0], last.idle_ns[0]), percent(now.idle_ns[1], last.idle_ns[1]),
        percent(now.idle_ns[2], last.idle_ns[2]), percent(now.idle_ns[3], last.idle_ns[3]),
        percent(now.all_idle_ns, last.all_idle_ns),
        average_us(now.present_ns - last.present_ns, now.presents - last.presents) / 1000.0,
        rate(now.presents, last.presents), rate(now.dequeues, last.dequeues),
        average_us(now.dequeue_ns - last.dequeue_ns, now.dequeues - last.dequeues) / 1000.0, double(dequeue_max) / 1e6);
    // The game's system calls by core: what each core's threads spent their time in (a wait's whole
    // length), the four longest a core
    {
        static uint64_t last_count[4][P::SvcIds] = {}, last_ns[4][P::SvcIds] = {};
        static const std::map<uint32_t, const char*> names = {
            {0x01, "SetHeapSize"}, {0x06, "QueryMemory"}, {0x08, "CreateThread"}, {0x09, "StartThread"},
            {0x0B, "SleepThread"}, {0x0D, "SetThreadPriority"}, {0x0F, "SetThreadCoreMask"},
            {0x11, "SignalEvent"}, {0x12, "ClearEvent"}, {0x16, "CloseHandle"}, {0x17, "ResetSignal"},
            {0x18, "WaitSynchronization"}, {0x19, "CancelSynchronization"}, {0x1A, "ArbitrateLock"},
            {0x1B, "ArbitrateUnlock"}, {0x1C, "WaitProcessWideKeyAtomic"}, {0x1D, "SignalProcessWideKey"},
            {0x1E, "GetSystemTick"}, {0x21, "SendSyncRequest"}, {0x22, "SendSyncRequestWithUserBuffer"},
            {0x25, "GetThreadId"}, {0x26, "Break"}, {0x27, "OutputDebugString"}, {0x29, "GetInfo"},
            {0x2C, "MapPhysicalMemory"}, {0x34, "WaitForAddress"}, {0x35, "SignalToAddress"},
            {0x36, "SynchronizePreemptionState"}, {0x40, "CreateSession"}, {0x45, "CreateEvent"}};
        line += "; the game's system calls, the time they took a second (waits included), by core:";
        for (size_t core = 0; core < 3; core++) {
            struct Entry {
                uint32_t id;
                uint64_t count, ns;
            };
            std::vector<Entry> entries;
            for (uint32_t id = 0; id < P::SvcIds; id++) {
                const uint64_t count = P::svc_cores[core].count[id].load(std::memory_order_relaxed);
                const uint64_t ns = P::svc_cores[core].total_ns[id].load(std::memory_order_relaxed);
                if (count > last_count[core][id])
                    entries.push_back({id, count - last_count[core][id], ns - last_ns[core][id]});
                last_count[core][id] = count;
                last_ns[core][id] = ns;
            }
            std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.ns > b.ns; });
            line += fmt::format("{} core {}:", core ? ";" : "", core);
            for (size_t i = 0; i < std::min<size_t>(entries.size(), 4); i++) {
                const auto name = names.find(entries[i].id);
                line += fmt::format("{} {} {:.1f} ms ({:.0f} calls)", i ? "," : "",
                                    name != names.end() ? std::string(name->second) : fmt::format("svc {:#x}", entries[i].id),
                                    double(entries[i].ns) / 1e6 / seconds, double(entries[i].count) / seconds);
            }
        }
    }
    // The game's busiest threads: each one's share of the period on its core, and in its waits
    // (which threads a frame's time goes through, and what each waits on). A slot whose thread did
    // nothing in the period is freed (a thread that has gone, or sleeps).
    {
        constexpr size_t kWaits = size_t(P::ThreadWait::Count);
        struct Last {
            uint64_t id = 0, run = 0, wait[kWaits] = {};
        };
        static Last last[P::MaxGuestThreads];
        struct Row {
            uint64_t id, run, wait[kWaits];
            uint32_t core;
            int32_t priority;
        };
        std::vector<Row> rows;
        for (size_t i = 0; i < P::MaxGuestThreads; i++) {
            P::GuestThread& t = P::guest_threads[i];
            const uint64_t id = t.id.load(std::memory_order_acquire);
            if (id == 0)
                continue;
            if (last[i].id != id)
                last[i] = {id};
            Row row{id, t.run_ns.load(std::memory_order_relaxed) - last[i].run, {}, t.core.load(std::memory_order_relaxed),
                    t.priority.load(std::memory_order_relaxed)};
            last[i].run += row.run;
            bool active = row.run != 0;
            for (size_t w = 0; w < kWaits; w++) {
                row.wait[w] = t.wait_ns[w].load(std::memory_order_relaxed) - last[i].wait[w];
                last[i].wait[w] += row.wait[w];
                active |= row.wait[w] != 0;
            }
            if (!active) {
                t.id.store(0, std::memory_order_release);
                last[i] = {};
                continue;
            }
            rows.push_back(row);
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.run > b.run; });
        const auto share = [&](uint64_t ns) { return 100.0 * double(ns) / (seconds * 1e9); };
        line += "; the busiest threads (on core, priority: running; waiting in condition variables, locks, on "
                "objects, sleeps, service calls, addresses, in % of the time):";
        for (size_t i = 0; i < std::min<size_t>(rows.size(), 8); i++) {
            const Row& r = rows[i];
            line += fmt::format("{} #{} core {} p{}: {:.0f}; {:.0f}, {:.0f}, {:.0f}, {:.0f}, {:.0f}, {:.0f}", i ? "," : "",
                                r.id, r.core, r.priority, share(r.run), share(r.wait[0]), share(r.wait[1]),
                                share(r.wait[2]), share(r.wait[3]), share(r.wait[4]), share(r.wait[5]));
        }
    }
    // The kernel's timer, of the timer events (guest threads' sleeps and timed waits ending: each
    // can interrupt an emulated core)
    {
        static uint64_t last_kernel_timer = 0;
        const uint64_t kernel_timer = P::kernel_timer_tasks.load(std::memory_order_relaxed);
        line += fmt::format("; of the timer events, the kernel's timer (the game's threads' sleeps and timeouts "
                            "ending) {:.0f} a second",
                            double(kernel_timer - last_kernel_timer) / seconds);
        last_kernel_timer = kernel_timer;
    }
    // The host locks the emulated cores take (04.06: tried again for up to 20 us before sleeping):
    // how often each was found taken, and whether its waiter got it spinning or slept on it
    {
        struct LockTotals {
            uint64_t spun, spun_ns, slept, slept_ns;
        };
        const auto take = [](const P::LockStats& l) {
            return LockTotals{l.spun.load(std::memory_order_relaxed), l.spun_ns.load(std::memory_order_relaxed),
                              l.slept.load(std::memory_order_relaxed), l.slept_ns.load(std::memory_order_relaxed)};
        };
        static LockTotals last_locks[5] = {};
        static uint64_t last_wakes = 0, last_skipped = 0;
        const P::LockStats* locks[5] = {&P::lock_scheduler, &P::lock_timer, &P::lock_kernel_other,
                                        &P::lock_core_context, &P::lock_timing};
        const char* names[5] = {"scheduler", "timer", "other kernel", "cores' contexts", "timing"};
        line += "; host locks found taken, a second (got by spinning, us each / slept on, us each):";
        for (size_t i = 0; i < 5; i++) {
            const LockTotals t = take(*locks[i]);
            const LockTotals& l = last_locks[i];
            line += fmt::format("{} {} {:.0f}, {:.1f} / {:.0f}, {:.0f}", i ? "," : "", names[i],
                                double(t.spun - l.spun) / seconds, average_us(t.spun_ns - l.spun_ns, t.spun - l.spun),
                                double(t.slept - l.slept) / seconds,
                                average_us(t.slept_ns - l.slept_ns, t.slept - l.slept));
            last_locks[i] = t;
        }
        const uint64_t wakes = P::timing_wakes.load(std::memory_order_relaxed);
        const uint64_t skipped = P::timing_wakes_skipped.load(std::memory_order_relaxed);
        line += fmt::format("; the timing thread woken {:.0f} a second for new events, {:.0f} not (later ones)",
                            double(wakes - last_wakes) / seconds, double(skipped - last_skipped) / seconds);
        last_wakes = wakes;
        last_skipped = skipped;
    }
    // Eden's GPU thread's load (04.08): draws, command words, and those in runs of plain register
    // writes (one loop, DmaPusher::SinkRun)
    {
        static uint64_t last_draws = 0, last_words = 0, last_run_words = 0;
        const uint64_t draws = P::gpu_thread.draws.load(std::memory_order_relaxed);
        const uint64_t words = P::gpu_thread.command_words.load(std::memory_order_relaxed);
        const uint64_t run_words = P::gpu_thread.sink_run_words.load(std::memory_order_relaxed);
        line += fmt::format("; the GPU thread: {:.0f} draws a second, {:.0f} command words a second ({:.0f}% of them "
                            "register writes in runs, one loop each)",
                            double(draws - last_draws) / seconds, double(words - last_words) / seconds,
                            words > last_words ? 100.0 * double(run_words - last_run_words) / double(words - last_words)
                                               : 0.0);
        last_draws = draws;
        last_words = words;
        last_run_words = run_words;
        // (04.09) what was new to it: what a race's first seconds wait on
        static uint64_t last_new = 0, last_skipped = 0, last_images = 0, last_buffers = 0;
        const uint64_t pipelines = P::gpu_pipelines_new.load(std::memory_order_relaxed);
        const uint64_t skipped = P::gpu_draws_skipped.load(std::memory_order_relaxed);
        const uint64_t images = P::gpu_images_created.load(std::memory_order_relaxed);
        const uint64_t buffers = P::gpu_buffers_created.load(std::memory_order_relaxed);
        line += fmt::format("; new to it, a second: {:.1f} graphics pipelines not in the shader cache, {:.0f} draws "
                            "skipped while theirs built, {:.1f} images, {:.1f} buffers",
                            double(pipelines - last_new) / seconds, double(skipped - last_skipped) / seconds,
                            double(images - last_images) / seconds, double(buffers - last_buffers) / seconds);
        last_new = pipelines;
        last_skipped = skipped;
        last_images = images;
        last_buffers = buffers;
    }
    // (04.12) Uniform buffers (docs/ryujinx-comparison.md, 1): constant buffer data written; binds
    // copied every draw against cached; frames copying. (04.14: what memory already held no longer
    // compared)
    {
        const P::GpuThreadCounters& g = P::gpu_thread;
        static uint64_t last[7] = {};
        const uint64_t now[7] = {g.cb_bytes.load(),   g.ubo_copied.load(),      g.ubo_copied_bytes.load(),
                                 g.ubo_cached.load(), g.ubo_cached_hits.load(), g.ubo_frames.load(),
                                 g.ubo_frames_copying.load()};
        uint64_t d[7];
        for (int i = 0; i < 7; i++)
            d[i] = now[i] - last[i];
        const auto pct = [](uint64_t part, uint64_t whole) { return whole ? 100.0 * double(part) / double(whole) : 0.0; };
        line += fmt::format("; uniform buffers: constant buffer data written {:.0f} KiB a second; binds a second: "
                            "{:.0f} copied ({:.0f} KiB), {:.0f} from the cache ({:.0f}% with nothing to upload); "
                            "frames copying every draw {:.0f}%",
                            double(d[0]) / 1024.0 / seconds, double(d[1]) / seconds, double(d[2]) / 1024.0 / seconds,
                            double(d[3]) / seconds, pct(d[4], d[3]), pct(d[6], d[5]));
        std::copy(std::begin(now), std::end(now), std::begin(last));
    }
    // (04.15) Graphics pipeline lookups that reused the last one (none of the key's registers, the
    // topology, the shaders changed), the reuses checked by a refresh, and those found different
    {
        const P::GpuThreadCounters& g = P::gpu_thread;
        static uint64_t last[4] = {};
        const uint64_t now[4] = {g.pipeline_lookups.load(), g.pipeline_reuses.load(), g.pipeline_reuse_checks.load(),
                                 g.pipeline_reuse_mismatches.load()};
        line += fmt::format("; graphics pipeline lookups: {:.0f} a second, {:.0f}% the last one again with its key "
                            "unchanged (not refreshed); {} of those checked so far, {} found changed{}",
                            double(now[0] - last[0]) / seconds,
                            now[0] > last[0] ? 100.0 * double(now[1] - last[1]) / double(now[0] - last[0]) : 0.0,
                            now[2], now[3], now[3] != 0 ? " (reuse turned off)" : "");
        std::copy(std::begin(now), std::end(now), std::begin(last));
        // (04.16) texture/sampler descriptors read straight from the pool in host memory
        static uint64_t last_reads = 0, last_in_place = 0;
        const uint64_t reads = g.descriptor_reads.load(), in_place = g.descriptor_reads_in_place.load();
        line += fmt::format("; texture descriptors read {:.0f} a second, {:.0f}% in place",
                            double(reads - last_reads) / seconds,
                            reads > last_reads ? 100.0 * double(in_place - last_in_place) / double(reads - last_reads)
                                               : 0.0);
        last_reads = reads;
        last_in_place = in_place;
        // (04.17) uniform buffer copies sampled: the slot's next copy the same range with the same
        // bytes (could have been reused), changed, another range
        static uint64_t last_ubo[4] = {};
        const uint64_t ubo[4] = {g.ubo_checks.load(), g.ubo_same.load(), g.ubo_changed.load(), g.ubo_moved.load()};
        const uint64_t checks = ubo[0] - last_ubo[0];
        if (checks != 0)
            line += fmt::format("; uniform buffer copies compared with the slot's next: {}: {:.0f}% the same bytes, "
                                "{:.0f}% changed, {:.0f}% another range",
                                checks, 100.0 * double(ubo[1] - last_ubo[1]) / double(checks),
                                100.0 * double(ubo[2] - last_ubo[2]) / double(checks),
                                100.0 * double(ubo[3] - last_ubo[3]) / double(checks));
        std::copy(std::begin(ubo), std::end(ubo), std::begin(last_ubo));
    }
    // (04.17) Pipelines built while the game ran (not at boot): Eden's translation of their shaders
    // to SPIR-V, the driver's creation, and how many it found in its pipeline cache
    {
        const P::PipelineWork& w = P::pipelines_in_play;
        static uint64_t last[7] = {};
        const uint64_t now[7] = {w.translated.load(), w.translate_us.load(), w.created.load(), w.create_us.load(),
                                 w.reported.load(), w.hits.load(), w.quick.load()};
        if (now[2] != last[2] || now[0] != last[0])
            line += fmt::format("; pipelines built while it ran: {} (their shaders translated {:.0f} ms, the driver's "
                                "creation {:.0f} ms; in the driver's cache {} of {} reported; under 2 ms {})",
                                now[2] - last[2], double(now[1] - last[1]) / 1000.0, double(now[3] - last[3]) / 1000.0,
                                now[5] - last[5], now[4] - last[4], now[6] - last[6]);
        std::copy(std::begin(now), std::end(now), std::begin(last));
    }
    // (04.11) The game's waits on objects (svcWaitSynchronization), by what ended them: an event by
    // its service's name, one the game made, a thread, a timeout. Per core, the four longest.
    {
        static uint64_t last_count[4][P::WaitTable::Max] = {}, last_ns[4][P::WaitTable::Max] = {};
        line += "; the game's waits on objects, by what ended them (a second, ms each):";
        for (size_t core = 0; core < 3; core++)
        {
            const P::WaitTable& table = P::object_waits[core];
            const size_t used = std::min(table.used.load(std::memory_order_acquire), P::WaitTable::Max);
            struct Item
            {
                const char* label;
                uint64_t count, ns;
            };
            std::vector<Item> items;
            for (size_t i = 0; i < used; i++)
            {
                const uint64_t count = table.entries[i].count.load(std::memory_order_relaxed);
                const uint64_t ns = table.entries[i].total_ns.load(std::memory_order_relaxed);
                if (count > last_count[core][i])
                    items.push_back({table.entries[i].label, count - last_count[core][i], ns - last_ns[core][i]});
                last_count[core][i] = count;
                last_ns[core][i] = ns;
            }
            std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.ns > y.ns; });
            line += fmt::format("{} core {}:", core ? ";" : "", core);
            if (items.empty())
                line += " none";
            for (size_t i = 0; i < std::min<size_t>(items.size(), 4); i++)
                line += fmt::format("{} {} {:.1f}, {:.2f}", i ? "," : "", items[i].label,
                                    double(items[i].count) / seconds, average_us(items[i].ns, items[i].count) / 1000.0);
        }
    }
    // (04.10) Vulkan render pass ends skipped: none begun in their command buffer (RADV crashed on
    // one, once)
    if (const uint64_t unmatched = P::vk_unmatched_render_pass_ends.load(std::memory_order_relaxed); unmatched != 0)
        line += fmt::format("; Vulkan render pass ends without a beginning, skipped so far: {}", unmatched);
    // (04.13) framebuffer removals from another thread (an nvdrv unmap), left to the GPU thread
    if (const uint64_t handed = P::framebuffer_removals_handed_over.load(std::memory_order_relaxed); handed != 0)
        line += fmt::format("; framebuffer removals from other threads, left to the GPU thread so far: {}", handed);
    // The services' host threads waiting for their next request (04.07: a longer spin, to 250 us,
    // after a short wait): woken while spinning, or after sleeping
    {
        static uint64_t last_spin = 0, last_long = 0, last_sleep = 0, last_sleep_ns = 0;
        const uint64_t spin = P::service_spin_wakes.load(std::memory_order_relaxed);
        const uint64_t long_spins = P::service_long_spins.load(std::memory_order_relaxed);
        const uint64_t sleep = P::service_sleep_wakes.count.load(std::memory_order_relaxed);
        const uint64_t sleep_ns = P::service_sleep_wakes.total_ns.load(std::memory_order_relaxed);
        const uint64_t sleep_max = P::service_sleep_wakes.max_ns.exchange(0, std::memory_order_relaxed);
        line += fmt::format("; the services' threads woken {:.0f} times a second while spinning, {:.0f} after "
                            "sleeping ({:.0f} us after the signal, at most {:.0f}); {:.0f} of their waits a second "
                            "spun longer (a short wait before)",
                            double(spin - last_spin) / seconds, double(sleep - last_sleep) / seconds,
                            average_us(sleep_ns - last_sleep_ns, sleep - last_sleep), double(sleep_max) / 1000.0,
                            double(long_spins - last_long) / seconds);
        last_spin = spin;
        last_long = long_spins;
        last_sleep = sleep;
        last_sleep_ns = sleep_ns;
    }
    // Fastmem: the arena's pages mapped, and its faults (resolved: a page mapped; declined: the
    // recompiler's fallback took the access, and the code there was recompiled without fastmem)
    {
        static uint64_t last_resolved = 0, last_declined = 0;
        static size_t last_why[4] = {};
        const uint64_t resolved = Common::Orbis::FastmemFaultsResolved(), declined = Common::Orbis::FastmemFaultsDeclined();
        size_t why[4];
        Common::Orbis::FastmemDeclineReasons(why[0], why[1], why[2], why[3]);
        const auto per_second = [&](size_t now, size_t before) { return double(now - before) / seconds; };
        if (Common::Orbis::FastmemArenaBits() != 0) {
            line += fmt::format("; fastmem (a 2^{} arena at {:#x}): {} MiB of the game's memory mapped at its own "
                                "addresses, {:.0f} faults a second resolved, {:.0f} declined (not plain memory {:.0f}, "
                                "writes to memory the GPU caches {:.0f}, pages not back to back {:.0f}, other {:.0f})",
                                Common::Orbis::FastmemArenaBits(), Common::Orbis::FastmemArenaBase(),
                                Common::Orbis::FastmemMappedBytes() >> 20,
                                double(resolved - last_resolved) / seconds, double(declined - last_declined) / seconds,
                                per_second(why[0], last_why[0]), per_second(why[1], last_why[1]),
                                per_second(why[2], last_why[2]), per_second(why[3], last_why[3]));
            // the game's aliased memory (thread stacks) lined up with the PS4's pages for it
            const uint64_t aliases = P::aliases.load(std::memory_order_relaxed);
            if (aliases != 0) {
                line += fmt::format("; stacks and other aliased memory: {} so far, {} moved to line up with the "
                                    "PS4's 16 KiB pages, {} left as they were (too large, or no memory); spare pages "
                                    "beside them in {} 16 KiB pages, mapped {} times",
                                    aliases, P::aliases_moved.load(std::memory_order_relaxed),
                                    P::aliases_not_moved.load(std::memory_order_relaxed),
                                    Common::Orbis::ArenaPaddingCount(), Common::Orbis::ArenaPaddedMaps());
            }
        } else {
            line += "; fastmem: off";
        }
        last_resolved = resolved;
        last_declined = declined;
        std::copy(std::begin(why), std::end(why), std::begin(last_why));
    }
    // The services the game's calls spent their time in, the five busiest
    P::ServiceCall calls[P::MaxServiceCalls];
    const size_t n = P::TakeServiceCalls(calls, P::MaxServiceCalls);
    std::sort(calls, calls + n, [](const P::ServiceCall& a, const P::ServiceCall& b) { return a.total_ns > b.total_ns; });
    line += "; the services' own handling of the calls, the five busiest:";
    for (size_t i = 0; i < std::min<size_t>(n, 5) && calls[i].count; i++) {
        line += fmt::format("{} {} command {}: {:.0f} a second, {:.0f} us each (at most {:.0f}; {:.0f} us before its "
                            "thread took it)",
                            i ? "," : "", calls[i].name ? calls[i].name : "(others)",
                            calls[i].command > 0xFFFF ? fmt::format("{:#x}", calls[i].command)
                                                      : std::to_string(calls[i].command),
                            double(calls[i].count) / seconds, average_us(calls[i].total_ns, calls[i].count),
                            double(calls[i].max_ns) / 1000.0, average_us(calls[i].queue_ns, calls[i].count));
    }
    last = now;
    return line;
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
        int seconds = 0, stale = 0, shutdown_seconds = 0;
        bool dumped = false;
        uint64_t reported = base;
        while (!stop)
        {
            for (int i = 0; i < 10 && !stop; i++)
                usleep(100 * 1000);
            seconds++;
            const uint64_t frames = Frames();
            // (04.09) A program's shutdown that doesn't end: its threads in hang.log, then the app
            // starts again (as after any game: a fresh process, the start page), instead of staying
            // frozen. (A shutdown takes a few seconds; 04.08's once stopped for good, at a step no
            // report named.)
            if (Common::PS4Diagnostics::shutting_down.load(std::memory_order_relaxed))
            {
                if (++shutdown_seconds == kShutdownLimit)
                {
                    char reason[256];
                    snprintf(reason, sizeof(reason), "%s: the shutdown stuck for %d s, at: %s (the GPU display: %s)",
                             name.c_str(), shutdown_seconds, Common::PS4Diagnostics::shutdown_stage.load(),
                             Common::PS4Diagnostics::stage.load());
                    ps4_dump_thread_stacks(kHangFile, reason);
                    Trace(std::string("hang watchdog: ") + reason + ": thread stacks in hang.log; restarting the app");
                    const int restart = sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
                    Trace(fmt::format("restart refused ({:#x}): closing the app", uint32_t(restart)));
                    sceSystemServiceLoadExec("exit", nullptr);
                    _exit(0);
                }
            }
            else
            {
                shutdown_seconds = 0;
            }
            if (seconds % 30 == 0)
            {
                // how it runs: the frame rate, and the memory (what a game needs, on the console)
                Trace(fmt::format("running {} s: {:.1f} frames a second; {}; {}", seconds,
                                  double(frames - reported) / 30.0, GpuLine(30.0), MemoryLine()));
                TraceLong(fmt::format("running {} s: ", seconds), PerfLine(30.0));
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
            if (started && Common::PS4Diagnostics::shutting_down.load())
                snprintf(reason, sizeof(reason), "%s: no new frame for %d s, %d s after booting, %llu frames so far, "
                         "shutting down, at: %s (the GPU display: %s)", name.c_str(), stale, seconds,
                         (unsigned long long)(frames - base), Common::PS4Diagnostics::shutdown_stage.load(), stage);
            else if (started)
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

    static constexpr int kShutdownLimit = 30;
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
    KeepPrevious(std::string(kDataDir) + "/gpu/radv-ps4", ".log");
    // Eden's Vulkan renderer on the PS4's GPU when Mesa's RADV is linked, unless System Settings
    // (or settings.txt, EDEN_PS4_RENDERER=null) chose the software renderer, or Vulkan didn't
    // start this session
    const char* renderer = std::getenv("EDEN_PS4_RENDERER");
    const bool radv = vk_icdGetInstanceProcAddr(nullptr, "vkCreateInstance") != nullptr;
    const bool forced_null = renderer && std::string_view{renderer} == "null";
    bool vulkan_failed = false;
    Ui::MenuSettings settings = LoadMenuSettings();
    // where Import (Games and Keys) looks first: filled from a USB drive with a file manager
    std::error_code import_ec;
    std::filesystem::create_directories(std::string(kDataDir) + "/import", import_ec);
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
        Launcher::SetJitCache(settings.jit_cache);
        Launcher::SetDrawShortcuts(settings.draw_shortcuts);
        Launcher::SetAsyncShaders(settings.async_shaders);
        Launcher::SetCpuUnsafe(settings.cpu_unsafe);
        Launcher::SetGpuAccuracyHigh(settings.gpu_high);
        Launcher::SetFpsOverlay(settings.fps_overlay);
        Launcher::SetUnlockedFrames(settings.unlocked_frames);
        Launcher::SetSpeedPercent(settings.speed);
        {
            // keys and firmware on a USB drive are used from there, else the user folder's
            const Launcher::UsbKeysAndFirmware usb = KeysAndFirmwareOnUsb();
            Launcher::SetKeysAndFirmware(usb.keys_folder, usb.firmware_folder);
            Trace(fmt::format("keys: {}; firmware: {}",
                              usb.keys_folder.empty() ? std::string("the user folder's")
                                                      : usb.keys_folder + (usb.title_keys ? " (with title.keys)" : ""),
                              usb.firmware_folder.empty()
                                  ? std::string("the user folder's")
                                  : fmt::format("{} ({} files)", usb.firmware_folder, usb.firmware_files)));
        }
        ps4_set_app_cores(settings.seventh_core ? 0x7F : 0x3F);
        // read by Eden's HostMemory as it maps the Switch's RAM (common/host_memory.cpp)
        setenv("EDEN_PS4_DRAM_ON_DEMAND", settings.ram_on_demand ? "1" : "0", 1);
        // read by the runtime as Eden's threads name themselves (port/ps4_runtime.cpp, ps4_place_thread)
        setenv("EDEN_PS4_PIN_THREADS", settings.pin_threads ? "1" : "0", 1);
        // read by Eden's HostMemory as it maps the Switch's RAM (common/host_memory.cpp)
        setenv("EDEN_PS4_FASTMEM", settings.fastmem ? "1" : "0", 1);
        NoteRecent(program);
        const std::string setup = fmt::format(
            "renderer: {}; console mode: {}; resolution {}%; shader cache {}, translation cache {}, asynchronous shaders {}, "
            "GPU accuracy {}, frame pacing {}, game speed {}%, draw shortcuts {}; CPU {}, {} cores, threads {}, fastmem {}; "
            "the Switch's RAM {}{}",
            g_renderer_label, settings.docked ? "docked" : "handheld", settings.resolution,
            settings.shader_cache ? "on" : "off", settings.jit_cache ? "on" : "off", settings.async_shaders ? "on" : "off",
            settings.gpu_high ? "high" : "normal", settings.unlocked_frames ? "unlocked" : "60 Hz", settings.speed,
            settings.draw_shortcuts ? "on" : "off", settings.cpu_unsafe ? "unsafe" : "auto", settings.seventh_core ? 7 : 6,
            settings.pin_threads ? "pinned" : "free", settings.fastmem ? "on" : "off",
            settings.ram_on_demand ? "on demand" : "all up front",
            settings.profiler ? "; profiler on" : "");
        Trace(setup);
        dynarmic_ps4_block_map_enable(settings.profiler ? 1 : 0);
        if (settings.profiler)
        {
            ps4_sampler_set_enabled(1);
            // the build that recorded it: the log bundle names its addresses only with its own symbols
            ps4_sampler_note(fmt::format("[build {}] {} ({})", EDEN_PS4_VERSION, program, setup).c_str());
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
        // A fresh process for the next game: the C heap never gives back what a game took (after
        // Mario Kart 8 Deluxe it held 1.1 GiB, 72 MiB of it in use, and Captain Toad started with
        // 1.8 of the 4.6 GiB of direct memory and 37 MiB of flexible memory, and was closed by the
        // system). (Not after a game that didn't load: the start page says why.)
        if (result.status != Launcher::BootResult::Status::LoadFailed)
        {
            Trace("restarting the app: a fresh process, with all its memory, for the next game");
            const int restart = sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
            Trace(fmt::format("restart refused ({:#x}): carrying on in this process", uint32_t(restart)));
        }
    }
    return nullptr;
}

} // namespace

// Can the PS4 map the same direct memory at two addresses for this app? Fastmem (Eden's recompiler
// reading and writing guest memory without walking a page table) needs the emulated RAM mapped
// twice. Tried once with 64 KiB: both mappings, a word written through one and read through the
// other; the boot trace says what happened. Nothing else depends on it yet.
bool g_dmem_aliasing = false; // (ProbeDoubleMapping: the same direct memory maps at two addresses)

// Whether the PS4 lets this app map the same direct memory at two addresses, which fastmem needs
// (the Switch's memory at its own addresses, beside Eden's view of it): plainly, which it refused
// in 03.79 (EBUSY), and after libkernel's sceKernelEnableDmemAliasing / ...2 (exported, not in
// the SDK's headers) and through sceKernelMapDirectMemory2. Each read and write test runs under a
// temporary fault guard, so a mapping that isn't what it seems is reported, not a crash. Aliasing,
// once enabled, stays enabled (fastmem will want it).
extern "C" int32_t sceKernelEnableDmemAliasing(void);
extern "C" int32_t sceKernelEnableDmemAliasing2(void);
extern "C" int32_t sceKernelMapDirectMemory2(void** addr, size_t len, int32_t type, int32_t prot, int32_t flags, off_t phys,
                                            size_t alignment);

namespace
{
sigjmp_buf g_probe_jump;
volatile sig_atomic_t g_probe_guarded = 0;

void ProbeFault(int sig, siginfo_t*, void*)
{
    if (g_probe_guarded)
        siglongjmp(g_probe_jump, sig);
    signal(sig, SIG_DFL);
    raise(sig);
}

// The same memory at a and b? (the guard catches a fault, as "faulted")
std::string SameMemory(void* a, void* b)
{
    volatile uint32_t* x = static_cast<volatile uint32_t*>(a);
    volatile uint32_t* y = static_cast<volatile uint32_t*>(b);
    const int sig = sigsetjmp(g_probe_jump, 1);
    if (sig != 0)
    {
        g_probe_guarded = 0;
        return fmt::format("a read or write faulted (signal {})", sig);
    }
    g_probe_guarded = 1;
    x[100] = 0x5EEDF00Du;
    const uint32_t seen = y[100];
    y[(1 << 20) / 4] = 0x0DDBA11u; // (a megabyte in)
    const uint32_t back = x[(1 << 20) / 4];
    g_probe_guarded = 0;
    return seen == 0x5EEDF00Du && back == 0x0DDBA11u ? "yes, the same memory at two addresses"
                                                     : fmt::format("mapped twice, but not the same memory "
                                                                   "(read back {:#x}, {:#x})", seen, back);
}

// Maps phys a second time (map2: through sceKernelMapDirectMemory2) beside `first`: the outcome
std::string SecondMapping(void* first, off_t phys, size_t size, bool map2)
{
    void* second = nullptr;
    const int32_t r = map2 ? sceKernelMapDirectMemory2(&second, size, 0, 0x03, 0, phys, size)
                           : sceKernelMapDirectMemory(&second, size, 0x03, 0, phys, size);
    if (r != 0)
        return fmt::format("refused ({:#x})", uint32_t(r));
    std::string result = SameMemory(first, second) + fmt::format(" (at {} and {})", first, second);
    sceKernelMunmap(second, size);
    return result;
}
} // namespace

static void ProbeDoubleMapping()
{
    constexpr size_t kSize = 2 * 1024 * 1024; // (the on-demand DRAM's chunk, a large page)
    off_t phys = 0;
    int32_t r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), kSize, kSize, 0, &phys);
    if (r != 0)
    {
        Trace(fmt::format("double mapping: allocating direct memory failed ({:#x})", uint32_t(r)));
        return;
    }
    void* first = nullptr;
    r = sceKernelMapDirectMemory(&first, kSize, 0x03, 0, phys, kSize);
    if (r != 0)
    {
        Trace(fmt::format("double mapping: the first mapping failed ({:#x})", uint32_t(r)));
        sceKernelReleaseDirectMemory(phys, kSize);
        return;
    }
    struct sigaction guard {}, old_segv {}, old_bus {};
    guard.sa_sigaction = ProbeFault;
    guard.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&guard.sa_mask);
    sigaction(SIGSEGV, &guard, &old_segv);
    sigaction(SIGBUS, &guard, &old_bus);

    std::string report = "plainly: " + SecondMapping(first, phys, kSize, false);
    bool done = report.find("yes,") != std::string::npos;
    if (!done)
    {
        const int32_t e1 = sceKernelEnableDmemAliasing();
        const std::string again = SecondMapping(first, phys, kSize, false);
        report += fmt::format("; after sceKernelEnableDmemAliasing ({:#x}): {}", uint32_t(e1), again);
        done = again.find("yes,") != std::string::npos;
    }
    if (!done)
    {
        const int32_t e2 = sceKernelEnableDmemAliasing2();
        const std::string again = SecondMapping(first, phys, kSize, false);
        report += fmt::format("; after sceKernelEnableDmemAliasing2 ({:#x}): {}", uint32_t(e2), again);
        done = again.find("yes,") != std::string::npos;
    }
    if (!done)
        report += "; through sceKernelMapDirectMemory2: " + SecondMapping(first, phys, kSize, true);

    sigaction(SIGSEGV, &old_segv, nullptr);
    sigaction(SIGBUS, &old_bus, nullptr);
    sceKernelMunmap(first, kSize);
    sceKernelReleaseDirectMemory(phys, kSize);
    g_dmem_aliasing = report.find("yes,") != std::string::npos;
    Trace(fmt::format("double mapping: {} -> {}", report,
                      g_dmem_aliasing ? "fastmem is possible" : "no way found yet"));
}

int main()
{
    mkdir(kDataDir, 0777);
    Trace("started");
    ProbeDoubleMapping();
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
