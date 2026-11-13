// The NV2A: the Xbox's GPU, as its driver code (nxdk's pbkit, the XDK's Direct3D) programs it.
//
//  - registers: PMC (interrupts), PFIFO, PFB, PTIMER, PGRAPH, PCRTC, the VGA ports, PRAMIN (the GPU's
//    instance memory: DMA objects, graphics objects, RAMHT), USER (the channels' DMA PUT/GET)
//  - PFIFO: a GPU thread runs the DMA pusher over the push buffer (methods, jumps, calls) whenever
//    the driver moves PUT, and hands each method to PGRAPH; object handles go through RAMHT
//  - PGRAPH: the protocol the driver waits on (notify interrupts for its software methods,
//    context switches, flip counters and stalls); every Kelvin (3D) method's value is kept for
//    the renderer (render.cpp), which draws into guest memory
// Register semantics follow the hardware as xemu documents it (hw/xbox/nv2a); the code is this
// project's own.
#include <sys/stat.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "profiler.h"
#include "core.h"
#include "diagnostics.h"
#include "devices.h"
#include "gpu.h"
#include "nv2a.h"

namespace xb {

namespace {

// blocks (offsets from 0xfd000000)
constexpr u32 kPfifo = 0x002000, kPtimer = 0x009000, kPfb = 0x100000, kPgraph = 0x400000, kPcrtc = 0x600000,
              kPramin = 0x700000, kRaminSize = 0x100000, kUser = 0x800000, kSize = 0x01000000;
// registers
constexpr u32 PMC_BOOT_0 = 0x000000, PMC_INTR_0 = 0x000100, PMC_INTR_EN_0 = 0x000140;
constexpr u32 PFIFO_INTR_0 = kPfifo + 0x100, PFIFO_INTR_EN_0 = kPfifo + 0x140, PFIFO_RAMHT = kPfifo + 0x210,
              PFIFO_CACHES = kPfifo + 0x500, PFIFO_CACHE1_PUSH0 = kPfifo + 0x1200, PFIFO_CACHE1_PUSH1 = kPfifo + 0x1204,
              PFIFO_CACHE1_STATUS = kPfifo + 0x1214, PFIFO_CACHE1_DMA_PUSH = kPfifo + 0x1220,
              PFIFO_CACHE1_DMA_STATE = kPfifo + 0x1228, PFIFO_CACHE1_DMA_INSTANCE = kPfifo + 0x122c,
              PFIFO_CACHE1_DMA_PUT = kPfifo + 0x1240, PFIFO_CACHE1_DMA_GET = kPfifo + 0x1244,
              PFIFO_CACHE1_REF = kPfifo + 0x1248, PFIFO_CACHE1_DMA_SUBROUTINE = kPfifo + 0x124c,
              PFIFO_CACHE1_PULL0 = kPfifo + 0x1250;
constexpr u32 PTIMER_TIME_0 = kPtimer + 0x400, PTIMER_TIME_1 = kPtimer + 0x410;
constexpr u32 PFB_CFG0 = kPfb + 0x200, PFB_CFG1 = kPfb + 0x204, PFB_WC_CACHE = kPfb + 0x410;
constexpr u32 PGRAPH_INTR = kPgraph + 0x100, PGRAPH_NSOURCE = kPgraph + 0x108, PGRAPH_INTR_EN = kPgraph + 0x140,
              PGRAPH_CTX_CONTROL = kPgraph + 0x144, PGRAPH_CTX_USER = kPgraph + 0x148,
              PGRAPH_CTX_SWITCH1 = kPgraph + 0x14c, PGRAPH_CTX_CACHE1 = kPgraph + 0x160,
              PGRAPH_STATUS = kPgraph + 0x700, PGRAPH_TRAPPED_ADDR = kPgraph + 0x704,
              PGRAPH_TRAPPED_DATA_LOW = kPgraph + 0x708, PGRAPH_SURFACE = kPgraph + 0x710,
              PGRAPH_INCREMENT = kPgraph + 0x71c, PGRAPH_FIFO = kPgraph + 0x720,
              PGRAPH_CHANNEL_CTX_POINTER = kPgraph + 0x784, PGRAPH_CHANNEL_CTX_TRIGGER = kPgraph + 0x788;
constexpr u32 PCRTC_INTR = kPcrtc + 0x100, PCRTC_INTR_EN = kPcrtc + 0x140, PCRTC_START = kPcrtc + 0x800,
              PRMCIO_INP0 = 0x6013da;
constexpr u32 INTR_ERROR = 1u << 20, INTR_CONTEXT_SWITCH = 1u << 12;

std::vector<u32> g_regs(kPramin / 4);
// RAMIN is the RAM's last 1 MiB, as on the Xbox: the driver writes objects there through memory
// (MmClaimGpuInstanceMemory) and the GPU reads them as RAMIN
u8* g_ramin = nullptr;
std::mutex g_m;
std::condition_variable g_kick;
// XB_NV2A_SYNC=1: a write of PUT waits until the GPU thread has run the push buffer up to it (or
// can't go on: a wait for the CPU, the FIFO off), as a GPU that keeps up with the CPU. For tests
// whose CPU reuses memory the GPU hasn't read yet (nxdk_pgraph_tests frees each vertex buffer as
// it allocates the next); games keep the GPU's own pace.
const bool g_sync_put = std::getenv("XB_NV2A_SYNC") != nullptr;
std::condition_variable g_drained;
u32 g_pgraph_pending = 0, g_pfifo_pending = 0, g_pcrtc_pending = 0;
bool g_waiting_for_nop = false, g_waiting_for_context_switch = false, g_waiting_for_flip = false;
const auto g_start = std::chrono::steady_clock::now();

u32& R(u32 offset)
{
    return g_regs[offset / 4];
}
u32 Field(u32 v, u32 mask)
{
    return (v & mask) >> __builtin_ctz(mask);
}
void SetField(u32& v, u32 mask, u32 x)
{
    v = (v & ~mask) | ((x << __builtin_ctz(mask)) & mask);
}
u32 Ramin32(u32 offset)
{
    u32 v = 0;
    if (offset + 4 <= kRaminSize)
        std::memcpy(&v, &g_ramin[offset], 4);
    return v;
}

// PMC: which units have an enabled interrupt pending; the NV2A's line follows it
u32 PendingUnits()
{
    u32 v = 0;
    if (g_pfifo_pending & R(PFIFO_INTR_EN_0))
        v |= 1u << 8;
    if (g_pgraph_pending & R(PGRAPH_INTR_EN))
        v |= 1u << 12;
    if (g_pcrtc_pending & R(PCRTC_INTR_EN))
        v |= 1u << 24;
    return v;
}
void UpdateIrq()
{
    SetIrq(3, (R(PMC_INTR_EN_0) & 1) && PendingUnits());
}

// ---- PFIFO

struct RamhtEntry
{
    u32 instance = 0, engine = 0, channel = 0;
    bool valid = false;
};

RamhtEntry RamhtLookup(u32 handle)
{
    const u32 ramht = R(PFIFO_RAMHT);
    const u32 size = 1u << (((ramht >> 16) & 3) + 12);
    const u32 bits = __builtin_ctz(size) - 1;
    u32 hash = 0;
    for (u32 h = handle; h; h >>= bits)
        hash ^= h & ((1u << bits) - 1);
    hash ^= (R(PFIFO_CACHE1_PUSH1) & 0x1f) << (bits - 4);
    const u32 at = ((ramht & 0x1f0) << 8) + hash * 8; // NV_PFIFO_RAMHT_BASE_ADDRESS, in 4 KiB units
    RamhtEntry e;
    if (Ramin32(at) != handle)
        return e;
    const u32 context = Ramin32(at + 4);
    e.instance = (context & 0xffff) << 4;
    e.engine = (context >> 16) & 3;
    e.channel = (context >> 24) & 0x1f;
    e.valid = context & 0x80000000;
    return e;
}

// a DMA object in RAMIN: its memory's physical base and limit
struct Dma
{
    u32 address = 0, limit = 0;
};
Dma LoadDma(u32 instance)
{
    const u32 flags = Ramin32(instance), limit = Ramin32(instance + 4), frame = Ramin32(instance + 8);
    return {(frame & 0xfffff000) | (flags >> 20), limit};
}

bool FifoAccess()
{
    return R(PGRAPH_FIFO) & 1;
}

bool FlipStallComplete()
{
    const u32 s = R(PGRAPH_SURFACE);
    return Field(s, 0x07000000) != Field(s, 0x00700000); // read != write
}

bool PullerStalled()
{
    if (g_waiting_for_flip)
    {
        if (!FlipStallComplete())
            return true;
        g_waiting_for_flip = false;
    }
    return g_waiting_for_nop || g_waiting_for_context_switch || !FifoAccess();
}

// PGRAPH: a method on a subchannel (method 0: bind the object `parameter` is the instance of).
// False when it can't run yet (a context switch first): the pusher stops before it.
bool Method(u32 subchannel, u32 method, u32 parameter);

// The DMA pusher: runs the push buffer from GET to PUT, stopping where PGRAPH stalls.
void RunPusher()
{
    u32& push0 = R(PFIFO_CACHE1_PUSH0);
    u32& dma_push = R(PFIFO_CACHE1_DMA_PUSH);
    if (!(push0 & 1) || !(dma_push & 1) || (dma_push & (1u << 12)))
        return;
    const Dma dma = LoadDma((R(PFIFO_CACHE1_DMA_INSTANCE) & 0xffff) << 4);
    const u32 base = PhysToGuest(dma.address);
    if (!base)
        return;
    u32& state = R(PFIFO_CACHE1_DMA_STATE);
    u32& get = R(PFIFO_CACHE1_DMA_GET);
    u32& subroutine = R(PFIFO_CACHE1_DMA_SUBROUTINE);
    while (FifoAccess() && !g_waiting_for_nop)
    {
        const u32 put = R(PFIFO_CACHE1_DMA_PUT);
        if (get == put)
            break;
        if (get > dma.limit)
        {
            Log("nv2a: push buffer GET %#x beyond its DMA object's limit %#x", get, dma.limit);
            SetField(state, 0xe0000000, 6);
            break;
        }
        const u32 word = Read<u32>(base + get);
        u32 count = Field(state, 0x1ffc0000);
        if (count)
        {
            if (PullerStalled())
                break;
            const u32 method = Field(state, 0x00001ffc) << 2, sub = Field(state, 0x0000e000);
            R(PFIFO_CACHE1_STATUS) &= ~(1u << 4);
            if (!Method(sub, method, word))
                break;
            get += 4;
            if (!(state & 1)) // increasing
                SetField(state, 0x00001ffc, (method + 4) >> 2);
            SetField(state, 0x1ffc0000, count - 1);
            R(PFIFO_CACHE1_STATUS) |= 1u << 4;
            continue;
        }
        get += 4;
        if ((word & 0xe0000003) == 0x20000000) // old jump
            get = word & 0x1fffffff;
        else if ((word & 3) == 1) // jump
            get = word & 0xfffffffc;
        else if ((word & 3) == 2) // call
        {
            subroutine = get | 1;
            get = word & 0xfffffffc;
        }
        else if (word == 0x00020000) // return
        {
            get = subroutine & 0xfffffffc;
            subroutine = 0;
        }
        else if ((word & 0xe0030003) == 0 || (word & 0xe0030003) == 0x40000000)
        {
            SetField(state, 0x00001ffc, (word & 0x1fff) >> 2);
            SetField(state, 0x0000e000, (word >> 13) & 7);
            SetField(state, 0x1ffc0000, (word >> 18) & 0x7ff);
            state = (state & ~1u) | (word & 0x40000000 ? 1 : 0);
        }
        else
        {
            Log("nv2a: reserved push buffer command %#x at %#x", word, get - 4);
            SetField(state, 0xe0000000, 4);
            break;
        }
    }
}

// ---- PGRAPH

u32 g_subchannel_class[8] = {}, g_subchannel_instance[8] = {};

// The driver handles the interrupt; the puller waits until it is acknowledged.
void RaiseNotify(u32 subchannel, u32 method, u32 parameter)
{
    const u32 channel = Field(R(PGRAPH_CTX_USER), 0x1f000000);
    u32& trapped = R(PGRAPH_TRAPPED_ADDR);
    SetField(trapped, 0x01f00000, channel);
    SetField(trapped, 0x00070000, subchannel);
    SetField(trapped, 0x00001fff, method);
    R(PGRAPH_TRAPPED_DATA_LOW) = parameter;
    R(PGRAPH_NSOURCE) = 1; // notification
    g_pgraph_pending |= INTR_ERROR;
    g_waiting_for_nop = true;
    UpdateIrq();
}

void ContextSwitch(u32 channel)
{
    const bool valid = (R(PGRAPH_CTX_CONTROL) & (1u << 16)) && Field(R(PGRAPH_CTX_USER), 0x1f000000) == channel;
    if (valid)
        return;
    SetField(R(PGRAPH_TRAPPED_ADDR), 0x01f00000, channel);
    g_waiting_for_context_switch = true;
    g_pgraph_pending |= INTR_CONTEXT_SWITCH;
    UpdateIrq();
}

bool Method(u32 subchannel, u32 method, u32 parameter)
{
    if (method == 0)
    {
        const RamhtEntry e = RamhtLookup(parameter);
        if (!e.valid)
        {
            Log("nv2a: object %#x not in RAMHT", parameter);
            return true;
        }
        ContextSwitch(e.channel);
        if (g_waiting_for_context_switch)
            return false; // run again once the driver has switched
        parameter = e.instance;
        g_subchannel_class[subchannel] = Ramin32(parameter) & 0xff;
        g_subchannel_instance[subchannel] = parameter;
        for (int i = 0; i < 4; i++)
            R(PGRAPH_CTX_CACHE1 + 0x20 * i + subchannel * 4) = Ramin32(parameter + 4 * i);
        R(PGRAPH_CTX_CACHE1 + 0x80 + subchannel * 4) = parameter;
    }
    else if (method >= 0x180 && method < 0x200)
    {
        // methods that take objects: the handle becomes the object's instance
        const RamhtEntry e = RamhtLookup(parameter);
        parameter = e.valid ? e.instance : 0;
    }
    for (int i = 0; i < 5; i++)
        R(PGRAPH_CTX_SWITCH1 + 4 * i) = R(PGRAPH_CTX_CACHE1 + 0x20 * i + subchannel * 4);
    R(PGRAPH_CTX_SWITCH1) = (R(PGRAPH_CTX_SWITCH1) & ~0xffu) | g_subchannel_class[subchannel];
    const u32 cls = g_subchannel_class[subchannel];
    diag::Method(cls, method, parameter);
    if (g_verbose >= 4)
        Log("nv2a: [%u] class %#x method %#06x = %#x", subchannel, cls, method, parameter);
    if (cls != 0x97)
    {
        RenderOtherClass(cls, method, parameter);
        return true;
    }
    u32& surface = R(PGRAPH_SURFACE);
    switch (method)
    {
    case 0x100: // NO_OPERATION: a software method when its parameter isn't 0
        if (parameter)
            RaiseNotify(subchannel, method, parameter);
        return true;
    case 0x120: SetField(surface, 0x07000000, parameter); return true; // SET_FLIP_READ
    case 0x124: SetField(surface, 0x00700000, parameter); return true; // SET_FLIP_WRITE
    case 0x128: SetField(surface, 0x70000000, parameter); return true; // SET_FLIP_MODULO
    case 0x12c:                                                          // FLIP_INCREMENT_WRITE
    {
        const u32 modulo = std::max<u32>(1, Field(surface, 0x70000000));
        SetField(surface, 0x00700000, (Field(surface, 0x00700000) + 1) % modulo);
        return true;
    }
    case 0x130: // FLIP_STALL
        g_waiting_for_flip = true;
        return true;
    }
    // SET_ZSTENCIL_CLEAR_VALUE and SET_COLOR_CLEAR_VALUE are also PGRAPH's PARAMETER_A and _B, which
    // a driver's software methods read their arguments from (pbkit's PB_SETOUTER)
    if (method == 0x1d8c)
        R(kPgraph + 0x1a88) = parameter;
    else if (method == 0x1d90)
        R(kPgraph + 0x186c) = parameter;
    KelvinMethod(method, parameter);
    return true;
}

void GpuThread()
{
    prof::RegisterThread("NV2A (xbox-core's)");
    std::unique_lock lock(g_m);
    u32 last_get = ~0u;
    auto last_report = std::chrono::steady_clock::now();
    while (!g_quit)
    {
        RunPusher();
        if (g_sync_put)
            g_drained.notify_all();
        g_kick.wait_for(lock, std::chrono::milliseconds(5));
        // a push buffer that doesn't move: why (-v 2)
        const auto now = std::chrono::steady_clock::now();
        if (g_verbose >= 2 && now - last_report > std::chrono::seconds(1))
        {
            last_report = now;
            const u32 get = R(PFIFO_CACHE1_DMA_GET), put = R(PFIFO_CACHE1_DMA_PUT);
            if (get != put && get == last_get)
                Log("nv2a: stalled at GET %#x (PUT %#x): nop %d, context switch %d, flip %d, fifo access %d, push0 %#x, "
                    "dma push %#x, dma state %#x, pgraph intr %#x en %#x, pmc en %#x",
                    get, put, g_waiting_for_nop, g_waiting_for_context_switch, g_waiting_for_flip, FifoAccess(),
                    R(PFIFO_CACHE1_PUSH0), R(PFIFO_CACHE1_DMA_PUSH), R(PFIFO_CACHE1_DMA_STATE), g_pgraph_pending,
                    R(PGRAPH_INTR_EN), R(PMC_INTR_EN_0));
            last_get = get;
        }
    }
}

u32 ReadReg(u32 offset, u32 size)
{
    if (offset >= kUser)
    {
        // a channel's PUT, GET and REF (channel 0 is the only one used)
        switch (offset & 0xffff)
        {
        case 0x40: return R(PFIFO_CACHE1_DMA_PUT);
        case 0x44: return R(PFIFO_CACHE1_DMA_GET);
        case 0x48: return R(PFIFO_CACHE1_REF);
        }
        return 0;
    }
    if (offset >= kPramin)
    {
        u32 v = 0;
        if (offset - kPramin + size <= kRaminSize)
            std::memcpy(&v, &g_ramin[offset - kPramin], size);
        return v;
    }
    switch (offset)
    {
    case PMC_BOOT_0: return 0x02A000A2;
    case PMC_INTR_0: return PendingUnits();
    case PFIFO_INTR_0: return g_pfifo_pending;
    case PGRAPH_INTR: return g_pgraph_pending;
    case PCRTC_INTR: return g_pcrtc_pending;
    case PGRAPH_STATUS: return 0; // idle
    case PFB_CFG0: return 0x03070103;
    case PFB_CFG1: return 0x11448000;
    case PFB_WC_CACHE: return R(PFB_WC_CACHE) & ~0x10000u; // a flush is done at once
    case PFIFO_CACHES: return R(PFIFO_CACHES) & ~0x10u;
    case kPfifo + 0x400: return 0x10; // RUNOUT_STATUS: the runout queue is empty
    case PFIFO_CACHE1_DMA_PUSH: return R(PFIFO_CACHE1_DMA_PUSH) & ~0x10u; // not busy
    case kPcrtc + 0x808: // RASTER: the line being scanned out, from the time (525 lines a 1/59.94 s frame)
    {
        const u64 us = u64(std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - g_start).count());
        return u32((us % 16683) * 525 / 16683);
    }
    case PTIMER_TIME_0:
    case PTIMER_TIME_1:
    {
        const u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - g_start).count());
        return offset == PTIMER_TIME_0 ? u32(ns) & ~0x1fu : u32(ns >> 32);
    }
    case PRMCIO_INP0:
    {
        static u32 n = 0; // bit 3: in vertical retrace; toggles so that polling loops finish
        return (++n & 8) ? 0x08 : 0x00;
    }
    }
    u32 v = R(offset & ~3u);
    if (size < 4)
        v = (v >> ((offset & 3) * 8)) & (size == 1 ? 0xff : 0xffff);
    return v;
}

