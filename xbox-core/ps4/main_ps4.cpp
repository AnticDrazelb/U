// Midway (xbox-core) on the PS4: the HOME menu (frontend/home.h, New Eden's: the samples in the
// package and the player's games in /data/xbox-core/games and on USB drives, with covers, details and
// history), then the chosen one running natively behind a loading screen, its screen scaled up on
// VideoOut, the log on top for a while (L3+R3 together toggle it). The log also goes to
// /data/xbox-core/log.txt. One program per start of the app: to run another, close it (PS button)
// and open it again.
#include <algorithm>
#include <atomic>
#include <ctime>
#include <strings.h>
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
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <orbis/AudioOut.h>
#include <orbis/Pad.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <stb/stb_easy_font.h>

#include "../frontend/home.h"
#include "../src/diagnostics.h"
#include "../src/gpu.h"
#include "../src/core.h"
#include "../src/kernel.h"
#include "../src/platform.h"
#include "../src/profiler.h"

extern "C" {
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int32_t type, off_t* physOut);
int32_t sceKernelMapDirectMemory(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t align);
int32_t sceKernelCreateEqueue(OrbisKernelEqueue* queue, const char* name);
int32_t sceKernelWaitEqueue(OrbisKernelEqueue queue, void* events, int32_t count, int32_t* out, uint32_t* timeout_us);
int32_t sceSysmoduleLoadModuleInternal(uint32_t id);
void ps4_boot_trace(const char* stage); // port/ps4_crashlog.cpp
void ps4_dump_thread_stacks(const char* path, const char* reason); // port/ps4_sampler.cpp
struct SwVersion
{
    uint64_t size;
    char text[0x1c];
    uint32_t version;
};
int32_t sceKernelGetSystemSwVersion(SwVersion* version);
}

