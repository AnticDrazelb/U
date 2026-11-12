// USB: the MCPX's two OHCI host controllers, at 0xfed00000 (IRQ 1) with an original Xbox controller
// (an XID gamepad) on root port 3, the front panel's first port: player 1, and at 0xfed08000 (IRQ 9)
// with nothing plugged in. A 1 ms frame thread runs the controller's endpoint lists in
// guest memory (periodic, control, bulk), completes transfers into the done queue and interrupts.
// The gamepad's state comes from the frontend (SetPad).
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>
#include <thread>
#include <vector>

#include "profiler.h"
#include "core.h"
#include "devices.h"

namespace xb {

namespace {

constexpr u32 kSize = 0x1000, kPorts = 4;
// registers
constexpr u32 HcRevision = 0x00, HcControl = 0x04, HcCommandStatus = 0x08, HcInterruptStatus = 0x0c,
              HcInterruptEnable = 0x10, HcInterruptDisable = 0x14, HcHCCA = 0x18, HcControlHeadED = 0x20,
              HcBulkHeadED = 0x28, HcFmInterval = 0x34, HcFmRemaining = 0x38, HcFmNumber = 0x3c, HcLSThreshold = 0x44,
              HcRhDescriptorA = 0x48, HcRhDescriptorB = 0x4c, HcRhStatus = 0x50, HcRhPortStatus = 0x54;
// interrupt status bits
constexpr u32 WDH = 1u << 1, SF = 1u << 2, FNO = 1u << 5, RHSC = 1u << 6, MIE = 1u << 31;

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
            Log("usb: the gamepad (address %u) %s", address, configured ? "configured" : "unconfigured");
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

// ---- the controller's internal hub (USB 2.0 chapter 11, a USB 1.1 full-speed hub): 3 ports, the
// gamepad on port 1 (2 and 3 are the memory-unit slots, empty), as the console's controller is built
// and as xemu and Cxbx-Reloaded attach theirs

const u8 kHubDeviceDescriptor[18] = {18, 1, 0x10, 0x01, 9, 0, 0, 8, 0x09, 0x04, 0xaa, 0x55, 0x01, 0x01, 0, 0, 0, 1};
const u8 kHubConfigDescriptor[25] = {
    9, 2, 25, 0, 1, 1, 0, 0xe0, 0, // configuration: 1 interface, self-powered
    9, 4, 0, 0, 1, 9, 0, 0, 0,     // interface: hub class, 1 endpoint
    7, 5, 0x81, 3, 1, 0, 0xff,     // interrupt IN 1: the status-change bitmap
};
const u8 kHubDescriptor[9] = {9, 0x29, 3, 0x09, 0x00, 1, 0, 0x00, 0xff}; // 3 ports, per-port power

constexpr u32 kHubPorts = 3;
// port status and change bits
constexpr u16 PS_CONNECTION = 1 << 0, PS_ENABLE = 1 << 1, PS_SUSPEND = 1 << 2, PS_RESET = 1 << 4, PS_POWER = 1 << 8;
constexpr u16 PC_CONNECTION = 1 << 0, PC_ENABLE = 1 << 1, PC_SUSPEND = 1 << 2, PC_RESET = 1 << 4;

struct Hub
{
    u8 address = 0, pending_address = 0;
    bool configured = false;
    std::vector<u8> control_in;
    size_t control_at = 0;
    Setup setup{};
    u16 status[kHubPorts] = {}, change[kHubPorts] = {};

    void Reset(Gamepad& pad)
    {
        address = pending_address = 0;
        configured = false;
        for (u32 i = 0; i < kHubPorts; i++)
            status[i] = change[i] = 0; // unpowered: nothing seen connected
        pad.address = 0;
        pad.configured = false;
        pad.report_new = true;
    }
    bool PadReachable() const { return (status[0] & (PS_ENABLE | PS_POWER)) == (PS_ENABLE | PS_POWER); }

