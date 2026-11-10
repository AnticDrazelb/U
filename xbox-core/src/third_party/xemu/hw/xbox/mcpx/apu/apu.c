/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
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

#include "apu_int.h"

MCPXAPUState *g_state; // Used via debug handlers

static void update_irq(MCPXAPUState *d)
{
    /* xbox-core: FETINTSTS is latched where the front end traps (fe_method), not ORed back in
     * from FECTL here: see SE2FE_IDLE_VOICE in vp.c. */
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        // fprintf(stderr, "mcpx irq raise ien=%08x ists=%08x\n",
        //         d->regs[NV_PAPU_IEN], d->regs[NV_PAPU_ISTS]);
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        // fprintf(stderr, "mcpx irq lower ien=%08x ists=%08x\n",
        //         d->regs[NV_PAPU_IEN], d->regs[NV_PAPU_ISTS]);
        pci_irq_deassert(PCI_DEVICE(d));
    }
}

static uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_XGSCNT:
        r = (uint64_t)d->ep_frame_div * NUM_SAMPLES_PER_FRAME;
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    trace_mcpx_apu_reg_read(addr, size, r);
    return r;
}

static void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size)
{
    MCPXAPUState *d = opaque;

    trace_mcpx_apu_reg_write(addr, size, val);

    switch (addr) {
    case NV_PAPU_ISTS:
        /* the bits of the interrupts to clear are written */
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
        /* xbox-core: a front end let run again first runs the methods that waited for it */
        if ((val & NV_PAPU_FECTL_FEMETHMODE) == NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING) {
            xb_fe_drain(d);
        }
        qatomic_set(&d->regs[addr], val);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_SECTL:
        qatomic_set(&d->regs[addr], val);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write'
         * This value is expected to be written to FEMEMADDR on completion of
         * something to do with notifies. Just do it now :/ */
        stl_le_phys(&address_space_memory, d->regs[NV_PAPU_FEMEMADDR], val);
        qatomic_set(&d->regs[addr], val);
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], val);
        }
        break;
    }
}

static const MemoryRegionOps mcpx_apu_mmio_ops = {
    .read = mcpx_apu_read,
    .write = mcpx_apu_write,
};

static void throttle_update_debug(MCPXAPUState *d, int64_t now_us)
{
    if (d->throttle.deviation.last_us > 0) {
        int64_t dev = (now_us - d->throttle.deviation.last_us) - EP_FRAME_US;
        if (d->throttle.deviation.count == 0) {
            d->throttle.deviation.min_us = d->throttle.deviation.max_us = dev;
        } else {
            d->throttle.deviation.min_us = MIN(dev, d->throttle.deviation.min_us);
            d->throttle.deviation.max_us = MAX(dev, d->throttle.deviation.max_us);
        }
        d->throttle.deviation.sum_us += dev;
        d->throttle.deviation.count++;
    }
    d->throttle.deviation.last_us = now_us;
}

static void throttle_publish_debug(MCPXAPUState *d)
{
    int pacing_sum = d->throttle.pacing.backoff + d->throttle.pacing.ok +
                     d->throttle.pacing.speedup;
    if (d->throttle.deviation.count == 0 || pacing_sum == 0) {
        return;
    }
    g_dbg.throttle.pacing.backoff = (float)d->throttle.pacing.backoff / pacing_sum;
    g_dbg.throttle.pacing.ok = (float)d->throttle.pacing.ok / pacing_sum;
    g_dbg.throttle.pacing.speedup = (float)d->throttle.pacing.speedup / pacing_sum;
    g_dbg.throttle.deviation.min_us = d->throttle.deviation.min_us;
    g_dbg.throttle.deviation.avg_us = d->throttle.deviation.sum_us / d->throttle.deviation.count;
    g_dbg.throttle.deviation.max_us = d->throttle.deviation.max_us;
    if (d->throttle.queued_bytes_count > 0) {
        g_dbg.throttle.latency.min_ms = d->throttle.queued_bytes_min / (float)MONITOR_BYTES_PER_MS;
        g_dbg.throttle.latency.avg_ms = d->throttle.queued_bytes_sum /
            (d->throttle.queued_bytes_count * (float)MONITOR_BYTES_PER_MS);
        g_dbg.throttle.latency.max_ms = d->throttle.queued_bytes_max / (float)MONITOR_BYTES_PER_MS;
        g_dbg.throttle.latency.low_ms = d->monitor.queued_bytes_low / (float)MONITOR_BYTES_PER_MS;
        g_dbg.throttle.latency.high_ms = d->monitor.queued_bytes_high / (float)MONITOR_BYTES_PER_MS;
    }
    d->throttle.pacing.backoff = d->throttle.pacing.ok = d->throttle.pacing.speedup = 0;
    d->throttle.deviation.min_us = d->throttle.deviation.max_us = d->throttle.deviation.sum_us = 0;
    d->throttle.deviation.count = 0;
    d->throttle.queued_bytes_min = d->throttle.queued_bytes_max = 0;
    d->throttle.queued_bytes_sum = 0;
    d->throttle.queued_bytes_count = 0;
}

