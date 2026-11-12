// Devices: the table of memory-mapped devices (an access faults and the fault handler calls them),
// level-triggered interrupt lines whose connected service routines run on an "interrupt" guest
// thread, DPCs, the vblank tick, KeTickCount, and the displayed picture (PNG on the desktop).
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <deque>
#include <unordered_map>
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
// the service routine the interrupt thread is in (its KINTERRUPT), 0 none: KeDisconnectInterrupt
// waits it out, as the kernel's synchronizes with the interrupt, so no routine runs after it returns
u32 g_isr_running = 0;
std::condition_variable g_isr_done;
std::atomic<GuestThread*> g_interrupt_thread{nullptr};
std::deque<u32> g_dpcs;
// DPCs queued by interrupt routines (counted) and finished or removed (counted): a thread at normal
// level waits for the ones queued before it to finish where the single-processor Xbox would have
// run them already (KeDisconnectInterrupt)
u64 g_dpcs_queued = 0, g_dpcs_done = 0;
std::atomic<u32> g_tick_count_variable{0};
std::atomic<u64> g_vblanks{0};

// The single processor's exclusion (see SetIrql). Owned by a guest thread, recursively; a wait longer
// than kExclusionWait (a guest bug, or a wait the console would not have made at that level) goes on
// without it, logged, instead of hanging.
class Exclusion
{
public:
    explicit Exclusion(const char* name) : name_(name) {}
    void Acquire()
    {
        GuestThread* me = CurrentThread();
        std::unique_lock lock(m_);
        if (owner_ == me)
        {
            count_++;
            return;
        }
        if (!cv_.wait_for(lock, kExclusionWait, [&] { return owner_ == nullptr; }))
        {
            if (timeouts_++ < 20)
                Log("devices: %s held too long by another thread: going on without it", name_);
            unowned_[me]++;
            return;
        }
        owner_ = me;
        count_ = 1;
    }
    // without waiting: false if another thread holds it
    bool TryAcquire()
    {
        GuestThread* me = CurrentThread();
        std::lock_guard lock(m_);
        if (owner_ != me && owner_ != nullptr)
            return false;
        owner_ = me;
        count_++;
        return true;
    }
    // taken without its owner letting go (held off too long): logged, as Acquire's timeout
    void Override()
    {
        GuestThread* me = CurrentThread();
        std::lock_guard lock(m_);
        if (owner_ == me || owner_ == nullptr)
        {
            owner_ = me;
            count_++;
            return;
        }
        if (timeouts_++ < 20)
            Log("devices: %s held too long by another thread: going on without it", name_);
        unowned_[me]++;
    }
    void Release()
    {
        GuestThread* me = CurrentThread();
        std::unique_lock lock(m_);
        if (owner_ != me)
        {
            auto it = unowned_.find(me);
            if (it != unowned_.end() && --it->second == 0)
                unowned_.erase(it);
            return;
        }
        if (--count_ == 0)
        {
            owner_ = nullptr;
            lock.unlock();
            cv_.notify_all();
        }
    }

private:
    static constexpr auto kExclusionWait = std::chrono::milliseconds(250);
    const char* name_;
    std::mutex m_;
    std::condition_variable cv_;
    GuestThread* owner_ = nullptr;
    u32 count_ = 0;
    std::unordered_map<GuestThread*, u32> unowned_;
    u32 timeouts_ = 0;
};
Exclusion g_dispatch_level("dispatch level"); // DPCs and threads at dispatch level
Exclusion g_device_level("device level");     // interrupt service routines and what holds them off
thread_local bool t_in_isr = false;            // in a service routine: its level is held already

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

// DPCs queued while another thread is at dispatch level wait for it to lower its level, as on the
// console (where interrupt routines still run meanwhile: the interrupt thread goes on with them);
// since when they have waited (none: zero), and past 250 ms they run anyway (logged)
std::chrono::steady_clock::time_point g_dpcs_held_since{};

