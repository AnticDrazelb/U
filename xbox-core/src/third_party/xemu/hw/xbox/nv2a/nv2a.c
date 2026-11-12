/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/xb_profile.h"
#include "qemu/main-loop.h"

void nv2a_update_irq(NV2AState *d)
{
    /* PFIFO */
    if (d->pfifo.pending_interrupts & d->pfifo.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PFIFO;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PFIFO;
    }

    /* PCRTC */
    if (d->pcrtc.pending_interrupts & d->pcrtc.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PCRTC;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PCRTC;
    }

    /* PGRAPH */
    if (d->pgraph.pending_interrupts & d->pgraph.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PGRAPH;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PGRAPH;
    }

    /* PTIMER */
    if (d->ptimer.pending_interrupts & d->ptimer.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PTIMER;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PTIMER;
    }

    if (d->pmc.pending_interrupts && d->pmc.enabled_interrupts) {
        trace_nv2a_irq(d->pmc.pending_interrupts);
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        pci_irq_deassert(PCI_DEVICE(d));
    }
}

DMAObject nv_dma_load(NV2AState *d, hwaddr dma_obj_address)
{
    assert(dma_obj_address < memory_region_size(&d->ramin));

    uint32_t *dma_obj = (uint32_t *)(d->ramin_ptr + dma_obj_address);
    uint32_t flags = ldl_le_p(dma_obj);
    uint32_t limit = ldl_le_p(dma_obj + 1);
    uint32_t frame = ldl_le_p(dma_obj + 2);

    return (DMAObject){
        .dma_class  = GET_MASK(flags, NV_DMA_CLASS),
        .dma_target = GET_MASK(flags, NV_DMA_TARGET),
        .address    = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST),
        .limit      = limit,
    };
}

void *nv_dma_map(NV2AState *d, hwaddr dma_obj_address, hwaddr *len)
{
    DMAObject dma = nv_dma_load(d, dma_obj_address);

    /* TODO: Handle targets and classes properly */
    trace_nv2a_dma_map(dma_obj_address, dma.dma_class, dma.dma_target,
                       dma.address, dma.limit);
    dma.address &= 0x07FFFFFF;

    assert(dma.address < memory_region_size(d->vram));
    // assert(dma.address + dma.limit < memory_region_size(d->vram));
    *len = dma.limit;
    return d->vram_ptr + dma.address;
}

hwaddr nv_clip_gpu_tile_blit(NV2AState *d, hwaddr blit_base_address, hwaddr len)
{
    const uint32_t *regs = d->pfb.regs;
    hwaddr blit_end = blit_base_address + len;
    for (int i = 0; i < NV_NUM_GPU_TILES; ++i) {
        uint32_t base_and_flags = regs[NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(i)];
        if (!(base_and_flags & NV_PFB_TILE_FLAGS_VALID)) {
            continue;
        }

        uint32_t limit = regs[NV_PFB_TILE_LIMIT(i)];

        if (blit_base_address < limit && blit_end > limit) {
            // TODO: Determine HW behavior if tiles are consecutive.
            return limit + 1 - blit_base_address;
        }
    }

    return len;
}

const NV2ABlockInfo blocktable[NV_NUM_BLOCKS] = {
    #define ENTRY(NAME, LNAME, OFFSET, SIZE) [NV_##NAME] = {            \
        .name   = #NAME,                                                \
        .offset = OFFSET,                                               \
        .size   = SIZE,                                                 \
        .ops    = { .read = LNAME ## _read, .write = LNAME ## _write }, \
    }
    ENTRY(PMC,      pmc,      0x000000, 0x001000),
    ENTRY(PBUS,     pbus,     0x001000, 0x001000),
    ENTRY(PFIFO,    pfifo,    0x002000, 0x002000),
    ENTRY(PRMA,     prma,     0x007000, 0x001000),
    ENTRY(PVIDEO,   pvideo,   0x008000, 0x001000),
    ENTRY(PTIMER,   ptimer,   0x009000, 0x001000),
    ENTRY(PCOUNTER, pcounter, 0x00a000, 0x001000),
    ENTRY(PVPE,     pvpe,     0x00b000, 0x001000),
    ENTRY(PTV,      ptv,      0x00d000, 0x001000),
    ENTRY(PRMFB,    prmfb,    0x0a0000, 0x020000),
    ENTRY(PRMVIO,   prmvio,   0x0c0000, 0x001000),
    ENTRY(PFB,      pfb,      0x100000, 0x001000),
    ENTRY(PSTRAPS,  pstraps,  0x101000, 0x001000),
    ENTRY(PGRAPH,   pgraph,   0x400000, 0x002000),
    ENTRY(PCRTC,    pcrtc,    0x600000, 0x001000),
    ENTRY(PRMCIO,   prmcio,   0x601000, 0x001000),
    ENTRY(PRAMDAC,  pramdac,  0x680000, 0x001000),
    ENTRY(PRMDIO,   prmdio,   0x681000, 0x001000),
    // ENTRY(PRAMIN,   pramin,   0x700000, 0x100000),
    ENTRY(USER,     user,     0x800000, 0x800000),
};
#undef ENTRY