    bool Control(const Setup& s, Gamepad& pad)
    {
        setup = s;
        control_in.clear();
        control_at = 0;
        auto give = [&](const u8* d, size_t n) { control_in.assign(d, d + std::min<size_t>(n, s.length)); };
        const u32 port = s.index - 1;
        switch (s.type << 8 | s.request)
        {
        case 0x8006: // GET_DESCRIPTOR
            switch (s.value >> 8)
            {
            case 1: give(kHubDeviceDescriptor, sizeof(kHubDeviceDescriptor)); return true;
            case 2: give(kHubConfigDescriptor, sizeof(kHubConfigDescriptor)); return true;
            }
            return false;
        case 0xa006: // GET_DESCRIPTOR (class): the hub descriptor
            if ((s.value >> 8) != 0x29)
                return false;
            give(kHubDescriptor, sizeof(kHubDescriptor));
            return true;
        case 0x0005: pending_address = u8(s.value & 0x7f); return true; // SET_ADDRESS
        case 0x0009: // SET_CONFIGURATION
            configured = s.value != 0;
            Log("usb: the controller's hub (address %u) %s", address, configured ? "configured" : "unconfigured");
            return true;
        case 0x8000: // GET_STATUS (device): self-powered
        {
            const u8 v[2] = {1, 0};
            give(v, 2);
            return true;
        }
        case 0xa000: // GET_STATUS (hub): no local power or over-current change
        {
            const u8 v[4] = {0, 0, 0, 0};
            give(v, 4);
            return true;
        }
        case 0xa300: // GET_STATUS (port)
        {
            if (port >= kHubPorts)
                return false;
            const u8 v[4] = {u8(status[port]), u8(status[port] >> 8), u8(change[port]), u8(change[port] >> 8)};
            give(v, 4);
            return true;
        }
        case 0x2001: // CLEAR_FEATURE (hub)
        case 0x2003: // SET_FEATURE (hub)
            return true;
        case 0x2303: // SET_FEATURE (port)
            if (port >= kHubPorts)
                return false;
            switch (s.value)
            {
            case 2: status[port] |= PS_SUSPEND; return true; // PORT_SUSPEND
            case 4:                                          // PORT_RESET: done at once, the port enabled
                if (status[port] & PS_CONNECTION)
                {
                    status[port] = u16((status[port] | PS_ENABLE) & ~(PS_RESET | PS_SUSPEND));
                    change[port] |= PC_RESET;
                    if (port == 0)
                    {
                        pad.address = 0; // a reset device answers at address 0
                        pad.configured = false;
                        pad.report_new = true;
                    }
                }
                return true;
            case 8: // PORT_POWER: power on, and what is plugged in is seen
                status[port] |= PS_POWER;
                if (port == 0 && !(status[port] & PS_CONNECTION))
                {
                    status[port] |= PS_CONNECTION;
                    change[port] |= PC_CONNECTION;
                }
                return true;
            }
            return true;
        case 0x2301: // CLEAR_FEATURE (port)
            if (port >= kHubPorts)
                return false;
            switch (s.value)
            {
            case 1: status[port] &= u16(~PS_ENABLE); return true;
            case 2: status[port] &= u16(~PS_SUSPEND); change[port] |= PC_SUSPEND; return true;
            case 8: status[port] &= u16(~(PS_POWER | PS_ENABLE | PS_CONNECTION)); return true;
            case 16: change[port] &= u16(~PC_CONNECTION); return true;
            case 17: change[port] &= u16(~PC_ENABLE); return true;
            case 18: change[port] &= u16(~PC_SUSPEND); return true;
            case 19: return true; // C_PORT_OVER_CURRENT
            case 20: change[port] &= u16(~PC_RESET); return true;
            }
            return true;
        }
        if ((s.type & 0x60) == 0) // other standard requests (SET_INTERFACE, CLEAR_FEATURE...): accepted
            return true;
        return false;
    }
    void StatusDone()
    {
        if (setup.type == 0x00 && setup.request == 5)
            address = pending_address;
    }
    // the status-change endpoint: bit n for port n (bit 0 the hub itself), NAK when nothing changed
    int InterruptIn(u8* out, u32 max)
    {
        if (!configured || max < 1)
            return -1;
        u8 bits = 0;
        for (u32 i = 0; i < kHubPorts; i++)
            if (change[i])
                bits |= u8(1u << (i + 1));
        if (!bits)
            return -1;
        out[0] = bits;
        return 1;
    }
};

// ---- the host controllers (OHCI 1.0a): USB 0 with the gamepad, USB 1 with nothing plugged in

struct Ohci
{
    u32 base = 0, irq = 0;
    bool has_pad = false;
    std::mutex m;
    u32 regs[kSize / 4] = {};
    u32 status = 0, enable = 0, done = 0;
    u32 port[kPorts] = {};
    Gamepad gamepad;
    Hub hub;
    std::chrono::steady_clock::time_point frame_start = std::chrono::steady_clock::now();

