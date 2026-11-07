// Devices: the NV2A's registers as plain memory with the few that must answer (the card's ID, the
// memory size, vblank status and interrupts), the vblank interrupt (service routines and DPCs run on
// an "interrupt" guest thread), KeTickCount, and the displayed picture saved as a PNG.
#include <chrono>
#include <cstdio>
#include <deque>
#include <thread>
#include <vector>

#include "core.h"
#include "kernel.h"

namespace xb {

namespace {

constexpr u32 kNv2aSize = 0x01000000;
std::vector<u8> g_nv2a(kNv2aSize);
std::mutex g_dev;

// NV2A registers (offsets from 0xfd000000)
constexpr u32 NV_PMC_BOOT_0 = 0x000000, NV_PMC_INTR_0 = 0x000100, NV_PMC_INTR_EN_0 = 0x000140,
              NV_PFB_CFG0 = 0x100200, NV_PCRTC_INTR = 0x600100, NV_PCRTC_INTR_EN = 0x600140,
              NV_PCRTC_START = 0x600800, NV_PCIO_CRTC_STATUS = 0x6013da;

u32 Reg(u32 offset)
{
    u32 v;
    std::memcpy(&v, &g_nv2a[offset], 4);
    return v;
}
void SetReg(u32 offset, u32 v)
{
    std::memcpy(&g_nv2a[offset], &v, 4);
}

std::vector<u32> g_interrupts; // connected KINTERRUPTs
std::deque<u32> g_dpcs;
std::condition_variable g_dpc_cv;
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

void RunDpcs(GuestThread* t)
{
    for (;;)
    {
        u32 dpc;
        {
            std::lock_guard lock(g_dev);
            if (g_dpcs.empty())
                return;
            dpc = g_dpcs.front();
            g_dpcs.pop_front();
        }
        Write<u8>(dpc + 2, 0);
        // DeferredRoutine(Dpc, DeferredContext, SystemArgument1, SystemArgument2)
        CallGuest(t, Read<u32>(dpc + 12), {dpc, Read<u32>(dpc + 16), Read<u32>(dpc + 20), Read<u32>(dpc + 24)});
    }
}

} // namespace

void InitDevices()
{
    SetReg(NV_PMC_BOOT_0, 0x02A000A2); // NV2A A2
    SetReg(NV_PFB_CFG0, 0x03070103);   // 64 MiB
}

bool DeviceRead(u32 address, u32 size, u32& value)
{
    value = 0;
    if (address < kDeviceBase || address - kDeviceBase + size > kNv2aSize)
        return true; // the other devices read as 0
    const u32 offset = address - kDeviceBase;
    std::lock_guard lock(g_dev);
    if (offset == NV_PCIO_CRTC_STATUS)
    {
        // bit 3: in vertical retrace; toggles so that polling loops finish
        static u32 n = 0;
        value = (++n & 8) ? 0x08 : 0x00;
        return true;
    }
    std::memcpy(&value, &g_nv2a[offset], size);
    return true;
}

bool DeviceWrite(u32 address, u32 size, u32 value)
{
    if (address < kDeviceBase || address - kDeviceBase + size > kNv2aSize)
        return true;
    const u32 offset = address - kDeviceBase;
    std::lock_guard lock(g_dev);
    if (offset == NV_PCRTC_INTR || offset == NV_PMC_INTR_0)
    {
        SetReg(offset, Reg(offset) & ~value); // write 1 to clear
        return true;
    }
    std::memcpy(&g_nv2a[offset], &value, size);
    if (offset == NV_PCRTC_START && g_verbose >= 1)
        Log("nv2a: display start %#x", value);
    return true;
}

void ConnectInterrupt(u32 vector, u32 kinterrupt)
{
    std::lock_guard lock(g_dev);
    g_interrupts.push_back(kinterrupt);
    Log("devices: interrupt %u connected (service routine %#x)", vector, Read<u32>(kinterrupt));
}

void DisconnectInterrupt(u32 kinterrupt)
{
    std::lock_guard lock(g_dev);
    std::erase(g_interrupts, kinterrupt);
}

void QueueDpc(u32 kdpc)
{
    {
        std::lock_guard lock(g_dev);
        g_dpcs.push_back(kdpc);
    }
    g_dpc_cv.notify_all();
}

void SetDisplayMode(u32 mode, u32 format, u32 pitch, u32 framebuffer)
{
    std::lock_guard lock(g_dev);
    g_display = {mode, format, pitch, framebuffer};
    Log("devices: display mode %#x, format %#x, pitch %u, framebuffer %#x", mode, format, pitch, framebuffer);
}

void SetTickCountVariable(u32 address)
{
    g_tick_count_variable = address;
}

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
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
    // vblank at 60 Hz: the interrupt (VBL_IRQ 3) when enabled, then the DPCs; DPCs queued between
    // vblanks run as they come
    GuestThread* t = NewGuestThread(0x10000, 0, "interrupts");
    StartHostThread(t, [t] {
        auto next = std::chrono::steady_clock::now();
        while (!g_quit)
        {
            {
                std::unique_lock lock(g_dev);
                g_dpc_cv.wait_until(lock, next, [] { return !g_dpcs.empty() || g_quit.load(); });
            }
            RunDpcs(t);
            if (std::chrono::steady_clock::now() < next)
                continue;
            next += std::chrono::microseconds(16683);
            g_vblanks++;
            std::vector<u32> isrs;
            {
                std::lock_guard lock(g_dev);
                const bool enabled = (Reg(NV_PCRTC_INTR_EN) & 1) && (Reg(NV_PMC_INTR_EN_0) & 1);
                if (!enabled)
                    continue;
                SetReg(NV_PCRTC_INTR, Reg(NV_PCRTC_INTR) | 1);
                SetReg(NV_PMC_INTR_0, Reg(NV_PMC_INTR_0) | (1u << 24));
                for (u32 i : g_interrupts)
                    if (Read<u32>(i + 8) == 3)
                        isrs.push_back(i);
            }
            for (u32 i : isrs)
                CallGuest(t, Read<u32>(i), {i, Read<u32>(i + 4)}); // ServiceRoutine(Interrupt, ServiceContext)
            RunDpcs(t);
        }
    });
}

