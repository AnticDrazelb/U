// xbox-core on the PS4: a menu of XBEs (the samples in the package, and /data/xbox-core/games/<name>/
// default.xbe), then the chosen one running natively, its screen scaled up on VideoOut, the log on
// top (L3+R3 together hide it). The log also goes to /data/xbox-core/log.txt. One program per start of
// the app: to run another, close it (PS button) and open it again.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <orbis/AudioOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <stb/stb_easy_font.h>

#include "../src/core.h"
#include "../src/kernel.h"

extern "C" {
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int32_t type, off_t* physOut);
int32_t sceKernelMapDirectMemory(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t align);
int32_t sceKernelCreateEqueue(OrbisKernelEqueue* queue, const char* name);
int32_t sceKernelWaitEqueue(OrbisKernelEqueue queue, void* events, int32_t count, int32_t* out, uint32_t* timeout_us);
int32_t sceSysmoduleLoadModuleInternal(uint32_t id);
void ps4_boot_trace(const char* stage); // port/ps4_crashlog.cpp
}

namespace {

using namespace xb;

constexpr const char* kDataDir = "/data/xbox-core";
constexpr const char* kLogFile = "/data/xbox-core/log.txt";
constexpr size_t MB = 1024 * 1024;

void Trace(const std::string& s)
{
    ps4_boot_trace(("xbox-core: " + s).c_str());
}

// ---- the screen: 1920x1080, three linear A8B8G8R8 buffers, as xbox-bench (and Crema's launcher)
// set it up on the same consoles

class Screen
{
public:
    static constexpr int kWidth = 1920, kHeight = 1080, kFrames = 3;

    bool Open()
    {
        Trace("screen: sceVideoOutOpen");
        handle = sceVideoOutOpen(0xFF, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
        if (handle < 0)
            return false;
        sceKernelCreateEqueue(&flip_queue, "xbox-core flip");
        sceVideoOutAddFlipEvent(flip_queue, handle, nullptr);
        const size_t buffer_size = (size_t(kWidth) * kHeight * 4 + 2 * MB - 1) & ~(2 * MB - 1);
        off_t phys = 0;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), buffer_size * kFrames, 2 * MB, 3, &phys) != 0)
            return false;
        void* base = nullptr;
        if (sceKernelMapDirectMemory(&base, buffer_size * kFrames, 0x33, 0, phys, 2 * MB) != 0)
            return false;
        for (int i = 0; i < kFrames; i++)
            buffers[i] = reinterpret_cast<uint32_t*>(static_cast<char*>(base) + buffer_size * i);
        OrbisVideoOutBufferAttribute attr;
        sceVideoOutSetBufferAttribute(&attr, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                      ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0, kWidth, kHeight, kWidth);
        if (sceVideoOutRegisterBuffers(handle, 0, reinterpret_cast<void* const*>(buffers), kFrames, &attr) < 0)
            return false;
        Trace("screen: open");
        return open = true;
    }

    // The image to draw into next (off screen).
    uint32_t* Begin()
    {
        if (!open)
            return nullptr;
        if (flips >= 2)
            for (int tries = 0; tries < 40; tries++)
            {
                OrbisVideoOutFlipStatus status = {};
                if (sceVideoOutGetFlipStatus(handle, &status) < 0 || status.flipArg >= int64_t(flips - 1))
                    break;
                unsigned char event[64];
                int32_t count = 0;
                uint32_t timeout = 5000;
                sceKernelWaitEqueue(flip_queue, event, 1, &count, &timeout);
            }
        return buffers[current];
    }
    void End()
    {
        sceVideoOutSubmitFlip(handle, current, ORBIS_VIDEO_OUT_FLIP_VSYNC, int64_t(++flips));
        current = (current + 1) % kFrames;
    }

    // colors as 0xRRGGBB; the buffers hold bytes R, G, B, A
    static uint32_t Native(uint32_t rgb)
    {
        return 0xFF000000u | (rgb >> 16 & 0xff) | (rgb & 0xff00) | (rgb & 0xff) << 16;
    }
    static void Fill(uint32_t* fb, int x0, int y0, int x1, int y1, uint32_t rgb)
    {
        const uint32_t c = Native(rgb);
        x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, kWidth), y1 = std::min(y1, kHeight);
        for (int y = y0; y < y1; y++)
            std::fill(fb + size_t(y) * kWidth + x0, fb + size_t(y) * kWidth + std::max(x0, x1), c);
    }
    static void Text(uint32_t* fb, int x, int y, const std::string& text, uint32_t rgb, float scale = 2.0f)
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
            if (ch[0] <= ' ' || ch[0] > 126)
                continue;
            const int advance = stb_easy_font_charinfo[ch[0] - 32].advance & 15;
            const float cx = float(col * 7) + float(7 - advance) / 2.0f;
            const int count = stb_easy_font_print(cx, 0, ch, nullptr, quads, sizeof(quads));
            for (int q = 0; q < count; q++)
            {
                const Vertex* v = &quads[q * 4];
                Fill(fb, x + int(v[0].x * scale + 0.5f), y + int(v[0].y * scale + 0.5f), x + int(v[2].x * scale + 0.5f),
                     y + int(v[2].y * scale + 0.5f), rgb);
            }
        }
    }