namespace {

using namespace xb;

constexpr const char* kDataDir = "/data/xbox-core";
constexpr const char* kLogFile = "/data/xbox-core/log.txt";
constexpr const char* kHistoryFile = "/data/xbox-core/history.txt";
constexpr const char* kSettingsFile = "/data/xbox-core/settings.txt";
constexpr const char* kAutostartFile = "/data/xbox-core/autostart.txt";

fe::Settings g_settings;
std::string g_running;            // the game running, for its history
// the loading screen, until the game's first picture: what it shows, and its stage (1 loading, 3
// starting); g_failed: the run stopped with an error (the log is shown in full instead)
Ui::LoadingScreen g_loading;
std::atomic<int> g_loading_stage{1};
std::atomic<bool> g_failed{false};
std::atomic<bool> g_result_recorded{false};

// how the run went: the history's "last run". A final result (how it stopped) is written once;
// progress ("showed its picture") until then.
void RecordResult(const std::string& result, bool final = true)
{
    if (g_running.empty() || g_result_recorded)
        return;
    if (final && g_result_recorded.exchange(true))
        return;
    fe::RecordRun(kHistoryFile, g_running, result);
}
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
    int Current() const { return current; }
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
FILE* g_usb_log = nullptr; // the same lines on a USB drive as they come: there even after a crash

// The log files onto the disk (fsync): what's written is kept even when the console kills the app,
// or goes down itself. Every half second (a thread), and at once for LogSync's lines.
void SyncLogs()
{
    std::lock_guard lock(g_log_mutex);
    for (FILE* f : {g_log_file, g_usb_log})
        if (f)
        {
            std::fflush(f);
            fsync(fileno(f));
        }
}

void LogSink(const char* line)
{
    std::lock_guard lock(g_log_mutex);
    for (FILE* f : {g_log_file, g_usb_log})
        if (f)
        {
            std::fprintf(f, "%s\n", line);
            std::fflush(f);
        }
    g_log_lines.push_back(line);
    const std::string l = line;
    if (l.find("] FATAL: ") != std::string::npos || l.find("guest fault") != std::string::npos || l.find("guest halted") != std::string::npos)
    {
        g_failed = true;
        std::thread([l] { RecordResult("Stopped: " + l.substr(0, 160)); }).detach();
    }
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
    g_audio_queued = [] {
        std::lock_guard lock(g_audio_mutex);
        return size_t(g_ring_write - g_ring_read);
    };
    std::thread([port] {
        prof::RegisterThread("audio out");
        std::vector<int16_t> grain(256 * 2);
        // a little held before playing (and again after the ring ran dry): one short silence where
        // the producer falls behind, not a grain of silence at every hiccup
        constexpr size_t kPrebuffer = 1024; // frames: 21 ms
        bool starved = true;
        for (;;)
        {
            {
                std::unique_lock lock(g_audio_mutex);
                g_audio_cv.wait_for(lock, std::chrono::milliseconds(20), [&] {
                    return g_ring_write - g_ring_read >= (starved ? kPrebuffer : 256);
                });
                const size_t have = g_ring_write - g_ring_read;
                if (starved && have >= kPrebuffer)
                    starved = false;
                if (!starved && have < 256)
                    starved = true, g_audio_dry++;
                for (size_t i = 0; i < 256; i++)
                {
                    const bool take = !starved && g_ring_read < g_ring_write;
                    const size_t at = (g_ring_read % kRingFrames) * 2;
                    grain[i * 2] = take ? g_ring[at] : 0;
                    grain[i * 2 + 1] = take ? g_ring[at + 1] : 0;
                    if (take)
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
    // bit 31: the system holds the input (its menu is open, xemu-ps4's notes): nothing pressed
    if (out.buttons & 0x80000000u)
    {
        out.buttons = 0;
        out.leftStick.x = out.leftStick.y = out.rightStick.x = out.rightStick.y = 128;
        out.analogButtons.l2 = out.analogButtons.r2 = 0;
    }
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

// Files the package carries for the hard disk (/app0/hdd-seed/<partition>/...: a game's data that
// would otherwise be downloaded, such as ClassiCube's texture pack) copied into its folders when
// they aren't there yet; what a game has written itself is never replaced.
void SeedHardDisk(const std::string& from, const std::string& to)
{
    DIR* d = opendir(from.c_str());
    if (!d)
        return;
    mkdir(to.c_str(), 0777);
    while (dirent* e = readdir(d))
    {
        const std::string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        const std::string src = from + "/" + name, dst = to + "/" + name;
        struct stat st;
        if (stat(src.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
        {
            SeedHardDisk(src, dst);
            continue;
        }
        struct stat have;
        if (stat(dst.c_str(), &have) == 0)
            continue;
        FILE* in = std::fopen(src.c_str(), "rb");
        FILE* out = in ? std::fopen(dst.c_str(), "wb") : nullptr;
        if (in && out)
        {
            char buf[65536];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0)
                std::fwrite(buf, 1, n, out);
            Log("launch: %s put on the hard disk (%s)", name.c_str(), dst.c_str());
        }
        if (in)
            std::fclose(in);
        if (out)
            std::fclose(out);
    }
    closedir(d);
}

// The samples in the package are /app0/samples/<name>.xbe; the player's own games are
// /data/xbox-core/games/<name>/default.xbe (with their files beside them: that folder is D:) or disc
// images, /data/xbox-core/games/<name>.iso (or an .iso inside a game's folder).
std::vector<Entry> FindXbes()
{
    std::vector<Entry> out;
    if (DIR* d = opendir("/app0/samples"))
    {
        while (dirent* e = readdir(d))
        {
            const std::string name = e->d_name;
            if (name.size() > 4 && (name.compare(name.size() - 4, 4, ".xbe") == 0 || name.compare(name.size() - 4, 4, ".iso") == 0))
                out.push_back({"sample: " + name.substr(0, name.size() - 4) +
                                   (name.compare(name.size() - 4, 4, ".iso") == 0 ? " (disc image)" : ""),
                               "/app0/samples/" + name});
        }
        closedir(d);
    }
    if (DIR* d = opendir("/data/xbox-core/games"))
    {
        while (dirent* e = readdir(d))
        {
            if (e->d_name[0] == '.')
                continue;
            const std::string name = e->d_name, folder = "/data/xbox-core/games/" + name;
            auto is_iso = [](const std::string& n) {
                return n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".iso") == 0;
            };
            if (is_iso(name))
            {
                out.push_back({name.substr(0, name.size() - 4) + " (disc image)", folder});
                continue;
            }
            struct stat st;
            if (stat((folder + "/default.xbe").c_str(), &st) == 0)
                out.push_back({name, folder + "/default.xbe"});
            else if (DIR* inner = opendir(folder.c_str())) // a folder holding a disc image
            {
                while (dirent* f = readdir(inner))
                    if (is_iso(f->d_name))
                    {
                        out.push_back({name + " (disc image)", folder + "/" + f->d_name});
                        break;
                    }
                closedir(inner);
            }
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
            Screen::Text(fb, 64, 48, "Midway " XB_VERSION ": original Xbox programs, run natively", 0x7EE787, 3.0f);
            Screen::Text(fb, 64, 110, "UP/DOWN choose, X run. Yours: /data/xbox-core/games/<name>/default.xbe or <name>.iso", 0xF2CC60, 2.0f);
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

// The HOME menu (frontend/home.h): its screen 30 times a second, the pad's buttons (the left stick
// steers too), until a game is chosen. Closing it from Power ends the app.
std::vector<std::string> UsbDrives();
std::vector<std::string> WriteLogBundle(const std::vector<fe::Game>& games);

std::string Library(const std::vector<std::string>& system)
{
    fe::HomePaths paths;
    paths.library = {{"/app0/samples"}, "/data/xbox-core/games", {}, "/data/xbox-core/covers", kHistoryFile};
    for (int i = 0; i < 8; i++)
        paths.usb.push_back("/mnt/usb" + std::to_string(i));
    for (int i = 0; i < 8; i++)
        paths.usb.push_back("/usb" + std::to_string(i)); // where some consoles show an app its drives
    paths.settings = kSettingsFile;
    paths.autostart = kAutostartFile;
    paths.picture = "/data/xbox-core/picture-1.png";
    paths.version = XB_VERSION;
    paths.system = system.size() > 2 ? system[2] + "; " + system[1] : system.empty() ? "" : system.back();
    paths.write_logs = [](const std::vector<fe::Game>& games) { return WriteLogBundle(games); };
    std::string user;
    int32_t id = -1;
    char name[32] = {};
    if (sceUserServiceGetInitialUser(&id) == 0 && sceUserServiceGetUserName(id, name, sizeof(name)) == 0 && name[0])
        user = name;
    Trace("menu: reading the library");
    fe::Home home(g_settings, paths, user);
    WriteLogBundle(home.Games());
    Log("menu: %zu titles", home.Games().size());
    Ui::Canvas canvas(Screen::kWidth, Screen::kHeight, Screen::kWidth / 1280.0f);
    const auto start = std::chrono::steady_clock::now();
    // buttons held as the menu opens (L1 to skip autostart) don't count as pressed
    home.Menu().SetHeld(~0u);
    for (;;)
    {
        OrbisPadData pad{};
        const bool connected = ReadPad(pad);
        uint32_t buttons = connected ? pad.buttons : 0;
        if (connected) // the left stick as the directions
        {
            if (pad.leftStick.x < 50) buttons |= ORBIS_PAD_BUTTON_LEFT;
            if (pad.leftStick.x > 205) buttons |= ORBIS_PAD_BUTTON_RIGHT;
            if (pad.leftStick.y < 50) buttons |= ORBIS_PAD_BUTTON_UP;
            if (pad.leftStick.y > 205) buttons |= ORBIS_PAD_BUTTON_DOWN;
        }
        uint32_t held = 0;
        const std::pair<uint32_t, uint32_t> map[] = {
            {ORBIS_PAD_BUTTON_UP, Ui::kUp},         {ORBIS_PAD_BUTTON_DOWN, Ui::kDown},
            {ORBIS_PAD_BUTTON_LEFT, Ui::kLeft},     {ORBIS_PAD_BUTTON_RIGHT, Ui::kRight},
            {ORBIS_PAD_BUTTON_CROSS, Ui::kCross},   {ORBIS_PAD_BUTTON_CIRCLE, Ui::kCircle},
            {ORBIS_PAD_BUTTON_TRIANGLE, Ui::kTriangle}, {ORBIS_PAD_BUTTON_SQUARE, Ui::kSquare},
            {ORBIS_PAD_BUTTON_OPTIONS, Ui::kOptions}, {ORBIS_PAD_BUTTON_L1, Ui::kL1},
            {ORBIS_PAD_BUTTON_R1, Ui::kR1},
        };
        for (const auto& [from, to] : map)
            if (buttons & from)
                held |= to;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const fe::Home::Result result = home.Update(held, now);
        if (result == fe::Home::Result::Boot)
        {
            g_loading = home.Loading();
            Log("menu: starting %s", home.Chosen().c_str());
            return home.Chosen();
        }
        if (result == fe::Home::Result::Quit)
        {
            Log("menu: closing the app");
            SyncLogs();
            sceSystemServiceLoadExec("exit", nullptr);
            _exit(0);
        }
        const time_t wall = time(nullptr);
        char clock[16];
        std::strftime(clock, sizeof(clock), "%H:%M", std::localtime(&wall));
        home.Draw(canvas, now, clock, connected);
        if (uint32_t* fb = g_screen.Begin())
        {
            // the canvas's bytes are the screen's (R, G, B, A)
            std::memcpy(fb, canvas.Pixels(), size_t(Screen::kWidth) * Screen::kHeight * 4);
            g_screen.End();
        }
        const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() - now;
        if (spent < 1.0 / 30)
            usleep(useconds_t((1.0 / 30 - spent) * 1e6));
    }
}

// The guest's picture, scaled by a whole number to fit, centred; the log on top unless hidden.
void PresentLoop()
{
    bool show_log = g_settings.show_log, saw_picture = false;
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
        const DisplayInfo d = GetDisplay();
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        // the picture as shown, every 10 s, the last three kept: the log bundle carries them (what a
        // report says is on the screen, seen)
        static double next_picture = 10;
        if (d.valid && now >= next_picture)
        {
            next_picture = now + 10;
            // on a thread of its own: encoding the PNG here held the screen for a moment every 10 s
            static std::atomic<bool> saving{false};
            if (!saving.exchange(true))
                std::thread([] {
                    const std::string dir = kDataDir;
                    std::rename((dir + "/picture-2.png").c_str(), (dir + "/picture-3.png").c_str());
                    std::rename((dir + "/picture-1.png").c_str(), (dir + "/picture-2.png").c_str());
                    SaveShownPicture(dir + "/picture-1.png");
                    saving = false;
                }).detach();
        }
        // the loading screen until the game's first picture (unless it stopped with an error: then the
        // log, in full), about 20 times a second, the log's last lines under it when it's on
        if (!saw_picture && !d.valid && !g_failed)
        {
            static Ui::Canvas* canvas = new Ui::Canvas(Screen::kWidth, Screen::kHeight, Screen::kWidth / 1280.0f);
            static double next_frame = 0;
            if (now < next_frame)
            {
                usleep(4 * 1000);
                continue;
            }
            next_frame = now + 1.0 / 20;
            g_loading.lines.clear();
            if (show_log)
            {
                std::lock_guard lock(g_log_mutex);
                for (size_t i = g_log_lines.size() > 3 ? g_log_lines.size() - 3 : 0; i < g_log_lines.size(); i++)
                    g_loading.lines.push_back(g_log_lines[i].substr(0, 200));
            }
            Ui::DrawLoadingScreen(*canvas, g_loading, g_loading_stage.load(), 0, 0, now);
            if (uint32_t* fb = g_screen.Begin())
            {
                std::memcpy(fb, canvas->Pixels(), size_t(Screen::kWidth) * Screen::kHeight * 4);
                g_screen.End();
            }
            continue;
        }
        const bool log_on = show_log && (now < 15 || !d.valid);
        // Nothing new to show (the same picture, layout and no log over it): the screen keeps the last
        // flip, and the CPU time (a 1080p frame is 8 MiB to write) stays the game's.
        u64 hash = 0xcbf29ce484222325ull ^ (u64(d.address) << 32) ^ u64(d.width) << 16 ^ u64(d.height) ^ u64(g_settings.picture) << 60;
        if (d.valid && d.pixels)
            hash = (hash ^ d.generation) * 0x100000001b3ull; // a copy: new only when its generation is
        else if (d.valid)
        {
            const u32 bytes = u32(d.height) * d.pitch;
            const u64* p = reinterpret_cast<const u64*>(d.Bytes());
            for (u32 i = 0; i < bytes / 8; i++)
                hash = (hash ^ p[i]) * 0x100000001b3ull;
        }
        static u64 shown = 0;
        static bool shown_log = true;
        if (!log_on && !shown_log && hash == shown)
        {
            usleep(4 * 1000);
            continue;
        }
        uint32_t* fb = g_screen.Begin();
        if (!fb)
        {
            usleep(16 * 1000);
            continue;
        }
        shown = hash, shown_log = log_on;
        // each buffer cleared only when what covers it changes (the picture overwrites its own area)
        static u64 layout_of[Screen::kFrames] = {};
        const u64 layout = (u64(d.valid) << 63) | (u64(d.width) << 40) | (u64(d.height) << 20) | (u64(g_settings.picture) << 4) | u64(log_on);
        if (layout_of[g_screen.Current()] != layout || log_on)
            Screen::Fill(fb, 0, 0, Screen::kWidth, Screen::kHeight, 0x000000);
        layout_of[g_screen.Current()] = layout;
        if (d.valid && !saw_picture)
        {
            saw_picture = true;
            std::thread([] { RecordResult("Showed its picture", false); }).detach();
        }
        // Rows built in the CPU's memory and only written to the screen's: the screen's buffer is the
        // GPU's (uncached), where reading a row back to copy it (as this did for each repeated row)
        // stalls on every load and took most of this thread's time on the console.
        static std::vector<uint32_t> line, out;
        if (d.valid && g_settings.picture != 0)
        {
            // fit 4:3 (the full height) or fill the screen, nearest texel
            const int ow = g_settings.picture == 1 ? Screen::kHeight * 4 / 3 : Screen::kWidth, oh = Screen::kHeight;
            const int x0 = (Screen::kWidth - ow) / 2;
            static std::vector<int> columns;
            columns.resize(size_t(ow));
            for (int x = 0; x < ow; x++)
                columns[size_t(x)] = x * d.width / ow;
            line.resize(size_t(d.width));
            out.resize(size_t(ow));
            int last = -1;
            for (int y = 0; y < oh; y++)
            {
                const int sy = y * d.height / oh;
                if (sy != last)
                {
                    last = sy;
                    for (int x = 0; x < d.width; x++)
                        line[size_t(x)] = Screen::Native(DisplayPixel(d, x, sy));
                    for (int x = 0; x < ow; x++)
                        out[size_t(x)] = line[size_t(columns[size_t(x)])];
                }
                std::memcpy(fb + size_t(y) * Screen::kWidth + x0, out.data(), size_t(ow) * 4);
            }
        }
        else if (d.valid)
        {
            const int scale = std::max(1, std::min(Screen::kWidth / d.width, Screen::kHeight / d.height));
            const int x0 = (Screen::kWidth - d.width * scale) / 2, y0 = (Screen::kHeight - d.height * scale) / 2;
            out.resize(size_t(d.width) * size_t(scale));
            for (int y = 0; y < d.height; y++)
            {
                for (int x = 0; x < d.width; x++)
                {
                    const uint32_t c = Screen::Native(DisplayPixel(d, x, y));
                    for (int s = 0; s < scale; s++)
                        out[size_t(x * scale + s)] = c;
                }
                for (int s = 0; s < scale; s++)
                    std::memcpy(fb + size_t(y0 + y * scale + s) * Screen::kWidth + x0, out.data(), out.size() * 4);
            }
        }
        if (log_on)
            DrawLog(fb, Screen::kHeight - 20 * 18 - 24);
        g_screen.End();
    }
}

// The USB drives as the app sees them: /mnt/usbN, or /usbN on consoles that show it its drives there
// (New Eden's finding)
std::vector<std::string> UsbDrives()
{
    std::vector<std::string> out;
    for (int i = 0; i < 8; i++)
        if (const std::string d = fe::UsbDrive(i); !d.empty())
            out.push_back(d);
    return out;
}

// Every log in one file at the top of each USB drive, xbox-core-logs.txt (as New Eden's
// eden-ps4-logs.txt): written as the app starts, when the library opens and when a program ends;
// also /data/xbox-core/diagnostics.txt.
#ifndef XB_VARIANT
#define XB_VARIANT "dev"
#endif
// each package (where it is linked, RADV in it or not) its own trail, so they can't overwrite each other's
const char* const kFaultTrail = "/data/xbox-core/fault-trail-" XB_VARIANT ".bin";
std::vector<std::string> g_system; // the report's first lines: the app and the console
std::vector<std::string> WriteLogBundle(const std::vector<fe::Game>& games)
{
    fe::DiagnosticsInput d;
    d.system = g_system;
    d.settings = &g_settings;
    d.games = games;
    d.usb_roots = UsbDrives();
    d.data_dir = kDataDir;
    d.all_logs = true; // every log in the data folder, code addresses named
    d.symbols = "/app0/symbols.txt";
    std::vector<std::string> outputs{"/data/xbox-core/diagnostics.txt"};
    for (const std::string& drive : d.usb_roots)
        outputs.push_back(drive + "/xbox-core-logs.txt");
    const std::vector<std::string> written = fe::WriteDiagnostics(d, outputs);
    for (const std::string& w : written)
        Log("logs written: %s", w.c_str());
    return written;
}

// SIGSYS: the console refusing a system call (the crash log doesn't take that signal). Where it
// happened goes straight into the logs (write() only), then the app ends.
void OnSigsys(int, siginfo_t*, void* context)
{
    const auto* regs = reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + 0x40);
    char line[200];
    const int n = std::snprintf(line, sizeof(line),
                                "FATAL: SIGSYS: the console refused a system call at rip %#llx (rax %#llx: its number)\n",
                                (unsigned long long)regs[20], (unsigned long long)regs[7]);
    for (FILE* f : {g_log_file, g_usb_log})
        if (f && n > 0)
            (void)!write(fileno(f), line, size_t(n));
    _exit(140);
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
    // the last session's log kept (it ran the game being asked about), and every log onto the USB
    // drives first thing, before anything that could fail; this session's log also goes to a drive
    // line by line, so a crash leaves it there
    // the last 10 sessions' logs kept: log.txt this one, log-1.txt the one before, ... log-9.txt;
    // the log bundle has them all
    std::remove("/data/xbox-core/log-previous.txt"); // the old name
    for (int i = 8; i >= 1; i--)
        std::rename(("/data/xbox-core/log-" + std::to_string(i) + ".txt").c_str(),
                    ("/data/xbox-core/log-" + std::to_string(i + 1) + ".txt").c_str());
    std::rename(kLogFile, "/data/xbox-core/log-1.txt");
    // crash.log gets every crash appended: each start marked, so a report says which build and run it
    // came from (an older build's addresses name the wrong functions); kept from growing too big
    {
        struct stat st;
        if (stat("/data/xbox-core/crash.log", &st) == 0 && st.st_size > 256 * 1024)
            std::rename("/data/xbox-core/crash.log", "/data/xbox-core/crash-previous.log");
        if (FILE* f = std::fopen("/data/xbox-core/crash.log", "a"))
        {
            const time_t now = time(nullptr);
            char when[32];
            std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
            std::fprintf(f, "\n--- xbox-core %s (build %s) started %s: what follows is from this run\n", XB_VERSION,
                         kBuildId, when);
            std::fclose(f);
        }
    }
    // the fault path's trail from the last run (a run the console killed in the fault path leaves where
    // it got): decoded into the data folder, so this start's log bundle carries it
    ReportFaultTrail(kFaultTrail);
    g_log_file = std::fopen(kLogFile, "w");
    g_system = {std::string("xbox-core ") + XB_VERSION + " (" XB_VARIANT ", build " + kBuildId + ", written as the app started)"};
    WriteLogBundle({});
    if (const std::vector<std::string> drives = UsbDrives(); !drives.empty())
    {
        // the last run's live log kept (reopening the app to bundle the logs mustn't wipe it)
        std::rename((drives.front() + "/xbox-core-live-log.txt").c_str(),
                    (drives.front() + "/xbox-core-live-log-previous.txt").c_str());
        g_usb_log = std::fopen((drives.front() + "/xbox-core-live-log.txt").c_str(), "w");
    }
    g_log_sink = LogSink;
    g_log_sync = SyncLogs;
    std::thread([] {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            SyncLogs();
        }
    }).detach();
    {
        struct sigaction action;
        std::memset(&action, 0, sizeof(action));
        void (*handler)(int, siginfo_t*, void*) = OnSigsys;
        std::memcpy(&action, &handler, sizeof(handler));
        action.sa_flags = 0x40 | 0x1; // SA_SIGINFO, SA_ONSTACK
        sigemptyset(&action.sa_mask);
        sigaction(12, &action, nullptr); // SIGSYS
    }
    g_fatal_hook = [] {
        WriteLogBundle({});
        for (;;)
            sleep(1); // the presenter keeps the message on screen
    };
    LoadSystemModules();
    g_screen.Open();
    OpenPad();
    StartAudio();
    // where the system put the app: inside the Xbox's addresses (below 0x4000000), games whose image
    // reaches it won't load
    const uintptr_t code = reinterpret_cast<uintptr_t>(&FindXbes);
    Log("xbox-core %s, its code at %#llx%s", XB_VERSION, (unsigned long long)code,
        code < 0x4000000 ? " (inside the Xbox's RAM addresses)" : " (clear of the Xbox's addresses)");
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
    // holding L1 while the app opens: the library, whatever autostart says
    if (PadButtons() & ORBIS_PAD_BUTTON_L1)
        xbe.clear();
    g_settings.Load(kSettingsFile);
    std::vector<std::string> system;
    {
        char line[256];
        std::snprintf(line, sizeof(line), "xbox-core %s, build %s", XB_VERSION, kBuildId);
        system.push_back(line);
        std::snprintf(line, sizeof(line), "app code at %#llx (%s)", (unsigned long long)code,
                      code < 0x4000000 ? "inside the Xbox's RAM addresses: the normal package" : "clear of them: the high package");
        system.push_back(line);
        SwVersion v{sizeof(SwVersion), {}, 0};
        if (sceKernelGetSystemSwVersion(&v) == 0)
            system.push_back(std::string("PS4 firmware ") + v.text);
        std::snprintf(line, sizeof(line), "direct memory %zu MB", sceKernelGetDirectMemorySize() / MB);
        system.push_back(line);
        system.push_back(std::string("autostart: ") + (xbe.empty() ? "none" : xbe));
    }
    g_system = system;
    if (xbe.empty())
        xbe = Library(system);
    if (xbe.empty())
        xbe = Menu(xbes);
    if (g_loading.name.empty()) // started without the menu (autostart): the file's name
    {
        const size_t slash = xbe.find_last_of('/');
        g_loading.name = xbe.substr(slash + 1);
        if (g_loading.name == "default.xbe" && slash != std::string::npos && slash > 0)
            g_loading.name = xbe.substr(xbe.find_last_of('/', slash - 1) + 1, slash - xbe.find_last_of('/', slash - 1) - 1);
        g_loading.white_theme = g_settings.theme == 1;
    }
    Log("starting %s", xbe.c_str());
    g_running = xbe;
    fe::RecordRun(kHistoryFile, xbe, "");
    std::thread([xbe] {
        LogSync("launch: setting up memory");
        InitMemory();
        LogSync("launch: setting up the CPU");
        g_selftest_file = "/data/xbox-core/selftest-step.txt";
        g_fault_trail_file = kFaultTrail;
        InitCpu();
        // the profiler (New Eden's way, on the sample signal the fault handler takes): every thread's
        // busiest functions in the log every 10 s and in profile.txt; after InitCpu, whose handler
        // takes the signal
        prof::Start("/app0/symbols.txt", "/data/xbox-core/profile.txt");
        prof::RegisterThread("launcher");
        gpu::SetGuardFile("/data/xbox-core/gpu-step.txt"); // before the devices: the NV2A's choice reads it
        SetNv2aChoice(g_settings.nv2a == 1); // Settings > Graphics
        SetApuDsp(g_settings.dsp == 1);      // Settings > Sound effects
        // the hard disk's folders before the devices (xemu's renderer keeps its shader cache there)
        SeedHardDisk("/app0/hdd-seed", "/data/xbox-core/hdd");
        SetDataDirectory("/data/xbox-core/hdd");
        LogSync("launch: devices");
        InitDevices();
        // drawing on the GPU (RADV), unless starting it killed the last run of this build
        gpu::SetWanted(true);
        LogSync("launch: kernel");
        InitKernel();
        LogSync("launch: loading %s", xbe.c_str());
        std::string error;
        if (!LoadGame(xbe, error))
            Fatal("%s: %s", xbe.c_str(), error.c_str());
        diag::StartMonitor("/data/xbox-core/hang.log", ps4_dump_thread_stacks, [] { WriteLogBundle({}); });
        // the logs onto the USB drives every minute while it runs: a console that freezes hard
        // (nothing runs to write them), or an app closed from the home screen, still leaves them.
        // (Every 15 s, the bundle's tens of MiB read, named and written twice were a stutter; the
        // live log streams to the drive line by line meanwhile, and a crash, a hang or the program's
        // end writes it at once.)
        std::thread([] {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(60));
                WriteLogBundle({});
            }
        }).detach();
        LogSync("launch: kernel imports");
        ResolveThunks();
        StartVblank();
        // signals that can arrive while guest code runs, made safe there: the stack dump's (its
        // handler set by a first dump, of the app alone) and SIGSYS's; the fault handler checked
        ps4_dump_thread_stacks("/data/xbox-core/stacks-at-launch.log", "every host thread at the launch");
        for (int sig : {12, 30})
        {
            const std::string problem = platform::WrapSignal(sig);
            Log("launch: signal %d %s", sig, problem.empty() ? "safe in guest code" : problem.c_str());
        }
        if (const std::string handler = platform::CheckFaultHandler(); !handler.empty())
            Log("launch: %s", handler.c_str());
        LogSync("launch: running the program");
        g_loading_stage = 3;
        RunXbe();
        RecordResult("Ended by itself");
        WriteLogBundle({});
        Log("the program ended; close the app (PS button) to choose another");
    }).detach();
    prof::RegisterThread("presenter (main)");
    PresentLoop();
}

// The crash log's hooks (port/ps4_crashlog.cpp): the guest's side of a crash. Into crash.log
// without locks (each guest thread's kernel call and latest calls, the GPU's latest methods), then,
// with locks allowed, the same and what wasn't emulated into the log.
extern "C" void ps4_crash_guest_regs(int fd)
{
    xb::diag::WriteStateLockFree(fd);
}
extern "C" void ps4_crash_guest_info()
{
    xb::diag::LogState("crash");
    WriteLogBundle({});
}
