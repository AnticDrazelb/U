// USB: the MCPX's OHCI host controller at 0xfed00000 (IRQ 1) with an original Xbox controller (an
// XID gamepad) on root port 3, which is the front panel's first port: player 1. A 1 ms frame thread runs the controller's endpoint lists in
// guest memory (periodic, control, bulk), completes transfers into the done queue and interrupts.
// The gamepad's state comes from the frontend (SetPad).
#include <chrono>
#include <thread>
#include <vector>

#include "core.h"
#include "devices.h"

namespace xb {

namespace {

constexpr u32 kBase = 0xfed00000, kSize = 0x1000, kIrq = 1, kPorts = 4;
// registers
constexpr u32 HcControl = 0x04, HcCommandStatus = 0x08, HcInterruptStatus = 0x0c, HcInterruptEnable = 0x10,
              HcInterruptDisable = 0x14, HcHCCA = 0x18, HcControlHeadED = 0x20, HcBulkHeadED = 0x28, HcFmInterval = 0x34,
              HcFmNumber = 0x3c, HcRhDescriptorA = 0x48, HcRhStatus = 0x50, HcRhPortStatus = 0x54;
// interrupt status bits
constexpr u32 WDH = 1u << 1, SF = 1u << 2, RHSC = 1u << 6, MIE = 1u << 31;

// ---- the gamepad (an XID device)

struct Setup
{
    u8 type, request;
    u16 value, index, length;
};

// What the gamepad reports: its buttons, analog buttons, sticks (xboxdevwiki's "Xbox Input Devices")
std::mutex g_pad_mutex;
PadState g_pad;
u16 g_rumble[2] = {};

const u8 kDeviceDescriptor[18] = {18, 1, 0x10, 0x01, 0, 0, 0, 8, 0x5e, 0x04, 0x02, 0x02, 0x00, 0x01, 0, 0, 0, 1};
const u8 kConfigDescriptor[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,       // configuration: 1 interface
    9, 4, 0, 0, 2, 0x58, 0x42, 0, 0,      // interface: XID gamepad, 2 endpoints
    7, 5, 0x82, 3, 32, 0, 4,              // interrupt IN 2: reports
    7, 5, 0x02, 3, 32, 0, 4,              // interrupt OUT 2: rumble
};
const u8 kXidDescriptor[16] = {0x10, 0x42, 0x00, 0x01, 1, 1, 20, 6, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

struct Gamepad
{
    u8 address = 0, pending_address = 0;
    bool configured = false;
    std::vector<u8> control_in; // a control transfer's data stage, device to host
    size_t control_at = 0;
    Setup setup{};
    u8 last_report[20] = {};
    bool report_new = true;

    void Report(u8* r) const
    {
        std::lock_guard lock(g_pad_mutex);
        r[0] = 0;
        r[1] = 20;
        r[2] = u8(g_pad.buttons);
        r[3] = u8(g_pad.buttons >> 8);
        for (int i = 0; i < 8; i++)
            r[4 + i] = g_pad.analog[i];
        const s16 sticks[4] = {g_pad.lx, g_pad.ly, g_pad.rx, g_pad.ry};
        std::memcpy(r + 12, sticks, 8);
    }

    // a SETUP packet: returns false to stall
    bool Control(const Setup& s)
    {
        setup = s;
        control_in.clear();
        control_at = 0;
        auto give = [&](const u8* d, size_t n) { control_in.assign(d, d + std::min<size_t>(n, s.length)); };
        if (s.type == 0x80 && s.request == 6) // GET_DESCRIPTOR
        {
            switch (s.value >> 8)
            {
            case 1: give(kDeviceDescriptor, sizeof(kDeviceDescriptor)); return true;
            case 2: give(kConfigDescriptor, sizeof(kConfigDescriptor)); return true;
            }
            return false;
        }
        if (s.type == 0x00 && s.request == 5) // SET_ADDRESS (takes effect after the status stage)
        {
            pending_address = u8(s.value & 0x7f);
            return true;
        }
        if (s.type == 0x00 && s.request == 9) // SET_CONFIGURATION
        {
            configured = s.value != 0;
            return true;
        }
        if (s.type == 0x80 && s.request == 0) // GET_STATUS
        {
            const u8 status[2] = {0, 0};
            give(status, 2);
            return true;
        }
        if (s.type == 0xc1 && s.request == 6 && (s.value >> 8) == 0x42) // XID descriptor
        {
            give(kXidDescriptor, sizeof(kXidDescriptor));
            return true;
        }
        if (s.type == 0xc1 && s.request == 1) // XID capabilities: what can change (all of it)
        {
            if ((s.value >> 8) == 1)
            {
                u8 caps[20] = {0, 20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                               0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
                give(caps, sizeof(caps));
            }
            else
            {
                const u8 caps[6] = {0, 6, 0xff, 0xff, 0xff, 0xff};
                give(caps, sizeof(caps));
            }
            return true;
        }
        if (s.type == 0xa1 && s.request == 1) // HID GET_REPORT
        {
            u8 r[20];
            Report(r);
            give(r, sizeof(r));
            return true;
        }
        if (s.type == 0x21 && s.request == 9) // HID SET_REPORT (rumble, data stage OUT)
            return true;
        if ((s.type & 0x60) == 0) // other standard requests (SET_INTERFACE, CLEAR_FEATURE...): accepted
            return true;
        return false;
    }
    void StatusDone()
    {
        if (setup.type == 0x00 && setup.request == 5)
            address = pending_address;
    }
    // an IN on an interrupt endpoint: a report when it changed, else NAK (-1)
    int InterruptIn(u8* out, u32 max)
    {
        if (!configured)
            return -1;
        u8 r[20];
        Report(r);
        if (!report_new && std::memcmp(r, last_report, 20) == 0)
            return -1;
        std::memcpy(last_report, r, 20);
        report_new = false;
        const u32 n = std::min<u32>(max, 20);
        std::memcpy(out, r, n);
        return int(n);
    }
    void InterruptOut(const u8* data, u32 n)
    {
        if (n >= 6)
        {
            std::lock_guard lock(g_pad_mutex);
            std::memcpy(g_rumble, data + 2, 4);
        }
    }
};

// ---- the host controller

std::mutex g_m;
u32 g_regs[kSize / 4] = {};
u32 g_status = 0, g_enable = 0, g_done = 0;
u32 g_port[kPorts] = {};
Gamepad g_gamepad;
bool g_attached = true;

u32& R(u32 o) { return g_regs[o / 4]; }

void UpdateIrq()
{
    SetIrq(kIrq, (g_enable & MIE) && (g_status & g_enable & ~MIE));
}

u32 Guest(u32 phys)
{
    return PhysToGuest(phys);
}

// One general TD of endpoint `ed`: returns false when it can't complete this frame (NAK).
bool RunTd(u32 ed, u32 td)
{
    const u32 ed0 = Read<u32>(ed);
    const u32 address = ed0 & 0x7f, endpoint = (ed0 >> 7) & 0xf, direction = (ed0 >> 11) & 3;
    const u32 td0 = Read<u32>(td);
    u32 cbp = Read<u32>(td + 4);
    const u32 be = Read<u32>(td + 12);
    const u32 pid = direction == 1 ? 1 : direction == 2 ? 2 : (td0 >> 19) & 3; // 0 SETUP, 1 OUT, 2 IN
    const u32 length = cbp ? be - cbp + 1 : 0;
    Gamepad& dev = g_gamepad;
    int done = 0;
    u32 cc = 0; // NoError
    if (!g_attached || address != dev.address)
        cc = 5; // DeviceNotResponding
    else if (endpoint == 0)
    {
        if (pid == 0 && length == 8)
        {
            Setup s;
            const u32 p = Guest(cbp);
            s.type = Read<u8>(p), s.request = Read<u8>(p + 1);
            s.value = Read<u16>(p + 2), s.index = Read<u16>(p + 4), s.length = Read<u16>(p + 6);
            if (!dev.Control(s))
                cc = 4; // STALL
            done = 8;
        }
        else if (pid == 2)
        {
            const size_t n = std::min<size_t>(length, dev.control_in.size() - dev.control_at);
            if (n)
                std::memcpy(Ptr(Guest(cbp)), dev.control_in.data() + dev.control_at, n);
            dev.control_at += n;
            done = int(n);
            if (length == 0 || (n == 0 && (dev.setup.type & 0x80) == 0))
                dev.StatusDone(); // the status stage of a transfer without IN data
        }
        else // OUT: data of a host-to-device transfer, or the status stage after IN data
        {
            done = int(length);
            if (length == 0)
                dev.StatusDone();
        }
    }
    else if (pid == 2)
    {
        u8 buf[64];
        const int n = dev.InterruptIn(buf, std::min<u32>(length, sizeof(buf)));
        if (n < 0)
            return false; // NAK: try again next time
        std::memcpy(Ptr(Guest(cbp)), buf, u32(n));
        done = n;
    }
    else
    {
        if (length && cbp)
            dev.InterruptOut(Ptr<u8>(Guest(cbp)), length);
        done = int(length);
    }
    // complete it: the condition code, the buffer pointer (0 when all of it moved), the toggle
    if (cc == 0 && u32(done) < length)
    {
        cbp += u32(done);
        if (!(td0 & (1u << 18))) // buffer rounding off: a short packet is an error
            cc = 9;               // DataUnderrun
    }
    else if (cc == 0)
        cbp = 0;
    u32 headp = Read<u32>(ed + 8);
    const u32 toggle = (td0 >> 24) & 3;
    const u32 next_toggle = toggle & 2 ? ((toggle & 1) ^ 1) | 2 : (((headp >> 1) & 1) ^ 1);
    Write<u32>(td, (td0 & 0x0fffffffu & ~(3u << 24)) | (cc << 28) | (toggle & 2 ? next_toggle << 24 : 0));
    Write<u32>(td + 4, cbp);
    const u32 next_td = Read<u32>(td + 8);
    headp = (next_td & ~0xfu) | ((next_toggle & 1) << 1) | (cc ? 1u : 0u); // halt on errors
    Write<u32>(ed + 8, headp);
    // into the done queue
    Write<u32>(td + 8, g_done);
    g_done = PhysicalAddress(td);
    return true;
}

// Runs the TDs of each ED of a list until each NAKs or ends. Returns whether any TD moved.
bool RunList(u32 head_phys)
{
    bool moved = false;
    for (u32 ed_phys = head_phys, guard = 0; ed_phys && guard < 64; guard++)
    {
        const u32 ed = Guest(ed_phys & ~0xfu);
        if (!ed)
            break;
        const u32 ed0 = Read<u32>(ed);
        if (!(ed0 & (1u << 14)) && !(ed0 & (1u << 15))) // not skipped, not isochronous
            for (int n = 0; n < 8; n++)
            {
                const u32 headp = Read<u32>(ed + 8), tailp = Read<u32>(ed + 4);
                if ((headp & 1) || (headp & ~0xfu) == (tailp & ~0xfu))
                    break;
                const u32 td = Guest(headp & ~0xfu);
                if (!td || !RunTd(ed, td))
                    break;
                moved = true;
            }
        ed_phys = Read<u32>(ed + 12);
    }
    return moved;
}

void Frame()
{
    const u32 control = R(HcControl);
    if (((control >> 6) & 3) != 2) // not operational
        return;
    const u32 hcca = Guest(R(HcHCCA));
    if (!hcca)
        return;
    R(HcFmNumber) = (R(HcFmNumber) + 1) & 0xffff;
    Write<u16>(hcca + 0x80, u16(R(HcFmNumber)));
    Write<u16>(hcca + 0x82, 0);
    if (control & (1u << 2)) // periodic list
        RunList(Read<u32>(hcca + 4 * (R(HcFmNumber) & 31)));
    if ((control & (1u << 4)) && (R(HcCommandStatus) & 2)) // control list, filled
    {
        if (!RunList(R(HcControlHeadED)))
            R(HcCommandStatus) &= ~2u;
    }
    if ((control & (1u << 5)) && (R(HcCommandStatus) & 4)) // bulk list
    {
        if (!RunList(R(HcBulkHeadED)))
            R(HcCommandStatus) &= ~4u;
    }
    g_status |= SF;
    if (g_done && !(g_status & WDH))
    {
        Write<u32>(hcca + 0x84, g_done | ((g_status & g_enable & ~WDH) ? 1 : 0));
        g_done = 0;
        g_status |= WDH;
    }
    UpdateIrq();
}

u32 ReadReg(u32 offset, u32)
{
    std::lock_guard lock(g_m);
    switch (offset)
    {
    case 0x00: return 0x10; // OHCI 1.0
    case HcInterruptStatus: return g_status;
    case HcInterruptEnable:
    case HcInterruptDisable: return g_enable;
    case HcRhDescriptorA: return kPorts | (1u << 9); // no power switching: always powered
    }
    if (offset >= HcRhPortStatus && offset < HcRhPortStatus + 4 * kPorts)
        return g_port[(offset - HcRhPortStatus) / 4];
    return offset < kSize ? R(offset) : 0;
}

void WriteReg(u32 offset, u32, u32 value)
{
    std::lock_guard lock(g_m);
    switch (offset)
    {
    case HcCommandStatus:
        if (value & 1) // host controller reset
        {
            R(HcControl) = 0;
            g_status = 0;
            g_enable = 0;
        }
        R(HcCommandStatus) |= value & ~1u;
        return;
    case HcInterruptStatus: g_status &= ~value; UpdateIrq(); return;
    case HcInterruptEnable: g_enable |= value; UpdateIrq(); return;
    case HcInterruptDisable: g_enable &= ~value; UpdateIrq(); return;
    case HcRhStatus: return;
    }
    if (offset >= HcRhPortStatus && offset < HcRhPortStatus + 4 * kPorts)
    {
        u32& p = g_port[(offset - HcRhPortStatus) / 4];
        if (!(p & 1)) // nothing connected: only the change bits clear
        {
            p &= ~(value & 0x1f0000);
            return;
        }
        if (value & 0x10) // SetPortReset: done at once, the port enabled
            p |= 0x2 | (1u << 20) | (1u << 17);
        if (value & 0x2)
            p |= 0x2;
        if (value & 0x1)
            p &= ~0x2u;
        if (value & 0x100)
            p |= 0x100;
        p &= ~(value & 0x1f0000);
        if (value & 0x10)
        {
            g_status |= RHSC;
            g_gamepad.address = 0; // a reset device answers at address 0
            g_gamepad.configured = false;
            g_gamepad.report_new = true;
        }
        UpdateIrq();
        return;
    }
    if (offset < kSize)
        R(offset) = value;
}

} // namespace

void SetPad(const PadState& pad)
{
    std::lock_guard lock(g_pad_mutex);
    g_pad = pad;
}

void GetRumble(u16& left, u16& right)
{
    std::lock_guard lock(g_pad_mutex);
    left = g_rumble[0];
    right = g_rumble[1];
}

void InitUsb()
{
    // the gamepad on port 3 (player 1): connected, powered, its connection a change to report
    for (u32 i = 0; i < kPorts; i++)
        g_port[i] = 0x100;
    g_port[2] = 0x1 | 0x100 | (1u << 16);
    g_status = RHSC;
    R(HcFmInterval) = 0x27782edf;
    AddDevice({kBase, kSize, "ohci", ReadReg, WriteReg});
    std::thread([] {
        auto next = std::chrono::steady_clock::now();
        while (!g_quit)
        {
            next += std::chrono::milliseconds(1);
            std::this_thread::sleep_until(next);
            std::lock_guard lock(g_m);
            Frame();
        }
    }).detach();
}

} // namespace xb