void RunDpcs(GuestThread* t)
{
    {
        std::lock_guard lock(g_irq_mutex);
        if (g_dpcs.empty())
        {
            g_dpcs_held_since = {};
            return;
        }
    }
    if (!g_dispatch_level.TryAcquire())
    {
        const auto now = std::chrono::steady_clock::now();
        if (g_dpcs_held_since == std::chrono::steady_clock::time_point{})
            g_dpcs_held_since = now;
        if (now - g_dpcs_held_since < std::chrono::milliseconds(250))
            return; // left queued
        g_dispatch_level.Override();
    }
    g_dpcs_held_since = {};
    struct Held
    {
        ~Held() { g_dispatch_level.Release(); }
    } held;
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
        struct Done
        {
            ~Done()
            {
                {
                    std::lock_guard lock(g_irq_mutex);
                    g_dpcs_done++;
                }
                g_isr_done.notify_all();
            }
        } done;
        // DeferredRoutine(Dpc, DeferredContext, SystemArgument1, SystemArgument2), at dispatch level
        // (held above)
        Write<u8>(t->kpcr + kpcr::Irql, 2);
        Write<u32>(t->kpcr + kpcr::DpcRoutineActive, 1);
        StopGuestThreads(t); // DPCs too run before any thread at normal level goes on
        CallGuest(t, Read<u32>(dpc + 12), {dpc, Read<u32>(dpc + 16), Read<u32>(dpc + 20), Read<u32>(dpc + 24)});
        ResumeGuestThreads();
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
            {
                // still connected? (a KeDisconnectInterrupt since the list was taken: not called)
                std::lock_guard lock(g_irq_mutex);
                if (std::find(g_interrupts.begin(), g_interrupts.end(), i) == g_interrupts.end())
                    continue;
                g_isr_running = i;
            }
            // above dispatch level: not held off by threads at dispatch level, as on the console
            g_device_level.Acquire();
            t_in_isr = true;
            Write<u8>(t->kpcr + kpcr::Irql, u8(std::max<u32>(3, Read<u8>(i + 0xc))));
            StopGuestThreads(t); // between two instructions of every other thread, as on the console
            CallGuest(t, Read<u32>(i), {i, Read<u32>(i + 4)});
            ResumeGuestThreads();
            Write<u8>(t->kpcr + kpcr::Irql, 0);
            t_in_isr = false;
            g_device_level.Release();
            {
                std::lock_guard lock(g_irq_mutex);
                g_isr_running = 0;
            }
            g_isr_done.notify_all();
        }
        RunDpcs(t);
    }
}

} // namespace

void SetIrql(u8 irql)
{
    GuestThread* t = CurrentThread();
    const u8 old = Read<u8>(t->kpcr + kpcr::Irql);
    if (!t_in_isr)
    {
        if (old < 2 && irql >= 2)
            g_dispatch_level.Acquire();
        if (old < 3 && irql >= 3)
            g_device_level.Acquire();
        if (old >= 3 && irql < 3)
            g_device_level.Release();
        if (old >= 2 && irql < 2)
            g_dispatch_level.Release();
    }
    Write<u8>(t->kpcr + kpcr::Irql, irql);
}

u32 SynchronizeWithInterrupt(u32 kinterrupt, u32 routine, u32 context)
{
    const u8 old = Read<u8>(CurrentThread()->kpcr + kpcr::Irql);
    SetIrql(u8(std::max<u32>({3u, old, Read<u8>(kinterrupt + 0xc)})));
    const u32 r = u32(CallGuestNested(routine, {context}));
    SetIrql(old);
    return r;
}

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
    InitNic();
}

const char* DeviceName(u32 address)
{
    const Device* d = Find(address);
    return d ? d->name : "no device";
}

bool DeviceRead(u32 address, u32 size, u32& value, u32 eip)
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
    diag::DeviceAccess(address, size, value, false, eip);
    if (g_verbose >= 3)
        Log("dev read  %#010x/%u = %#x", address, size, value);
    return true; // what nothing answers reads as 0
}

bool DeviceWrite(u32 address, u32 size, u32 value, u32 eip)
{
    if (g_verbose >= 3)
        Log("dev write %#010x/%u = %#x", address, size, value);
    diag::DeviceAccess(address, size, value, true, eip);
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
    std::unique_lock lock(g_irq_mutex);
    std::erase(g_interrupts, kinterrupt);
    // a routine of it running now finishes before this returns (not when the routine itself, on the
    // interrupt thread, disconnects)
    // and, as on the single-processor Xbox where they run before any thread at normal level does,
    // the DPCs interrupt routines queued before this
    if (CurrentThread() != g_interrupt_thread.load())
    {
        const u64 queued = g_dpcs_queued;
        g_isr_done.wait_for(lock, std::chrono::seconds(2), [&] {
            return (g_isr_running != kinterrupt && g_dpcs_done >= queued) || g_quit;
        });
    }
}

void QueueDpc(u32 kdpc)
{
    {
        std::lock_guard lock(g_irq_mutex);
        g_dpcs.push_back(kdpc);
        g_dpcs_queued++;
    }
    g_irq_cv.notify_all();
}

