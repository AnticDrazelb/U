// Devices: the table of memory-mapped devices (an access faults and the fault handler calls them),
// level-triggered interrupt lines whose connected service routines run on an "interrupt" guest
// thread, DPCs, the vblank tick, KeTickCount, and the displayed picture (PNG on the desktop).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <thread>
#include <vector>

#include "core.h"
#include "gpu.h"
#include "diagnostics.h"
#include "devices.h"
#include "kernel.h"

namespace xb {

namespace {

std::mutex g_table_mutex;
std::vector<Device> g_devices;

std::mutex g_irq_mutex;
std::condition_variable g_irq_cv;
u32 g_lines = 0;               // asserted interrupt lines
std::vector<u32> g_interrupts; // connected KINTERRUPTs
std::deque<u32> g_dpcs;
std::atomic<u32> g_tick_count_variable{0};
std::atomic<u64> g_vblanks{0};

struct Display
{
    u32 mode = 0, format = 0, pitch = 0, framebuffer = 0;
} g_display;

u32 Crc32(const u8* p, size_t n, u32 crc = 0)
{
    crc = ~crc;
    while (n--)
    {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

void Put32(std::vector<u8>& v, u32 x)
{
    for (int i = 3; i >= 0; i--)
        v.push_back(u8(x >> (i * 8)));
}

// A PNG of RGB rows, compressed with zlib's "stored" blocks (no compression library needed).
bool WritePng(const std::string& path, int w, int h, const std::vector<u8>& rgb)
{
    std::vector<u8> raw;
    raw.reserve(size_t(h) * (w * 3 + 1));
    for (int y = 0; y < h; y++)
    {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3);
    }
    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b = 0;
    for (u8 c : raw)
        a = (a + c) % 65521, b = (b + a) % 65521;
    for (size_t at = 0; at < raw.size();)
    {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(u8(n)), z.push_back(u8(n >> 8)), z.push_back(u8(~n)), z.push_back(u8(~n >> 8));
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
    }
    Put32(z, (b << 16) | a);
    std::vector<u8> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    auto chunk = [&](const char* type, const std::vector<u8>& data) {
        Put32(png, u32(data.size()));
        const size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        Put32(png, Crc32(&png[start], png.size() - start));
    };
    std::vector<u8> ihdr;
    Put32(ihdr, u32(w)), Put32(ihdr, u32(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return true;
}

const Device* Find(u32 address)
{
    std::lock_guard lock(g_table_mutex);
    for (const Device& d : g_devices)
        if (address - d.base < d.size)
            return &d;
    return nullptr;
}

void RunDpcs(GuestThread* t)
{
    for (;;)
    {
        u32 dpc;
        {
            std::lock_guard lock(g_irq_mutex);
            if (g_dpcs.empty())
                return;
            dpc = g_dpcs.front();
            g_dpcs.pop_front();
        }
        Write<u8>(dpc + 2, 0);
        // DeferredRoutine(Dpc, DeferredContext, SystemArgument1, SystemArgument2), at dispatch level
        Write<u8>(t->kpcr + kpcr::Irql, 2);
        Write<u32>(t->kpcr + kpcr::DpcRoutineActive, 1);
        CallGuest(t, Read<u32>(dpc + 12), {dpc, Read<u32>(dpc + 16), Read<u32>(dpc + 20), Read<u32>(dpc + 24)});
        Write<u32>(t->kpcr + kpcr::DpcRoutineActive, 0);
        Write<u8>(t->kpcr + kpcr::Irql, 0);
    }
}

// The service routines of every asserted line, until the lines drop (level-triggered); a routine
// that leaves its line up is called again on the next pass, at most a few times per pass.
void RunInterrupts(GuestThread* t)
{
    for (int pass = 0; pass < 16; pass++)
    {
        std::vector<u32> isrs;
        {
            std::lock_guard lock(g_irq_mutex);
            for (u32 i : g_interrupts)
                if (g_lines & (1u << (Read<u32>(i + 8) & 31)))
                    isrs.push_back(i);
        }
        if (isrs.empty())
            return;
        for (u32 i : isrs) // ServiceRoutine(Interrupt, ServiceContext), at the interrupt's level
        {
            Write<u8>(t->kpcr + kpcr::Irql, u8(std::max<u32>(3, Read<u8>(i + 0xc))));
            CallGuest(t, Read<u32>(i), {i, Read<u32>(i + 4)});
            Write<u8>(t->kpcr + kpcr::Irql, 0);
        }
        RunDpcs(t);
    }
}

} // namespace

void AddDevice(const Device& d)
{
    std::lock_guard lock(g_table_mutex);
    g_devices.push_back(d);
}

void SetIrq(int line, bool level)
{
    {
        std::lock_guard lock(g_irq_mutex);
        if (level)
            g_lines |= 1u << line;
        else
            g_lines &= ~(1u << line);
    }
    if (level)
        g_irq_cv.notify_all();
}

u32 PhysToGuest(u32 phys)
{
    phys &= 0x0fffffff;
    if (phys < kContiguousSize)
        return kContiguousBase + phys;
    if (phys >= kRamPhysical && phys - kRamPhysical < kRamEnd)
        return phys - kRamPhysical;
    return 0;
}

void InitDevices()
{
    // 0xf0000000 + physical address: the memory there, a byte, word or dword at a time
    AddDevice({kAliasBase, kAliasSize, "memory view",
               [](u32 offset, u32 size) -> u32 {
                   const u32 at = PhysToGuest(offset);
                   if (!at)
                       return 0;
                   return size == 1 ? Read<u8>(at) : size == 2 ? Read<u16>(at) : Read<u32>(at);
               },
               [](u32 offset, u32 size, u32 value) {
                   const u32 at = PhysToGuest(offset);
                   if (!at)
                       return;
                   if (size == 1)
                       Write<u8>(at, u8(value));
                   else if (size == 2)
                       Write<u16>(at, u16(value));
                   else
                       Write<u32>(at, value);
               }});
    InitNv2a();
    InitUsb();
    InitAudio();
    InitApu();
}

bool DeviceRead(u32 address, u32 size, u32& value)
{
    value = 0;
    if (const Device* d = Find(address))
        value = d->read(address - d->base, size);
    else
    {
        char what[48];
        std::snprintf(what, sizeof(what), "read at %#x", address & ~0xfffu);
        diag::Note("device register (no device)", what);
    }
    if (g_verbose >= 3)
        Log("dev read  %#010x/%u = %#x", address, size, value);
    return true; // what nothing answers reads as 0
}

bool DeviceWrite(u32 address, u32 size, u32 value)
{
    if (g_verbose >= 3)
        Log("dev write %#010x/%u = %#x", address, size, value);
    if (const Device* d = Find(address))
        d->write(address - d->base, size, value);
    else
    {
        char what[48];
        std::snprintf(what, sizeof(what), "write at %#x", address & ~0xfffu);
        diag::Note("device register (no device)", what);
    }
    return true;
}

void ConnectInterrupt(u32 vector, u32 kinterrupt)
{
    {
        std::lock_guard lock(g_irq_mutex);
        g_interrupts.push_back(kinterrupt);
    }
    Log("devices: interrupt %u connected (service routine %#x)", vector, Read<u32>(kinterrupt));
    g_irq_cv.notify_all();
}

void DisconnectInterrupt(u32 kinterrupt)
{
    std::lock_guard lock(g_irq_mutex);
    std::erase(g_interrupts, kinterrupt);
}

void QueueDpc(u32 kdpc)
{
    {
        std::lock_guard lock(g_irq_mutex);
        g_dpcs.push_back(kdpc);
    }
    g_irq_cv.notify_all();
}

void SetDisplayMode(u32 mode, u32 format, u32 pitch, u32 framebuffer)
{
    std::lock_guard lock(g_irq_mutex);
    g_display = {mode, format, pitch, framebuffer};
    Log("devices: display mode %#x, format %#x, pitch %u, framebuffer %#x", mode, format, pitch, framebuffer);
}

void SetTickCountVariable(u32 address)
{
    g_tick_count_variable = address;
}

std::atomic<u32> g_system_time_variable{0}, g_interrupt_time_variable{0};
void SetTimeVariable(bool system_time, u32 address)
{
    (system_time ? g_system_time_variable : g_interrupt_time_variable) = address;
}

namespace {
// a KSYSTEM_TIME as the clock interrupt writes it: High2Time, LowPart, High1Time (a reader takes
// High1Time, LowPart, then checks High2Time against it)
void WriteSystemTime(u32 at, s64 value)
{
    Write<u32>(at + 8, u32(u64(value) >> 32));
    Write<u32>(at, u32(value));
    Write<u32>(at + 4, u32(u64(value) >> 32));
}
} // namespace

void StartVblank()
{
    // KeTickCount: milliseconds since start
    std::thread([] {
        const auto start = std::chrono::steady_clock::now();
        while (!g_quit)
        {
            if (const u32 v = g_tick_count_variable)
                Write<u32>(v, u32(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - start).count()));
            if (const u32 v = g_system_time_variable)
                WriteSystemTime(v, SystemTime());
            if (const u32 v = g_interrupt_time_variable)
                WriteSystemTime(v, InterruptTime());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
    // the interrupt thread: vblank at 60 Hz, service routines of asserted lines, DPCs
    GuestThread* t = NewGuestThread(0x10000, 0, "interrupts");
    StartHostThread(t, [t] {
        auto next = std::chrono::steady_clock::now();
        while (!g_quit)
        {
            {
                std::unique_lock lock(g_irq_mutex);
                g_irq_cv.wait_until(lock, next, [] {
                    if (!g_dpcs.empty() || g_quit)
                        return true;
                    for (u32 i : g_interrupts)
                        if (g_lines & (1u << (Read<u32>(i + 8) & 31)))
                            return true;
                    return false;
                });
            }
            if (std::chrono::steady_clock::now() >= next)
            {
                next += std::chrono::microseconds(16683);
                g_vblanks++;
                // what the GPU drew, in memory for the picture (and anything else reading it)
                if (gpu::Enabled())
                    gpu::Sync();
                Nv2aVblank();
            }
            RunInterrupts(t);
            RunDpcs(t);
        }
    });
}

DisplayInfo GetDisplay()
{
    Display d;
    {
        std::lock_guard lock(g_irq_mutex);
        d = g_display;
    }
    const u32 start = Nv2aDisplayStart();
    DisplayInfo info;
    info.format = d.format;
    info.bpp = d.format == 0x12 ? 4 : d.format == 0x11 || d.format == 0x10 ? 2 : 4;
    info.pitch = d.pitch ? d.pitch : 640 * 4;
    info.width = int(info.pitch / info.bpp);
    info.height = info.width >= 1920 ? 1080 : info.width >= 1280 ? 720 : 480;
    info.address = PhysToGuest(start ? start : d.framebuffer);
    info.valid = d.pitch != 0 && info.address != 0;
    return info;
}

u32 DisplayPixel(const DisplayInfo& d, int x, int y)
{
    const u32 p = d.address + u32(y) * d.pitch + u32(x) * d.bpp;
    if (d.bpp == 4)
        return Read<u32>(p) & 0xffffff;
    const u16 v = Read<u16>(p);
    if (d.format == 0x11)
        return u32((v >> 11) << 3) << 16 | u32(((v >> 5) & 63) << 2) << 8 | u32((v & 31) << 3);
    return u32(((v >> 10) & 31) << 3) << 16 | u32(((v >> 5) & 31) << 3) << 8 | u32((v & 31) << 3);
}

bool SaveFramebuffer(const std::string& path)
{
    if (gpu::Enabled())
        gpu::Sync();
    const DisplayInfo d = GetDisplay();
    if (!d.valid)
    {
        Log("devices: no picture to save (no display mode set)");
        return false;
    }
    std::vector<u8> rgb(size_t(d.width) * d.height * 3);
    for (int y = 0; y < d.height; y++)
        for (int x = 0; x < d.width; x++)
        {
            const u32 v = DisplayPixel(d, x, y);
            u8* o = &rgb[(size_t(y) * d.width + x) * 3];
            o[0] = u8(v >> 16), o[1] = u8(v >> 8), o[2] = u8(v);
        }
    Log("devices: framebuffer at %#x (%dx%d, format %#x) -> %s; %llu vblanks", d.address, d.width, d.height, d.format,
        path.c_str(), (unsigned long long)g_vblanks.load());
    return WritePng(path, d.width, d.height, rgb);
}

} // namespace xb
