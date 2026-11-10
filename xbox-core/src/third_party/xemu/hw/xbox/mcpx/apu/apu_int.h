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
#ifndef HW_XBOX_MCPX_APU_INT_H
#define HW_XBOX_MCPX_APU_INT_H

#include "qemu/osdep.h"
#include <math.h>
#include <SDL3/SDL.h>
#include "hw/hw.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "cpu.h"
#include "exec/target_page.h"
#include "migration/vmstate.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "system/runstate.h"
#include "ui/xemu-settings.h"

#include "trace.h"
#include "apu.h"
#include "apu_regs.h"
#include "apu_debug.h"
#include "fpconv.h"
#include "vp/vp.h"
#include "dsp/gp_ep.h"

#define GET_MASK(v, mask) (((v) & (mask)) >> ctz32(mask))

#define SET_MASK(v, mask, val)                                       \
    do {                                                             \
        (v) &= ~(mask);                                              \
        (v) |= ((val) << ctz32(mask)) & (mask);                      \
    } while (0)

#define CASE_4(v, step)                                              \
    case (v):                                                        \
    case (v)+(step):                                                 \
    case (v)+(step)*2:                                               \
    case (v)+(step)*3

// #define DEBUG_MCPX

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#define MCPX_APU_DEVICE(obj) ((MCPXAPUState *)(obj)) /* xbox-core: no QOM */

typedef struct MCPXAPUState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    bool exiting;
    bool set_irq;

    QemuThread apu_thread;
    QemuMutex lock;
    QemuCond cond;
    QemuCond idle_cond;
    bool pause_requested;
    bool is_idle;

    MemoryRegion *ram;
    uint8_t *ram_ptr;
    MemoryRegion mmio;

    MCPXAPUVPState vp;
    MCPXAPUGPState gp;
    MCPXAPUEPState ep;

    uint32_t regs[0x20000];

    /* xbox-core: the front end's method queue (the PIO FIFO): methods written while the front end
     * is trapped or halted wait there, in order, until it runs again, as on the console; and the
     * idle voice a frame found, to trap on once the frame is done (under the big lock, where the
     * guest's methods run, so a trap and a method never interleave). */
#define XB_FE_QUEUE_LEN 4096
    struct {
        uint32_t method, argument;
    } fe_queue[XB_FE_QUEUE_LEN];
    int fe_queued;
    int64_t fe_busy_since_us;
    int idle_first, idle_second; /* this frame's first two idle voices, -1 none */
    int pending_idle_voice;      /* to trap on after the frame, -1 none */
    int last_idle_voice;         /* the last one trapped on */

    int ep_frame_div;
    int frame_work_acc_us;
    int frame_count;
    int64_t frame_count_time_us;
    int64_t next_frame_time_us;

    struct {
        struct {
            int backoff, ok, speedup;
        } pacing;
        struct {
            int64_t last_us;
            int64_t min_us, max_us, sum_us;
            int count;
        } deviation;
        int queued_bytes_min, queued_bytes_max;
        int64_t queued_bytes_sum;
        int queued_bytes_count;
    } throttle;

    struct {
        McpxApuDebugMonitorPoint point;
        int16_t frame_buf[256][2]; // 1 EP frame (0x400 bytes)
        SDL_AudioStream *stream;
        int queued_bytes_low, queued_bytes_high;
    } monitor;
} MCPXAPUState;

extern MCPXAPUState *g_state; // Used via debug handlers
extern struct McpxApuDebug g_dbg, g_dbg_cache;
extern int g_dbg_voice_monitor;
extern uint64_t g_dbg_muted_voices[4];

void mcpx_debug_begin_frame(void);
/* xbox-core: the front end's method queue and idle-voice trap (vp.c) */
void xb_fe_apply_idle_trap(MCPXAPUState *d);
void xb_fe_drain(MCPXAPUState *d);
bool xb_fe_running(MCPXAPUState *d);
void mcpx_debug_end_frame(void);

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp);
void mcpx_apu_monitor_finalize(MCPXAPUState *d);
void mcpx_apu_monitor_frame(MCPXAPUState *d);

#endif