static void throttle_record_queue(MCPXAPUState *d, int queued_bytes)
{
    if (d->throttle.queued_bytes_count == 0) {
        d->throttle.queued_bytes_min = d->throttle.queued_bytes_max = queued_bytes;
    } else {
        d->throttle.queued_bytes_min = MIN(queued_bytes, d->throttle.queued_bytes_min);
        d->throttle.queued_bytes_max = MAX(queued_bytes, d->throttle.queued_bytes_max);
    }
    d->throttle.queued_bytes_sum += queued_bytes;
    d->throttle.queued_bytes_count++;
    if (queued_bytes >= d->monitor.queued_bytes_high) {
        d->throttle.pacing.backoff++;
    } else if (queued_bytes <= d->monitor.queued_bytes_low) {
        d->throttle.pacing.speedup++;
    } else {
        d->throttle.pacing.ok++;
    }
}

static void throttle(MCPXAPUState *d)
{
    if (d->ep_frame_div % 8) {
        return;
    }

    int64_t start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    throttle_update_debug(d, start_us);
    int queued_bytes = -1;

    if (d->monitor.stream) {
        queued_bytes = SDL_GetAudioStreamQueued(d->monitor.stream);
        if (queued_bytes >= 0) {
            throttle_record_queue(d, queued_bytes);
        }
        while (!d->pause_requested && queued_bytes >= d->monitor.queued_bytes_high) {
            qemu_cond_timedwait(&d->cond, &d->lock, EP_FRAME_US / 1000);
            if (d->pause_requested) {
                break;
            }
            queued_bytes = SDL_GetAudioStreamQueued(d->monitor.stream);
        }
    }

    if (queued_bytes < 0 || queued_bytes > d->monitor.queued_bytes_low) {
        int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        if (d->next_frame_time_us == 0 ||
            now_us - d->next_frame_time_us > EP_FRAME_US) {
            d->next_frame_time_us = now_us;
        }
        while (!d->pause_requested) {
            now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
            int64_t remaining_ms = (d->next_frame_time_us - now_us) / 1000;
            if (remaining_ms > 0) {
                qemu_cond_timedwait(&d->cond, &d->lock, remaining_ms);
            } else {
                break;
            }
        }
        d->next_frame_time_us += EP_FRAME_US;

        /* Avoid drift toward watermark */
        if (queued_bytes >= 0) {
            int mid = (d->monitor.queued_bytes_low +
                       d->monitor.queued_bytes_high) / 2;
            d->next_frame_time_us += (queued_bytes > mid) - (queued_bytes < mid);
        }
    }
}

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    int64_t elapsed_us = start_us - d->frame_count_time_us;
    if (elapsed_us >= 1000000) {
        /* A rudimentary calculation to determine approximately how taxed the APU
         * thread is, by measuring how much time we spend building frames.
         * =1: thread is not sleeping and likely falling behind realtime
         * <1: thread is able to complete work on time
         */
        g_dbg.utilization = (double)d->frame_work_acc_us / (double)elapsed_us;
        g_dbg.frames_processed = (int)(d->frame_count * 1000000.0 / elapsed_us + 0.5);
        throttle_publish_debug(d);

        d->frame_count_time_us = start_us;
        d->frame_count = 0;
        d->frame_work_acc_us = 0;
    }
    d->frame_count++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = { 0 };

    mcpx_apu_vp_frame(d, mixbins);
    mcpx_apu_dsp_frame(d, mixbins);
    mcpx_apu_monitor_frame(d);

    d->ep_frame_div++;
    d->frame_work_acc_us += qemu_clock_get_us(QEMU_CLOCK_REALTIME) - start_us;

    mcpx_debug_end_frame();
}

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
    qemu_mutex_lock(&d->lock);
    while (!qatomic_read(&d->exiting)) {
        if (d->pause_requested) {
            d->is_idle = true;
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            continue;
        }

        int xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        if (xcntmode == NV_PAPU_SECTL_XCNTMODE_OFF ||
            (fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) ||
            (fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED)) {
            d->set_irq = true;
        }

        if (d->set_irq) {
            qemu_mutex_unlock(&d->lock);
            bql_lock();
            update_irq(d);
            bql_unlock();
            qemu_mutex_lock(&d->lock);
            d->set_irq = false;
        }

        xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                            NV_PAPU_SECTL_XCNTMODE);
        fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        if (xcntmode == NV_PAPU_SECTL_XCNTMODE_OFF ||
            (fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) ||
            (fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED)) {
            qemu_cond_timedwait(&d->cond, &d->lock, 5);
            continue;
        }

        throttle(d);
        se_frame(d);

        /* xbox-core: the idle voice the frame found is trapped on now, under the big lock (where
         * the guest's methods run), and its interrupt raised */
        if (d->pending_idle_voice >= 0) {
            qemu_mutex_unlock(&d->lock);
            bql_lock();
            xb_fe_apply_idle_trap(d);
            update_irq(d);
            bql_unlock();
            qemu_mutex_lock(&d->lock);
        }
    }
    qemu_mutex_unlock(&d->lock);
    return NULL;
}

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));

    mcpx_apu_vp_reset(d);

    // FIXME: Reset DSP state
    dsp_invalidate_opcache(d->gp.dsp);
    dsp_invalidate_opcache(d->ep.dsp);
    d->set_irq = false;
}

