// The MCPX audio processing unit (0xfe800000, IRQ 5): what DirectSound drives. This is xemu's model,
// whole (src/third_party/xemu/hw/xbox/mcpx/apu, GPL-2.0-or-later): its front end and voice processor
// (voices, ADPCM, filters, HRTF, resampling with libsamplerate), the GP and EP DSPs on its DSP56300
// interpreter, the frame thread and its pacing, with xemu's defaults (its DSP option off: the GP and
// EP get 1000 cycles a frame, and the sound heard is the voice processor's mix). Here: the 512 KiB
// window, guest physical memory, the interrupt line, and where the sound goes (into the AC'97's).
#include <cstring>

#include "core.h"
#include "devices.h"

extern "C" {
#include "hw/xbox/mcpx/apu/apu.h"
}

namespace xb {

void MixApuSound(const s16* samples, size_t frames); // audio.cpp

namespace {

constexpr u32 kBase = 0xfe800000, kSize = 0x80000, kIrq = 5;

u32 ReadReg(u32 offset, u32 size) { return u32(xb_mcpx_apu_read(offset, size)); }

void WriteReg(u32 offset, u32 size, u32 value)
{
    if ((offset == 0x3fffc || offset == 0x5fffc) && (value & 3) == 3)
    {
        static int told = 0;
        if (told++ < 8)
            Log("apu: the %s DSP is let run (its microcode bootstrapped from scratch memory)", offset == 0x3fffc ? "GP" : "EP");
    }
    xb_mcpx_apu_write(offset, value, size);
}

} // namespace

void InitApu()
{
    xb_mcpx_apu_init(Ptr<u8>(kContiguousBase));
    AddDevice({kBase, kSize, "apu", ReadReg, WriteReg});
}

} // namespace xb

// where xemu's APU's sound goes
extern "C" void xb_apu_sound(const int16_t* stereo, int frames) { xb::MixApuSound(stereo, size_t(frames)); }
