// The launcher on the PS4. A start page first: it shows what an unfinished last run left behind
// (the end of its log.txt, its crash report and boot trace) so that a photo of it is enough to see
// where it stopped, and lets the player pick a program from a list (D-pad, X): the package's
// (two games, a controller test, the test program hello.nro) and every .nro in /data/eden-ps4.
// Then the program runs. What it draws (with the CPU: there's no GPU emulation yet) fills the
// screen; otherwise its text output and Eden's warnings and errors are shown, and they always go
// to /data/eden-ps4/log.txt. The controller is player 1's Pro Controller. When the program ends,
// or L3 + R3 held for a second stop it, the start page comes back. Nothing is written outside /data/eden-ps4; Eden's data folders are
// in /data/eden-ps4/user.
//
// The screen, controller and start page are switch-bench's (../../switch-bench/src/platform_ps4.cpp).
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>
#include <orbis/AudioOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <stb/stb_easy_font.h>

#include "launcher.h"

// Declared by hand: the toolchain headers give these the wrong (or no) prototypes
// (as in port/ps4_runtime.cpp).
extern "C" {
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int32_t type,
                                      off_t* physOut);
int32_t sceKernelMapDirectMemory(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys,
                                 size_t align);
int32_t sceKernelCreateEqueue(OrbisKernelEqueue* queue, const char* name);
int32_t sceKernelWaitEqueue(OrbisKernelEqueue queue, void* events, int32_t count, int32_t* out, uint32_t* timeout_us);
int32_t sceSysmoduleLoadModuleInternal(uint32_t id);
void* scePthreadSelf(void);
int32_t scePthreadSetaffinity(void* thread, uint64_t cpus);
int32_t scePthreadSetprio(void* thread, int32_t priority);
// port/ps4_crashlog.cpp: a line in /data/eden-ps4/boot-trace.log
void ps4_boot_trace(const char* stage);
}

