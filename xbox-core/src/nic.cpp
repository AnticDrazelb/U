// The MCPX's network adapter (nForce MAC at 0xfef00000, IRQ 4) with its PHY at MII address 1, as
// xemu's machine documents its registers: enough for the XDK's network stack (XNet, linked into
// games such as Halo) to set it up and find no cable plugged in. A retail console always has it,
// so a game's start-up initialises it and waits on it (MDIO transfers, the transmit/receive engine
// going idle); no packets are sent or received.
#include <cstring>
#include <atomic>
#include <mutex>

#include "core.h"
#include "devices.h"

namespace xb {

namespace {

constexpr u32 kBase = 0xfef00000, kSize = 0x400, kIrq = 4;

// registers (offsets)
constexpr u32 kIrqStatus = 0x000, kIrqMask = 0x004, kSetup5 = 0x130, kTxRxControl = 0x144, kMiiStatus = 0x180,
              kMdioAddr = 0x190, kMdioData = 0x194;
constexpr u32 kTxRxKick = 0x01, kTxRxBit1 = 0x02, kTxRxIdle = 0x08;
constexpr u32 kSetup5Bit31 = 0x80000000u;
constexpr u32 kMdioInUse = 0x8000, kMdioWrite = 0x0400;
constexpr u32 kPhyAddress = 1;

// the PHY's registers (IEEE 802.3 clause 22): control, status, ID, advertisement, partner ability
enum { kBmcr = 0, kBmsr = 1, kPhyId1 = 2, kPhyId2 = 3, kAnar = 4, kAnlpar = 5, kPhyRegs = 6 };
constexpr u16 kBmcrFullDuplex = 0x0100, kBmcrAutoNeg = 0x1000;
constexpr u16 kBmsrAutoNegAble = 0x0008; // (link 0x0004 and negotiation done 0x0020 stay clear: no cable)
constexpr u16 kAbility10 = 0x0020, kAbility10Fd = 0x0040, kAbilityTx = 0x0080, kAbilityTxFd = 0x0100,
              kAbilityT4 = 0x0200, kAbilityCsma = 0x0001;

std::mutex g_mutex;
u8 g_regs[kSize];
u16 g_phy[kPhyRegs];

u32 Get(u32 at)
{
    u32 v;
    std::memcpy(&v, g_regs + at, 4);
    return v;
}
void Set(u32 at, u32 v) { std::memcpy(g_regs + at, &v, 4); }

void UpdateIrq() { SetIrq(kIrq, (Get(kIrqStatus) & Get(kIrqMask)) != 0); }

void Reset()
{
    std::memset(g_regs, 0, sizeof(g_regs));
    Set(kTxRxControl, kTxRxIdle);
    // no cable: the PHY can negotiate, has not, and has no link
    g_phy[kBmcr] = kBmcrFullDuplex | kBmcrAutoNeg;
    g_phy[kBmsr] = kBmsrAutoNegAble;
    g_phy[kPhyId1] = 0;
    g_phy[kPhyId2] = 0;
    g_phy[kAnar] = kAbility10 | kAbility10Fd | kAbilityTx | kAbilityTxFd | kAbilityT4 | kAbilityCsma;
    g_phy[kAnlpar] = 0;
}

// the first accesses logged: what a program's network stack does with the adapter (it waits on it)
std::atomic<u32> g_logged{0};
void Note(const char* what, u32 offset, u32 size, u32 value)
{
    if (g_logged++ < 300)
        Log("nic: %s %#05x/%u %s %#x", what, offset, size, what[0] == 'r' ? "=" : "<-", value);
}

u32 ReadReg(u32 offset, u32 size)
{
    std::lock_guard lock(g_mutex);
    if (offset + size > kSize)
        return 0;
    u32 v = 0;
    std::memcpy(&v, g_regs + offset, size);
    Note("read", offset, size, v);
    return v;
}

void WriteReg(u32 offset, u32 size, u32 value)
{
    std::lock_guard lock(g_mutex);
    if (offset + size > kSize)
        return;
    Note("write", offset, size, value);
    switch (offset)
    {
    case kMdioAddr: // an MDIO transfer to the PHY, done at once (the in-use bit clears)
    {
        Set(kMdioAddr, value);
        const u32 phy = (value >> 5) & 0x1f, reg = value & 0x1f;
        if (value & kMdioWrite)
        {
            if (phy == kPhyAddress && reg < kPhyRegs)
            {
                g_phy[reg] = u16(Get(kMdioData));
                if (reg == kBmcr && (g_phy[kBmcr] & 0x8000)) // PHY reset: self-clearing
                    g_phy[kBmcr] &= u16(~0x8000);
            }
        }
        else
            Set(kMdioData, phy == kPhyAddress ? (reg < kPhyRegs ? g_phy[reg] : 0) : 0xffffffffu);
        Set(kMdioAddr, Get(kMdioAddr) & ~kMdioInUse);
        return;
    }
    case kTxRxControl:
        // the idle bit is the adapter's: it stays as it is
        Set(kTxRxControl, (value & ~kTxRxIdle) | (Get(kTxRxControl) & kTxRxIdle));
        if (value & kTxRxKick) // no link: nothing is sent, the descriptors stay the driver's (as xemu's adapter)
        {
            static bool told = false;
            if (!told)
                Log("nic: the program sends a packet: not sent (no cable: the link is down)");
            told = true;
        }
        if (value & kTxRxBit1)
            Set(kIrqStatus, 0);
        else if (value == 0)
            Set(kSetup5, kSetup5Bit31); // what the driver waits for after stopping the engine
        UpdateIrq();
        return;
    case kIrqStatus:
    case kMiiStatus: // write 1 to clear
    {
        u32 v = 0;
        std::memcpy(&v, g_regs + offset, size);
        v &= ~value;
        std::memcpy(g_regs + offset, &v, size);
        UpdateIrq();
        return;
    }
    case kIrqMask:
        std::memcpy(g_regs + offset, &value, size);
        UpdateIrq();
        return;
    default:
        std::memcpy(g_regs + offset, &value, size);
        return;
    }
}

} // namespace

void InitNic()
{
    {
        std::lock_guard lock(g_mutex);
        Reset();
    }
    AddDevice({kBase, kSize, "nic", ReadReg, WriteReg});
}

} // namespace xb