void WriteReg(u32 offset, u32 size, u32 value)
{
    // PVIDEO_STOP: the overlay is torn down when bit 0 is written (xemu's pvideo.c)
    if (offset == 0x8704)
    {
        if (value & 1)
            R(0x8700) = 0;
        return;
    }
    if (offset >= kUser)
    {
        if ((offset & 0xffff) == 0x40)
        {
            R(PFIFO_CACHE1_DMA_PUT) = value;
            g_kick.notify_all();
        }
        else if ((offset & 0xffff) == 0x48) // REF: the driver's own counter, read back as written (as xemu has it)
            R(PFIFO_CACHE1_REF) = value;
        return;
    }
    if (offset >= kPramin)
    {
        if (offset - kPramin + size <= kRaminSize)
            std::memcpy(&g_ramin[offset - kPramin], &value, size);
        return;
    }
    switch (offset)
    {
    case PMC_INTR_0: return;
    case PFIFO_INTR_0: g_pfifo_pending &= ~value; UpdateIrq(); return;
    case PCRTC_INTR: g_pcrtc_pending &= ~value; UpdateIrq(); return;
    case PGRAPH_INTR:
        g_pgraph_pending &= ~value;
        if (!(g_pgraph_pending & INTR_ERROR))
            g_waiting_for_nop = false;
        if (!(g_pgraph_pending & INTR_CONTEXT_SWITCH))
            g_waiting_for_context_switch = false;
        UpdateIrq();
        g_kick.notify_all();
        return;
    case PGRAPH_INCREMENT:
        if (value & 2) // READ_3D
        {
            u32& s = R(PGRAPH_SURFACE);
            SetField(s, 0x07000000, (Field(s, 0x07000000) + 1) % std::max<u32>(1, Field(s, 0x70000000)));
            g_kick.notify_all();
        }
        return;
    case PGRAPH_CHANNEL_CTX_TRIGGER:
        if (value & 1) // READ_IN: the channel's context (its CTX_USER first)
            R(PGRAPH_CTX_USER) = Ramin32((R(PGRAPH_CHANNEL_CTX_POINTER) & 0xffff) << 4);
        return;
    }
    u32& r = R(offset & ~3u);
    if (size == 4)
        r = value;
    else
    {
        const u32 shift = (offset & 3) * 8, mask = (size == 1 ? 0xffu : 0xffffu) << shift;
        r = (r & ~mask) | ((value << shift) & mask);
    }
    if (offset == PGRAPH_FIFO || offset == PFIFO_CACHE1_DMA_PUSH || offset == PFIFO_CACHE1_PUSH0 ||
        offset == PFIFO_CACHE1_PULL0)
        g_kick.notify_all();
    if (offset == PMC_INTR_EN_0 || offset == PGRAPH_INTR_EN || offset == PCRTC_INTR_EN || offset == PFIFO_INTR_EN_0)
        UpdateIrq();
    if (offset == PCRTC_START)
        diag::g_frames++;
    if (offset == PCRTC_START && g_verbose >= 2)
        Log("nv2a: display start %#x", value);
}

} // namespace