static int nv2a_get_bpp(VGACommonState *s)
{
    NV2AState *d = container_of(s, NV2AState, vga);

    int depth = s->cr[0x28] & 3;

    int bpp;
    switch (depth) {
    case 0:
        /* FIXME: This case is sometimes hit during early Xbox startup.
         *        Presumably a race-condition where VGA isn't initialized, yet.
         *        `bpp = 0` mimics old code that did `bpp = depth * 8;`.
         *        This works around the issue of this mode being unhandled.
         *        However, QEMU VGA uses a 4bpp mode if `bpp = 0`.
         *        We don't know if Xbox hardware would do the same. */
        bpp = 0;
        break;
    case 2:
        bpp = d->pramdac.general_control &
              NV_PRAMDAC_GENERAL_CONTROL_ALT_MODE_SEL ? 16 : 15;
        break;
    case 3:
        bpp = 32;
        break;
    default:
        /* This is only a fallback path */
        bpp = depth * 8;
        fprintf(stderr, "Unknown VGA depth: %d\n", depth);
        assert(!"Unknown VGA depth - check logs for depth value");
        break;
    }

    return bpp;
}

static void nv2a_get_params(VGACommonState *s, VGADisplayParams *params)
{
    NV2AState *d = container_of(s, NV2AState, vga);
    params->line_offset = (s->cr[0x13] | ((s->cr[0x19] & 0xe0) << 3) |
                           ((s->cr[0x25] & 0x20) << 6))
                          << 3;
    params->start_addr = d->pcrtc.start / 4;
    params->line_compare = s->cr[VGA_CRTC_LINE_COMPARE] |
                           ((s->cr[VGA_CRTC_OVERFLOW] & 0x10) << 4) |
                           ((s->cr[VGA_CRTC_MAX_SCAN] & 0x40) << 3);
}

const uint8_t *nv2a_get_dac_palette(void)
{
    return g_nv2a->puserdac.palette;
}

int nv2a_get_screen_off(void)
{
    return g_nv2a->vga.sr[VGA_SEQ_CLOCK_MODE] & VGA_SR01_SCREEN_OFF;
}

static void nv2a_lock_fifo(NV2AState *d)
{
    qemu_mutex_lock(&d->pfifo.lock);
    qemu_cond_broadcast(&d->pfifo.fifo_cond);
    bql_unlock();
    qemu_cond_wait(&d->pfifo.fifo_idle_cond, &d->pfifo.lock);
    bql_lock();
    qemu_mutex_lock(&d->pgraph.lock);
}

static void nv2a_unlock_fifo(NV2AState *d)
{
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_unlock(&d->pfifo.lock);
}

static void nv2a_reset(NV2AState *d)
{
    nv2a_lock_fifo(d);
    bool halted = qatomic_read(&d->pfifo.halt);
    if (!halted) {
        qatomic_set(&d->pfifo.halt, true);
    }
    qemu_event_reset(&d->pgraph.flush_complete);
    qatomic_set(&d->pgraph.flush_pending, true);
    nv2a_unlock_fifo(d);
    bql_unlock();
    qemu_event_wait(&d->pgraph.flush_complete);
    bql_lock();
    nv2a_lock_fifo(d);
    if (!halted) {
        qatomic_set(&d->pfifo.halt, false);
    }

    memset(d->pfifo.regs, 0, sizeof(d->pfifo.regs));
    memset(d->pgraph.regs_, 0, sizeof(d->pgraph.regs_));
    memset(d->pvideo.regs, 0, sizeof(d->pvideo.regs));

    d->pcrtc.start = 0;
    d->pramdac.core_clock_coeff = 0x00011C01; /* 189MHz...? */
    d->pramdac.core_clock_freq = 233333324;
    d->pramdac.memory_clock_coeff = 0;
    d->pramdac.video_clock_coeff = 0x0003C20D; /* 25182Khz...? */

    d->pfifo.regs[NV_PFIFO_CACHE1_STATUS] |= NV_PFIFO_CACHE1_STATUS_LOW_MARK;

    vga_common_reset(&d->vga);
    /* seems to start in color mode */
    d->vga.msr = VGA_MIS_COLOR;

    d->pgraph.waiting_for_nop = false;
    d->pgraph.waiting_for_flip = false;
    d->pgraph.waiting_for_context_switch = false;

    d->pgraph.vsh_carry_fog[0] = 0.0f;
    d->pgraph.vsh_carry_fog[1] = 0.0f;
    d->pgraph.vsh_carry_fog[2] = 0.0f;
    d->pgraph.vsh_carry_fog[3] = 1.0f;

    d->pmc.pending_interrupts = 0;
    d->pfifo.pending_interrupts = 0;
    d->ptimer.pending_interrupts = 0;
    ptimer_reset(d);
    d->pcrtc.pending_interrupts = 0;

    for (int i = 0; i < 256; i++) {
        d->puserdac.palette[i*3]   = i;
        d->puserdac.palette[i*3+1] = i;
        d->puserdac.palette[i*3+2] = i;
    }

    nv2a_unlock_fifo(d);
}