bool SaveFramebuffer(const std::string& path)
{
    Display d;
    u32 start;
    {
        std::lock_guard lock(g_dev);
        d = g_display;
        start = Reg(NV_PCRTC_START);
    }
    const u32 bpp = d.format == 0x12 ? 4 : d.format == 0x11 || d.format == 0x10 ? 2 : 4;
    const u32 pitch = d.pitch ? d.pitch : 640 * 4;
    const int w = int(pitch / bpp);
    const int h = w >= 1920 ? 1080 : w >= 1280 ? 720 : 480;
    const u32 phys = start ? start : d.framebuffer & 0x0fffffff;
    const u32 at = phys < kContiguousSize ? kContiguousBase + phys : phys;
    std::vector<u8> rgb(size_t(w) * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            const u32 p = at + u32(y) * pitch + u32(x) * bpp;
            u8* o = &rgb[(size_t(y) * w + x) * 3];
            if (bpp == 4)
            {
                const u32 v = Read<u32>(p);
                o[0] = u8(v >> 16), o[1] = u8(v >> 8), o[2] = u8(v);
            }
            else
            {
                const u16 v = Read<u16>(p);
                if (d.format == 0x11)
                    o[0] = u8((v >> 11) << 3), o[1] = u8(((v >> 5) & 63) << 2), o[2] = u8((v & 31) << 3);
                else
                    o[0] = u8(((v >> 10) & 31) << 3), o[1] = u8(((v >> 5) & 31) << 3), o[2] = u8((v & 31) << 3);
            }
        }
    Log("devices: framebuffer at %#x (%dx%d, format %#x) -> %s; %llu vblanks", at, w, h, d.format, path.c_str(),
        (unsigned long long)g_vblanks.load());
    return WritePng(path, w, h, rgb);
}

} // namespace xb