    u32& R(u32 o) { return regs[o / 4]; }
    void UpdateIrq() { SetIrq(int(irq), (enable & MIE) && (status & enable & ~MIE)); }

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
        int moved = 0;
        u32 cc = 0; // NoError
        // the device at this address: the hub (behind root port 3, once it is enabled), or the
        // gamepad behind the hub's port 1 (once the hub has enabled that port)
        const bool hub_there = has_pad && (port[2] & 3) == 3;
        enum { kNone, kHub, kPad } who = kNone;
        if (hub_there && address == hub.address)
            who = kHub;
        else if (hub_there && hub.PadReachable() && address == gamepad.address)
            who = kPad;
        auto control = [&](auto& dev, auto&& setup_call) {
            if (pid == 0 && length == 8)
            {
                Setup su;
                const u32 p = PhysToGuest(cbp);
                su.type = Read<u8>(p), su.request = Read<u8>(p + 1);
                su.value = Read<u16>(p + 2), su.index = Read<u16>(p + 4), su.length = Read<u16>(p + 6);
                if (!setup_call(su))
                    cc = 4; // STALL
                moved = 8;
            }
            else if (pid == 2)
            {
                const size_t n = std::min<size_t>(length, dev.control_in.size() - dev.control_at);
                if (n)
                    std::memcpy(Ptr(PhysToGuest(cbp)), dev.control_in.data() + dev.control_at, n);
                dev.control_at += n;
                moved = int(n);
                if (length == 0 || (n == 0 && (dev.setup.type & 0x80) == 0))
                    dev.StatusDone(); // the status stage of a transfer without IN data
            }
            else // OUT: data of a host-to-device transfer, or the status stage after IN data
            {
                moved = int(length);
                if (length == 0)
                    dev.StatusDone();
            }
        };
        if (who == kNone)
            cc = 5; // DeviceNotResponding
        else if (endpoint == 0 && who == kHub)
            control(hub, [&](const Setup& su) { return hub.Control(su, gamepad); });
        else if (endpoint == 0)
            control(gamepad, [&](const Setup& su) { return gamepad.Control(su); });
        else if (pid == 2)
        {
            u8 buf[64];
            const int n = who == kHub ? hub.InterruptIn(buf, std::min<u32>(length, sizeof(buf)))
                                      : gamepad.InterruptIn(buf, std::min<u32>(length, sizeof(buf)));
            if (n < 0)
                return false; // NAK: try again next time
            std::memcpy(Ptr(PhysToGuest(cbp)), buf, u32(n));
            moved = n;
        }
        else
        {
            if (who == kPad && length && cbp)
                gamepad.InterruptOut(Ptr<u8>(PhysToGuest(cbp)), length);
            moved = int(length);
        }
        // complete it: the condition code, the buffer pointer (0 when all of it moved), the toggle
        if (cc == 0 && u32(moved) < length)
        {
            cbp += u32(moved);
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
        Write<u32>(td + 8, done);
        done = PhysicalAddress(td);
        return true;
    }

