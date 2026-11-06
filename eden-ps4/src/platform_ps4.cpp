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
#include <cmath>
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
#include <fcntl.h>
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
#include "common/ps4_game_state.h"
#include "common/ps4_perf.h"
#include "launcher.h"
#include "ui/avatars.h"
#include "ui/canvas.h"
#include "ui/home_menu.h"
#include "ui/loading_screen.h"
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
// port/ps4_crashlog.cpp (04.41): the boot trace synced within `ms` of each line, by a thread
void ps4_boot_trace_sync_every(int ms);
// Mesa's PS4 winsys (port/mesa/ac_ps4_drm.c; ps4_stubs.cpp without it): the GPU buffers' memory,
// and the GPU's statistics since it started (busy and span in GPU clock ticks, submissions, and
// the CPU's waits for the GPU's work: how many, and how long in all)
uint64_t ac_ps4_gpu_bytes();
void ac_ps4_get_gpu_stats(uint64_t* busy_ticks, uint64_t* span_ticks, uint64_t* submits, uint64_t* waits,
                          uint64_t* wait_ns);
// (04.40) the GPU's progress: submissions made and finished, RADV's last trace id, and how long the
// GPU has gone without finishing anything while work was waiting (0: it hasn't, or it's idle)
void ac_ps4_gpu_progress(uint64_t* submitted, uint64_t* completed, uint32_t* trace_id, int64_t* stalled_ms);
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
// (04.59) The touchpad's coordinate range (the DualShock 4's 1920x943 unless the pad says otherwise),
// and the HOME menu's Touchpad setting: true, its click is the Minus button (as before 04.59); false,
// the Switch's touch screen
float g_touch_max_x = 1919.0f, g_touch_max_y = 942.0f;
std::atomic<bool> g_touchpad_minus{false};
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
    if (g_pad >= 0)
    {
        OrbisPadInformation info{};
        if (scePadGetControllerInformation(g_pad, &info) == 0 && info.touchResolutionX > 1 && info.touchResolutionY > 1)
        {
            g_touch_max_x = float(info.touchResolutionX - 1);
            g_touch_max_y = float(info.touchResolutionY - 1);
        }
        Trace(fmt::format("pad: touchpad {}x{}", g_touch_max_x + 1, g_touch_max_y + 1));
        // (04.64) the motion sensors on (the header declares it without its parameters: the handle and
        // whether they're on)
        using SetMotionSensorState = int32_t (*)(int32_t handle, bool enable);
        const int32_t motion = reinterpret_cast<SetMotionSensorState>(&scePadSetMotionSensorState)(g_pad, true);
        Trace(fmt::format("pad: scePadSetMotionSensorState = {:#x}", uint32_t(motion)));
    }
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
// Circle B, Square Y, Triangle X; L1/R1 L/R, L2/R2 ZL/ZR, OPTIONS +, the touchpad's click -
// (04.59: with the touch screen on the touchpad, a click with two fingers; one finger touches).
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
    const bool touch_click = !g_touchpad_minus.load(std::memory_order_relaxed) && pad.touch.fingers == 1;
    for (const auto& [ps4, nx] : map)
        if ((pad.buttons & ps4) && !(ps4 == ORBIS_PAD_BUTTON_TOUCH_PAD && touch_click))
            state.buttons |= nx;
    const auto axis = [](uint8_t v) { return std::clamp((int(v) - 128) / 127.0f, -1.0f, 1.0f); };
    state.lx = axis(pad.leftStick.x);
    state.ly = -axis(pad.leftStick.y);
    state.rx = axis(pad.rightStick.x);
    state.ry = -axis(pad.rightStick.y);
    return state;
}

// (04.64) The controller's motion sensors, as Eden's SDL driver turns SDL's into its own: the PS4's
// axes are SDL's for a controller held in front of you (x to the right, y up out of its face, z
// towards you), the acceleration in G and the angular velocity in radians a second; Eden's are
// turns a second and G, its y and z SDL's z and y. Which of the two vectors after the orientation
// is the accelerometer isn't left to the header's names: at rest it reads about 1 G and the other
// about nothing, so the first 40 samples that say so decide (and the boot trace says which).
struct MotionReader
{
    uint64_t last_timestamp = 0;
    float last[6] = {};
    double last_time = 0;
    int votes_first = 0, votes_second = 0;
    int accel = -1; // -1: not known yet; 0: the first vector (the SDK's order), 1: the second
};
bool ReadMotion(const OrbisPadData& pad, MotionReader& reader, Launcher::MotionState& out)
{
    const float v1[3] = {pad.vel.x, pad.vel.y, pad.vel.z}, v2[3] = {pad.acell.x, pad.acell.y, pad.acell.z};
    // the same sample as the last poll's (its time and readings unchanged)
    if (pad.timestamp == reader.last_timestamp && std::equal(v1, v1 + 3, reader.last) &&
        std::equal(v2, v2 + 3, reader.last + 3))
        return false;
    reader.last_timestamp = pad.timestamp;
    std::copy(v1, v1 + 3, reader.last);
    std::copy(v2, v2 + 3, reader.last + 3);
    const auto length = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    if (reader.accel < 0)
    {
        const float l1 = length(v1), l2 = length(v2);
        if (l1 > 0.85f && l1 < 1.15f && l2 < 0.25f)
            reader.votes_first++;
        else if (l2 > 0.85f && l2 < 1.15f && l1 < 0.25f)
            reader.votes_second++;
        if (std::max(reader.votes_first, reader.votes_second) < 40)
            return false;
        reader.accel = reader.votes_first >= reader.votes_second ? 0 : 1;
        Trace(fmt::format("pad: motion sensors: the accelerometer is the {} vector ({} to {} samples at rest); "
                          "acceleration ({:.2f}, {:.2f}, {:.2f}) G",
                          reader.accel == 0 ? "first" : "second", std::max(reader.votes_first, reader.votes_second),
                          std::min(reader.votes_first, reader.votes_second), (reader.accel ? v2 : v1)[0],
                          (reader.accel ? v2 : v1)[1], (reader.accel ? v2 : v1)[2]));
    }
    const float* a = reader.accel == 0 ? v1 : v2;
    const float* w = reader.accel == 0 ? v2 : v1;
    const double now = Seconds();
    const double elapsed = reader.last_time > 0 ? now - reader.last_time : 0.004;
    reader.last_time = now;
    constexpr float kTurn = 2 * 3.14159265f;
    out.accel_x = -a[0];
    out.accel_y = a[2];
    out.accel_z = -a[1];
    out.gyro_x = w[0] / kTurn;
    out.gyro_y = -w[2] / kTurn;
    out.gyro_z = w[1] / kTurn;
    // (a pause, as the menu's, doesn't count as one long turn)
    out.delta_us = uint64_t(std::clamp(elapsed, 0.0005, 0.05) * 1e6);
    return true;
}

