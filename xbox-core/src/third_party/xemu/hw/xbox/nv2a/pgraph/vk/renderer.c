/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
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

#include <unistd.h>
#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"
#include "hw/xbox/nv2a/xb_profile.h"

#include "gloffscreen.h"

#if HAVE_EXTERNAL_MEMORY
static GloContext *g_gl_context;
#endif

static void early_context_init(void)
{
#if HAVE_EXTERNAL_MEMORY
    g_gl_context = glo_context_create();
#endif
}

static void pgraph_vk_init(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;

    xb_nv2a_step("the renderer's state");
    if (getenv("XB_VK_HANG")) { /* xbox-core: a test's renderer that never starts */
        for (;;) {
            sleep(1);
        }
    }
    pg->vk_renderer_state = (PGRAPHVkState *)g_malloc0(sizeof(PGRAPHVkState));

#if HAVE_EXTERNAL_MEMORY
    glo_set_current(g_gl_context);
#endif

    pgraph_vk_debug_init();

    xb_nv2a_step("Vulkan instance and device");
    pgraph_vk_init_instance(pg, errp);
    if (*errp) {
        return;
    }

    xb_nv2a_step("command buffers");
    pgraph_vk_init_command_buffers(pg);
    xb_nv2a_step("buffers");
    pgraph_vk_init_buffers(d);
    /* xbox-core: a buffer the driver couldn't give (its memory) is a renderer that can't start, not
     * a crash later: xbox-core's own NV2A is used instead */
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (!pg->vk_renderer_state->storage_buffers[i].allocation) {
            error_setg(errp, "the GPU couldn't give buffer %d (%zu bytes)", i,
                       (size_t)pg->vk_renderer_state->storage_buffers[i].buffer_size);
            return;
        }
    }
    /* xbox-core: each step named in the step file and the log: a start-up that dies says where */
    xb_nv2a_step("surfaces");
    pgraph_vk_init_surfaces(pg);
    xb_nv2a_step("shaders");
    pgraph_vk_init_shaders(pg);
    xb_nv2a_step("pipelines");
    pgraph_vk_init_pipelines(pg);
    xb_nv2a_step("textures");
    pgraph_vk_init_textures(pg);
    xb_nv2a_step("reports");
    pgraph_vk_init_reports(pg);
    xb_nv2a_step("compute pipelines");
    pgraph_vk_init_compute(pg);
    xb_nv2a_step("display");
    pgraph_vk_init_display(pg);

    xb_nv2a_step("the RAM's vertex buffer");
    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                   memory_region_size(d->vram));

    xb_nv2a_step("the GPU's properties (test draws)");
    pgraph_vk_determine_gpu_properties(d);
    xb_nv2a_step("started");
}

static void pgraph_vk_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_vk_finalize_display(pg);
    pgraph_vk_finalize_compute(pg);
    pgraph_vk_finalize_reports(pg);
    pgraph_vk_finalize_textures(pg);
    pgraph_vk_finalize_pipelines(pg);
    pgraph_vk_finalize_shaders(pg);
    pgraph_vk_finalize_surfaces(pg);
    pgraph_vk_finalize_buffers(d);
    pgraph_vk_finalize_command_buffers(pg);
    pgraph_vk_finalize_instance(pg);

    g_free(pg->vk_renderer_state);
    pg->vk_renderer_state = NULL;
}

