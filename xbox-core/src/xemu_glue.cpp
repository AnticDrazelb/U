// What xemu's devices (src/third_party/xemu) call of xbox-core: guest physical memory (the DMA they
// do), their interrupt lines, and the settings they read. Their shims (src/third_party/xemu/shim)
// declare these.
#include <cstring>

#include "core.h"
#include "devices.h"

extern "C" {
#include "ui/xemu-settings.h"

// xemu's audio settings at its defaults, but 2 voice workers (the PS4's cores are the program's too)
struct XbXemuConfig g_config = {{false, false, true, 1.0f, {2}}};
struct AddressSpace { int unused; } address_space_memory;
}

namespace {
using namespace xb;

u32 Phys(u64 address, u32 bytes)
{
    const u32 g = PhysToGuest(u32(address));
    return g && GuestMapped(g, bytes) ? g : 0;
}
} // namespace

extern "C" {
uint32_t xb_phys_ld32(uint64_t a) { const u32 g = Phys(a, 4); return g ? Read<u32>(g) : 0; }
uint32_t xb_phys_ld16(uint64_t a) { const u32 g = Phys(a, 2); return g ? Read<u16>(g) : 0; }
uint32_t xb_phys_ld8(uint64_t a) { const u32 g = Phys(a, 1); return g ? Read<u8>(g) : 0; }
void xb_phys_st32(uint64_t a, uint32_t v) { if (const u32 g = Phys(a, 4)) Write<u32>(g, v); }
void xb_phys_st8(uint64_t a, uint32_t v) { if (const u32 g = Phys(a, 1)) Write<u8>(g, u8(v)); }

// a block, page by page (physical pages need not be contiguous in the guest's view); what isn't
// memory reads as 0 and takes no writes, as the bus would
void xb_phys_read(uint64_t addr, void* buf, size_t len)
{
    u8* out = static_cast<u8*>(buf);
    while (len)
    {
        const size_t chunk = std::min<size_t>(len, 4096 - (addr & 4095));
        if (const u32 g = Phys(addr, u32(chunk)))
            std::memcpy(out, Ptr<u8>(g), chunk);
        else
            std::memset(out, 0, chunk);
        out += chunk, addr += chunk, len -= chunk;
    }
}
void xb_phys_write(uint64_t addr, const void* buf, size_t len)
{
    const u8* in = static_cast<const u8*>(buf);
    while (len)
    {
        const size_t chunk = std::min<size_t>(len, 4096 - (addr & 4095));
        if (const u32 g = Phys(addr, u32(chunk)))
            std::memcpy(Ptr<u8>(g), in, chunk);
        in += chunk, addr += chunk, len -= chunk;
    }
}
uint64_t xb_ram_size(void) { return kContiguousSize; }
void xb_pci_irq(int irq, int level) { SetIrq(u32(irq), level != 0); }
}