/* xbox-core: the device without QEMU's PCI device, reset and snapshot plumbing. The APU's
 * 0x80000-byte window dispatched by offset as QEMU's memory regions do (its registers, the VP's
 * method window at 0x20000, the GP's at 0x30000, the EP's at 0x50000). */
uint64_t xb_ram_size(void);
void xb_mcpx_apu_init(uint8_t *ram_ptr)
{
    MCPXAPUState *d = calloc(1, sizeof(MCPXAPUState));
    g_state = d;
    d->ram_ptr = ram_ptr;
    static MemoryRegion ram;
    ram.ram = ram_ptr;
    ram.size = xb_ram_size();
    d->ram = &ram;
    d->parent_obj.irq = 5; /* xbox-core: IRQ 5 */
    d->idle_first = d->idle_second = -1; /* xbox-core: no idle voice found or trapped on yet */
    d->pending_idle_voice = -1;
    d->last_idle_voice = -1;

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;
    qemu_mutex_init(&d->lock);
    qemu_mutex_lock(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);

    mcpx_apu_vp_init(d);
    mcpx_apu_dsp_init(d);

    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread", mcpx_apu_frame_thread,
                       d, QEMU_THREAD_JOINABLE);
    mcpx_apu_wait_for_idle(d);
    mcpx_apu_resume(d); /* the machine runs (QEMU's VM state change does this) */
    qemu_mutex_unlock(&d->lock);
}

uint64_t xb_mcpx_apu_read(hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = g_state;
    if (addr >= 0x20000 && addr < 0x30000) {
        return vp_ops.read(d, addr - 0x20000, size);
    } else if (addr >= 0x30000 && addr < 0x40000) {
        return gp_ops.read(d, addr - 0x30000, size);
    } else if (addr >= 0x50000 && addr < 0x60000) {
        return ep_ops.read(d, addr - 0x50000, size);
    }
    return mcpx_apu_mmio_ops.read(d, addr, size);
}

void xb_mcpx_apu_write(hwaddr addr, uint64_t val, unsigned int size)
{
    MCPXAPUState *d = g_state;
    if (addr >= 0x20000 && addr < 0x30000) {
        vp_ops.write(d, addr - 0x20000, val, size);
    } else if (addr >= 0x30000 && addr < 0x40000) {
        gp_ops.write(d, addr - 0x30000, val, size);
    } else if (addr >= 0x50000 && addr < 0x60000) {
        ep_ops.write(d, addr - 0x50000, val, size);
    } else {
        mcpx_apu_mmio_ops.write(d, addr, val, size);
    }
}