    // Runs the TDs of each ED of a list until each NAKs or ends. Returns whether any TD moved.
    bool RunList(u32 head_phys)
    {
        bool moved = false;
        for (u32 ed_phys = head_phys, guard = 0; ed_phys && guard < 64; guard++)
        {
            const u32 ed = PhysToGuest(ed_phys & ~0xfu);
            if (!ed || !GuestMapped(ed, 16))
                break;
            const u32 ed0 = Read<u32>(ed);
            if (!(ed0 & (1u << 14)) && !(ed0 & (1u << 15))) // not skipped, not isochronous
                for (int n = 0; n < 8; n++)
                {
                    const u32 headp = Read<u32>(ed + 8), tailp = Read<u32>(ed + 4);
                    if ((headp & 1) || (headp & ~0xfu) == (tailp & ~0xfu))
                        break;
                    const u32 td = PhysToGuest(headp & ~0xfu);
                    if (!td || !GuestMapped(td, 16) || !RunTd(ed, td))
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
        frame_start = std::chrono::steady_clock::now();
        const u32 hcca = R(HcHCCA) ? PhysToGuest(R(HcHCCA)) : 0;
        if (!hcca || !GuestMapped(hcca, 256))
            return;
        const u32 before = R(HcFmNumber);
        R(HcFmNumber) = (before + 1) & 0xffff;
        if ((before ^ R(HcFmNumber)) & 0x8000)
            status |= FNO; // the frame number's top bit changed
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
        status |= SF;
        if (done && !(status & WDH))
        {
            Write<u32>(hcca + 0x84, done | ((status & enable & ~WDH) ? 1 : 0));
            done = 0;
            status |= WDH;
        }
        UpdateIrq();
    }

    void Reset()
    {
        // the registers' reset values (OHCI 1.0a, 7.1-7.4)
        std::memset(regs, 0, sizeof(regs));
        status = enable = done = 0;
        R(HcFmInterval) = 0x27782edf; // FSLargestDataPacket 0x2778, FrameInterval 11999
        R(HcLSThreshold) = 0x628;
        for (u32 i = 0; i < kPorts; i++)
            port[i] = 0x100; // powered (no power switching)
        if (has_pad)
        {
            port[2] = 0x1 | 0x100 | (1u << 16); // the gamepad: connected, its connection a change to report
            status = RHSC;
        }
    }

    u32 ReadReg(u32 offset)
    {
        std::lock_guard lock(m);
        switch (offset)
        {
        case HcRevision: return 0x10; // OHCI 1.0
        case HcInterruptStatus: return status;
        case HcInterruptEnable:
        case HcInterruptDisable: return enable;
        case HcFmRemaining: // counts down through the 1 ms frame
        {
            const u32 interval = R(HcFmInterval) & 0x3fff;
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                                  frame_start).count();
            const u32 left = us >= 1000 ? 0 : u32(interval - interval * u32(us) / 1000);
            return left | (R(HcFmInterval) & 0x80000000u); // FRT follows FIT
        }
        case HcRhDescriptorA: return kPorts | (1u << 9); // no power switching: always powered
        case HcRhDescriptorB: return 0;                  // every port's device removable
        case HcRhStatus: return 0;
        }
        if (offset >= HcRhPortStatus && offset < HcRhPortStatus + 4 * kPorts)
            return port[(offset - HcRhPortStatus) / 4];
        return offset < kSize ? R(offset) : 0;
    }

    void WriteReg(u32 offset, u32 value)
    {
        std::lock_guard lock(m);
        switch (offset)
        {
        case HcRevision: return; // read-only
        case HcCommandStatus:
            if (value & 1) // host controller reset: the registers to their reset values, at once
            {
                const u32 keep_ports[kPorts] = {port[0], port[1], port[2], port[3]};
                Reset();
                std::memcpy(port, keep_ports, sizeof(port)); // the root hub isn't reset by HCR
                R(HcControl) = 0xc0; // and the controller in USBSUSPEND (OHCI 1.0a 7.1.2)
            }
            R(HcCommandStatus) |= value & 0x0e; // CLF, BLF, OCR (ownership: already the driver's)
            return;
        case HcInterruptStatus: status &= ~value; UpdateIrq(); return;
        case HcInterruptEnable: enable |= value; UpdateIrq(); return;
        case HcInterruptDisable: enable &= ~value; UpdateIrq(); return;
        case HcHCCA: R(HcHCCA) = value & ~0xffu; return; // 256-byte aligned: the low bits read 0
        case HcControl: R(HcControl) = value & ~(1u << 8); return; // interrupt routing: never to SMM
        case HcFmRemaining:
        case HcFmNumber: return; // read-only
        case HcRhStatus: return;
        case HcRhDescriptorA:
        case HcRhDescriptorB: return;
        }
        if (offset >= HcRhPortStatus && offset < HcRhPortStatus + 4 * kPorts)
        {
            u32& p = port[(offset - HcRhPortStatus) / 4];
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
                status |= RHSC;
                hub.Reset(gamepad); // the reset hub answers at address 0, its ports off
            }
            UpdateIrq();
            return;
        }
        if (offset < kSize)
            R(offset) = value;
    }
};

Ohci g_ohci[2];

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

} // namespace xb

// QEMU's USB (src/third_party/xemu/hw/usb: hcd-ohci, its core, bus and hub) with xemu's original Xbox
// controller (hw/xbox/xid*): the default; XB_USB=own picks this file's own controllers. The pad's state
// and rumble go through xemu's input hooks (ui/xemu-input.h), here.
extern "C" {
#include "ui/xemu-input.h"
void xb_usb_init(void);
uint64_t xb_ohci_read(int which, uint64_t addr, unsigned int size);
void xb_ohci_write(int which, uint64_t addr, uint64_t val, unsigned int size);

static ControllerState g_xemu_pad;

ControllerState* xemu_input_get_bound(int index) { return index == 0 ? &g_xemu_pad : nullptr; }

// xbox-core's pad as xemu's controller state: the analog buttons pressed past a quarter (xemu's XID
// reports them all-or-nothing), the triggers' 0-255 as xemu's axes (it reports them >> 7)
void xemu_input_update_controller(ControllerState* state)
{
    using namespace xb;
    PadState pad;
    {
        std::lock_guard lock(g_pad_mutex);
        pad = g_pad;
    }
    u16 b = 0;
    const u16 kAnalog[6] = {CONTROLLER_BUTTON_A, CONTROLLER_BUTTON_B, CONTROLLER_BUTTON_X,
                            CONTROLLER_BUTTON_Y, CONTROLLER_BUTTON_BLACK, CONTROLLER_BUTTON_WHITE};
    for (int i = 0; i < 6; i++)
        if (pad.analog[i] > 0x40)
            b |= kAnalog[i];
    const std::pair<u16, u16> kDigital[8] = {{0x01, CONTROLLER_BUTTON_DPAD_UP},    {0x02, CONTROLLER_BUTTON_DPAD_DOWN},
                                             {0x04, CONTROLLER_BUTTON_DPAD_LEFT},  {0x08, CONTROLLER_BUTTON_DPAD_RIGHT},
                                             {0x10, CONTROLLER_BUTTON_START},      {0x20, CONTROLLER_BUTTON_BACK},
                                             {0x40, CONTROLLER_BUTTON_LSTICK},     {0x80, CONTROLLER_BUTTON_RSTICK}};
    for (const auto& [ours, theirs] : kDigital)
        if (pad.buttons & ours)
            b |= theirs;
    state->buttons = b;
    state->axis[CONTROLLER_AXIS_LTRIG] = s16(pad.analog[6] << 7);
    state->axis[CONTROLLER_AXIS_RTRIG] = s16(pad.analog[7] << 7);
    state->axis[CONTROLLER_AXIS_LSTICK_X] = pad.lx;
    state->axis[CONTROLLER_AXIS_LSTICK_Y] = pad.ly;
    state->axis[CONTROLLER_AXIS_RSTICK_X] = pad.rx;
    state->axis[CONTROLLER_AXIS_RSTICK_Y] = pad.ry;
}

void xemu_input_update_rumble(ControllerState* state)
{
    std::lock_guard lock(xb::g_pad_mutex);
    xb::g_rumble[0] = state->rumble_l;
    xb::g_rumble[1] = state->rumble_r;
}
}

namespace xb {

void InitUsb()
{
    const char* choice = std::getenv("XB_USB");
    if (!(choice && std::string(choice) == "own"))
    {
        Log("usb: QEMU's OHCI controllers with xemu's Xbox controller (player 1: a hub in port 3, the pad in its port 1)");
        xb_usb_init();
        const u32 bases[2] = {0xfed00000, 0xfed08000};
        for (int i = 0; i < 2; i++)
            AddDevice({bases[i], kSize, i == 0 ? "ohci0" : "ohci1",
                       [i](u32 o, u32 size) { return u32(xb_ohci_read(i, o, size)); },
                       [i](u32 o, u32 size, u32 v) { xb_ohci_write(i, o, v, size); }});
        return;
    }
    // USB 0 (0xfed00000, IRQ 1): the gamepad on root port 3, the front panel's first port (player 1);
    // USB 1 (0xfed08000, IRQ 9): nothing plugged in
    const u32 bases[2] = {0xfed00000, 0xfed08000}, irqs[2] = {1, 9};
    for (int i = 0; i < 2; i++)
    {
        Ohci& c = g_ohci[i];
        c.base = bases[i];
        c.irq = irqs[i];
        c.has_pad = i == 0;
        c.Reset();
        AddDevice({c.base, kSize, i == 0 ? "ohci0" : "ohci1", [&c](u32 o, u32) { return c.ReadReg(o); },
                   [&c](u32 o, u32, u32 v) { c.WriteReg(o, v); }});
    }
    std::thread([] {
        prof::RegisterThread("USB (xbox-core's)");
        auto next = std::chrono::steady_clock::now();
        while (!g_quit)
        {
            next += std::chrono::milliseconds(1);
            std::this_thread::sleep_until(next);
            for (Ohci& c : g_ohci)
            {
                std::lock_guard lock(c.m);
                c.Frame();
            }
        }
    }).detach();
}

} // namespace xb