#ifdef XB_XEMU_NV2A
// xemu's NV2A (src/third_party/xemu/hw/xbox/nv2a: PFIFO, PGRAPH and its Vulkan renderer, PTIMER,
// PCRTC...), chosen at start: the default where it is built; XB_NV2A=own picks this file's
extern "C" {
bool xb_nv2a_init(uint8_t* ram, uint64_t ram_size);
uint64_t xb_nv2a_read(uint64_t addr, unsigned int size);
void xb_nv2a_write(uint64_t addr, uint64_t val, unsigned int size);
void xb_nv2a_vblank(void);
// xemu's renderer: the surfaces drawn are back in RAM (a vblank's download): the picture is copied
void xb_nv2a_picture_ready(uint64_t writebacks) { xb::TakePicture(writebacks); }
int xb_nv2a_profile(char* out, int size, double seconds);
// the folder xemu's renderer keeps the shaders and pipelines it made in, from one run to the next
// (XB_NO_SHADER_CACHE=1: none)
const char* xb_shader_cache_dir(void)
{
    static const std::string dir = [] {
        if (std::getenv("XB_NO_SHADER_CACHE"))
            return std::string();
        const std::string d = xb::DataDirectory() + "/shader-cache";
        mkdir(d.c_str(), 0755);
        return d;
    }();
    return dir.empty() ? nullptr : dir.c_str();
}
uint32_t xb_nv2a_display_start(void);
}
namespace {
bool g_xemu_nv2a = false;
}
#endif