// (04.59) The Switch's touch screen from the touchpad, as Crema does the Wii U GamePad's: a finger
// resting on it shows a pointer where it would touch (the touchpad is the picture, corner to
// corner); a click with that one finger touches there, for as long as it's held (sliding drags); a
// quick tap (down and up within 0.3 s without sliding) touches there for 0.12 s, as on a phone.
// Two fingers: no touch (their click is the Minus button).
struct TouchpadTap
{
    bool down = false, moved = false;
    double start = 0, until = 0;
    float start_x = 0, start_y = 0, x = 0, y = 0;
};
Launcher::TouchState ToSwitchTouch(const OrbisPadData& pad, TouchpadTap& tap)
{
    Launcher::TouchState state;
    if (g_touchpad_minus.load(std::memory_order_relaxed))
        return state;
    const int fingers = pad.touch.fingers;
    const bool finger = fingers > 0;
    float x = 0, y = 0;
    if (finger)
    {
        x = std::clamp(float(pad.touch.touch[0].x) / g_touch_max_x, 0.0f, 1.0f);
        y = std::clamp(float(pad.touch.touch[0].y) / g_touch_max_y, 0.0f, 1.0f);
    }
    bool touching = fingers == 1 && (pad.buttons & ORBIS_PAD_BUTTON_TOUCH_PAD);
    const double now = Seconds();
    if (touching)
        tap.down = false; // a click is a touch already
    else if (finger)
    {
        if (!tap.down)
        {
            tap.down = true;
            tap.moved = fingers != 1;
            tap.start = now;
            tap.start_x = x;
            tap.start_y = y;
        }
        else if (fingers != 1 || std::abs(x - tap.start_x) > 0.04f || std::abs(y - tap.start_y) > 0.06f)
            tap.moved = true;
    }
    else if (tap.down)
    {
        if (!tap.moved && now - tap.start < 0.3)
        {
            tap.until = now + 0.12;
            tap.x = tap.start_x;
            tap.y = tap.start_y;
        }
        tap.down = false;
    }
    if (!touching && now < tap.until)
    {
        touching = true;
        x = tap.x;
        y = tap.y;
    }
    state.pointer = finger || touching;
    state.touching = touching;
    state.x = x;
    state.y = y;
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
std::atomic<uint64_t> g_frames_shown{0}; // (for the hang watchdog)

// (04.71) The boot watcher. Every game starts with the normal settings; while it starts (until two
// minutes past its first frame) the launcher watches for it failing: the game's own error (its
// abort, an exception it didn't handle: Common::PS4Diagnostics::guest_exceptions/guest_aborts) or
// the app crashing (ps4_crash_restart below). A failure stops it and starts it again by itself, a
// fresh process each time, one step safer: (04.73) the translator's PS4 shortcuts off, the
// recompiler's IR passes off, then CPU accuracy Accurate, then Paranoid. The step that gets it running is remembered for that game in this
// build (a new build tries the normal settings first again: its fixes may be what it needed).
// Little Kitty, Big City was the reason (04.66-04.70: one recompiler bug, fixed in 04.71). L3 + R3
// while it watches stops the game as usual and the steps with it. State: safe-boot.txt.
struct SafeBoot
{
    bool active = false, pending = false;
    std::string program;
    int level = 0; // the step being tried (SafeBootLevels)
    std::string notice; // what the HOME menu says once when it next shows ("title|text")
    std::vector<std::string> remembered; // "build|level|program": a step that got a game running
    std::vector<std::string> history;
};
constexpr const char* kSafeBootFile = "/data/eden-ps4/safe-boot.txt";
constexpr double kSafeBootWatchSeconds = 120; // past the first frame without failing: it works
struct SafeBootLevel
{
    const char* jit_test;  // EDEN_PS4_JIT_TEST
    const char* accuracy;  // EDEN_PS4_CPU_ACCURACY
    const char* label;     // the log's
    const char* screen;    // the loading screen's
};
constexpr SafeBootLevel kSafeBootLevels[] = {
    {"", "", "the normal settings", ""},
    // (04.73) the translator as 04.72 had it under Auto: its PS4 shortcuts (on again with Auto from
    // 04.73), loop registers and stack range checks off, at nearly the same speed
    {"extras", "", "the CPU translation's PS4 shortcuts off", "Starting it again with safer settings (step 2 of 5)"},
    {"ir", "", "the recompiler's IR passes off", "Starting it again with safer settings (step 3 of 5)"},
    {"", "accurate", "CPU accuracy Accurate", "Starting it again with safer settings (step 4 of 5)"},
    {"", "paranoid", "CPU accuracy Paranoid", "Starting it again with the safest settings (step 5 of 5)"},
};
constexpr int kSafeBootLevelCount = int(sizeof(kSafeBootLevels) / sizeof(kSafeBootLevels[0]));

SafeBoot LoadSafeBoot()
{
    SafeBoot s;
    for (const std::string& line : ReadLines(kSafeBootFile, 256 * 1024))
    {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "active")
            s.active = value == "1";
        else if (key == "pending")
            s.pending = value == "1";
        else if (key == "program")
            s.program = value;
        else if (key == "level")
            s.level = std::clamp(std::atoi(value.c_str()), 0, kSafeBootLevelCount - 1);
        else if (key == "notice")
            s.notice = value;
        else if (key == "remembered")
            s.remembered.push_back(value);
        else if (key == "history")
            s.history.push_back(value);
    }
    return s;
}

void SaveSafeBoot(SafeBoot s)
{
    // (the last 40 lines of history are plenty)
    if (s.history.size() > 40)
        s.history.erase(s.history.begin(), s.history.end() - 40);
    if (FILE* f = std::fopen(kSafeBootFile, "w"))
    {
        std::fprintf(f, "active=%d\npending=%d\nprogram=%s\nlevel=%d\nnotice=%s\n", int(s.active), int(s.pending),
                     s.program.c_str(), s.level, s.notice.c_str());
        for (const std::string& r : s.remembered)
            std::fprintf(f, "remembered=%s\n", r.c_str());
        for (const std::string& h : s.history)
            std::fprintf(f, "history=%s\n", h.c_str());
        std::fflush(f);
        fsync(fileno(f));
        std::fclose(f);
    }
}

// The step remembered for a game in this build (0: none, the normal settings)
int SafeBootRemembered(const SafeBoot& s, const std::string& program)
{
    const std::string prefix = std::string(EDEN_PS4_VERSION) + "|";
    for (const std::string& r : s.remembered)
    {
        if (r.compare(0, prefix.size(), prefix) != 0)
            continue;
        const size_t bar = r.find('|', prefix.size());
        if (bar != std::string::npos && r.substr(bar + 1) == program)
            return std::clamp(std::atoi(r.c_str() + prefix.size()), 0, kSafeBootLevelCount - 1);
    }
    return 0;
}

void SafeBootRemember(SafeBoot& s, const std::string& program, int level)
{
    // this game's entries go (and other builds' entries: their steps don't apply to this one)
    const std::string prefix = std::string(EDEN_PS4_VERSION) + "|";
    std::erase_if(s.remembered, [&](const std::string& r) {
        const size_t bar = r.find('|', r.find('|') + 1);
        return r.compare(0, prefix.size(), prefix) != 0 || (bar != std::string::npos && r.substr(bar + 1) == program);
    });
    if (level > 0)
        s.remembered.push_back(fmt::format("{}|{}|{}", EDEN_PS4_VERSION, level, program));
}

