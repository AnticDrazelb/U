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
#include "ui/xemu-settings.h"

// xemu's audio settings at its defaults, but 2 voice workers (the PS4's cores are the program's too)
struct XbXemuConfig g_config = {{false, false, true, 1.0f, {2}}};
struct AddressSpace { int unused; } address_space_memory;
}

namespace xb {

void MixApuSound(const s16* samples, size_t frames); // audio.cpp

namespace {

constexpr u32 kBase = 0xfe800000, kSize = 0x80000, kIrq = 5;

u32 Phys(u64 address, u32 bytes)
{
    const u32 g = PhysToGuest(u32(address));
    return g && GuestMapped(g, bytes) ? g : 0;
}

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

// what xemu's APU calls (its shims: src/third_party/xemu/shim)
extern "C" {
uint32_t xb_phys_ld32(uint64_t a) { const xb::u32 g = xb::Phys(a, 4); return g ? xb::Read<xb::u32>(g) : 0; }
uint32_t xb_phys_ld16(uint64_t a) { const xb::u32 g = xb::Phys(a, 2); return g ? xb::Read<xb::u16>(g) : 0; }
uint32_t xb_phys_ld8(uint64_t a) { const xb::u32 g = xb::Phys(a, 1); return g ? xb::Read<xb::u8>(g) : 0; }
void xb_phys_st32(uint64_t a, uint32_t v) { if (const xb::u32 g = xb::Phys(a, 4)) xb::Write<xb::u32>(g, v); }
void xb_phys_st8(uint64_t a, uint32_t v) { if (const xb::u32 g = xb::Phys(a, 1)) xb::Write<xb::u8>(g, xb::u8(v)); }
uint64_t xb_ram_size(void) { return xb::kContiguousSize; }
void xb_apu_irq(int level) { xb::SetIrq(5, level != 0); }
void xb_apu_sound(const int16_t* stereo, int frames) { xb::MixApuSound(stereo, size_t(frames)); }
}
