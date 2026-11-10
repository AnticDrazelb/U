// The MCPX's AC'97 controller at 0xfec00000 (IRQ 6), as nxdk's audio drives it: the codec's
// registers, and the bus masters' descriptor lists (PCM in 0x100, PCM out 0x110, S/PDIF 0x170)
// played in real time at 48 kHz. PCM out's samples (16-bit stereo) go to the sound sink; S/PDIF is
// consumed in step with it. (Games use the APU at 0xfe800000 through DirectSound; that is separate.)
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "core.h"
#include "devices.h"

namespace xb {

void (*g_audio_sink)(const s16* samples, size_t frames) = nullptr;
size_t (*g_audio_queued)() = nullptr;

namespace {

// the APU's sound (apu.cpp), mixed into what the AC'97 plays: a ring of stereo frames
std::mutex g_apu_mutex;
std::vector<s16> g_apu_ring(48000 * 2 / 4); // a quarter second
size_t g_apu_read = 0, g_apu_fill = 0;

// for the stats line: the APU's frames come in, the mixer short of them, the loudest of each
std::atomic<u64> g_apu_in{0}, g_mixer_short{0}, g_mixer_ticks{0}, g_sink_frames{0};
std::atomic<int> g_apu_peak{0}, g_ac97_peak{0};

// Takes up to `frames` APU frames, added into `pcm`; returns how many there were.
size_t TakeApuSound(s16* pcm, size_t frames)
{
    std::lock_guard lock(g_apu_mutex);
    const size_t capacity = g_apu_ring.size() / 2;
    const size_t n = std::min(frames, g_apu_fill);
    if (n && n < frames)
        g_mixer_short++;
    for (size_t i = 0; i < n; i++)
    {
        const size_t at = (g_apu_read + i) % capacity;
        for (int c = 0; c < 2; c++)
            pcm[i * 2 + c] = s16(std::clamp(int(pcm[i * 2 + c]) + int(g_apu_ring[at * 2 + c]), -32768, 32767));
    }
    g_apu_read = (g_apu_read + n) % capacity;
    g_apu_fill -= n;
    return n;
}

constexpr u32 kBase = 0xfec00000, kSize = 0x1000, kIrq = 6;
constexpr u32 kGlobalStatus = 0x130;
constexpr u32 kBoxes[3] = {0x100, 0x110, 0x170};
// a box's registers: BDBAR +0, CIV +4, LVI +5, SR +6, PICB +8, PIV +0xa, CR +0xb
constexpr u32 SR_DCH = 1, SR_CELV = 2, SR_LVBCI = 4, SR_BCIS = 8, SR_FIFOE = 16;
constexpr u32 CR_RUN = 1, CR_RESET = 2, CR_LVBIE = 4, CR_FEIE = 8, CR_IOCE = 16;

std::mutex g_m;
u8 g_regs[kSize] = {};

u8& B(u32 o) { return g_regs[o]; }
u16 W(u32 o) { u16 v; std::memcpy(&v, &g_regs[o], 2); return v; }
void SetW(u32 o, u16 v) { std::memcpy(&g_regs[o], &v, 2); }
u32 D(u32 o) { u32 v; std::memcpy(&v, &g_regs[o], 4); return v; }

void UpdateIrq()
{
    bool level = false;
    for (u32 box : kBoxes)
    {
        const u16 sr = W(box + 6);
        const u8 cr = B(box + 0xb);
        level |= ((sr & SR_BCIS) && (cr & CR_IOCE)) || ((sr & SR_LVBCI) && (cr & CR_LVBIE)) ||
                 ((sr & SR_FIFOE) && (cr & CR_FEIE));
    }
    SetIrq(kIrq, level);
}

// A box's state beyond its registers: the descriptor being played (QEMU's bd: address, control and
// length), loaded when the box starts, advances or is restarted.
struct Descriptor
{
    u32 address = 0, ctl_len = 0;
    bool valid = false;
};
Descriptor g_bd[3];
int BoxIndex(u32 box) { return box == 0x100 ? 0 : box == 0x110 ? 1 : 2; }

// QEMU's fetch_bd: the descriptor at CIV, PICB its length (samples)
void FetchDescriptor(u32 box)
{
    Descriptor& bd = g_bd[BoxIndex(box)];
    const u32 at = PhysToGuest(D(box)) + B(box + 4) * 8u;
    bd.address = at >= 8 ? PhysToGuest(Read<u32>(at)) : 0;
    bd.ctl_len = at >= 8 ? Read<u32>(at + 4) : 0;
    bd.valid = true;
    SetW(box + 8, u16(bd.ctl_len & 0xffff));
}

// QEMU's next descriptor: CIV takes PIV, PIV moves on
void NextDescriptor(u32 box)
{
    B(box + 4) = B(box + 0xa);
    B(box + 0xa) = u8((B(box + 0xa) + 1) % 32);
    FetchDescriptor(box);
}

// QEMU's reset_bm_regs: everything cleared but the interrupt enables, halted
void ResetBox(u32 box)
{
    std::memset(&g_regs[box], 0, 4);         // BDBAR
    B(box + 4) = B(box + 5) = B(box + 0xa) = 0; // CIV, LVI, PIV
    SetW(box + 8, 0);                         // PICB
    SetW(box + 6, SR_DCH);
    B(box + 0xb) &= CR_IOCE | CR_FEIE | CR_LVBIE;
    g_bd[BoxIndex(box)] = {};
}

// Plays `samples` 16-bit samples of a box (QEMU's transfer_audio for an output box); returns how many
// came from guest memory (the rest is silence, when it ran dry). Under g_m.
size_t Play(u32 box, s16* out, size_t samples)
{
    size_t done = 0;
    Descriptor& bd = g_bd[BoxIndex(box)];
    if (!(B(box + 0xb) & CR_RUN) || (W(box + 6) & SR_DCH))
        return 0;
    while (done < samples)
    {
        if (!bd.valid)
            FetchDescriptor(box);
        u16 picb = W(box + 8);
        if (!picb)
        {
            // a fresh descriptor that is empty
            if (B(box + 4) == B(box + 5))
            {
                SetW(box + 6, W(box + 6) | SR_DCH);
                break;
            }
            SetW(box + 6, W(box + 6) & ~SR_CELV);
            NextDescriptor(box);
            break;
        }
        const size_t n = std::min<size_t>(picb, samples - done);
        for (size_t i = 0; i < n; i++)
            out[done + i] = bd.address ? Read<s16>(bd.address + u32(i) * 2) : 0;
        bd.address += u32(n) * 2;
        done += n;
        picb = u16(picb - n);
        SetW(box + 8, picb);
        if (picb)
            continue;
        // the buffer is done
        u16 sr = u16(W(box + 6) & ~SR_CELV);
        if (bd.ctl_len & 0x80000000u) // IOC
            sr |= SR_BCIS;
        if (B(box + 4) == B(box + 5)) // it was the last valid one: halt
        {
            sr |= SR_LVBCI | SR_DCH | SR_CELV;
            SetW(box + 6, sr);
            break;
        }
        NextDescriptor(box);
        SetW(box + 6, sr);
    }
    return done;
}

// The codec's registers at reset (QEMU's mixer_reset: a SigmaTel STAC9700; 48 kHz everywhere, master
// muted, PCM out and record gain at their reset volumes, every section ready)
void ResetMixer()
{
    std::memset(g_regs, 0, 0x80);
    const std::pair<u32, u16> kReset[] = {{0x26, 0x000f}, {0x7c, 0x8384}, {0x7e, 0x7600}, {0x28, 0x0809},
                                          {0x2a, 0x0009}, {0x2c, 0xbb80}, {0x2e, 0xbb80}, {0x30, 0xbb80},
                                          {0x32, 0xbb80}, {0x34, 0xbb80}, {0x02, 0x8000}, {0x18, 0x8808},
                                          {0x1c, 0x8808}};
    for (const auto& [reg, value] : kReset)
        SetW(reg, value);
}

void Player()
{
    // 10 ms at a time: 480 stereo frames, on the clock; where the sink says what it holds (the
    // console's sound device, which plays on its own clock) a little faster or slower to keep about
    // 40 ms there: the two clocks never quite agree, and a sink that ran dry or over would be heard
    constexpr size_t kFrames = 480;
    constexpr size_t kLow = 1024, kHigh = 3072; // frames queued in the sink
    std::vector<s16> pcm(kFrames * 2), spdif(kFrames * 2);
    auto next = std::chrono::steady_clock::now();
    bool fed = false; // the sink was given sound last time (its fill means something)
    while (!g_quit)
    {
        auto period = std::chrono::microseconds(10000);
        if (g_audio_queued && fed)
        {
            const size_t queued = g_audio_queued();
            if (queued < kLow)
                period = std::chrono::microseconds(9000);
            else if (queued > kHigh)
                period = std::chrono::microseconds(11000);
        }
        next += period;
        const auto now = std::chrono::steady_clock::now();
        if (now - next > std::chrono::milliseconds(100))
            next = now; // far behind (a stall): start again from now, not a burst of catching up
        std::this_thread::sleep_until(next);
        bool playing;
        {
            std::lock_guard lock(g_m);
            std::fill(pcm.begin(), pcm.end(), s16(0));
            playing = B(0x110 + 0xb) & CR_RUN;
            Play(0x110, pcm.data(), pcm.size());
            Play(0x170, spdif.data(), spdif.size());
            UpdateIrq();
        }
        int ac97_peak = 0;
        for (s16 v : pcm)
            ac97_peak = std::max(ac97_peak, std::abs(int(v)));
        if (ac97_peak > g_ac97_peak.load(std::memory_order_relaxed))
            g_ac97_peak = ac97_peak;
        if (TakeApuSound(pcm.data(), kFrames))
            playing = true;
        g_mixer_ticks++;
        if (g_audio_sink && playing)
        {
            g_audio_sink(pcm.data(), kFrames);
            g_sink_frames += kFrames;
        }
        fed = playing;
    }
}

u32 ReadReg(u32 offset, u32 size)
{
    std::lock_guard lock(g_m);
    if (offset == kGlobalStatus)
    {
        // the primary codec is ready (bit 8), and each box's pending interrupt as its own bit: PCM
        // in 0x20, PCM out 0x40, and on nForce/MCPX the S/PDIF box (bus master 0x70) 0x10 (Linux's
        // intel8x0: ICH_NVSPINT). They show the box's status, whatever its interrupt enables (those
        // gate the IRQ): DirectSound polls this with interrupts off, waiting on a buffer.
        u32 v = D(kGlobalStatus) | 0x100;
        constexpr u32 kBits[3] = {0x20, 0x40, 0x10};
        for (u32 i = 0; i < 3; i++)
            if (W(kBoxes[i] + 6) & (SR_LVBCI | SR_BCIS | SR_FIFOE))
                v |= kBits[i];
        return v;
    }
    if (offset < 0x100 && size == 2 && offset == 0x26)
        return (W(0x26) & ~0xfu) | 0x000f; // the codec's powerdown status: every section ready
    u32 v = 0;
    std::memcpy(&v, &g_regs[offset], std::min<u32>(size, kSize - offset));
    return v;
}

void WriteReg(u32 offset, u32 size, u32 value)
{
    std::lock_guard lock(g_m);
    for (u32 box : kBoxes)
    {
        if (offset == box + 0xb && size == 1) // CR (QEMU's)
        {
            if (value & CR_RESET)
                ResetBox(box);
            else
            {
                static int told = 0;
                if (((B(box + 0xb) ^ value) & CR_RUN) && told < 32)
                {
                    told++;
                    Log("ac97: box %#x %s (descriptors at %#x, last valid %u, control %#x)", box,
                        (value & CR_RUN) ? "runs" : "stops", D(box), B(box + 5), value);
                }
                B(box + 0xb) = u8(value & 0x1f);
                if (!(value & CR_RUN))
                    SetW(box + 6, W(box + 6) | SR_DCH);
                else
                {
                    NextDescriptor(box);
                    SetW(box + 6, W(box + 6) & ~SR_DCH);
                }
            }
            UpdateIrq();
            return;
        }
        if (offset == box + 6 && size <= 2) // SR: the interrupt bits are cleared by writing 1; DCH, CELV read-only
        {
            const u16 wclear = SR_LVBCI | SR_BCIS | SR_FIFOE, ro = SR_DCH | SR_CELV;
            u16 sr = u16(W(box + 6) | (value & ~(ro | wclear) & 0x1f));
            SetW(box + 6, u16(sr & ~(value & wclear)));
            UpdateIrq();
            return;
        }
        if (offset == box + 5 && size == 1) // LVI: a new last buffer restarts a halted, running box (QEMU's)
        {
            if ((B(box + 0xb) & CR_RUN) && (W(box + 6) & SR_DCH))
            {
                SetW(box + 6, W(box + 6) & ~(SR_DCH | SR_CELV));
                NextDescriptor(box);
            }
            B(box + 5) = u8(value % 32);
            return;
        }
    }
    if (offset == 0x00 && size == 2) // the codec's reset register: the mixer back to its reset values
    {
        ResetMixer();
        return;
    }
    if (offset == kGlobalStatus && size == 4)
    {
        // RCS, the resume bits and GSCI are cleared by writing 1; the status bits are read-only
        constexpr u32 kWriteClear = 0x8000 | 0x800 | 0x400 | 1, kReadOnly = 0x7000 | 0x300 | 0xfe | 0x10;
        constexpr u32 kWritable = 0x3ffff & ~(kWriteClear | kReadOnly);
        const u32 v = (D(kGlobalStatus) & ~(value & kWriteClear) & ~kWritable) | (value & kWritable);
        std::memcpy(&g_regs[offset], &v, 4);
        return;
    }
    std::memcpy(&g_regs[offset], &value, std::min<u32>(size, kSize - offset));
}

} // namespace

// what the APU's frame thread has queued here (xemu's throttle() reads it as its stream's), in bytes
int ApuQueuedBytes()
{
    std::lock_guard lock(g_apu_mutex);
    return int(g_apu_fill * 4);
}

void MixApuSound(const s16* samples, size_t frames)
{
    int peak = 0;
    for (size_t i = 0; i < frames * 2; i++)
        peak = std::max(peak, std::abs(int(samples[i])));
    if (peak > g_apu_peak.load(std::memory_order_relaxed))
        g_apu_peak = peak;
    g_apu_in += frames;
    std::lock_guard lock(g_apu_mutex);
    const size_t capacity = g_apu_ring.size() / 2;
    for (size_t i = 0; i < frames; i++)
    {
        if (g_apu_fill == capacity) // full: the oldest goes
        {
            g_apu_read = (g_apu_read + 1) % capacity;
            g_apu_fill--;
        }
        const size_t at = (g_apu_read + g_apu_fill) % capacity;
        g_apu_ring[at * 2] = samples[i * 2];
        g_apu_ring[at * 2 + 1] = samples[i * 2 + 1];
        g_apu_fill++;
    }
}

std::atomic<u64> g_audio_dry{0};

std::string SoundStats(double seconds)
{
    static u64 last_in = 0, last_short = 0, last_ticks = 0, last_sink = 0, last_dry = 0;
    const u64 in = g_apu_in, shorts = g_mixer_short, ticks = g_mixer_ticks, sink = g_sink_frames, dry = g_audio_dry;
    size_t apu_queued;
    {
        std::lock_guard lock(g_apu_mutex);
        apu_queued = g_apu_fill;
    }
    const size_t device = g_audio_queued ? g_audio_queued() : 0;
    char out[400];
    std::snprintf(out, sizeof(out),
                  "the APU made %.0f%% of real time's sound (loudest %d%%), %.1f ms queued for the mixer, short of it "
                  "%llu of %llu times; %.0f%% of real time to the output (the AC'97's loudest %d%%)%s",
                  100.0 * double(in - last_in) / 48000.0 / seconds, g_apu_peak.exchange(0) * 100 / 32768,
                  apu_queued / 48.0, (unsigned long long)(shorts - last_short), (unsigned long long)(ticks - last_ticks),
                  100.0 * double(sink - last_sink) / 48000.0 / seconds, g_ac97_peak.exchange(0) * 100 / 32768,
                  g_audio_queued ? (", " + std::to_string(int(device / 48.0)) + " ms in the device, which ran dry " +
                                    std::to_string(dry - last_dry) + " times")
                                       .c_str()
                                 : "");
    last_in = in, last_short = shorts, last_ticks = ticks, last_sink = sink, last_dry = dry;
    return out;
}

void InitAudio()
{
    ResetMixer();
    for (u32 box : kBoxes)
        ResetBox(box);
    AddDevice({kBase, kSize, "ac97", ReadReg, WriteReg});
    std::thread(Player).detach();
}

} // namespace xb

extern "C" int xb_apu_queued_bytes(void) { return xb::ApuQueuedBytes(); }