namespace {

constexpr size_t MB = 1024 * 1024;
constexpr int32_t kProtCpuGpuRw = 0x33;
constexpr const char* kDataDir = "/data/eden-ps4";
constexpr const char* kLogFile = "/data/eden-ps4/log.txt";
constexpr const char* kCrashFile = "/data/eden-ps4/crash.log";
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
        sceKernelCreateEqueue(&flip_queue, "eden-ps4 flip");
        sceVideoOutAddFlipEvent(flip_queue, handle, nullptr);
        Trace("screen: direct memory for 3 frames");
        const size_t buffer_size = (size_t(kWidth) * kHeight * 4 + 2 * MB - 1) & ~(2 * MB - 1);
        off_t phys = 0;
        int32_t ret = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), buffer_size * kFrames,
                                                    2 * MB, 3 /* write-combined, as Crema's VRAM */, &phys);
        if (ret != 0)
            return Failed("sceKernelAllocateDirectMemory", ret);
        Trace("screen: map it");
        void* base = nullptr;
        ret = sceKernelMapDirectMemory(&base, buffer_size * kFrames, kProtCpuGpuRw, 0, phys, 2 * MB);
        if (ret != 0)
            return Failed("sceKernelMapDirectMemory", ret);
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
        return true;
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
    // A bytes: the buffers' own order.
    void DrawFrame(std::span<const uint32_t> pixels, uint32_t width, uint32_t height)
    {
        if (!open || width == 0 || height == 0)
            return;
        if (map_width != width || map_height != height)
        {
            x_map.resize(kWidth);
            y_map.resize(kHeight);
            for (int x = 0; x < kWidth; x++)
                x_map[x] = uint32_t(uint64_t(x) * width / kWidth);
            for (int y = 0; y < kHeight; y++)
                y_map[y] = uint32_t(uint64_t(y) * height / kHeight);
            map_width = width;
            map_height = height;
        }
        const double wait_start = Seconds();
        WaitForImageOffScreen();
        last_wait = Seconds() - wait_start;
        uint32_t* fb = buffers[current];
        for (int y = 0; y < kHeight; y++)
        {
            uint32_t* dst = fb + size_t(y) * kWidth;
            if (y > 0 && y_map[y] == y_map[y - 1])
            {
                std::memcpy(dst, dst - kWidth, kWidth * sizeof(uint32_t));
                continue;
            }
            const uint32_t* src = pixels.data() + size_t(y_map[y]) * width;
            for (int x = 0; x < kWidth; x++)
                dst[x] = src[x_map[x]];
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
    int handle = -1;
    bool open = false;
    OrbisKernelEqueue flip_queue = 0;
    uint32_t* buffers[kFrames] = {};
    int current = 0;
    uint64_t flips = 0;
    float scale = 2.0f;
    int line_height = 24;
    std::vector<Line> lines;
    std::string status;
    std::vector<uint32_t> x_map, y_map;
    uint32_t map_width = 0, map_height = 0;

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
        // 0x8026000E: already initialized
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
void* PadThread(void*)
{
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
void ShowFrame(std::span<const uint32_t> pixels, uint32_t width, uint32_t height)
{
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

// The programs on the start page: the package's, then every .nro in the data folder.
struct Program {
    std::string path;
    std::string label;
};
std::vector<Program> ListPrograms()
{
    std::vector<Program> programs = {
        {"/app0/breakout.nro", "Breakout (a game: stick or D-pad to move, X to launch, OPTIONS to quit)"},
        {"/app0/blocks.nro", "Blocks (a game: D-pad to move, X / O to rotate, up to drop, OPTIONS to quit)"},
        {"/app0/padtest.nro", "Controller test (OPTIONS + touchpad click to quit)"},
        {kTestProgram, "The test program (hello.nro: text only)"},
    };
    std::vector<std::string> found;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(kDataDir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".nro" && it->is_regular_file(ec))
            found.push_back(it->path().string());
    std::sort(found.begin(), found.end());
    for (const std::string& path : found)
        programs.push_back({path, path});
    return programs;
}

// Shows the start page and waits for a choice. On the first one, without a controller or after a
// finished run, the test program boots after a countdown; later ones wait for a button.
std::string StartPage(bool first)
{
    const std::vector<std::string> log = ReadLines(kLogFile);
    const std::vector<std::string> crash = ReadLines(kCrashFile);
    const std::vector<std::string> trace = ReadLines(kLastTraceFile);
    const bool unfinished = first && (!crash.empty() || (!log.empty() && log.back() != kFinishedLine));
    const std::vector<Program> programs = ListPrograms();
    static size_t selected = 0; // kept between runs
    if (selected >= programs.size())
        selected = 0;

    std::lock_guard lock{g_mutex};
    if (first)
        OpenPad();
    const auto page = [&] {
        g_screen.Clear();
        g_screen.SetScale(1.75f);
        g_screen.Add("Eden for PS4 " EDEN_PS4_VERSION ": Eden's Switch core, booting Switch homebrew (drawn with the CPU: no GPU emulation yet)",
                     kGreen, false);
        if (unfinished)
        {
            g_screen.Add("The last run didn't finish. What it left behind is below: take a photo of this screen first.",
                         kRed, false);
            g_screen.Add("", kWhite, false);
            g_screen.Add("the end of its output (log.txt):", kYellow, false);
            if (log.empty())
                g_screen.Add("   (none: it stopped before its program printed anything)", kWhite, false);
            for (const std::string& l : Last(log, 12))
                g_screen.Add("   " + l, kWhite, false);
            g_screen.Add("its crash report (crash.log):", kYellow, false);
            const std::vector<std::string> report = LastCrash(crash);
            if (report.empty())
                g_screen.Add("   (none)", kWhite, false);
            for (const std::string& l : report)
                g_screen.Add(l, kRed, false);
            g_screen.Add("its last steps (boot-trace):", kYellow, false);
            if (trace.empty())
                g_screen.Add("   (none)", kWhite, false);
            for (const std::string& l : Last(trace, 12))
                g_screen.Add("   " + l, kWhite, false);
        }
        else if (!first)
        {
            g_screen.Add("The program ended; its output is in /data/eden-ps4/log.txt.", kWhite, false);
        }
        g_screen.Add("", kWhite, false);
        g_screen.Add("Choose a program with the D-pad, X to boot it. Put more (.nro) in /data/eden-ps4.", kYellow, false);
        g_screen.Add("In a program, both sticks pressed in (L3 + R3) for a second stop it.", kGrey, false);
        for (size_t i = 0; i < programs.size(); i++)
            g_screen.Add((i == selected ? "  >  " : "      ") + programs[i].label, i == selected ? kGreen : kWhite,
                         false);
    };
    page();
    Trace(fmt::format("start page: last run {}, pad {}, {} programs", unfinished ? "unfinished" : "finished or none",
                      g_pad, programs.size()));

    // with a controller after an unfinished run, or after this session's first run: wait for the
    // choice (the photo comes first)
    bool wait = g_pad >= 0 && (unfinished || !first);
    if (!first && g_pad < 0)
    {
        g_screen.SetStatus("no controller: close the app (PS button)");
        for (;;)
            sleep(1);
    }
    const int countdown = g_pad < 0 ? 60 : 15;
    const double start = Seconds();
    uint32_t previous = PadButtons(); // a button still held from before doesn't count
    std::string program = kTestProgram;
    for (int last_shown = -1;;)
    {
        const uint32_t buttons = PadButtons();
        const uint32_t pressed = buttons & ~previous;
        previous = buttons;
        if (pressed & ORBIS_PAD_BUTTON_CROSS)
        {
            program = programs[selected].path;
            break;
        }
        if (pressed & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN))
        {
            selected = (selected + (pressed & ORBIS_PAD_BUTTON_UP ? programs.size() - 1 : 1)) % programs.size();
            wait = true; // a choice is being made: no countdown
            last_shown = -1;
            page();
        }
        const int left = countdown - int(Seconds() - start);
        if (!wait && left <= 0)
            break;
        if (!wait && left != last_shown)
        {
            last_shown = left;
            g_screen.SetStatus(fmt::format("{}the test program boots in {} s", g_pad < 0 ? "no controller found: " : "",
                                           left));
        }
        else if (wait && last_shown != 0)
        {
            last_shown = 0;
            g_screen.SetStatus("waiting for a button");
        }
        g_mutex.unlock();
        usleep(50 * 1000);
        g_mutex.lock();
    }
    Trace("start page: booting " + program);

    // the last run's files are kept, under other names
    rename(kLogFile, "/data/eden-ps4/log-previous.txt");
    rename(kCrashFile, "/data/eden-ps4/crash-previous.log");
    g_screen.Clear();
    g_screen.SetScale(2.0f);
    return program;
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

void* Run(void*)
{
    // Eden finds its data under $HOME when no folder is given, and a few paths it always takes
    // from there: keep them in the app's folder.
    setenv("HOME", kDataDir, 1);
    Launcher::SetAudio(MakeAudioOutput);
    for (bool first = true;; first = false)
    {
        const std::string program = StartPage(first);
        {
            std::lock_guard lock{g_mutex};
            g_screen.SetStatus(fmt::format("booting {}", program));
        }
        g_drawing = false;
        g_running = true;
        pthread_t pad_thread;
        pthread_create(&pad_thread, nullptr, PadThread, nullptr);
        const auto result = Launcher::Boot(program, kDataDir, ShowLine, ShowFrame, std::chrono::hours(24));
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