static void pgraph_vk_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
    pgraph_vk_surface_flush(d);
    pgraph_vk_mark_textures_possibly_dirty(d, 0, memory_region_size(d->vram));
    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                       memory_region_size(d->vram));
    for (int i = 0; i < 4; i++) {
        pg->texture_dirty[i] = true;
    }

    /* FIXME: Flush more? */

    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_vk_sync(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    pgraph_vk_render_display(pg);

    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

uint32_t xb_nv2a_display_start(void); /* (nv2a.c) */

static void pgraph_vk_process_pending(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (qatomic_read(&r->downloads_pending) ||
        qatomic_read(&r->download_dirty_surfaces_pending) ||
        qatomic_read(&r->xb_display_download_pending) ||
        qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending) ||
        qatomic_read(&d->pgraph.xb_idle_sync_pending)
    ) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (qatomic_read(&r->downloads_pending)) {
            pgraph_vk_process_pending_downloads(d);
        }
        if (qatomic_read(&r->download_dirty_surfaces_pending)) {
            qatomic_set(&r->xb_display_download_pending, false);
            pgraph_vk_download_dirty_surfaces(d);
            /* xbox-core: the picture shown is copied now, whole (its presenter reads RAM) */
            xb_nv2a_picture_ready(__atomic_load_n(&xb_surface_writebacks, __ATOMIC_RELAXED));
        } else if (qatomic_read(&r->xb_display_download_pending)) {
            /* xbox-core: the surface the display starts in, alone (the one being drawn, the depth
             * buffer and the rest stay on the GPU: no write-back of a frame half drawn) */
            qatomic_set(&r->xb_display_download_pending, false);
            const uint32_t xb_asked = qatomic_read(&r->xb_display_download_addr);
            pgraph_vk_download_surfaces_in_range_if_dirty(&d->pgraph, xb_asked, 1);
            /* ... and the one shown now, if the game flipped since the vblank asked: the picture is
             * copied from it next (held on the GPU, it would be read as RAM had it: a stale frame) */
            const uint32_t xb_now = xb_nv2a_display_start();
            if (xb_now != xb_asked) {
                pgraph_vk_download_surfaces_in_range_if_dirty(&d->pgraph, xb_now, 1);
            }
            xb_nv2a_picture_ready(__atomic_load_n(&xb_surface_writebacks, __ATOMIC_RELAXED));
        }
        if (qatomic_read(&d->pgraph.sync_pending)) {
            pgraph_vk_sync(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_vk_flush(d);
        }
        if (qatomic_read(&d->pgraph.xb_idle_sync_pending)) {
            /* xbox-core: the work so far done (reports written), the surfaces drawn in RAM */
            pgraph_vk_finish(&d->pgraph, VK_FINISH_REASON_STALLED);
            if (!xb_lazy_surfaces()) { /* (held surfaces are written back when something reads them) */
                pgraph_vk_download_dirty_surfaces(d);
            }
            qatomic_set(&d->pgraph.xb_idle_sync_pending, false);
            qemu_event_set(&d->pgraph.xb_idle_sync_complete);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

static void pgraph_vk_flip_stall(NV2AState *d)
{
    pgraph_vk_finish(&d->pgraph, VK_FINISH_REASON_FLIP_STALL);
    pgraph_vk_debug_frame_terminator();
}

/* xbox-core: a vblank's write-back of the displayed surface (see xb_display_download_pending) */
void pgraph_vk_xb_request_display_download(NV2AState *d, uint32_t addr)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    if (!r) {
        return;
    }
    qatomic_set(&r->xb_display_download_addr, addr);
    qatomic_set(&r->xb_display_download_pending, true);
}

static void pgraph_vk_pre_savevm_trigger(NV2AState *d)
{
    qatomic_set(&d->pgraph.vk_renderer_state->download_dirty_surfaces_pending, true);
    qemu_event_reset(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_vk_pre_savevm_wait(NV2AState *d)
{
    qemu_event_wait(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_vk_pre_shutdown_trigger(NV2AState *d)
{
    // qatomic_set(&d->pgraph.vk_renderer_state->shader_cache_writeback_pending, true);
    // qemu_event_reset(&d->pgraph.vk_renderer_state->shader_cache_writeback_complete);
}

static void pgraph_vk_pre_shutdown_wait(NV2AState *d)
{
    // qemu_event_wait(&d->pgraph.vk_renderer_state->shader_cache_writeback_complete);   
}

static int pgraph_vk_get_framebuffer_surface(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    qemu_mutex_lock(&d->pfifo.lock);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_vk_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color) {
        qemu_mutex_unlock(&d->pfifo.lock);
        return 0;
    }

    assert(surface->color);

    surface->frame_time = pg->frame_time;

#if HAVE_EXTERNAL_MEMORY
    qemu_event_reset(&d->pgraph.sync_complete);
    qatomic_set(&pg->sync_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.sync_complete);
    return r->display.gl_texture_id;
#else
    qemu_mutex_unlock(&d->pfifo.lock);
    pgraph_vk_wait_for_surface_download(surface);
    return 0;
#endif
}

static PGRAPHRenderer pgraph_vk_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_VULKAN,
    .name = "Vulkan",
    .ops = {
        .init = pgraph_vk_init,
        .early_context_init = early_context_init,
        .finalize = pgraph_vk_finalize,
        .clear_report_value = pgraph_vk_clear_report_value,
        .clear_surface = pgraph_vk_clear_surface,
        .draw_begin = pgraph_vk_draw_begin,
        .draw_end = pgraph_vk_draw_end,
        .flip_stall = pgraph_vk_flip_stall,
        .flush_draw = pgraph_vk_flush_draw,
        .get_report = pgraph_vk_get_report,
        .image_blit = pgraph_vk_image_blit,
        .pre_savevm_trigger = pgraph_vk_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_vk_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_vk_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_vk_pre_shutdown_wait,
        .process_pending = pgraph_vk_process_pending,
        .process_pending_reports = pgraph_vk_process_pending_reports,
        .surface_update = pgraph_vk_surface_update,
        .set_surface_scale_factor = pgraph_vk_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_vk_get_surface_scale_factor,
        .get_framebuffer_surface = pgraph_vk_get_framebuffer_surface,
        .get_gpu_properties = pgraph_vk_get_gpu_properties,
    }
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_vk_renderer);
}

void pgraph_vk_check_memory_budget(PGRAPHState *pg)
{
#if 0 // FIXME
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPhysicalDeviceMemoryProperties const *props;
    vmaGetMemoryProperties(r->allocator, &props);

    g_autofree VmaBudget *budgets = g_malloc_n(props->memoryHeapCount, sizeof(VmaBudget));
    vmaGetHeapBudgets(r->allocator, budgets);

    const float budget_threshold = 0.8;
    bool near_budget = false;

    for (int i = 0; i < props->memoryHeapCount; i++) {
        VmaBudget *b = &budgets[i];
        float use_to_budget_ratio =
            (double)b->statistics.allocationBytes / (double)b->budget;
        NV2A_VK_DPRINTF("Heap %d: used %lu/%lu MiB (%.2f%%)", i,
                        b->statistics.allocationBytes / (1024 * 1024),
                        b->budget / (1024 * 1024), use_to_budget_ratio * 100);
        near_budget |= use_to_budget_ratio > budget_threshold;
    }

    // If any heaps are near budget, free up some resources
    if (near_budget) {
        pgraph_vk_trim_texture_cache(pg);
    }
#endif

#if 0
    char *s;
    vmaBuildStatsString(r->allocator, &s, VK_TRUE);
    puts(s);
    vmaFreeStatsString(r->allocator, s);
#endif
}