private:
    int handle = -1;
    bool open = false;
    OrbisKernelEqueue flip_queue = 0;
    uint32_t* buffers[kFrames] = {};
    int current = 0;
    uint64_t flips = 0;
};

Screen g_screen;

// ---- the log: a file and the last lines on screen

std::mutex g_log_mutex;
std::deque<std::string> g_log_lines;
FILE* g_log_file = nullptr;

void LogSink(const char* line)
{
    std::lock_guard lock(g_log_mutex);
    if (g_log_file)
    {
        std::fprintf(g_log_file, "%s\n", line);
        std::fflush(g_log_file);
    }
    g_log_lines.push_back(line);
    while (g_log_lines.size() > 18)
        g_log_lines.pop_front();
}

void DrawLog(uint32_t* fb, int y0)
{
    std::lock_guard lock(g_log_mutex);
    int y = y0;
    for (const std::string& l : g_log_lines)
    {
        Screen::Text(fb, 32, y, l.substr(0, 260), l.find("FATAL") != std::string::npos || l.find("fault") != std::string::npos
                                                       ? 0xFF6B6B : 0xE6E6E6, 1.5f);
        y += 20;
    }
}

// ---- sound: the guest's AC'97 output (48 kHz, 16-bit stereo) through a ring to sceAudioOut, in
// 256-frame grains as Crema plays it

constexpr size_t kRingFrames = 48000 / 4; // 250 ms
std::mutex g_audio_mutex;
std::condition_variable g_audio_cv;
std::vector<int16_t> g_ring(kRingFrames * 2);
size_t g_ring_read = 0, g_ring_write = 0; // in frames, ever increasing

void AudioSink(const int16_t* samples, size_t frames)
{
    std::lock_guard lock(g_audio_mutex);
    for (size_t i = 0; i < frames; i++, g_ring_write++)
    {
        if (g_ring_write - g_ring_read >= kRingFrames)
            g_ring_read++; // the oldest is dropped
        const size_t at = (g_ring_write % kRingFrames) * 2;
        g_ring[at] = samples[i * 2];
        g_ring[at + 1] = samples[i * 2 + 1];
    }
    g_audio_cv.notify_one();
}