/* xbox-core: the device without QEMU's PCI device, memory regions, display console, snapshot and
 * reset plumbing. QEMU calls device code with its big lock (BQL) held; here xb_nv2a_read/write hold
 * it (shim/xb_bql.c), and the code takes and drops it as in QEMU. */
void xb_bql_lock(void);
void xb_bql_unlock(void);

static MemoryRegion g_vram;
static QemuEvent g_renderer_started;
static bool g_renderer_ok;

void xb_nv2a_renderer_started(bool ok)
{
    g_renderer_ok = ok;
    qemu_event_set(&g_renderer_started);
}

void xb_nv2a_step(const char *step); /* xbox-core: the start-up's step file (nv2a.cpp) */
void pgraph_vk_xb_request_display_download(NV2AState *d, uint32_t addr); /* (vk/renderer.c) */

/* xbox-core: the renderer thread's work, counted (xb_profile.h) */
uint64_t xb_prof_n[XB_PROF_COUNT], xb_prof_ns[XB_PROF_COUNT];

int xb_nv2a_profile(char *out, int size, double seconds)
{
    static const char *const names[XB_PROF_COUNT] = {
        "busy", "draws", "shaders made", "shaders from the cache", "pipelines made",
        "waits for the GPU", "surfaces written back", "surfaces uploaded", "textures uploaded",
    };
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < XB_PROF_COUNT && n < size - 1; i++) {
        const uint64_t count = __atomic_exchange_n(&xb_prof_n[i], 0, __ATOMIC_RELAXED);
        const uint64_t ns = __atomic_exchange_n(&xb_prof_ns[i], 0, __ATOMIC_RELAXED);
        const double share = seconds > 0 ? 100.0 * (double)ns / 1e9 / seconds : 0;
        if (i == XB_PROF_BUSY) {
            n += snprintf(out + n, size - n, "busy %.0f%% of the time", share);
        } else if (count) {
            n += snprintf(out + n, size - n, "; %s %.0f/s (%.1f%%)", names[i], (double)count / seconds, share);
        }
    }
    return n;
}

/* true when the renderer started (else this device isn't to be used: xbox-core's own NV2A is) */
bool xb_nv2a_init(uint8_t *ram, uint64_t ram_size)
{
    xb_nv2a_step("the device's state");
    qemu_event_init(&g_renderer_started, false);
    NV2AState *d = calloc(1, sizeof(NV2AState));
    PCIDevice *dev = PCI_DEVICE(d);

    dev->irq = 3;
    pci_set_word(dev->config + PCI_VENDOR_ID, 0x10de);   /* NVIDIA */
    pci_set_word(dev->config + 0x02, 0x02a0);            /* NV2A */
    pci_set_long(dev->config + PCI_CLASS_REVISION, 0x030000a1);
    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    qemu_mutex_init(&d->pfifo.lock);
    qemu_cond_init(&d->pfifo.fifo_cond);
    qemu_cond_init(&d->pfifo.fifo_idle_cond);

    /* xbox is UMA - vram *is* ram */
    g_vram.ram = ram;
    g_vram.size = ram_size;
    d->vram = &g_vram;
    d->vram_ptr = ram;

    /* RAMIN: its own 1 MiB, as xemu has it */
    d->ramin.size = 0x100000;
    d->ramin.ram = calloc(1, d->ramin.size);
    d->ramin_ptr = d->ramin.ram;

    memory_region_set_dirty(d->vram, 0, memory_region_size(d->vram));

    d->vga.get_bpp = nv2a_get_bpp;
    d->vga.get_params = nv2a_get_params;
    d->vga.vram_ptr = ram;
    d->vga.vram_size_mb = ram_size / MiB;

    xb_bql_lock();
    xb_nv2a_step("PGRAPH's state");
    pgraph_init(d);
    xb_nv2a_step("the PFIFO thread (it starts the renderer)");
    qemu_thread_create(&d->pfifo.thread, "nv2a.pfifo_thread", pfifo_thread, d, QEMU_THREAD_JOINABLE);
    xb_bql_unlock();
    /* xbox-core: a renderer that hasn't started in a minute (a driver call that never returns)
     * isn't waited for: xbox-core's own NV2A draws instead, and the log says at which step */
    const char *wait_ms = getenv("XB_NV2A_START_MS"); /* (a test's shorter wait) */
    const int wait = wait_ms ? atoi(wait_ms) : 60000;
    if (!xb_qemu_event_timedwait(&g_renderer_started, wait)) {
        xb_xemu_log("nv2a: xemu's renderer hasn't started in %d s (the step above): it is left "
                    "where it is, and isn't used", wait / 1000);
        return false;
    }
    if (!g_renderer_ok) {
        return false;
    }
    xb_bql_lock();
    ptimer_init(d);
    nv2a_reset(d);
    xb_bql_unlock();
    return true;
}

