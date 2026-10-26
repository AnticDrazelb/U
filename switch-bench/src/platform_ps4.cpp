// PS4: results on the TV (a text console drawn into a video-out framebuffer), in
// /data/switch-bench/results.txt and as system notifications; and the memory probes, which ask the
// system for what a Switch emulator would need.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>
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
}

namespace {

constexpr size_t MB = 1024 * 1024;
constexpr size_t GB = 1024 * MB;
constexpr int32_t kProtCpuRw = 0x3, kProtCpuGpuRw = 0x33;
constexpr int32_t kMapFixed = 0x10;
constexpr const char* kResultsDir = "/data/switch-bench";
constexpr const char* kResultsFile = "/data/switch-bench/results.txt";

struct NotificationRequest
{
    int32_t type, reqId, priority, msgId, targetId, userId, unk1, unk2, appId, errorNum, unk3;
    unsigned char useIconImageUri;
    char message[1024];
    char iconUri[1024];
    char unk[1024];
};

// The screen: 1920x1080 linear ARGB, two buffers, redrawn whenever a line is added.
class Screen
{
public:
    static constexpr int kWidth = 1920, kHeight = 1080;
    static constexpr float kScale = 2.0f;
    static constexpr int kCell = 7; // stb_easy_font's widest advance: drawn as a fixed-width font
    static constexpr int kLineHeight = 24, kMargin = 48;

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

    void Add(const std::string& line)
    {
        // wrap at the screen's width (in columns), on spaces; continuation lines indented
        const size_t columns = (kWidth - 2 * kMargin) / int(kCell * kScale);
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
        const int rows = (kHeight - 2 * kMargin) / kLineHeight;
        const size_t first = lines.size() > size_t(rows) ? lines.size() - rows : 0;
        int y = kMargin;
        for (size_t i = first; i < lines.size(); i++, y += kLineHeight)
            Text(fb, kMargin, y, lines[i], Color(lines[i], i));
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
    std::vector<std::string> lines;

    static double Seconds()
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec * 1e-9;
    }
    static uint32_t Color(const std::string& line, size_t index)
    {
        if (line.find("WRONG") != std::string::npos || line.find("PROBLEMS") != std::string::npos ||
            line.find("fault") != std::string::npos || line.find("failed") != std::string::npos)
            return 0xFFFF6B6Bu;
        if (index == 0 || line.rfind("Switch code runs", 0) == 0)
            return 0xFF7EE787u;
        if (line.rfind("kernel ", 0) == 0 || line.rfind("three cores", 0) == 0 || line.rfind("memory:", 0) == 0)
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
    static void Text(uint32_t* fb, int x, int y, const std::string& text, uint32_t color)
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
                Fill(fb, x + int(v[0].x * kScale + 0.5f), y + int(v[0].y * kScale + 0.5f),
                     x + int(v[2].x * kScale + 0.5f), y + int(v[2].y * kScale + 0.5f), color);
            }
        }
    }
};

std::mutex g_mutex;
Screen g_screen;
FILE* g_file = nullptr;

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

std::string Mib(size_t bytes)
{
    return bytes >= 10 * GB / 10 ? fmt::format("{:.2f} GiB", double(bytes) / GB) : fmt::format("{} MiB", bytes / MB);
}

// Direct memory (the GPU's pool, 4.5 GiB on a retail console): how much can be allocated, and can
// 4 GiB of it be mapped as one CPU buffer (the Switch's DRAM, as Eden's DeviceMemory needs)?
std::vector<std::string> ProbeDirect()
{
    std::vector<std::string> out;
    struct Chunk
    {
        off_t phys;
        size_t size;
    };
    std::vector<Chunk> chunks;
    size_t total = 0;
    for (size_t size : {256 * MB, 64 * MB, 16 * MB})
        for (;;)
        {
            off_t phys = 0;
            if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 2 * MB, 0, &phys) != 0)
                break;
            chunks.push_back({phys, size});
            total += size;
        }
    out.push_back(fmt::format("direct memory: {} in all, {} of it free for this app", Mib(sceKernelGetDirectMemorySize()),
                              Mib(total)));

    // map up to 4 GiB of it into one reserved range, back to back
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
    out.push_back(fmt::format("  Switch DRAM (4 GiB) as one CPU buffer: {}{}",
                              mapped >= 4 * GB ? "yes" : fmt::format("no, {} mapped", Mib(mapped)),
                              reserved ? "" : " (no 4 GiB address range)"));
    out.push_back(fmt::format("  left for the GPU after that: {}", Mib(total > 4 * GB ? total - 4 * GB : 0)));
    for (const Chunk& c : chunks)
        sceKernelReleaseDirectMemory(c.phys, c.size);
    return out;
}

// Flexible memory (system and regular pools): the app heap, page tables, code caches come from here.
std::vector<std::string> ProbeFlexible()
{
    std::vector<std::string> out;
    size_t regular = 0;
    sceKernelAvailableFlexibleMemorySize(&regular);
    out.push_back(fmt::format("flexible memory: {} free; the app heap (malloc) is {} of {}, {} in use", Mib(regular),
                              Mib(ps4rt_heap_size()), ps4rt_heap_source(), Mib(ps4rt_heap_footprint())));

    // the system pool, 64 MiB at a time
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
    out.push_back(fmt::format("  system flexible memory still free: {}", Mib(blocks.size() * 64 * MB)));
    for (void* b : blocks)
        sceKernelMunmap(b, 64 * MB);

    // Eden's page table for a 39-bit address space: 2^27 entries of 8 bytes
    void* table = mmap(nullptr, GB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    out.push_back(fmt::format("  Eden's page table (1 GiB for a 39-bit address space): {}",
                              table == MAP_FAILED ? "can't be allocated" : "allocated"));
    if (table != MAP_FAILED)
        munmap(table, GB);
    return out;
}

// Executable memory for the recompilers' code caches (Eden: 512 MiB per core).
std::string ProbeExecutable()
{
    std::vector<void*> blocks;
    for (int i = 0; i < 24; i++)
    {
        void* p = mmap(nullptr, 64 * MB, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED)
            break;
        blocks.push_back(p);
    }
    for (void* b : blocks)
        munmap(b, 64 * MB);
    return fmt::format("executable memory: {} obtainable (asked for up to 1.5 GiB: Eden's 3 x 512 MiB code caches)",
                       Mib(blocks.size() * 64 * MB));
}

} // namespace

namespace Platform {

void Init()
{
    mkdir(kResultsDir, 0777);
    g_file = std::fopen(kResultsFile, "w");
    g_screen.Open();
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

std::string MemoryReport()
{
    std::vector<std::string> lines = {"memory:"};
    for (auto& l : ProbeFlexible())
        lines.push_back(l);
    for (auto& l : ProbeDirect())
        lines.push_back(l);
    lines.push_back(ProbeExecutable()); // last: the runtime may not give all of it back
    std::string report;
    for (auto& l : lines)
        report += (report.empty() ? "" : "\n") + l;
    return report;
}

void Finish(const std::string& summary)
{
    Log(fmt::format("Done. Saved to {}. Take a photo of this screen, then close the app (PS button).", kResultsFile));
    SendNotification(summary.substr(0, 1000));
    for (;;)
        sleep(1);
}

} // namespace Platform