namespace {
bool g_own_chosen = false;
std::string g_xemu_guard; // the step file while xemu's renderer's first frames are drawn
std::string g_xemu_step_file; // the step file during its start-up (xb_nv2a_step writes it)
// the ends in its start-up or first frames in a row, this run's included should it end there too
// (closing the app then, with the PS button, looks the same as the console stopping it: only 3 in
// a row mean the renderer, and then Midway's own NV2A is used once; before, one did, and a game
// closed while it started made the next run use Midway's own, at 3 frames a second)
int g_xemu_strikes = 1;
constexpr int kXemuStrikes = 3;
[[maybe_unused]] u32 g_xemu_vblanks = 0;
}

void SetNv2aChoice(bool own) { g_own_chosen = own; }

void InitNv2a()
{
#ifdef XB_XEMU_NV2A
    const char* choice = std::getenv("XB_NV2A");
    g_xemu_nv2a = !(choice && std::string(choice) == "own") && !g_own_chosen;
    // a start that died in xemu's renderer's start-up (the console's step file, as the GPU path's)
    // is followed by one with xbox-core's own
    std::string guard = gpu::GuardFile();
    if (!guard.empty())
        guard = guard.substr(0, guard.rfind('/') + 1) + "xemu-nv2a-step.txt";
    if (g_xemu_nv2a && !guard.empty())
        if (FILE* f = std::fopen(guard.c_str(), "r"))
        {
            char step[160] = {}, build[128] = {}, count[32] = {};
            const bool same = std::fgets(step, sizeof(step), f) && std::fgets(build, sizeof(build), f) &&
                              std::string(build).rfind(kBuildId, 0) == 0;
            const int strikes = same ? (std::fgets(count, sizeof(count), f) ? std::max(1, std::atoi(count)) : 1) : 0;
            std::fclose(f);
            const std::string where(step, std::strcspn(step, "\n"));
            if (same && strikes >= kXemuStrikes)
            {
                g_xemu_nv2a = false;
                Log("nv2a: the last %d starts ended in xemu's renderer (%s): this one uses Midway's own NV2A (the "
                    "next tries xemu's again)", strikes, where.c_str());
                std::remove(guard.c_str());
            }
            else if (same)
            {
                g_xemu_strikes = strikes + 1;
                Log("nv2a: the last start ended in xemu's renderer (%s): closed then, or the console stopped it (%d in "
                    "a row; after %d, Midway's own NV2A once)", where.c_str(), strikes, kXemuStrikes);
            }
        }
    if (g_xemu_nv2a)
    {
        if (!guard.empty())
            if (FILE* f = std::fopen(guard.c_str(), "w"))
            {
                std::fprintf(f, "xemu renderer start-up\n%s\n%d\n", kBuildId, g_xemu_strikes);
                std::fclose(f);
            }
        Log("nv2a: xemu's (PGRAPH on its Vulkan renderer)");
        g_xemu_step_file = guard;
        const bool ok = xb_nv2a_init(Ptr<u8>(kContiguousBase), kContiguousSize);
        g_xemu_step_file.clear();
        if (!guard.empty())
        {
            std::remove(guard.c_str());
            // its first half minute of drawing is guarded too (a driver that takes the app down in
            // the renderer's first frames): the step file stays until then (Nv2aVblank)
            if (ok)
                if (FILE* f = std::fopen(guard.c_str(), "w"))
                {
                    std::fprintf(f, "xemu renderer's first frames\n%s\n%d\n", kBuildId, g_xemu_strikes);
                    std::fclose(f);
                    g_xemu_guard = guard;
                }
        }
        if (!ok)
        {
            Log("nv2a: xemu's renderer didn't start (why: the xemu line before): xbox-core's own NV2A");
            g_xemu_nv2a = false;
        }
    }
    if (g_xemu_nv2a)
    {
        // what the console's BIOS leaves in the memory controller (xemu runs the BIOS; xbox-core's
        // kernel stands in for it): the RAM configuration, which drivers read (pbkit sizes its
        // memory from them); the console's values
        xb_nv2a_write(0x100200, 0x03070103, 4); // PFB_CFG0
        xb_nv2a_write(0x100204, 0x11448000, 4); // PFB_CFG1
        AddDevice({kDeviceBase, kSize, "nv2a", [](u32 offset, u32 size) { return u32(xb_nv2a_read(offset, size)); },
                   [](u32 offset, u32 size, u32 value) { xb_nv2a_write(offset, value, size); }});
        return;
    }
#endif
    g_ramin = Ptr<u8>(kGpuInstanceMemory);
    R(PFIFO_CACHE1_STATUS) = 1u << 4; // empty
    // the clocks as the BIOS leaves them (16.667 MHz crystal): the core at 233 MHz (M 1, N 28,
    // P 1), memory at 200 MHz; drivers read them (pbkit gives up on an unset core PLL)
    R(0x680500) = 0x00011c01; // NVPLL_COEFF
    R(0x680504) = 0x00007801; // MPLL_COEFF
    R(0x680508) = 0x0003c20d; // VPLL_COEFF
    AddDevice({kDeviceBase, kSize, "nv2a",
               [](u32 offset, u32 size) {
                   u32 v;
                   bool idle;
                   {
                       std::lock_guard lock(g_m);
                       v = ReadReg(offset, size);
                       idle = offset == PGRAPH_STATUS && R(PFIFO_CACHE1_DMA_GET) == R(PFIFO_CACHE1_DMA_PUT);
                   }
                   // The program sees the GPU idle (pbkit's pb_busy: GET at PUT, then this register):
                   // what the GPU path drew goes to memory now, as the console's would be there, so the
                   // CPU reading a surface next reads it (nxdk_pgraph_tests saves its pictures so: on
                   // the GPU path the last draws were missing until the next vblank's write-back)
                   if (idle && gpu::Enabled())
                       gpu::Sync();
                   return v;
               },
               [](u32 offset, u32 size, u32 value) {
                   std::unique_lock lock(g_m);
                   WriteReg(offset, size, value);
                   // XB_NV2A_SYNC: a PUT waits for the push buffer to be run up to it (g_sync_put)
                   if (g_sync_put && offset >= kUser && (offset & 0xffff) == 0x40)
                       g_drained.wait_for(lock, std::chrono::seconds(2), [] {
                           return R(PFIFO_CACHE1_DMA_GET) == R(PFIFO_CACHE1_DMA_PUT) || PullerStalled() || g_quit;
                       });
               }});
    std::thread(GpuThread).detach();
}