/* PGRAPH's status read with the push buffer drained (pbkit's pb_busy, Direct3D's idle checks): on the
 * console the GPU is idle only once its work is done, reports written and surfaces in memory; xemu's
 * renderer finishes that later, so here it is made to now (once per new PUT) */
static void idle_sync(NV2AState *d)
{
    static uint32_t synced_put = 0xffffffff;
    const uint32_t put = qatomic_read(&d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT]);
    const uint32_t get = qatomic_read(&d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET]);
    if (get != put || put == synced_put || !d->pgraph.renderer) {
        return;
    }
    qemu_mutex_lock(&d->pfifo.lock);
    qemu_event_reset(&d->pgraph.xb_idle_sync_complete);
    qatomic_set(&d->pgraph.xb_idle_sync_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.xb_idle_sync_complete);
    synced_put = put;
}

uint64_t xb_nv2a_read(uint64_t addr, unsigned int size)
{
    NV2AState *d = g_nv2a;
    uint64_t r = 0;
    if (addr == 0x400700) {
        idle_sync(d);
    }
    xb_bql_lock();
    if (addr >= 0x700000 && addr < 0x800000) {
        /* RAMIN */
        memcpy(&r, d->ramin_ptr + (addr - 0x700000), size);
    } else {
        for (int i = 0; i < ARRAY_SIZE(blocktable); i++) {
            const NV2ABlockInfo *b = &blocktable[i];
            if (b->name && addr >= b->offset && addr < b->offset + b->size) {
                r = b->ops.read(d, addr - b->offset, size);
                break;
            }
        }
    }
    xb_bql_unlock();
    return r;
}

void xb_nv2a_write(uint64_t addr, uint64_t val, unsigned int size)
{
    NV2AState *d = g_nv2a;
    xb_bql_lock();
    if (addr >= 0x700000 && addr < 0x800000) {
        memcpy(d->ramin_ptr + (addr - 0x700000), &val, size);
    } else {
        for (int i = 0; i < ARRAY_SIZE(blocktable); i++) {
            const NV2ABlockInfo *b = &blocktable[i];
            if (b->name && addr >= b->offset && addr < b->offset + b->size) {
                b->ops.write(d, addr - b->offset, val, size);
                break;
            }
        }
    }
    xb_bql_unlock();
}

/* the vblank (xemu's from QEMU's display refresh, nv2a_vga_gfx_update): PCRTC's interrupt, the
 * raster back to 0; and the surfaces drawn since downloaded to RAM (on the PGRAPH thread, as
 * before a snapshot), where xbox-core's presenter reads the picture */
void xb_nv2a_vblank(void)
{
    NV2AState *d = g_nv2a;
    xb_bql_lock();
    d->pcrtc.pending_interrupts |= NV_PCRTC_INTR_0_VBLANK;
    d->pcrtc.raster = 0;
    nv2a_update_irq(d);
    xb_bql_unlock();

    /* xbox-core: each vblank the surface the display starts in back in RAM (the picture); every
     * half second every surface drawn (what the CPU might read of them; a wait for the GPU to be
     * idle writes them all back too). XB_DOWNLOAD_ALL=1: every surface each vblank, as before. */
    static int all_every = -1;
    static unsigned vblanks;
    if (all_every < 0) {
        all_every = getenv("XB_DOWNLOAD_ALL") ? 1 : 30;
    }
    qemu_mutex_lock(&d->pfifo.lock);
    if (d->pgraph.renderer && d->pgraph.renderer->ops.pre_savevm_trigger) {
        if (++vblanks % (unsigned)all_every == 0) {
            d->pgraph.renderer->ops.pre_savevm_trigger(d);
        } else {
            pgraph_vk_xb_request_display_download(d, (uint32_t)d->pcrtc.start);
        }
    }
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}

uint32_t xb_nv2a_display_start(void)
{
    return (uint32_t)g_nv2a->pcrtc.start;
}