// The boot in flight failed: the next step, or the end of them
void SafeBootFailed(SafeBoot& s, const char* why)
{
    s.history.push_back(fmt::format("{}: {} failed ({})", std::filesystem::path(s.program).stem().string(),
                                    kSafeBootLevels[s.level].label, why));
    s.pending = false;
    if (s.level + 1 < kSafeBootLevelCount)
    {
        s.level++;
    }
    else
    {
        s.active = false;
        s.history.push_back("no step got it running");
        s.notice = std::filesystem::path(s.program).stem().string() +
                   " couldn't start|It stopped with an error with the normal settings and with each safer one. "
                   "Send the logs as usual: they say where.";
        SafeBootRemember(s, s.program, 0);
    }
    SaveSafeBoot(s);
}

// (04.71) A crash while a game runs: the app starts again (ps4_crash_restart, below) and the HOME
// menu says which game it was (crash-notice.txt, the game's name). The name is set before each boot
// (the crash handler only reads it).
constexpr const char* kCrashNoticeFile = "/data/eden-ps4/crash-notice.txt";
char g_crash_program[256];
std::atomic<bool> g_crash_restart_armed{false};

void ArmCrashRestart(const std::string& program)
{
    const std::string name = std::filesystem::path(program).stem().string();
    std::snprintf(g_crash_program, sizeof(g_crash_program), "%s", name.c_str());
    g_crash_restart_armed = true;
}

// The name the last crash left, once (empty: none)
std::string TakeCrashNotice()
{
    const std::vector<std::string> lines = ReadLines(kCrashNoticeFile, 4096);
    if (lines.empty())
        return {};
    std::remove(kCrashNoticeFile);
    return lines[0].empty() ? std::string("The game") : lines[0];
}

std::atomic<bool> g_safe_boot_watching{false}, g_safe_boot_decided{false};
std::atomic<bool> g_safe_boot_retrying{false}; // (the loading screen: stopped, about to start again)
// (the HOME menu says this once when it next shows)
std::string g_menu_message_title, g_menu_message_text;

// The pad thread's look at a watched boot: the game failed, or ran long enough past its first
// frame (`first_frame`: when it came, Seconds(); -1 until then)
void SafeBootWatch(double& first_frame, uint64_t frames_at_boot)
{
    namespace D = Common::PS4Diagnostics;
    if (!g_safe_boot_watching.load() || g_safe_boot_decided.load())
        return;
    const bool failed = D::guest_exceptions.load() + D::guest_aborts.load() != 0;
    if (!failed)
    {
        const uint64_t frames = D::composites.load(std::memory_order_relaxed) +
                                g_frames_shown.load(std::memory_order_relaxed);
        if (first_frame < 0 && frames > frames_at_boot)
            first_frame = Seconds();
        if (first_frame < 0 || Seconds() - first_frame < kSafeBootWatchSeconds)
            return;
    }
    if (g_safe_boot_decided.exchange(true))
        return;
    SafeBoot s = LoadSafeBoot();
    if (!failed)
    {
        s.active = false;
        s.pending = false;
        s.history.push_back(fmt::format("{}: {} work", std::filesystem::path(s.program).stem().string(),
                                        kSafeBootLevels[s.level].label));
        SafeBootRemember(s, s.program, s.level);
        SaveSafeBoot(s);
        Trace(fmt::format("boot watcher: {}", s.history.back()));
        return;
    }
    const int tried = s.level;
    SafeBootFailed(s, "the game's error");
    Trace(fmt::format("boot watcher: {}; {}", s.history.back(),
                      s.active ? fmt::format("starting it again with {}", kSafeBootLevels[s.level].label)
                               : std::string("stopping it")));
    if (s.active)
    {
        // stopped, and started again by the run loop (a fresh process)
        g_safe_boot_retrying = true;
        Launcher::Stop();
    }
    // (every step failed: the loading screen says so and how to leave, and the HOME menu once after)
    (void)tried;
}