bool RemoveQueuedDpc(u32 kdpc)
{
    bool removed = false;
    {
        std::lock_guard lock(g_irq_mutex);
        const size_t before = g_dpcs.size();
        std::erase(g_dpcs, kdpc);
        removed = g_dpcs.size() != before;
        g_dpcs_done += before - g_dpcs.size();
    }
    if (removed)
        g_isr_done.notify_all();
    return removed;
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

void StartPictureProbe();

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
    StartPictureProbe();
    // the interrupt thread: vblank at 60 Hz, service routines of asserted lines, DPCs
    GuestThread* t = NewGuestThread(0x10000, 0, "interrupts");
    g_interrupt_thread = t;
    StartHostThread(t, [t] {
        auto next = std::chrono::steady_clock::now();
        while (!g_quit)
        {
            {
                std::unique_lock lock(g_irq_mutex);
                // DPCs held off by a thread at dispatch level: looked at again each millisecond
                const bool held = g_dpcs_held_since != std::chrono::steady_clock::time_point{};
                const auto until = held ? std::min(next, std::chrono::steady_clock::now() + std::chrono::milliseconds(1))
                                        : next;
                g_irq_cv.wait_until(lock, until, [held] {
                    if ((!g_dpcs.empty() && !held) || g_quit)
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

// The picture's geometry from the display mode and start address; with the overlay's (PVIDEO's
// registers, read through the device: not from the renderer's thread, which holds PGRAPH's lock
// while the device's big lock waits for it) if asked
DisplayInfo DisplayOf(const Display& d, u32 start, bool overlay);
namespace {
// the latest whole picture (TakePicture), where the renderer draws apart from RAM
std::mutex g_picture_mutex;
std::shared_ptr<const std::vector<u8>> g_picture;
u32 g_picture_address = 0;
} // namespace

const u8* DisplayInfo::Bytes() const
{
    return pixels ? pixels->data() : Ptr<const u8>(address);
}

void TakePicture()
{
    Display d;
    {
        std::lock_guard lock(g_irq_mutex);
        d = g_display;
    }
    const DisplayInfo info = DisplayOf(d, Nv2aDisplayStart(), false);
    const size_t bytes = size_t(info.pitch) * info.height;
    if (!info.valid || !GuestMapped(info.address, u32(bytes)))
        return;
    auto copy = std::make_shared<std::vector<u8>>(bytes);
    std::memcpy(copy->data(), Ptr<const u8>(info.address), bytes);
    std::lock_guard lock(g_picture_mutex);
    g_picture = std::move(copy);
    g_picture_address = info.address;
}

// XB_PICTURE_PROBE=x,y,rrggbb: a test's eye on the picture as a presenter reads it: every 2 ms the
// pixel at (x, y) of what GetDisplay gives, once it first shows rrggbb, counted when it doesn't (a
// frame half drawn shown); logged each second
void StartPictureProbe()
{
    const char* probe = std::getenv("XB_PICTURE_PROBE");
    int x = 0, y = 0;
    unsigned rgb = 0;
    if (!probe || std::sscanf(probe, "%d,%d,%x", &x, &y, &rgb) != 3)
        return;
    std::thread([x, y, rgb] {
        u64 samples = 0, wrong = 0;
        bool seen = false;
        auto next_log = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!g_quit)
        {
            const DisplayInfo d = GetDisplay();
            if (d.valid && x < d.width && y < d.height && GuestMapped(d.address, d.pitch * u32(d.height)))
            {
                const u32 v = DisplayPixel(d, x, y);
                seen = seen || v == rgb;
                if (seen)
                {
                    samples++;
                    if (v != rgb)
                        wrong++;
                }
            }
            if (std::chrono::steady_clock::now() >= next_log)
            {
                next_log += std::chrono::seconds(1);
                Log("picture probe: %llu samples, %llu not %06x at (%d, %d)", (unsigned long long)samples,
                    (unsigned long long)wrong, rgb, x, y);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }).detach();
}

DisplayInfo GetDisplay()
{
    Display d;
    {
        std::lock_guard lock(g_irq_mutex);
        d = g_display;
    }
    const u32 start = Nv2aDisplayStart();
    DisplayInfo info = DisplayOf(d, start, true);
    if (PictureIsCopied())
    {
        // the latest whole picture (its own address: until the frame flipped to is back in RAM, the
        // one before it, complete, is shown)
        std::lock_guard lock(g_picture_mutex);
        if (g_picture && g_picture->size() == size_t(info.pitch) * info.height)
            info.pixels = g_picture, info.address = g_picture_address;
    }
    return info;
}

DisplayInfo DisplayOf(const Display& d, u32 start, bool overlay)
{
    DisplayInfo info;
    info.format = d.format;
    info.bpp = d.format == 0x12 ? 4 : d.format == 0x11 || d.format == 0x10 ? 2 : 4;
    info.pitch = d.pitch ? d.pitch : 640 * 4;
    info.width = int(info.pitch / info.bpp);
    info.height = info.width >= 1920 ? 1080 : info.width >= 1280 ? 720 : 480;
    info.address = PhysToGuest(start ? start : d.framebuffer);
    info.valid = d.pitch != 0 && info.address != 0;
    if (!overlay)
        return info;
    // the video overlay, as xemu reads it (pgraph/vk/display.c: get_pvideo_state)
    DisplayInfo::Overlay& o = info.overlay;
    const u32 size_in = Nv2aPvideo(0x928);
    o.on = (Nv2aPvideo(0x700) & 1) && size_in != 0xffffffff && (Nv2aPvideo(0x958) >> 16 & 3) == 1;
    if (o.on)
    {
        const u32 format = Nv2aPvideo(0x958), size_out = Nv2aPvideo(0x950), point_in = Nv2aPvideo(0x930),
                  point_out = Nv2aPvideo(0x948), ds_dx = Nv2aPvideo(0x938), dt_dy = Nv2aPvideo(0x940);
        o.address = PhysToGuest(Nv2aPvideo(0x900) + Nv2aPvideo(0x920));
        o.pitch = format & 0x1fff;
        o.in_w = int(size_in & 0x7ff), o.in_h = int((size_in >> 16) & 0x7ff);
        o.out_w = int(size_out & 0xfff), o.out_h = int((size_out >> 16) & 0xfff);
        o.out_x = int(point_out & 0xfff), o.out_y = int((point_out >> 16) & 0xfff);
        o.in_x = float(point_in & 0x7fff) / 16.f, o.in_y = float(point_in >> 17) / 8.f;
        auto scale = [](u32 din_dout, int out) {
            if (din_dout == 0x100000 || out <= 0)
                return 1.f;
            const float in = std::floor(float(din_dout) * float(out - 1) / float(1 << 20) + 0.5f);
            return (in + 1.f) / float(out);
        };
        o.scale_x = scale(ds_dx, o.out_w), o.scale_y = scale(dt_dy, o.out_h);
        if (o.in_w > o.out_w)
            o.in_w = int(std::floor(float(o.out_w) * o.scale_x + 0.5f));
        if (o.in_h > o.out_h)
            o.in_h = int(std::floor(float(o.out_h) * o.scale_y + 0.5f));
        o.color_key = (format >> 20) & 1;
        o.key = Nv2aPvideo(0xb00) & 0xffffff;
        o.on = o.address && o.pitch && o.in_w > 0 && o.in_h > 0 && o.out_w > 0 && o.out_h > 0 &&
               GuestMapped(o.address, o.pitch * u32(o.in_h));
    }
    return info;
}

namespace {
u32 FramebufferPixel(const DisplayInfo& d, int x, int y)
{
    const u8* p = d.Bytes() + size_t(y) * d.pitch + size_t(x) * d.bpp;
    if (d.bpp == 4)
    {
        u32 v;
        std::memcpy(&v, p, 4);
        return v & 0xffffff;
    }
    u16 v;
    std::memcpy(&v, p, 2);
    if (d.format == 0x11)
        return u32((v >> 11) << 3) << 16 | u32(((v >> 5) & 63) << 2) << 8 | u32((v & 31) << 3);
    return u32(((v >> 10) & 31) << 3) << 16 | u32(((v >> 5) & 31) << 3) << 8 | u32((v & 31) << 3);
}
} // namespace

u32 DisplayPixel(const DisplayInfo& d, int x, int y)
{
    const u32 fb = FramebufferPixel(d, x, y);
    const DisplayInfo::Overlay& o = d.overlay;
    if (!o.on || x < o.out_x || y < o.out_y || x >= o.out_x + o.out_w || y >= o.out_y + o.out_h ||
        (o.color_key && fb != o.key))
        return fb;
    // the overlay's texel (nearest), YUY2 to RGB as xemu converts it (BT.601, video range)
    const int ix = std::clamp(int(o.in_x + float(x - o.out_x) * o.scale_x), 0, o.in_w - 1);
    const int iy = std::clamp(int(o.in_y + float(y - o.out_y) * o.scale_y), 0, o.in_h - 1);
    const u32 line = o.address + u32(iy) * o.pitch;
    const int c = int(Read<u8>(line + u32(ix) * 2)) - 16;
    int dd, e;
    if (ix % 2)
        dd = int(Read<u8>(line + u32(ix) * 2 - 1)) - 128, e = int(Read<u8>(line + u32(ix) * 2 + 1)) - 128;
    else
        dd = int(Read<u8>(line + u32(ix) * 2 + 1)) - 128, e = int(Read<u8>(line + u32(ix) * 2 + 3)) - 128;
    auto clip = [](int v) { return u32(std::clamp(v, 0, 255)); };
    return clip((298 * c + 409 * e + 128) >> 8) << 16 | clip((298 * c - 100 * dd - 208 * e + 128) >> 8) << 8 |
           clip((298 * c + 516 * dd + 128) >> 8);
}

bool SaveFramebuffer(const std::string& path)
{
    if (gpu::Enabled())
        gpu::Sync();
    return SaveShownPicture(path);
}

bool SaveShownPicture(const std::string& path)
{
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