void StartAudio()
{
    sceAudioOutInit();
    const int32_t port = sceAudioOutOpen(0xFF, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000,
                                         ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    Log("audio: port %#x", uint32_t(port));
    if (port < 0)
        return;
    g_audio_sink = AudioSink;
    std::thread([port] {
        std::vector<int16_t> grain(256 * 2);
        for (;;)
        {
            {
                std::unique_lock lock(g_audio_mutex);
                g_audio_cv.wait_for(lock, std::chrono::milliseconds(20),
                                    [] { return g_ring_write - g_ring_read >= 256; });
                for (size_t i = 0; i < 256; i++)
                {
                    const bool have = g_ring_read < g_ring_write;
                    const size_t at = (g_ring_read % kRingFrames) * 2;
                    grain[i * 2] = have ? g_ring[at] : 0;
                    grain[i * 2 + 1] = have ? g_ring[at + 1] : 0;
                    if (have)
                        g_ring_read++;
                }
            }
            sceAudioOutOutput(port, grain.data()); // blocks for the grain's time
        }
    }).detach();
}

// ---- the pad

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
bool ReadPad(OrbisPadData& out)
{
    union {
        OrbisPadData data;
        unsigned char room[512];
    } state{};
    if (g_pad < 0 || scePadReadState(g_pad, &state.data) != 0 || !state.data.connected)
        return false;
    out = state.data;
    return true;
}
uint32_t PadButtons()
{
    OrbisPadData data;
    return ReadPad(data) ? data.buttons : 0;
}

// The DualShock 4 as an Xbox controller: Cross/Circle/Square/Triangle are A/B/X/Y, L1/R1 white/black,
// L2/R2 the triggers, OPTIONS start, the touch pad's click back. Its rumble goes back to the DS4.
void FeedGuestPad(const OrbisPadData& d)
{
    auto axis = [](uint8_t v, bool flip) {
        int a = (int(v) - 128) * 258;
        if (flip)
            a = -a - 1;
        return s16(std::clamp(a, -32768, 32767));
    };
    auto held = [&](uint32_t b) { return u8(d.buttons & b ? 255 : 0); };
    PadState p;
    if (d.buttons & ORBIS_PAD_BUTTON_UP) p.buttons |= 0x01;
    if (d.buttons & ORBIS_PAD_BUTTON_DOWN) p.buttons |= 0x02;
    if (d.buttons & ORBIS_PAD_BUTTON_LEFT) p.buttons |= 0x04;
    if (d.buttons & ORBIS_PAD_BUTTON_RIGHT) p.buttons |= 0x08;
    if (d.buttons & ORBIS_PAD_BUTTON_OPTIONS) p.buttons |= 0x10;
    if (d.buttons & ORBIS_PAD_BUTTON_TOUCH_PAD) p.buttons |= 0x20;
    if (d.buttons & ORBIS_PAD_BUTTON_L3) p.buttons |= 0x40;
    if (d.buttons & ORBIS_PAD_BUTTON_R3) p.buttons |= 0x80;
    p.analog[0] = held(ORBIS_PAD_BUTTON_CROSS);
    p.analog[1] = held(ORBIS_PAD_BUTTON_CIRCLE);
    p.analog[2] = held(ORBIS_PAD_BUTTON_SQUARE);
    p.analog[3] = held(ORBIS_PAD_BUTTON_TRIANGLE);
    p.analog[4] = held(ORBIS_PAD_BUTTON_R1); // black
    p.analog[5] = held(ORBIS_PAD_BUTTON_L1); // white
    p.analog[6] = d.analogButtons.l2;
    p.analog[7] = d.analogButtons.r2;
    p.lx = axis(d.leftStick.x, false);
    p.ly = axis(d.leftStick.y, true); // the DS4's y grows downwards, the Xbox's upwards
    p.rx = axis(d.rightStick.x, false);
    p.ry = axis(d.rightStick.y, true);
    SetPad(p);
    static u16 last_left = 0, last_right = 0;
    u16 left, right;
    GetRumble(left, right);
    if (left != last_left || right != last_right)
    {
        last_left = left, last_right = right;
        OrbisPadVibeParam v{u8(left >> 8), u8(right >> 8)};
        scePadSetVibration(g_pad, &v);
    }
}

// ---- the menu

struct Entry
{
    std::string name, path;
};

// The samples in the package are /app0/samples/<name>.xbe; the player's own programs are
// /data/xbox-core/games/<name>/default.xbe (with their files beside them: that folder is D:).
std::vector<Entry> FindXbes()
{
    std::vector<Entry> out;
    if (DIR* d = opendir("/app0/samples"))
    {
        while (dirent* e = readdir(d))
        {
            const std::string name = e->d_name;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".xbe") == 0)
                out.push_back({"sample: " + name.substr(0, name.size() - 4), "/app0/samples/" + name});
        }
        closedir(d);
    }
    if (DIR* d = opendir("/data/xbox-core/games"))
    {
        while (dirent* e = readdir(d))
        {
            if (e->d_name[0] == '.')
                continue;
            const std::string path = std::string("/data/xbox-core/games/") + e->d_name + "/default.xbe";
            struct stat st;
            if (stat(path.c_str(), &st) == 0)
                out.push_back({e->d_name, path});
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return out;
}

std::string Menu(const std::vector<Entry>& xbes)
{
    size_t selected = 0;
    uint32_t previous = PadButtons();
    for (;;)
    {
        const uint32_t buttons = PadButtons(), pressed = buttons & ~previous;
        previous = buttons;
        if ((pressed & ORBIS_PAD_BUTTON_DOWN) && selected + 1 < xbes.size())
            selected++;
        if ((pressed & ORBIS_PAD_BUTTON_UP) && selected > 0)
            selected--;
        if ((pressed & ORBIS_PAD_BUTTON_CROSS) && !xbes.empty())
            return xbes[selected].path;
        if (uint32_t* fb = g_screen.Begin())
        {
            Screen::Fill(fb, 0, 0, Screen::kWidth, Screen::kHeight, 0x101418);
            Screen::Fill(fb, 0, 0, Screen::kWidth, 6, 0x48E06C);
            Screen::Text(fb, 64, 48, "xbox-core " XB_VERSION ": original Xbox programs, run natively", 0x7EE787, 3.0f);
            Screen::Text(fb, 64, 110, "UP/DOWN choose, X run. Your own: /data/xbox-core/games/<name>/default.xbe", 0xF2CC60, 2.0f);
            if (xbes.empty())
                Screen::Text(fb, 64, 180, "no XBEs found", 0xFF6B6B, 2.0f);
            for (size_t i = 0; i < xbes.size(); i++)
            {
                const int y = 180 + int(i) * 40;
                if (i == selected)
                    Screen::Fill(fb, 48, y - 8, Screen::kWidth - 48, y + 32, 0x2A3445);
                Screen::Text(fb, 64, y, xbes[i].name, i == selected ? 0xFFFFFF : 0xB0B8C0, 2.5f);
            }
            DrawLog(fb, Screen::kHeight - 20 * 18 - 24);
            g_screen.End();
        }
        usleep(16 * 1000);
    }
}

// The guest's picture, scaled by a whole number to fit, centred; the log on top unless hidden.
void PresentLoop()
{
    bool show_log = true;
    uint32_t previous = PadButtons();
    const auto start = std::chrono::steady_clock::now();
    constexpr uint32_t kLogToggle = ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3;
    for (;;)
    {
        OrbisPadData pad{};
        const bool connected = ReadPad(pad);
        const uint32_t buttons = connected ? pad.buttons : 0;
        if ((buttons & kLogToggle) == kLogToggle && (previous & kLogToggle) != kLogToggle)
            show_log = !show_log;
        previous = buttons;
        if (connected)
            FeedGuestPad(pad);
        uint32_t* fb = g_screen.Begin();
        if (!fb)
        {
            usleep(16 * 1000);
            continue;
        }
        const DisplayInfo d = GetDisplay();
        Screen::Fill(fb, 0, 0, Screen::kWidth, Screen::kHeight, 0x000000);
        if (d.valid)
        {
            const int scale = std::max(1, std::min(Screen::kWidth / d.width, Screen::kHeight / d.height));
            const int x0 = (Screen::kWidth - d.width * scale) / 2, y0 = (Screen::kHeight - d.height * scale) / 2;
            for (int y = 0; y < d.height; y++)
            {
                uint32_t* row = fb + size_t(y0 + y * scale) * Screen::kWidth + x0;
                for (int x = 0; x < d.width; x++)
                {
                    const uint32_t c = Screen::Native(DisplayPixel(d, x, y));
                    for (int s = 0; s < scale; s++)
                        row[x * scale + s] = c;
                }
                for (int s = 1; s < scale; s++)
                    std::memcpy(row + size_t(s) * Screen::kWidth, row, size_t(d.width) * scale * 4);
            }
        }
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (show_log && (t < 15 || !d.valid))
            DrawLog(fb, Screen::kHeight - 20 * 18 - 24);
        g_screen.End();
    }
}

void LoadSystemModules()
{
    const std::pair<const char*, uint32_t> modules[] = {{"SystemService", 0x80000010},
                                                        {"UserService", 0x80000011},
                                                        {"VideoOut", 0x80000022},
                                                        {"AudioOut", 0x80000001},
                                                        {"Pad", 0x80000024},
                                                        {"Net", 0x8000001C}};
    for (const auto& [name, id] : modules)
    {
        const int32_t r = sceSysmoduleLoadModuleInternal(id);
        Trace(std::string("system module ") + name + (r == 0 ? ": loaded" : ": FAILED"));
    }
}

} // namespace