void* PadThread(void*)
{
    // above the emulation's busy threads (650): a poll is a moment's work, and with all seven
    // cores busy at a lower priority than theirs the controller would lag
    scePthreadSetprio(scePthreadSelf(), 600);
    constexpr uint32_t kStopButtons = ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3;
    double stop_held_since = -1;
    TouchpadTap tap;
    MotionReader motion_reader;
    double first_frame = -1;
    const uint64_t frames_at_boot = Common::PS4Diagnostics::composites.load() + g_frames_shown.load();
    while (g_running)
    {
        SafeBootWatch(first_frame, frames_at_boot);
        union {
            OrbisPadData data;
            unsigned char room[512]; // in case the system's structure is larger than the header's
        } state{};
        if (g_pad >= 0 && scePadReadState(g_pad, &state.data) == 0 && state.data.connected)
        {
            Launcher::SetPad(ToSwitchPad(state.data));
            Launcher::SetTouch(ToSwitchTouch(state.data, tap));
            Launcher::MotionState motion;
            if (ReadMotion(state.data, motion_reader, motion))
                Launcher::SetMotion(motion);
            if ((state.data.buttons & kStopButtons) != kStopButtons)
                stop_held_since = -1;
            else if (stop_held_since < 0)
                stop_held_since = Seconds();
            else if (Seconds() - stop_held_since >= 1)
            {
                Trace("L3 + R3 held: stopping the program");
                if (g_safe_boot_watching.load() && !g_safe_boot_decided.exchange(true))
                {
                    // (04.71) the boot watcher's steps stop with the game (nothing remembered)
                    SafeBoot s = LoadSafeBoot();
                    s.active = false;
                    s.pending = false;
                    s.history.push_back(fmt::format("{}: stopped (L3 + R3) with {}",
                                                    std::filesystem::path(s.program).stem().string(),
                                                    kSafeBootLevels[s.level].label));
                    SaveSafeBoot(s);
                }
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
        else if (line == "macros=interpreted")
            settings.compiled_macros = false;
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
        else if (line == "touchpad=minus")
            settings.touchpad_minus = true;
        else if (line == "draws=serial")
            settings.serial_draws = true;
        else if (line == "motion=off")
            settings.motion_controls = false;
        else if (line.starts_with("games_folder="))
            settings.games_folder = line.substr(13);
        else if (line.starts_with("nickname="))
        {
            // (04.64) printable ASCII, as the keyboard types, and the Switch's length
            std::string name;
            for (const char ch : line.substr(9))
                if (ch >= ' ' && ch <= '~' && name.size() < Ui::kNicknameLength)
                    name += ch;
            if (name.find_first_not_of(' ') != std::string::npos)
                settings.nickname = name;
        }
        else if (line.starts_with("avatar="))
            settings.avatar = std::clamp(std::atoi(line.c_str() + 7), 0, Ui::AvatarCount() - 1);
        else if (line.starts_with("avatar_color="))
            settings.avatar_color = std::clamp(std::atoi(line.c_str() + 13), 0, Ui::AvatarColorCount() - 1);
    }
    return settings;
}

void SaveMenuSettings(const Ui::MenuSettings& settings)
{
    if (FILE* f = std::fopen(kMenuFile, "w"))
    {
        std::fprintf(f, "theme=%s\nrenderer=%s\nmode=%s\nresolution=%d\nshader_cache=%s\nasync_shaders=%s\ngpu=%s\n"
                        "cpu=%s\ncores=%d\nprofiler=%s\nram=%s\ngames_folder=%s\nfps=%s\nthreads=%s\npacing=%s\nspeed=%d\nfastmem=%s\n"
                        "jit_cache=%s\ndraw_shortcuts=%s\nmacros=%s\ntouchpad=%s\ndraws=%s\nmotion=%s\n"
                        "nickname=%s\navatar=%d\navatar_color=%d\n",
                     settings.white_theme ? "white" : "black", settings.software_renderer ? "software" : "vulkan",
                     settings.docked ? "docked" : "handheld", settings.resolution, settings.shader_cache ? "on" : "off",
                     settings.async_shaders ? "on" : "off", settings.gpu_high ? "high" : "normal",
                     settings.cpu_unsafe ? "unsafe" : "auto", settings.seventh_core ? 7 : 6,
                     settings.profiler ? "on" : "off", settings.ram_on_demand ? "on-demand" : "reserved",
                     settings.games_folder.c_str(), settings.fps_overlay ? "on" : "off",
                     settings.pin_threads ? "pinned" : "free", settings.unlocked_frames ? "unlocked" : "60hz", settings.speed,
                     settings.fastmem ? "on" : "off", settings.jit_cache ? "on" : "off",
                     settings.draw_shortcuts ? "on" : "off", settings.compiled_macros ? "compiled" : "interpreted",
                     settings.touchpad_minus ? "minus" : "touch", settings.serial_draws ? "serial" : "parallel",
                     settings.motion_controls ? "on" : "off", settings.nickname.c_str(), settings.avatar,
                     settings.avatar_color);
        // (04.37) on the disk at once: a crash of the whole PS4 lost a change made just before
        std::fflush(f);
        fsync(fileno(f));
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
        {"New Eden " EDEN_PS4_VERSION,
         "04.76: Frame Pacing: Unlocked shows each frame as soon as it's done but no sooner than the game asks, "
         "so games keep their proper speed (Breath of the Wild ran too fast where it could go above 30).\n"
         "04.75: copying and clearing memory (memcpy, memmove, memset) runs natively on the PS4 instead of "
         "being translated, in every game; tiny functions are translated into their callers.\n"
         "04.74: 32-bit games' calls into the system libraries jump straight to their target too, as "
         "64-bit games' do since 04.73.\n"
         "04.73: faster CPU emulation in every game. The translated game code does about a quarter less work: "
         "the CPU translation's PS4 shortcuts are on by default again (they were held back while two games' "
         "problems were tracked down; both turned out to be elsewhere), loops keep their values in the PS4's own "
         "registers, stack accesses are checked once per block instead of each time, and 64-bit games' calls into the "
         "system libraries jump straight to their target. 64-bit and 32-bit games "
         "both gain. If a game has trouble, the boot watcher's first safer step turns these off by itself.\n"
         "04.72: a test of the PS4's large memory pages when the app starts (once a version, a fraction of a "
         "second): where they're faster, games' memory is mapped in 2 MiB blocks. A game closed with L3 + R3 "
         "no longer shows an error when Eden trips while shutting it down.\n"
         "04.71: the CPU translation prepares a game's code about six times faster, so games load faster. "
         "Little Kitty, Big City runs without the slow workaround (the cause of its stop while loading, found "
         "by 04.70's search, is fixed: a CPU translation bug that could affect any game). A boot watcher: a game that "
         "stops with an error while it starts is started again by itself with safer settings, and the ones "
         "that worked are remembered. A crash while a game runs goes back to the HOME menu instead of the "
         "PS4's error screen.\n"
         "04.70: a search for why a game stopped while loading.\n"
         "04.69: the loading screen says when a game has stopped with an error, and how to leave it.\n"
         "04.68: two fixes in the CPU translation: a multiply and a sign extension that could get the upper half "
         "of a 64-bit result wrong, and a division by zero that didn't give zero. Games that stopped or "
         "misbehaved because of them can run, at full speed.\n"
         "04.67: a game that stopped while loading now runs (04.65-04.66: test builds for it).\n"
         "04.64: a User Page: choose the top left icon for the nickname and icon games show for you, from 30 "
         "characters on 12 colours, with an on-screen keyboard. Motion controls: the controller's motion "
         "sensors work in games that steer or aim by tilting (System Settings, Console, to turn them off).\n"
         "04.63: a loading screen instead of a black one from choosing a game until it shows its first "
         "frame: the game's name and icon, what it's doing, and a progress bar while its shaders build. "
         "The shaders build faster at the start, on more of the PS4's cores.\n"
         "04.62: faster: the GPU thread and idle CPU cores take on new work sooner, fewer background "
         "checks on the GPU's work, and no pause after loading screens.\n"
         "04.61: the app is now New Eden, with a new icon. The touchpad pointer is a circle. Tidier settings "
         "and news.\n"
         "04.60: faster GPU settings. New in System Settings, Graphics: Draw Order (One at a Time can help a game "
         "that freezes the GPU). The last two GPU error reports are kept.\n"
         "04.59: the touchpad is the Switch's touch screen, with a pointer on the screen: click to touch, hold the "
         "click to drag, or tap. A click with two fingers is the Minus button (System Settings, Console, "
         "Touchpad).\n"
         "04.57: fixed old cached shaders that could freeze the GPU. Each game's shaders are built again once, so "
         "its first start takes longer.\n"
         "04.45: fixed GPU freezes in games that use indirect drawing."},
        {"Earlier",
         "04.39 to 04.56: when the GPU freezes, the app closes with an error report instead of the whole console "
         "crashing; more detailed GPU reports in the logs.\n"
         "04.15 to 04.38: faster CPU translation and drawing; the translation cache (games start and run "
         "smoother the next time); the FPS counter; switches for the GPU macro compiler and the CPU translator; "
         "fixes for several crashes.\n"
         "03.84 to 04.14: fastmem (faster memory access), CPU translator speed-ups, a fresh start for every game, "
         "Game Speed, deleting games and copying keys and firmware to the console.\n"
         "03.70 to 03.83: the Switch's memory taken only as games use it (more for the GPU), firmware zips, USB "
         "drives, Game Folder, keys and firmware used from a USB drive, copying games to the console.\n"
         "Before 03.70: games, keys, firmware and logs through a web browser, the Profiler, sound, and one game "
         "after another without restarting."},
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
    // what the launcher has to say once (04.71: the boot watcher, a crash)
    if (!g_menu_message_title.empty())
    {
        menu.ShowMessage(g_menu_message_title, g_menu_message_text);
        g_menu_message_title.clear();
    }
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

// (04.67) The translator's settings for a game: (04.71) the boot watcher's step for it (SafeBoot):
// the normal settings unless a step it's taking or one remembered for it in this build says
// otherwise. Returns what's set for the log; text in [] is the loading screen's line.
std::string GameProfile(const std::string& program)
{
    unsetenv("EDEN_PS4_MEM_LOG");
    unsetenv("EDEN_PS4_ALIGNED_REST");
    unsetenv("EDEN_PS4_CPU_ACCURACY");
    unsetenv("EDEN_PS4_JIT_TEST");
    unsetenv("EDEN_PS4_GETSET_RANGE");
    unsetenv("EDEN_PS4_DUMP_RANGE");
    g_safe_boot_decided = false;
    g_safe_boot_retrying = false;
    SafeBoot s = LoadSafeBoot();
    const bool fresh = !s.active || s.program != program;
    if (fresh)
    {
        // a new boot of it: normal first, unless a safer step got it running in this build
        s.active = true;
        s.program = program;
        s.level = SafeBootRemembered(s, program);
    }
    s.pending = true;
    SaveSafeBoot(s);
    g_safe_boot_watching = true;
    const SafeBootLevel& level = kSafeBootLevels[s.level];
    if (level.jit_test[0])
        setenv("EDEN_PS4_JIT_TEST", level.jit_test, 1);
    if (level.accuracy[0])
        setenv("EDEN_PS4_CPU_ACCURACY", level.accuracy, 1);
    if (s.level == 0)
        return "boot watcher: the normal settings";
    // (a step remembered from an earlier boot says so more plainly than one being tried now)
    return fmt::format("boot watcher: {} [{}]", level.label,
                       fresh ? "With safer settings (it needed them before)" : level.screen);
}

// (04.63) The loading screen, from the moment a game is chosen until its first frame: drawn here on
// the launcher's screen until the game display opens (Common::PS4Diagnostics::release_screen, which
// it calls first), then by the display into its own buffers (draw_loading). What it shows, under
// g_loading_mutex (both threads that draw it read it).
std::mutex g_loading_mutex;
Ui::LoadingScreen g_loading;
double g_loading_start = 0;
std::atomic<bool> g_loading_shown{false}; // the launcher's screen shows it: the program's lines aren't drawn
std::atomic<bool> g_loading_stop{false};
pthread_t g_loading_thread;
bool g_loading_running = false;

void ShowLine(Launcher::LineKind kind, std::string_view text);

// One frame of it into `canvas`
void DrawLoadingFrame(Ui::Canvas& canvas)
{
    namespace D = Common::PS4Diagnostics;
    std::lock_guard lock{g_loading_mutex};
    Ui::DrawLoadingScreen(canvas, g_loading, D::loading_stage.load(), D::loading_done.load(), D::loading_total.load(),
                          Seconds() - g_loading_start, D::guest_exceptions.load() + D::guest_aborts.load() != 0,
                          g_safe_boot_retrying.load());
}

// On the launcher's screen, about 7 times a second, until stopped
void* LoadingThread(void*)
{
    Ui::Canvas canvas(1920, 1080, 1.5f);
    while (!g_loading_stop.load())
    {
        DrawLoadingFrame(canvas);
        {
            std::lock_guard lock{g_mutex};
            if (g_loading_stop.load())
                break;
            g_screen.DrawFrame({canvas.Pixels(), size_t(canvas.Width()) * canvas.Height()}, uint32_t(canvas.Width()),
                               uint32_t(canvas.Height()));
        }
        for (int i = 0; i < 15 && !g_loading_stop.load(); i++)
            usleep(10000);
    }
    return nullptr;
}

void StopLoadingThread()
{
    g_loading_stop = true;
    if (g_loading_running)
        pthread_join(g_loading_thread, nullptr);
    g_loading_running = false;
}

// The game display opens: the launcher's screen goes to it (the loading screen continues there)
void ReleaseScreenForGame()
{
    StopLoadingThread();
    std::lock_guard lock{g_mutex};
    g_screen.Release();
    g_loading_shown = false;
}

// The game display's frames of it: drawn at its size, copied into its buffer row by row
void DrawLoadingInto(uint32_t* pixels, uint32_t width, uint32_t height, uint32_t pitch, bool argb)
{
    static std::unique_ptr<Ui::Canvas> canvas;
    if (!canvas || uint32_t(canvas->Width()) != width || uint32_t(canvas->Height()) != height)
        canvas = std::make_unique<Ui::Canvas>(int(width), int(height), float(width) / 1280.0f);
    DrawLoadingFrame(*canvas);
    const uint32_t* src = canvas->Pixels();
    for (uint32_t y = 0; y < height; y++)
    {
        const uint32_t* row = src + size_t(y) * width;
        uint32_t* out = pixels + size_t(y) * pitch;
        if (!argb)
        {
            std::memcpy(out, row, width * sizeof(uint32_t));
            continue;
        }
        for (uint32_t x = 0; x < width; x++)
        {
            const uint32_t p = row[x];
            out[x] = (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
        }
    }
}

// What the loading screen shows for `program`: what its last boot left (name, publisher, icon), else
// its file's name
Ui::LoadingScreen LoadingScreenFor(const std::string& program, bool white_theme)
{
    Ui::LoadingScreen screen;
    screen.white_theme = white_theme;
    const std::vector<std::string> cached = ReadLines(Launcher::TitleCachePath(kDataDir, program, "txt").c_str());
    if (!cached.empty() && !cached[0].empty())
    {
        screen.name = cached[0];
        screen.publisher = cached.size() > 1 ? cached[1] : "";
        const std::vector<uint8_t> icon = ReadFile(Launcher::TitleCachePath(kDataDir, program, "jpg").string());
        if (!icon.empty())
            screen.icon = Ui::DecodeImage(icon.data(), icon.size());
    }
    if (screen.name.empty())
        screen.name = NameFromFile(std::filesystem::path(program));
    return screen;
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
    g_screen.Add(std::string(text), color, !g_drawing && !g_loading_shown);
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
            // (04.72) of what's mapped, whole 2 MiB blocks (large pages when the PS4 gives them)
            if (const size_t blocks = Common::Orbis::FastmemLargeBlocks(); blocks != 0)
                line += fmt::format(", {} MiB of it in 2 MiB blocks", blocks * 2);
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

// (04.40) The black box: four times a second while a program runs, a line with the GPU's progress,
// the frames shown, the GPU thread's stage and the memory, in /data/eden-ps4/blackbox.txt, on the
// disk before the next (fsync). Captain Toad's level 3 takes the whole PS4 down within seconds, and
// what the boot trace (synced once a second, written every 5-10 s) and the logs (not synced) hold
// stops up to 10 s before: this says what the last quarter second looked like. A ring of 480 lines
// (2 minutes) after a header; each line starts with its number, the highest is the last.
// EDEN_PS4_BLACKBOX=0 in settings.txt turns it off.
class BlackBox
{
public:
    void Start(const std::string& program)
    {
        const char* off = std::getenv("EDEN_PS4_BLACKBOX");
        if (off && std::string_view{off} == "0")
            return;
        fd = open(kBlackBoxFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0)
            return;
        std::string header = fmt::format("New Eden {}: {}; a ring of {} lines of {} bytes, 4 a second, "
                                         "the highest number the last (times: the monotonic clock)",
                                         EDEN_PS4_VERSION, program, kLines, kLineBytes);
        header.resize(kHeaderBytes - 1, ' ');
        header += '\n';
        (void)!pwrite(fd, header.data(), header.size(), 0);
        fsync(fd);
        Trace(fmt::format("black box: {} from monotonic {:.3f} s", kBlackBoxFile, Now()));
        stop = false;
        thread = std::thread([this] { Run(); });
    }
    void Stop()
    {
        stop = true;
        if (thread.joinable())
            thread.join();
        if (fd >= 0)
            close(fd);
        fd = -1;
    }

private:
    static double Now()
    {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return double(now.tv_sec) + double(now.tv_nsec) / 1e9;
    }
    void Run()
    {
        scePthreadSetprio(scePthreadSelf(), 600); // (it sleeps; when it looks, it must get to)
        uint64_t number = 0, busy0 = 0, span0 = 0;
        {
            uint64_t submits, waits, wait_ns;
            ac_ps4_get_gpu_stats(&busy0, &span0, &submits, &waits, &wait_ns);
        }
        while (!stop)
        {
            usleep(250 * 1000);
            uint64_t submitted = 0, completed = 0, busy = 0, span = 0, submits = 0, waits = 0, wait_ns = 0;
            uint32_t trace = 0;
            int64_t stalled = 0;
            ac_ps4_gpu_progress(&submitted, &completed, &trace, &stalled);
            ac_ps4_get_gpu_stats(&busy, &span, &submits, &waits, &wait_ns);
            const double busy_share = span > span0 ? double(busy - busy0) * 100.0 / double(span - span0) : 0.0;
            busy0 = busy;
            span0 = span;
            size_t flexible = 0;
            sceKernelAvailableFlexibleMemorySize(&flexible);
            const uint64_t frames = Common::PS4Diagnostics::composites.load(std::memory_order_relaxed) +
                                    g_frames_shown.load(std::memory_order_relaxed);
            const char* stage = Common::PS4Diagnostics::stage.load();
            std::string line = fmt::format(
                "#{:06} {:.3f}: frames {}; GPU {} of {} done{}, trace {}, busy {:.0f}%; RAM {} MiB, GPU buffers {} "
                "MiB, flexible {} MiB, direct {} MiB free; GPU thread: {}",
                number, Now(), frames, completed, submitted,
                stalled ? fmt::format(", NOTHING FINISHED FOR {} ms", stalled) : std::string(), trace, busy_share,
                Common::Orbis::OnDemandBytes() / MB, ac_ps4_gpu_bytes() / MB, flexible / MB,
                Common::PS4Diagnostics::LargestFreeDirect() / MB, stage ? stage : "-");
            line.resize(kLineBytes - 1, ' ');
            line += '\n';
            (void)!pwrite(fd, line.data(), line.size(), kHeaderBytes + off_t(number % kLines) * kLineBytes);
            fsync(fd);
            number++;
        }
    }

    static constexpr const char* kBlackBoxFile = "/data/eden-ps4/blackbox.txt";
    static constexpr int kLines = 480, kLineBytes = 256, kHeaderBytes = 256;
    int fd = -1;
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
    // (04.39) the GPU hang (Captain Toad's level 3, Zelda: Echoes of Wisdom after the cliff scene):
    // RADV writes a number as the GPU's command processor reaches each draw, dispatch and cache
    // flush, so gpu/gpu-hang.txt shows where it stopped and which shaders were running
    // (gpu/hang-shaders.txt); and the app ends once a hang is sure, instead of waiting until the
    // PS4's system software crashes (CE-36329-3, ~40 s later). settings.txt can turn either off:
    // RADV_PS4_TRACE=0, RADV_PS4_HANG_EXIT=0 (RADV_PS4_TRACE=1: Crema's flight recorder as well).
    setenv("RADV_PS4_TRACE", "ids", 0);
    setenv("RADV_PS4_HANG_EXIT", "1", 0);
    // (04.55) The kitchen sink against Captain Toad's level 3 GPU hang: RADV_PS4_MITIGATE (hex bits)
    // - Mesa: 1 no CP DMA prefetch, 2 no VS late alloc, 4 conservative IA_MULTI_VGT_PARAM; the driver
    // layer: 0x10 new buffers zeroed; here: 0x20 RADV_DEBUG=nohiz,nofastclears (unless settings.txt
    // sets RADV_DEBUG). Each one in the boot trace. (04.60) Off unless settings.txt sets it (04.55-04.59:
    // ff): Toad's level 3 hang was its shaders from another GPU window (04.57), and with it on Zelda
    // and Mario Kart hung, which ran on 04.48 without it.
    {
        const char* bits = std::getenv("RADV_PS4_MITIGATE");
        const unsigned long mitigate = bits && bits[0] ? std::strtoul(bits, nullptr, 16) : 0;
        if ((mitigate & 0x20) && !std::getenv("RADV_DEBUG"))
            setenv("RADV_DEBUG", "nohiz,nofastclears", 1);
        const char* debug = std::getenv("RADV_DEBUG");
        Trace(fmt::format("kitchen sink: RADV_PS4_MITIGATE={:x}, RADV_DEBUG={}", mitigate, debug ? debug : "(none)"));
    }
    // (04.42) draws with a geometry shader or tessellation, and RADV's writes of those rings'
    // registers (the system's on the PS4), are counted and the first ones logged; with
    // RADV_PS4_SKIP_GS_TESS=1 in settings.txt they become no-ops (04.42's test, its default). 04.43
    // sets those rings up as the PS4's system uses them instead (patches/mesa-ps4-rings.patch,
    // sceGnmSetGsRingSizes in port/mesa/ac_ps4_drm.c), and draws them.
    // (04.41) the boot trace on the disk within 50 ms of each line (EDEN_PS4_TRACE_SYNC_MS; 0:
    // once a second, as before): Captain Toad's level 3 takes the whole PS4 down within a quarter
    // second of the last line that made it to the disk
    {
        const char* sync = std::getenv("EDEN_PS4_TRACE_SYNC_MS");
        ps4_boot_trace_sync_every(sync ? std::atoi(sync) : 50);
    }
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
        // (04.71) the boot watcher: a boot that never decided (the app crashed or closed) failed;
        // a step under way boots its game by itself; what it found, the HOME menu says once
        MenuChoice choice;
        {
            SafeBoot boot = LoadSafeBoot();
            if (boot.active && boot.pending)
            {
                SafeBootFailed(boot, "the app crashed or closed");
                Trace(fmt::format("boot watcher: {}", boot.history.back()));
            }
            if (boot.active && !boot.program.empty())
            {
                Trace(fmt::format("boot watcher: starting {} again by itself with {}", boot.program,
                                  kSafeBootLevels[boot.level].label));
                if (first)
                {
                    std::unique_lock lock{g_mutex};
                    OpenPad();
                }
                choice = MenuChoice{boot.program, settings};
            }
            else if (!boot.notice.empty())
            {
                const size_t bar = boot.notice.find('|');
                g_menu_message_title = boot.notice.substr(0, bar);
                g_menu_message_text = bar == std::string::npos ? std::string() : boot.notice.substr(bar + 1);
                boot.notice.clear();
                SaveSafeBoot(boot);
            }
            else if (const std::string crashed = TakeCrashNotice(); !crashed.empty())
            {
                // (04.71) a game the app crashed in (ps4_crash_restart started it again); (04.72)
                // not one being closed (the crash was Eden's shutdown's: nothing to tell)
                if (crashed.rfind("closing|", 0) == 0)
                {
                    Trace("the last game crashed while it was being closed (Eden's shutdown): started again");
                }
                else
                {
                    g_menu_message_title = crashed + " closed because of an error";
                    g_menu_message_text = "New Eden started again by itself. The logs are saved: send them as usual.";
                }
            }
        }
        if (choice.program.empty())
            choice = ShowHomeMenu(first, radv, settings);
        const std::string& program = choice.program;
        settings = choice.settings;
        gpu = choose_renderer();
        Launcher::SetGpuDisplay(gpu);
        Launcher::SetDocked(settings.docked);
        Launcher::SetResolution(settings.resolution);
        Launcher::SetShaderCache(settings.shader_cache);
        Launcher::SetJitCache(settings.jit_cache);
        Launcher::SetDrawShortcuts(settings.draw_shortcuts);
        Launcher::SetCompiledMacros(settings.compiled_macros);
        Launcher::SetAsyncShaders(settings.async_shaders);
        Launcher::SetCpuUnsafe(settings.cpu_unsafe);
        Launcher::SetGpuAccuracyHigh(settings.gpu_high);
        Launcher::SetFpsOverlay(settings.fps_overlay);
        Launcher::SetUnlockedFrames(settings.unlocked_frames);
        Launcher::SetSpeedPercent(settings.speed);
        g_touchpad_minus = settings.touchpad_minus;
        Launcher::SetMotionControls(settings.motion_controls);
        // (04.64) the Switch user games see: the nickname and icon from the User Page (the art is
        // freed after: the game needs the memory more, and the menu makes it again when it's back)
        Launcher::SetUser(settings.nickname,
                          Ui::EncodeJpeg(Ui::MakeAvatar(settings.avatar, settings.avatar_color, 256, false)));
        Ui::ReleaseAvatarArt();
        // (04.60) read by the driver layer as the game's Vulkan device starts (port/mesa/ac_ps4_drm.c,
        // ps4_serialize_init): One at a Time, every draw waits for the one before and the GPU's caches
        // are flushed in between (04.58-04.59 for every game); Parallel, as up to 04.48
        setenv("RADV_PS4_SERIALIZE", settings.serial_draws ? "full" : "off", 1);
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
        const std::string game_test = GameProfile(program);
        if (!game_test.empty())
            Trace(game_test);
        NoteRecent(program);
        const std::string setup = fmt::format(
            "renderer: {}; console mode: {}; resolution {}%; shader cache {}, translation cache {}, asynchronous shaders {}, "
            "GPU accuracy {}, frame pacing {}, game speed {}%, draw shortcuts {}, GPU macros {}; CPU {}, {} cores, threads {}, fastmem {}; "
            "the Switch's RAM {}; touchpad: {}; draw order: {}; motion controls {}{}",
            g_renderer_label, settings.docked ? "docked" : "handheld", settings.resolution,
            settings.shader_cache ? "on" : "off", settings.jit_cache ? "on" : "off", settings.async_shaders ? "on" : "off",
            settings.gpu_high ? "high" : "normal", settings.unlocked_frames ? "unlocked" : "60 Hz", settings.speed,
            settings.draw_shortcuts ? "on" : "off", settings.compiled_macros ? "compiled" : "interpreted",
            settings.cpu_unsafe ? "unsafe" : "auto", settings.seventh_core ? 7 : 6,
            settings.pin_threads ? "pinned" : "free", settings.fastmem ? "on" : "off",
            settings.ram_on_demand ? "on demand" : "all up front",
            settings.touchpad_minus ? "the Minus button" : "the touch screen",
            settings.serial_draws ? "one at a time" : "parallel", settings.motion_controls ? "on" : "off",
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
            // (04.63) the loading screen until the game's first frame: here until the program's
            // display takes the screen (what it prints still goes to log.txt)
            {
                std::lock_guard lock{g_loading_mutex};
                g_loading = LoadingScreenFor(program, settings.white_theme);
                // (04.71) the boot watcher's line, when it's taking a safer step
                if (const size_t at = game_test.find('['); at != std::string::npos)
                    g_loading.publisher = game_test.substr(at + 1, game_test.find(']') - at - 1);
                g_loading_start = Seconds();
            }
            Common::PS4Diagnostics::loading_stage = 1;
            Common::PS4Diagnostics::loading_done = 0;
            Common::PS4Diagnostics::loading_total = 0;
            g_loading_shown = true;
            g_loading_stop = false;
            g_loading_running = pthread_create(&g_loading_thread, nullptr, LoadingThread, nullptr) == 0;
            Common::PS4Diagnostics::draw_loading = DrawLoadingInto;
            Common::PS4Diagnostics::release_screen = ReleaseScreenForGame;
        }
        // The C heap's overflow (once its 384 MiB are full) leaves direct memory to the GPU: the
        // Switch's 4 GiB leave about 480 MiB of it, and RADV needs most of what's left
        ps4_heap_set_overflow_gpu_reserve((gpu ? 256 : 32) * MB);
        MemoryTrace("before booting");
        HangWatchdog watchdog;
        watchdog.Start(program);
        BlackBox black_box;
        black_box.Start(program);
        ArmCrashRestart(program);
        auto result = Launcher::Boot(program, kDataDir, ShowLine, ShowFrame, std::chrono::hours(24));
        g_crash_restart_armed = false;
        black_box.Stop();
        watchdog.Stop();
        if (gpu)
        {
            // (04.63) the display never took the screen (the boot stopped before it opened): the
            // launcher's is still open
            Common::PS4Diagnostics::draw_loading = nullptr;
            const bool still_ours = Common::PS4Diagnostics::release_screen.exchange(nullptr) != nullptr;
            StopLoadingThread();
            g_loading_shown = false;
            Common::PS4Diagnostics::loading_stage = 0;
            std::lock_guard lock{g_mutex};
            if (!still_ours)
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
        {
            // (04.71) a boot that ended before the watcher decided: a game that exited by itself or
            // didn't load isn't the translator's failure; the watch ends with it
            SafeBoot boot = LoadSafeBoot();
            if (boot.active && boot.pending && !g_safe_boot_retrying.load())
            {
                boot.active = false;
                boot.pending = false;
                SaveSafeBoot(boot);
            }
            g_safe_boot_watching = false;
        }
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

// (04.71) The crash handler's last step (port/ps4_crashlog.cpp, once crash.log is written): while a
// game runs, the app starts again (the HOME menu, or the boot watcher's next step) instead of the
// system's error screen, and the HOME menu says what happened. Not while the menu itself runs (a
// crash there would come back each time). A restart that hasn't happened 10 s on ends the process
// as before.
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" void ps4_crash_restart()
{
    static std::atomic<bool> tried{false};
    if (!g_crash_restart_armed.load() || tried.exchange(true))
        return;
    const int fd = open(kCrashNoticeFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        // (04.72) a crash while the game was being closed (Eden's shutdown: 04.70-04.71's
        // KThread::EndWait) isn't the game's error: marked, and the HOME menu says nothing
        if (Common::PS4Diagnostics::shutting_down.load(std::memory_order_relaxed))
        {
            const ssize_t marked = write(fd, "closing|", 8);
            (void)marked;
        }
        const ssize_t written = write(fd, g_crash_program, strlen(g_crash_program));
        (void)written;
        close(fd);
    }
    pthread_t timer;
    __real_pthread_create(&timer, nullptr, [](void*) -> void* {
        sleep(10);
        _exit(126);
    }, nullptr);
    sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
}

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

// (04.72) Does the PS4 give 2 MiB pages to a direct memory mapping that could have them? A game
// like Little Kitty, Big City runs a flat profile over ~1.8 GiB of memory (and 200+ MiB of
// translated code): if every access pays for 4 KiB address translations (TLB misses), mapping the
// emulated RAM in 2 MiB pieces could be worth more than any translator change. Random reads over
// 128 MiB, timed through three mappings of the same memory: one 2 MiB aligned (virtual and
// physical: a large page is possible), one misaligned on purpose (it isn't), and one 2 MiB aligned
// but made of 512 KiB pieces, as the fastmem arena maps the game's memory. Once per version (the
// result is kept in large-pages.txt; ~0.3 s).
static bool ProbeLargePages()
{
    constexpr const char* kFile = "/data/eden-ps4/large-pages.txt";
    // (this version's last line: its result, or "measuring" if the app never got past it: then
    // it's not tried again, and large pages stay off)
    std::string before;
    for (const std::string& line : ReadLines(kFile, 16384))
        if (line.rfind(std::string(EDEN_PS4_VERSION) + ": ", 0) == 0)
            before = line.substr(std::strlen(EDEN_PS4_VERSION) + 2);
    if (!before.empty())
    {
        Trace("large pages (measured before): " + before);
        return before.find("large pages help") != std::string::npos;
    }
    if (FILE* f = std::fopen(kFile, "a"))
    {
        std::fprintf(f, "%s: measuring (the app stopped while it measured: not tried again)\n", EDEN_PS4_VERSION);
        std::fflush(f);
        fsync(fileno(f));
        std::fclose(f);
    }
    constexpr size_t kSize = 128 * MB, kLarge = 2 * MB, kPiece = 512 * 1024;
    off_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), kSize + kLarge, kLarge, 0, &phys) != 0)
    {
        Trace("large pages: no direct memory to measure with");
        return false;
    }
    void* aligned = nullptr;
    void* misaligned = nullptr;
    void* pieces = nullptr;
    const bool a_ok = sceKernelMapDirectMemory(&aligned, kSize, 0x03, 0, phys, kLarge) == 0;
    // (the same memory 16 KiB on: a virtual address 2 MiB aligned over a physical one that isn't)
    const bool m_ok = g_dmem_aliasing && sceKernelMapDirectMemory(&misaligned, kSize, 0x03, 0, phys + 16 * 1024, kLarge) == 0;
    bool p_ok = g_dmem_aliasing && sceKernelMapDirectMemory(&pieces, kSize, 0x03, 0, phys, kLarge) == 0;
    if (p_ok)
    {
        // (unmapped and mapped again in pieces where it was, as the arena's faults map runs)
        sceKernelMunmap(pieces, kSize);
        for (size_t at = 0; p_ok && at < kSize; at += kPiece)
        {
            void* where = static_cast<char*>(pieces) + at;
            p_ok = sceKernelMapDirectMemory(&where, kPiece, 0x03, 0x10, phys + off_t(at), 16 * 1024) == 0 &&
                   where == static_cast<char*>(pieces) + at;
        }
    }
    std::string report;
    // (a fault while measuring is caught, reported, and the memory given back)
    struct sigaction guard {}, old_segv {}, old_bus {};
    guard.sa_sigaction = ProbeFault;
    guard.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&guard.sa_mask);
    sigaction(SIGSEGV, &guard, &old_segv);
    sigaction(SIGBUS, &guard, &old_bus);
    if (const int sig = sigsetjmp(g_probe_jump, 1); sig != 0)
    {
        g_probe_guarded = 0;
        report = fmt::format("a read faulted while measuring (signal {})", sig);
    }
    else if (a_ok)
    {
        g_probe_guarded = 1;
        // a random cycle through one cache line in every 512 bytes, as offsets from the memory's
        // start (from 16 KiB on: the misaligned view starts there)
        constexpr uint32_t kFirst = 32, kNodes = kSize / 512 - kFirst;
        auto* const node = static_cast<uint32_t*>(aligned);
        std::vector<uint32_t> order(kNodes);
        for (uint32_t i = 0; i < kNodes; i++)
            order[i] = kFirst + i;
        uint64_t seed = 0x9E3779B97F4A7C15ull;
        for (uint32_t i = kNodes - 1; i > 0; i--)
        {
            seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
            std::swap(order[i], order[seed % i]); // (Sattolo: one cycle through all of them)
        }
        for (uint32_t i = 0; i < kNodes; i++)
            node[size_t(order[i]) * 128] = order[(i + 1) % kNodes] * 128;
        const auto chase = [&](const void* base) {
            const auto* const words = static_cast<const volatile uint32_t*>(base);
            constexpr int kHops = 400000;
            uint32_t at = order[0] * 128;
            for (int i = 0; i < 20000; i++) // (warm up)
                at = words[at];
            const double start = Seconds();
            for (int i = 0; i < kHops; i++)
                at = words[at];
            const double ns = (Seconds() - start) * 1e9 / kHops;
            return at == 0xFFFFFFFFu ? -1.0 : ns;
        };
        const double t_aligned = chase(aligned);
        // (the misaligned view's first byte is the memory's 16 KiB: its start is 16 KiB before it)
        const double t_mis = m_ok ? chase(static_cast<const char*>(misaligned) - 16 * 1024) : -1;
        const double t_pieces = p_ok ? chase(pieces) : -1;
        report = fmt::format("random reads over 128 MiB: {:.0f} ns each through a 2 MiB-aligned mapping, {} through a "
                             "misaligned one, {} through 512 KiB pieces (as the arena)",
                             t_aligned, t_mis < 0 ? std::string("-") : fmt::format("{:.0f} ns", t_mis),
                             t_pieces < 0 ? std::string("-") : fmt::format("{:.0f} ns", t_pieces));
        if (t_mis > 0)
            report += fmt::format(" -> {}", t_aligned < t_mis * 0.85 ? "large pages help" : "no large-page gain");
        g_probe_guarded = 0;
    }
    else
    {
        report = "the aligned mapping failed";
    }
    sigaction(SIGSEGV, &old_segv, nullptr);
    sigaction(SIGBUS, &old_bus, nullptr);
    if (aligned)
        sceKernelMunmap(aligned, kSize);
    if (misaligned)
        sceKernelMunmap(misaligned, kSize);
    if (pieces)
        sceKernelMunmap(pieces, kSize);
    sceKernelReleaseDirectMemory(phys, kSize + kLarge);
    Trace("large pages: " + report);
    if (FILE* f = std::fopen(kFile, "a"))
    {
        std::fprintf(f, "%s: %s\n", EDEN_PS4_VERSION, report.c_str());
        std::fclose(f);
    }
    return report.find("large pages help") != std::string::npos;
}

int main()
{
    mkdir(kDataDir, 0777);
    Trace("started");
    ProbeDoubleMapping();
    // (04.72) the fastmem arena maps whole 2 MiB blocks where it can, if the PS4 gives them large pages
    {
        const bool large = g_dmem_aliasing && ProbeLargePages();
        Common::Orbis::SetFastmemLargePages(large);
        Trace(large ? "fastmem: 2 MiB blocks (large pages) where the game's memory allows"
                    : "fastmem: 16 KiB pages only (no large-page gain measured)");
    }
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
