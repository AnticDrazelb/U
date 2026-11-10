// The MCPX's network adapter (nForce MAC at 0xfef00000, IRQ 4) with its PHY: xemu's model, whole
// (src/third_party/xemu/hw/xbox/mcpx/nvnet, GPL-2.0-or-later): registers, MII/PHY and link
// negotiation, the descriptor rings and their DMA. There is no network behind it: the link is down,
// as a console with no cable; what the program sends is dropped (and said, the first few times).
#include "core.h"
#include "devices.h"

extern "C" {
void xb_nvnet_init(const uint8_t mac[6]);
uint64_t xb_nvnet_read(uint64_t addr, unsigned int size);
void xb_nvnet_write(uint64_t addr, uint64_t val, unsigned int size);

void xb_nic_send(const uint8_t* buf, size_t size)
{
    (void)buf;
    static int told = 0;
    if (told++ < 4)
        xb::Log("nic: the program sends a packet (%zu bytes): not sent (no cable: the link is down)", size);
}
}

namespace xb {

namespace {
constexpr u32 kBase = 0xfef00000, kSize = 0x400;

u32 ReadReg(u32 offset, u32 size) { return u32(xb_nvnet_read(offset, size)); }
void WriteReg(u32 offset, u32 size, u32 value) { xb_nvnet_write(offset, value, size); }
} // namespace

void InitNic()
{
    static const u8 kMac[6] = {0x02, 0x58, 0x43, 0x00, 0x00, 0x01}; // the EEPROM's (kernel.cpp)
    xb_nvnet_init(kMac);
    AddDevice({kBase, kSize, "nic", ReadReg, WriteReg});
}

} // namespace xb
