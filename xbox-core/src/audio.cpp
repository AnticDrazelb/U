// The MCPX's AC'97 controller at 0xfec00000 (IRQ 6), as nxdk's audio drives it: the codec's
// registers, and the bus masters' descriptor lists (PCM in 0x100, PCM out 0x110, S/PDIF 0x170)
// played in real time at 48 kHz. PCM out's samples (16-bit stereo) go to the sound sink; S/PDIF is
// consumed in step with it. (Games use the APU at 0xfe800000 through DirectSound; that is separate.)
#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include "core.h"
#include "devices.h"

namespace xb {

void (*g_audio_sink)(const s16* samples, size_t frames) = nullptr;

namespace {

// the APU's sound (apu.cpp), mixed into what the AC'97 plays: a ring of stereo frames
std::mutex g_apu_mutex;
std::vector<s16> g_apu_ring(48000 * 2 / 4); // a quarter second
size_t g_apu_read = 0, g_apu_fill = 0;

// Takes up to `frames` APU frames, added into `pcm`; returns how many there were.
size_t TakeApuSound(s16* pcm, size_t frames)
{
    std::lock_guard lock(g_apu_mutex);
    const size_t capacity = g_apu_ring.size() / 2;
    const size_t n = std::min(frames, g_apu_fill);
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

void ResetBox(u32 box)
{
    B(box + 4) = B(box + 5) = B(box + 0xa) = 0;
    SetW(box + 8, 0);
    SetW(box + 6, SR_DCH);
    B(box + 0xb) &= ~(CR_RUN | CR_RESET);
}

// Plays `samples` 16-bit samples of a box; returns how many came from guest memory (the rest is
// silence, when it ran dry). Under g_m.
size_t Play(u32 box, s16* out, size_t samples)
{
    size_t done = 0;
    u8& civ = B(box + 4);
    while (done < samples && (B(box + 0xb) & CR_RUN) && !(W(box + 6) & SR_DCH))
    {
        const u32 descriptor = PhysToGuest(D(box)) + civ * 8u;
        if (descriptor < 8)
            break;
        const u32 address = PhysToGuest(Read<u32>(descriptor));
        const u16 length = Read<u16>(descriptor + 4), control = Read<u16>(descriptor + 6);
        u16 left = W(box + 8); // PICB: samples left in the current buffer
        if (left == 0)
            left = length;
        const size_t n = std::min<size_t>(left, samples - done);
        const u32 at = address + u32(length - left) * 2;
        for (size_t i = 0; i < n; i++)
            out[done + i] = address ? Read<s16>(at + u32(i) * 2) : 0;
        done += n;
        left = u16(left - n);
        SetW(box + 8, left);
        if (left)
            continue;
        // the buffer is done
        u16 sr = W(box + 6);
        if (control & 0x8000) // IOC
            sr |= SR_BCIS;
        if (civ == B(box + 5)) // it was the last valid one: halt
        {
            sr |= SR_LVBCI | SR_DCH | SR_CELV;
            SetW(box + 6, sr);
            break;
        }
        civ = u8((civ + 1) & 31);
        B(box + 0xa) = u8((civ + 1) & 31);
        SetW(box + 6, sr);
    }
    return done;
}

void Player()
{
    // 10 ms at a time: 480 stereo frames
    constexpr size_t kFrames = 480;
    std::vector<s16> pcm(kFrames * 2), spdif(kFrames * 2);
    auto next = std::chrono::steady_clock::now();
    while (!g_quit)
    {
        next += std::chrono::milliseconds(10);
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
        if (TakeApuSound(pcm.data(), kFrames))
            playing = true;
        if (g_audio_sink && playing)
            g_audio_sink(pcm.data(), kFrames);
    }
}

u32 ReadReg(u32 offset, u32 size)
{
    std::lock_guard lock(g_m);
    if (offset == kGlobalStatus)
    {
        // the primary codec is ready (bit 8), and each box's interrupt as its own bit: PCM in 5,
        // PCM out 6 (a driver waiting on its buffers polls these)
        u32 v = D(kGlobalStatus) | 0x100;
        for (u32 i = 0; i < 2; i++)
        {
            const u32 box = kBoxes[i];
            const u16 sr = W(box + 6);
            const u8 cr = B(box + 0xb);
            if (((sr & SR_BCIS) && (cr & CR_IOCE)) || ((sr & SR_LVBCI) && (cr & CR_LVBIE)))
                v |= i == 0 ? 0x20 : 0x40;
        }
        return v;
    }
    if (offset < 0x100 && size == 2 && offset == 0x26)
        return 0x000f; // the codec's powerdown status: everything ready
    u32 v = 0;
    std::memcpy(&v, &g_regs[offset], std::min<u32>(size, kSize - offset));
    return v;
}

void WriteReg(u32 offset, u32 size, u32 value)
{
    std::lock_guard lock(g_m);
    for (u32 box : kBoxes)
    {
        if (offset == box + 0xb && size == 1) // CR
        {
            if (value & CR_RESET)
                ResetBox(box);
            else
            {
                B(box + 0xb) = u8(value);
                if (value & CR_RUN)
                    SetW(box + 6, W(box + 6) & ~SR_DCH);
            }
            UpdateIrq();
            return;
        }
        if (offset == box + 6 && size <= 2) // SR: the interrupt bits are cleared by writing 1
        {
            SetW(box + 6, u16(W(box + 6) & ~(value & (SR_LVBCI | SR_BCIS | SR_FIFOE))));
            UpdateIrq();
            return;
        }
        if (offset == box + 5 && size == 1) // LVI: a new last buffer restarts a halted box
        {
            B(box + 5) = u8(value & 31);
            if ((B(box + 0xb) & CR_RUN) && (W(box + 6) & SR_DCH))
            {
                SetW(box + 6, W(box + 6) & ~(SR_DCH | SR_CELV));
                B(box + 4) = u8((B(box + 4) + (W(box + 8) == 0 && B(box + 4) != B(box + 5) ? 1 : 0)) & 31);
            }
            return;
        }
    }
    if (offset == kGlobalStatus && size == 4)
    {
        // RCS, the resume bits and GSCI are cleared by writing 1; the status bits are read-only
        constexpr u32 kWriteClear = 0x8000 | 0x800 | 0x400 | 1, kReadOnly = 0x7000 | 0x300 | 0xfe;
        constexpr u32 kWritable = 0x3ffff & ~(kWriteClear | kReadOnly);
        const u32 v = (D(kGlobalStatus) & ~(value & kWriteClear) & ~kWritable) | (value & kWritable);
        std::memcpy(&g_regs[offset], &v, 4);
        return;
    }
    std::memcpy(&g_regs[offset], &value, std::min<u32>(size, kSize - offset));
}

} // namespace

void MixApuSound(const s16* samples, size_t frames)
{
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

void InitAudio()
{
    for (u32 box : kBoxes)
        ResetBox(box);
    AddDevice({kBase, kSize, "ac97", ReadReg, WriteReg});
    std::thread(Player).detach();
}

} // namespace xb