int main()
{
    mkdir(kDataDir, 0777);
    mkdir("/data/xbox-core/games", 0777);
    g_log_file = std::fopen(kLogFile, "w");
    g_log_sink = LogSink;
    g_fatal_hook = [] {
        for (;;)
            sleep(1); // the presenter keeps the message on screen
    };
    LoadSystemModules();
    g_screen.Open();
    OpenPad();
    StartAudio();
    Log("xbox-core %s", XB_VERSION);
    const std::vector<Entry> xbes = FindXbes();
    // /data/xbox-core/autostart.txt: an XBE's path to run without the menu (tests, a favourite game)
    std::string xbe;
    if (FILE* f = std::fopen("/data/xbox-core/autostart.txt", "r"))
    {
        char line[512] = {};
        if (std::fgets(line, sizeof(line), f))
            xbe = line;
        std::fclose(f);
        while (!xbe.empty() && (xbe.back() == '\n' || xbe.back() == '\r' || xbe.back() == ' '))
            xbe.pop_back();
    }
    if (xbe.empty())
        xbe = Menu(xbes);
    Log("starting %s", xbe.c_str());
    std::thread([xbe] {
        InitMemory();
        InitCpu();
        InitDevices();
        InitKernel();
        std::string error;
        if (!LoadXbe(xbe, g_xbe, error))
            Fatal("%s: %s", xbe.c_str(), error.c_str());
        SetGameDirectory(xbe.substr(0, xbe.find_last_of('/')));
        SetDataDirectory("/data/xbox-core/hdd");
        ResolveThunks();
        StartVblank();
        RunXbe();
        Log("the program ended; close the app (PS button) to choose another");
    }).detach();
    PresentLoop();
}