void Nv2aVblank()
{
#ifdef XB_XEMU_NV2A
    if (g_xemu_nv2a)
    {
        if (!g_xemu_guard.empty() && ++g_xemu_vblanks == 30 * 60) // half a minute: it draws here
        {
            std::remove(g_xemu_guard.c_str());
            g_xemu_guard.clear();
        }
        return xb_nv2a_vblank();
    }
#endif
    std::lock_guard lock(g_m);
    g_pcrtc_pending |= 1;
    UpdateIrq();
}

std::string Nv2aWorkStats(double seconds)
{
#ifdef XB_XEMU_NV2A
    if (g_xemu_nv2a)
    {
        char out[700];
        xb_nv2a_profile(out, sizeof(out), seconds);
        return out;
    }
#endif
    (void)seconds;
    return "";
}

bool PictureIsCopied()
{
#ifdef XB_XEMU_NV2A
    static const bool from_ram = std::getenv("XB_PICTURE_RAM") != nullptr; // (a test's "before")
    return g_xemu_nv2a && !from_ram;
#else
    return false;
#endif
}

u32 Nv2aDisplayStart()
{
#ifdef XB_XEMU_NV2A
    if (g_xemu_nv2a)
        return xb_nv2a_display_start();
#endif
    std::lock_guard lock(g_m);
    return R(PCRTC_START);
}

u32 Nv2aPvideo(u32 reg)
{
#ifdef XB_XEMU_NV2A
    if (g_xemu_nv2a)
        return u32(xb_nv2a_read(0x8000 + reg, 4));
#endif
    std::lock_guard lock(g_m);
    return R(0x8000 + reg);
}

u32 Nv2aObjectClass(u32 instance)
{
    return instance ? Ramin32(instance) & 0xff : 0;
}

u32 Nv2aDmaAddress(u32 instance, u32* limit)
{
    const Dma d = LoadDma(instance);
    if (limit)
        *limit = d.limit;
    return PhysToGuest(d.address);
}

} // namespace xb

// xemu's renderer's start-up, step by step: in the step file (a start that dies there is followed by
// one with xbox-core's NV2A, and its log says which step) and in the log, written through at once
extern "C" void xb_nv2a_step(const char* step)
{
    using namespace xb;
    if (!g_xemu_step_file.empty())
        if (FILE* f = std::fopen(g_xemu_step_file.c_str(), "w"))
        {
            std::fprintf(f, "xemu renderer start-up: %s\n%s\n%d\n", step, kBuildId, g_xemu_strikes);
            std::fclose(f);
        }
    LogSync("nv2a: xemu's renderer: %s", step);
}
