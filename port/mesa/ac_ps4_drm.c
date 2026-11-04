/*
 * PS4 implementation of Mesa's amdgpu kernel interface (ac_linux_drm.h), so that RADV's amdgpu
 * winsys runs unchanged on top of Sony's GNM driver.
 *
 * The PS4's GPU ("Liverpool", GCN gfx7) shares the process address space: a GPU virtual address
 * is the CPU virtual address. Buffers are direct memory mapped at the address the winsys
 * reserved for them, command buffers are submitted with sceGnmSubmitCommandBuffers, and each
 * submission ends with an end-of-pipe write of its sequence number, which fences and sync
 * objects compare against.
 *
 * Copyright 2026 DolphinPS4. SPDX-License-Identifier: MIT
 */

#include "ac_linux_drm.h"
#include "ac_gpu_info.h"
#include "addrlib/src/amdgpu_asic_addr.h"
#include "ac_debug.h"
#include "util/os_time.h"
#include "util/simple_mtx.h"
#include "util/u_math.h"
#include "util/u_sync_provider.h"
#include "util/vma.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <emmintrin.h>

/* Where the winsys writes its log and its GPU hang and fault reports: /data/Crema/gpu, or the
 * folder RADV_PS4_DIR names (an app that isn't Crema: Eden for PS4 sets /data/eden-ps4/gpu). */
static const char *ps4_gpu_dir(void)
{
   const char *dir = getenv("RADV_PS4_DIR");
   return dir && dir[0] ? dir : "/data/Crema/gpu";
}

/* A file in that folder (a few calls' worth of paths kept, for each thread). */
static const char *ps4_gpu_file(const char *name)
{
   static __thread char paths[4][256];
   static __thread unsigned next;
   char *path = paths[next++ % 4];
   snprintf(path, sizeof(paths[0]), "%s/%s", ps4_gpu_dir(), name);
   return path;
}

/* libkernel (declared here rather than through the OpenOrbis headers, which clash with Mesa's). */
int sceKernelAllocateDirectMemory(off_t search_start, off_t search_end, size_t len, size_t align,
                                  int type, off_t *phys);
int sceKernelReleaseDirectMemory(off_t start, size_t len);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, off_t phys,
                             size_t align);
int sceKernelReserveVirtualRange(void **addr, size_t len, int flags, size_t align);
int sceKernelMunmap(void *addr, size_t len);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelUsleep(unsigned int usec);
const char *sceKernelGetFsSandboxRandomWord(void);
int sceKernelLoadStartModule(const char *path, size_t args, const void *argp, unsigned flags,
                             void *opt, int *res);
int sceKernelDlsym(int handle, const char *symbol, void **address);
int sceKernelDebugOutText(int channel, const char *text);
int sceKernelCreateEqueue(void **eq, const char *name);
int sceKernelWaitEqueue(void *eq, void *events, int num, int *out, unsigned *timeout_us);

struct ps4_vq_info {
   void *start, *end;
   off_t offset;
   int protection, memory_type;
   unsigned is_flexible : 1, is_direct : 1, is_stack : 1, is_pooled : 1, is_committed : 1;
   char name[32];
};
int sceKernelVirtualQuery(const void *addr, int flags, struct ps4_vq_info *info, size_t size);

#define PS4_PROT_CPU_RW 0x03
#define PS4_PROT_GPU_RW 0x30
#define PS4_PROT_GPU_READ 0x10
#define PS4_MAP_FIXED 0x10
#define PS4_WB_ONION 0
#define PS4_WC_GARLIC 3
#define PS4_PAGE 0x4000ull /* direct memory and mapping granularity */
/* Every buffer gets this much zeroed memory of its own mapped right after it: something wrote
 * past the end of a buffer into the next one (a command buffer) and the system killed the app.
 * Writes past the end now land here, harmlessly, and are reported (ps4_check_tail). */
#define PS4_TAIL 0x10000ull

/* GPU virtual address windows: ranges of the process address space reserved at startup (the
 * kernel picks free ones - fixed addresses collided with the app heap), below the GPU's 40-bit
 * VM limit. The 32-bit window is 4 GiB aligned: address32_hi is its upper half. */
#define PS4_VA_SIZE 0x0400000000ull /* 16 GiB */
#define PS4_VA32_SIZE 0x0100000000ull
#define PS4_VA_HINT 0x4000000000ull /* search from 256 GiB */
#define PS4_GPU_VA_LIMIT 0x10000000000ull

/* Liverpool GB_TILE_MODE0..31 and GB_MACROTILE_MODE0..15, rebuilt from shadPS4's per-mode
 * attributes (8 pipes P8_32x32_16x16, 16 banks), and GB_ADDR_CONFIG: 8 pipes, 256 B pipe
 * interleave, 2 shader engines, 1 KiB DRAM rows. */
static const uint32_t ps4_tile_modes[32] = {
   0x00800310, 0x00800b10, 0x00801310, 0x00801b10, 0x00802310, 0x00800308, 0x00801318,
   0x00802318, 0x00000304, 0x00000308, 0x02000310, 0x02000294, 0x02000318, 0x00400308,
   0x02400310, 0x024002b0, 0x02400294, 0x02400318, 0x0240032c, 0x0100030c, 0x0100031c,
   0x010002b4, 0x010002a4, 0x01000328, 0x010002bc, 0x01000320, 0x010002b8, 0, 0, 0, 0, 0,
};
static const uint32_t ps4_macrotile_modes[16] = {
   0xe8, 0xd4, 0xd0, 0xd0, 0x80, 0x40, 0x00, 0x00, 0xec, 0xe8, 0xd4, 0xd0, 0x80, 0x40, 0x00, 0x00,
};
#define PS4_GB_ADDR_CONFIG 0x00011003
#define PS4_MC_ARB_RAMCFG 0x2 /* 16 banks */

struct amdgpu_bo {
   uint32_t handle;
   uint64_t size;
   off_t phys;
   void *cpu;     /* mapping address (= GPU VA) once mapped */
   bool cpu_owned; /* mapped by bo_cpu_map rather than a VA op */
   uint32_t heap;
   uint64_t flags;
   off_t pad_phys; /* RADEON_FLAG_VM_PAD_1PAGE page, when it lies past the buffer (else -1) */
   uint64_t last_va; /* last GPU address mapped at offset 0 (kept after unmap, for reports) */
   bool tail_mapped;   /* the overflow tail is mapped after the buffer (see PS4_TAIL) */
   bool tail_reported;
   bool gpu_ro; /* mapped GPU read-only (command buffers) */
   bool write_combined; /* garlic memory (VRAM); else CPU-cached onion */
   bool user_mem; /* the app's own memory (VK_EXT_external_memory_host): GPU VA = its CPU address,
                   * nothing of ours to map, unmap or release (ac_drm_create_bo_from_user_mem) */
};

struct amdgpu_va {
   struct ac_drm_device *dev;
   struct util_vma_heap *heap;
   uint64_t addr;
   uint64_t size;
};

/* A sync object: binary (point 0) or timeline. Each point is "signalled" once the submission
 * with sequence number `seq` has finished (seq 0 = signalled by the CPU). */
struct ps4_point {
   uint64_t point;
   uint64_t seq;
};
struct ps4_syncobj {
   bool used;
   struct ps4_point *points;
   unsigned num_points, max_points;
};

struct ac_drm_device {
   struct util_sync_provider p; /* first: the provider callbacks recover the device from it */
   simple_mtx_t lock;

   int (*submit)(uint32_t count, void *dcb[], uint32_t *dcb_sizes, void *ccb[],
                 uint32_t *ccb_sizes);
   int (*submit_done)(void);

   struct util_vma_heap va_heap, va32_heap;
   uint64_t va_start, va32_start;

   /* GPU end-of-pipe interrupts (sceGnmAddEqEvent): fence waits sleep on this queue. */
   int (*add_eq_event)(void *eq, uint64_t id, void *udata);
   int (*submit_and_flip)(uint32_t count, void *dcb[], uint32_t *dcb_sizes, void *ccb[],
                          uint32_t *ccb_sizes, uint32_t video, uint32_t buffer, uint32_t mode,
                          int64_t arg);
   int gnm_module;
   /* ac_ps4_submit_flip: small IBs ending in Gnm's prepareFlip packet, and the submission each
    * last used (a slot is reused once that has completed). */
   uint32_t *flip_ibs;
   uint64_t flip_seq[16];
   unsigned flip_count;
   void *eop_queue;

   /* Statistics for the app's monitor (ac_ps4_get_stats). */
   uint64_t bo_bytes, bo_count;

   /* Handle table. ps4_bo() reads it without the lock (from any thread, e.g. Dolphin's shader
    * compiler threads), so a full table is copied, never reallocated: old tables stay valid. */
   struct amdgpu_bo **bos;
   unsigned num_bos, bos_cap;

   struct ps4_syncobj *syncobjs;
   unsigned num_syncobjs;

   /* Fence page (WB onion, CPU-coherent): [0] = last completed sequence number. Followed by a
    * ring of fence IBs. */
   volatile uint64_t *fence;
   uint32_t *fence_ibs;
   /* GPU busy time (ac_ps4_get_gpu_busy): per sequence slot, the GPU clock when the command
    * processor started the submission (COPY_DATA in a small IB in front of it) and when all its
    * work had finished (bottom-of-pipe timestamp in the fence IB). Busy = the union of those
    * intervals; span = first start to last end. Both in GPU clock ticks, so the ratio needs no
    * clock frequency. */
   volatile uint64_t *ts_start, *ts_end;
   uint32_t *ts_ibs;
   uint64_t ts_accounted, ts_prev_end, ts_busy, ts_span_first, ts_span_last;
   uint64_t last_seq; /* last submitted */

   /* The last submissions' command buffers, for the GPU hang report. */
   struct {
      uint64_t seq;
      unsigned num_ibs;
      uint64_t va[8];
      uint32_t bytes[8];
   } recent[16];
   bool hang_reported;

   /* Unmaps, frees and address-range frees wait here until the GPU has finished every submission
    * made before them: the amdgpu kernel driver keeps a buffer alive while submissions use it,
    * and RADV relies on that (e.g. a queue's scratch or preamble buffers are replaced while the
    * GPU may still use the old ones). Freed at once, the GPU's late writes landed in whatever got
    * the memory next - command buffers. Guarded by `lock`, in order. */
   struct ps4_deferred {
      uint64_t seq;
      char op; /* 'U' unmap, 'F' free, 'V' address range free */
      struct amdgpu_bo *bo;
      uint32_t handle;
      uint64_t addr, size;
      struct amdgpu_va *va;
   } *deferred;
   unsigned num_deferred, deferred_cap;

   /* Recent unmaps and frees (GPU hang report: was a command buffer's memory reused?). */
   struct ps4_event {
      char what;
      uint32_t handle;
      uint64_t va, size, seq;
      off_t phys;
   } events[8192];
   unsigned num_events;
};

#define FENCE_IB_DW 24 /* user fence EOP + timestamp EOP + fence EOP (6 each), padded */
#define TS_IB_DW 8
#define FENCE_IB_COUNT 512

static void ps4_scan_tails(ac_drm_device *dev);
static bool ps4_mapped(const void *p, uint64_t len);
static void ps4_scan_writes(ac_drm_device *dev, uint32_t *ib, unsigned num_dw, uint64_t seq,
                            int depth);
struct ps4_deferred;
static void ps4_defer(ac_drm_device *dev, struct ps4_deferred d);
static void ps4_run_deferred(ac_drm_device *dev);
static void ps4_run_deferred_ex(ac_drm_device *dev, uint64_t keep);
static void ps4_note_released(uint32_t handle, uint64_t va, uint64_t size, uint64_t seq);
#define PS4_QUARANTINE_BYTES (96ull << 20)
static int ps4_do_unmap(ac_drm_device *dev, struct amdgpu_bo *b, uint32_t bo_handle, uint64_t addr,
                        uint64_t size);
static void ps4_do_free(ac_drm_device *dev, struct amdgpu_bo *b);
static void ps4_do_va_free(amdgpu_va_handle va);

/* Guards the address-space heaps (va_range_free has no device argument). */
static simple_mtx_t ps4_va_lock = SIMPLE_MTX_INITIALIZER;

/* RADV's trace ids (ac_ps4_set_trace_va): where the command processor writes them, and the word. */
static volatile const uint32_t *ps4_trace_id;
static uint64_t ps4_trace_va;

/* RADV_PS4_TRACE: "ids" turns on only RADV's trace ids (cheap: a number written as the command
 * processor reaches each draw, dispatch and cache flush, which the GPU hang report follows); any
 * other value (Crema's ps4.ini radv_trace=on sets 1) also the flight recorder, which writes every
 * submission's commands to the disk. "0" or "off": neither. */
static bool
ps4_trace_option(bool *flight_recorder)
{
   const char *t = getenv("RADV_PS4_TRACE");
   const bool on = t && t[0] && strcmp(t, "0") != 0 && strcmp(t, "off") != 0;
   if (flight_recorder)
      *flight_recorder = on && strcmp(t, "ids") != 0;
   return on;
}

/* radv-ps4.log's descriptor (-2: not opened yet), fsynced before the app ends on a GPU hang. */
static int ps4_log_fd = -2;

static void
ps4_log(const char *fmt, ...)
{
   char line[256];
   va_list args;
   va_start(args, fmt);
   vsnprintf(line, sizeof(line), fmt, args);
   va_end(args);
   fputs(line, stderr);
   sceKernelDebugOutText(0, line);
   /* Also straight to a file: the app may not route stderr anywhere. Opened once: without braces
    * here every line opened (and truncated) the file again and never closed it, ~one descriptor
    * per 7 s in Tekken Tag Tournament 2 until the PS4's ~250 open file limit stopped the game's own
    * file loads (Crema 1.71-1.77). */
   int *const fd = &ps4_log_fd;
   int current = __atomic_load_n(fd, __ATOMIC_ACQUIRE);
   if (current == -2) {
      mkdir(ps4_gpu_dir(), 0777);
      int opened = open(ps4_gpu_file("radv-ps4.log"), O_WRONLY | O_CREAT | O_TRUNC, 0666);
      int expected = -2;
      if (__atomic_compare_exchange_n(fd, &expected, opened, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
         current = opened;
      } else {
         /* another thread opened it first */
         if (opened >= 0)
            close(opened);
         current = expected;
      }
   }
   if (current >= 0)
      write(current, line, strlen(line));
}

#undef fprintf
int
ac_ps4_fprintf(FILE *f, const char *fmt, ...)
{
   char line[512];
   va_list args;
   va_start(args, fmt);
   int n = vsnprintf(line, sizeof(line), fmt, args);
   va_end(args);
   if (f == stderr || f == stdout)
      ps4_log("%s", line);
   else
      fputs(line, f);
   return n;
}

/* ---------------------------------------------------------------------------------------------
 * Fences
 */

static uint64_t
ps4_completed(ac_drm_device *dev)
{
   return __atomic_load_n(dev->fence, __ATOMIC_ACQUIRE);
}

/* Waits until submission `seq` has finished; returns false on timeout. */
static void ps4_progress(const char *fmt, ...);

/* Writes a range's lines back from the CPU caches and drops them (clflush works through any
 * mapping: the caches are physically tagged). Direct memory changes hands between CPU-cached
 * buffers (onion: GTT, and the emulator's own memory) and write-combined ones (garlic: VRAM), and
 * a dirty line left from the cached owner can be written back over what is written through the
 * new write-combined mapping. Donkey Kong Country: Tropical Freeze (1.83): Mesa's new geometry
 * ring table (VRAM) took the memory of the old one (GTT), got the old ring addresses back, and the
 * GPU wrote to the freed ring - a GPU page fault that ends the app. */
static void
ps4_flush_cpu_cache(const void *addr, uint64_t size)
{
   _mm_mfence();
   for (uintptr_t p = (uintptr_t)addr & ~(uintptr_t)63; p < (uintptr_t)addr + size; p += 64)
      _mm_clflush((const void *)p);
   _mm_mfence();
}

/* GPU addresses are CPU addresses: the IB parser can follow chained IBs directly. */
static void
ps4_record(ac_drm_device *dev, char what, struct amdgpu_bo *b, uint64_t va, uint64_t size)
{
   struct ps4_event *e = &dev->events[dev->num_events++ % ARRAY_SIZE(dev->events)];
   e->what = what;
   e->handle = b ? b->handle : 0;
   e->va = va;
   e->size = size;
   e->phys = b ? b->phys : -1;
   e->seq = dev->last_seq;
   /* Also to the flight recorder (when on): a GPU page fault kills the app before anything can
    * be asked, so the buffers mapped at the faulting address are looked up there afterwards
    * (Donkey Kong Country: Tropical Freeze, 1.82: a GPU write to an unmapped page). */
   ps4_progress("%c bo %u va %#llx size %#llx heap %#x seq %llu\n", what, e->handle,
                (unsigned long long)va, (unsigned long long)size, b ? b->heap : 0,
                (unsigned long long)dev->last_seq);
}

/* Which live buffer maps `va`, and what happened recently around it. */
static void
ps4_describe_va(ac_drm_device *dev, FILE *f, uint64_t va)
{
   fprintf(f, "  address %#llx:\n", (unsigned long long)va);
   for (unsigned h = 1; h < dev->num_bos; h++) {
      struct amdgpu_bo *b = dev->bos[h];
      if (b && b->last_va && va >= b->last_va && va < b->last_va + b->size)
         fprintf(f, "    live bo %u: va %#llx size %#llx phys %#llx heap %#x flags %#llx mapped %s\n",
                 b->handle, (unsigned long long)b->last_va, (unsigned long long)b->size,
                 (unsigned long long)b->phys, b->heap, (unsigned long long)b->flags,
                 b->cpu ? "yes" : "NO");
   }
   for (unsigned i = 0; i < ARRAY_SIZE(dev->events) && i < dev->num_events; i++) {
      const struct ps4_event *e = &dev->events[i];
      if (va >= e->va && va < e->va + e->size)
         fprintf(f, "    event %c bo %u va %#llx size %#llx phys %#llx at submission %llu\n", e->what,
                 e->handle, (unsigned long long)e->va, (unsigned long long)e->size,
                 (unsigned long long)e->phys, (unsigned long long)e->seq);
   }
}

/* Live buffers sharing physical memory (with direct memory aliasing enabled for fastmem, a
 * stale or double allocation wouldn't fail any more - it would silently share memory). */
static void
ps4_check_overlaps(ac_drm_device *dev, FILE *f)
{
   unsigned found = 0;
   for (unsigned a = 1; a < dev->num_bos && found < 20; a++) {
      struct amdgpu_bo *x = dev->bos[a];
      if (!x || x->user_mem)
         continue;
      for (unsigned c = a + 1; c < dev->num_bos && found < 20; c++) {
         struct amdgpu_bo *y = dev->bos[c];
         if (y && !y->user_mem && x->phys < y->phys + (off_t)y->size && y->phys < x->phys + (off_t)x->size) {
            fprintf(f, "  PHYS OVERLAP: bo %u [%#llx+%#llx] and bo %u [%#llx+%#llx]\n", x->handle,
                    (unsigned long long)x->phys, (unsigned long long)x->size, y->handle,
                    (unsigned long long)y->phys, (unsigned long long)y->size);
            found++;
         }
      }
   }
   fprintf(f, "  %u physical overlaps among %u buffer handles\n", found, dev->num_bos);
}

static void
ps4_ib_addr(void *data, uint64_t addr, struct ac_addr_info *info)
{
   ac_drm_device *dev = data;
   memset(info, 0, sizeof(*info));
   const bool in_window = (addr >= dev->va_start && addr < dev->va_start + PS4_VA_SIZE) ||
                          (addr >= dev->va32_start && addr < dev->va32_start + PS4_VA32_SIZE);
   if (in_window) {
      info->cpu_addr = (void *)(uintptr_t)addr;
      info->valid = true;
   }
}


/* Execution check for the hang report: walks the PM4 stream (following IB chains) and, for
 * every WRITE_DATA to memory and every EVENT_WRITE_EOP, tells whether the value is in memory -
 * i.e. whether the GPU got past it. With RADV_DEBUG=hang RADV writes an increasing trace id
 * after each draw, so the last "done" write marks where the GPU stopped. */
static unsigned ps4_walk_lines;

static void
ps4_walk_ib(ac_drm_device *dev, FILE *f, const uint32_t *ib, unsigned num_dw, int depth)
{
   struct ac_addr_info info;
   for (unsigned i = 0; i < num_dw && ps4_walk_lines < 4000;) {
      const uint32_t h = ib[i];
      const unsigned type = h >> 30;
      if (type == 2 || h == 0xffff1000u) { /* (PKT3_NOP_PAD: a header-only NOP) */
         i++;
         continue;
      }
      if (type != 3) {
         fprintf(f, "%*s%p: non-type-3 header %#x, stop\n", depth * 2, "", (void *)&ib[i], h);
         ps4_describe_va(dev, f, (uintptr_t)&ib[i]);
         return;
      }
      const unsigned op = (h >> 8) & 0xff;
      const unsigned body = ((h >> 16) & 0x3fff) + 1;
      const uint32_t *b = &ib[i + 1];
      if ((op == 0x3f || op == 0x33) && body >= 3) { /* INDIRECT_BUFFER(_CONST) */
         const uint64_t va = b[0] | ((uint64_t)(b[1] & 0xffff) << 32);
         const unsigned size = b[2] & 0xfffff;
         const bool chain = b[2] & (1u << 20);
         ps4_ib_addr(dev, va, &info);
         fprintf(f, "%*s%p: INDIRECT_BUFFER %#llx, %u dwords%s%s\n", depth * 2, "", (void *)&ib[i],
                 (unsigned long long)va, size, chain ? " (chain)" : "", info.valid ? "" : " INVALID");
         ps4_walk_lines++;
         if (info.valid && depth < 4)
            ps4_walk_ib(dev, f, info.cpu_addr, size, chain ? depth : depth + 1);
         if (chain)
            return;
      } else if (op == 0x37 && body >= 4) { /* WRITE_DATA */
         const unsigned dst_sel = (b[0] >> 8) & 0xf;
         if (dst_sel == 5 || dst_sel == 2 || dst_sel == 1) {
            const uint64_t va = b[1] | ((uint64_t)b[2] << 32);
            ps4_ib_addr(dev, va, &info);
            const uint32_t now = info.valid ? *(volatile uint32_t *)info.cpu_addr : 0xdeadbeef;
            fprintf(f, "%*s%p: WRITE_DATA %#llx <- %#x, memory %#x %s\n", depth * 2, "",
                    (void *)&ib[i], (unsigned long long)va, b[3], now,
                    now == b[3] ? "done" : "NOT DONE");
            ps4_walk_lines++;
         }
      } else if (op == 0x47 && body >= 5) { /* EVENT_WRITE_EOP */
         const uint64_t va = b[1] | ((uint64_t)(b[2] & 0xffff) << 32);
         ps4_ib_addr(dev, va, &info);
         const uint32_t now = info.valid ? *(volatile uint32_t *)info.cpu_addr : 0xdeadbeef;
         fprintf(f, "%*s%p: EVENT_WRITE_EOP %#llx <- %#x, memory %#x %s\n", depth * 2, "",
                 (void *)&ib[i], (unsigned long long)va, b[3], now,
                 now == b[3] ? "done" : "NOT DONE");
         ps4_walk_lines++;
      } else if (op == 0x2d || op == 0x27 || op == 0x15 || op == 0x16) { /* draws, dispatch */
         fprintf(f, "%*s%p: %s\n", depth * 2, "", (void *)&ib[i],
                 op == 0x2d ? "DRAW_INDEX_AUTO" : op == 0x27 ? "DRAW_INDEX_2" : "DISPATCH");
         ps4_walk_lines++;
      }
      i += 1 + body;
   }
}

/* ---- hang walker: begin (also built on a PC: eden-ps4/tools/gpu-tests/hangwalk.sh) ----
 *
 * Where the GPU stopped, for the hang report. Every packet of the unfinished submissions that the
 * command processor (CP) acts on - draws, dispatches, waits, cache flushes, memory writes - in the
 * CP's order, with the shaders bound at each draw and dispatch. RADV's trace ids (RADV_PS4_TRACE)
 * are WRITE_DATAs of a number, different for every write (mesa-ps4-trace-ids.patch), to one
 * address after each draw, dispatch and cache flush; the CP does each as it gets to it, so the one
 * whose number is in memory is the last place the CP passed. The CP doesn't wait for a draw or a
 * dispatch to finish, only at waits (partial flushes, SURFACE_SYNC, WAIT_REG_MEM): it stopped at the
 * first wait after that trace id (or on a packet before the next trace id, if there is no wait in
 * between), and the work that never finished was issued before that wait. Memory is read through
 * ps4_hw_read() (NULL: not mapped). */

static const void *ps4_hw_read(uint64_t va, uint64_t bytes);
void ps4_boot_trace(const char *stage); /* the app's (port/ps4_crashlog.cpp) */

/* the shader stages, as the hang report names them; CS: compute */
enum { PS4_LS, PS4_HS, PS4_ES, PS4_GS, PS4_VS, PS4_PS, PS4_CS, PS4_STAGES };
static const char ps4_stage_name[PS4_STAGES][3] = {"LS", "HS", "ES", "GS", "VS", "PS", "CS"};

struct ps4_pkt {
   uint64_t va;            /* the packet */
   uint64_t addr;          /* memory it writes, waits on, or reads its arguments from */
   uint64_t sh[PS4_STAGES]; /* draws and dispatches: the shaders they run (0: none) */
   uint32_t v[8];          /* the packet's values (see ps4_pkt_text) */
   uint32_t stages;        /* draws: VGT_SHADER_STAGES_EN when issued */
   uint8_t op, depth, sub;
   int8_t done;            /* the memory check: 1 done (or satisfied), 0 not, -1 none */
};

struct ps4_walk {
   struct ps4_pkt *pkts;
   unsigned count, max, dropped;
   uint32_t lo[PS4_STAGES], hi[PS4_STAGES]; /* SPI_SHADER_PGM_LO/HI_*, COMPUTE_PGM_LO/HI */
   uint32_t stages_en;                      /* VGT_SHADER_STAGES_EN */
   uint32_t threads[3];                     /* COMPUTE_NUM_THREAD_X/Y/Z */
   uint32_t instances;                      /* NUM_INSTANCES */
   uint64_t base[4];                        /* SET_BASE */
   uint8_t sub;                             /* the submission being walked (index) */
   bool light;                              /* no memory checks, no indirect arguments (per submission) */
};

static uint32_t
ps4_hw_read32(uint64_t va, bool *ok)
{
   const volatile uint32_t *p = ps4_hw_read(va & ~3ull, 4);
   *ok = p != NULL;
   return p ? *p : 0;
}

static uint64_t
ps4_hw_read64(uint64_t va, bool *ok)
{
   const volatile uint32_t *p = ps4_hw_read(va & ~3ull, 8);
   *ok = p != NULL;
   return p ? p[0] | ((uint64_t)p[1] << 32) : 0;
}

static uint64_t
ps4_walk_shader(const struct ps4_walk *w, unsigned stage)
{
   return ((uint64_t)w->lo[stage] << 8) | ((uint64_t)(w->hi[stage] & 0xff) << 40);
}

static struct ps4_pkt *
ps4_walk_add(struct ps4_walk *w, uint64_t va, unsigned op, int depth)
{
   if (w->count >= w->max) {
      w->dropped++;
      return NULL;
   }
   struct ps4_pkt *p = &w->pkts[w->count++];
   memset(p, 0, sizeof(*p));
   p->va = va;
   p->op = (uint8_t)op;
   p->depth = (uint8_t)depth;
   p->sub = w->sub;
   p->done = -1;
   return p;
}

/* The shaders a draw runs: VGT_SHADER_STAGES_EN picks the hardware stages in use. */
static void
ps4_walk_draw_shaders(const struct ps4_walk *w, struct ps4_pkt *p)
{
   const uint32_t en = w->stages_en;
   p->stages = en;
   if (en & 3) /* LS_EN */
      p->sh[PS4_LS] = ps4_walk_shader(w, PS4_LS);
   if (en & 4) /* HS_EN */
      p->sh[PS4_HS] = ps4_walk_shader(w, PS4_HS);
   if ((en >> 3) & 3) /* ES_EN */
      p->sh[PS4_ES] = ps4_walk_shader(w, PS4_ES);
   if (en & (1u << 5)) /* GS_EN */
      p->sh[PS4_GS] = ps4_walk_shader(w, PS4_GS);
   p->sh[PS4_VS] = ps4_walk_shader(w, PS4_VS);
   p->sh[PS4_PS] = ps4_walk_shader(w, PS4_PS);
}

/* Indirect draws and dispatches: their arguments, read from memory (v[6] = 1 when read). */
static void
ps4_walk_args(const struct ps4_walk *w, struct ps4_pkt *p, uint64_t va, unsigned dwords)
{
   p->addr = va;
   if (w->light)
      return;
   const uint32_t *a = ps4_hw_read(va, dwords * 4ull);
   if (!a)
      return;
   for (unsigned k = 0; k < dwords && k < 5; k++)
      p->v[k] = a[k];
   p->v[6] = 1;
}

static void
ps4_walk_collect(struct ps4_walk *w, uint64_t ib_va, unsigned num_dw, int depth)
{
   for (unsigned link = 0; link < 256 && num_dw; link++) {
      const uint32_t *ib = ps4_hw_read(ib_va, num_dw * 4ull);
      if (!ib) {
         struct ps4_pkt *p = ps4_walk_add(w, ib_va, 0xff, depth); /* "not mapped" */
         if (p)
            p->v[0] = num_dw;
         return;
      }
      uint64_t next_va = 0;
      unsigned next_dw = 0;
      for (unsigned i = 0; i < num_dw;) {
         const uint32_t h = ib[i];
         if (h >> 30 == 2 || h == 0xffff1000u) { /* (and PKT3_NOP_PAD: a header-only NOP) */
            i++;
            continue;
         }
         const unsigned op = (h >> 8) & 0xff;
         const unsigned body = ((h >> 16) & 0x3fff) + 1;
         const uint64_t va = ib_va + 4ull * i;
         if (h >> 30 != 3 || i + 1 + body > num_dw) {
            struct ps4_pkt *p = ps4_walk_add(w, va, 0xfe, depth); /* "not a packet" */
            if (p)
               p->v[0] = h;
            return;
         }
         const uint32_t *b = &ib[i + 1];
         struct ps4_pkt *p = NULL;
         bool ok = false;
         switch (op) {
         case 0x3f: /* INDIRECT_BUFFER */
         case 0x33: /* INDIRECT_BUFFER_CONST */
            if (body >= 3) {
               const uint64_t target = b[0] | ((uint64_t)(b[1] & 0xffff) << 32);
               const unsigned size = b[2] & 0xfffff;
               if (b[2] & (1u << 20)) { /* chained: the rest of this IB is there */
                  next_va = target;
                  next_dw = size;
               } else if (depth < 4) {
                  ps4_walk_collect(w, target, size, depth + 1);
               }
            }
            break;
         case 0x76: /* SET_SH_REG: shader addresses, compute workgroup size */
            for (unsigned k = 1; k < body; k++) {
               const uint32_t v = b[k];
               switch (b[0] + k - 1) {
               case 0x008: w->lo[PS4_PS] = v; break;
               case 0x009: w->hi[PS4_PS] = v; break;
               case 0x048: w->lo[PS4_VS] = v; break;
               case 0x049: w->hi[PS4_VS] = v; break;
               case 0x088: w->lo[PS4_GS] = v; break;
               case 0x089: w->hi[PS4_GS] = v; break;
               case 0x0c8: w->lo[PS4_ES] = v; break;
               case 0x0c9: w->hi[PS4_ES] = v; break;
               case 0x108: w->lo[PS4_HS] = v; break;
               case 0x109: w->hi[PS4_HS] = v; break;
               case 0x148: w->lo[PS4_LS] = v; break;
               case 0x149: w->hi[PS4_LS] = v; break;
               case 0x207: w->threads[0] = v; break;
               case 0x208: w->threads[1] = v; break;
               case 0x209: w->threads[2] = v; break;
               case 0x20c: w->lo[PS4_CS] = v; break;
               case 0x20d: w->hi[PS4_CS] = v; break;
               default: break;
               }
            }
            break;
         case 0x79: /* SET_UCONFIG_REG: the GS and tessellation rings' (system-owned on the PS4):
                     * VGT_ESGS/GSVS_RING_SIZE (0x30900/4), VGT_TF_RING_SIZE, VGT_HS_OFFCHIP_PARAM,
                     * VGT_TF_MEMORY_BASE (0x30938-0x30940); not VGT_PRIMITIVE_TYPE and the others
                     * between, which every draw sets */
            if (body >= 2) {
               bool any = false, all = true;
               for (unsigned k = 1; k < body; k++) {
                  const unsigned reg = b[0] + k - 1;
                  const bool ring = reg == 0x240 || reg == 0x241 || (reg >= 0x24e && reg <= 0x250);
                  any |= ring;
                  all &= ring;
               }
               if (any && (p = ps4_walk_add(w, va, op, depth))) {
                  p->v[0] = b[0];             /* first register (dwords from 0x30000) */
                  p->v[1] = body - 1;         /* how many */
                  for (unsigned k = 1; k < body && k <= 5; k++)
                     p->v[1 + k] = b[k];      /* v[2..6]: their values */
                  p->v[7] = all;              /* only ring registers in it */
               }
            }
            break;
         case 0x69: /* SET_CONTEXT_REG */
            for (unsigned k = 1; k < body; k++)
               if (b[0] + k - 1 == 0x2d5) /* VGT_SHADER_STAGES_EN */
                  w->stages_en = b[k];
            break;
         case 0x11: /* SET_BASE */
            if (body >= 3)
               w->base[b[0] & 3] = b[1] | ((uint64_t)(b[2] & 0xffff) << 32);
            break;
         case 0x2f: /* NUM_INSTANCES */
            w->instances = b[0];
            break;
         case 0x15: /* DISPATCH_DIRECT */
         case 0x16: /* DISPATCH_INDIRECT */
            if ((p = ps4_walk_add(w, va, op, depth))) {
               p->sh[PS4_CS] = ps4_walk_shader(w, PS4_CS);
               if (op == 0x15 && body >= 3) {
                  p->v[0] = b[0];
                  p->v[1] = b[1];
                  p->v[2] = b[2];
                  p->v[6] = 1;
               } else {
                  ps4_walk_args(w, p, w->base[1] + b[0], 3);
               }
               p->v[3] = w->threads[0];
               p->v[4] = w->threads[1];
               p->v[5] = w->threads[2];
            }
            break;
         case 0x27: /* DRAW_INDEX_2 */
         case 0x2d: /* DRAW_INDEX_AUTO */
         case 0x35: /* DRAW_INDEX_OFFSET_2 */
            if ((p = ps4_walk_add(w, va, op, depth))) {
               ps4_walk_draw_shaders(w, p);
               p->v[0] = op == 0x27 && body >= 4 ? b[3] : op == 0x35 && body >= 3 ? b[2] : b[0];
               p->v[1] = w->instances;
               if (op == 0x27 && body >= 3)
                  p->addr = b[1] | ((uint64_t)b[2] << 32);
            }
            break;
         case 0x24: /* DRAW_INDIRECT */
         case 0x25: /* DRAW_INDEX_INDIRECT */
         case 0x2c: /* DRAW_INDIRECT_MULTI (on the PS4 the short form: no draw index, no count address) */
         case 0x38: /* DRAW_INDEX_INDIRECT_MULTI (the same) */
         case 0x9c: /* DRAW_INDIRECT_COUNT_MULTI: the PS4's for desktop 0x2C's layout */
         case 0x9d: /* DRAW_INDEX_INDIRECT_COUNT_MULTI: for desktop 0x38's */
            if ((p = ps4_walk_add(w, va, op, depth))) {
               ps4_walk_draw_shaders(w, p);
               const bool indexed = op == 0x25 || op == 0x38 || op == 0x9d;
               ps4_walk_args(w, p, w->base[1] + b[0], indexed ? 5 : 4);
               p->v[5] = 1; /* draws */
               if ((op == 0x2c || op == 0x38) && body == 6) { /* the PS4's short form */
                  p->v[5] = b[3];
                  p->v[7] = b[4];
               } else if ((op == 0x2c || op == 0x38 || op == 0x9c || op == 0x9d) && body >= 8) {
                  p->v[5] = b[4];
                  p->v[7] = b[7]; /* stride */
                  if ((b[3] & (1u << 30)) && !w->light) { /* COUNT_INDIRECT_ENABLE: the count from memory */
                     const uint32_t count = ps4_hw_read32(b[5] | ((uint64_t)b[6] << 32), &ok);
                     p->v[5] = ok ? MIN2(count, b[4]) : 0xffffffffu;
                  }
               }
            }
            break;
         case 0x37: /* WRITE_DATA (to memory only) */
            if (body >= 4) {
               const unsigned dst = (b[0] >> 8) & 0xf;
               if ((dst == 1 || dst == 2 || dst == 5) && (p = ps4_walk_add(w, va, op, depth))) {
                  p->addr = b[1] | ((uint64_t)b[2] << 32);
                  p->v[0] = b[3];
                  p->v[1] = body - 3;
                  const uint32_t now = w->light ? 0 : ps4_hw_read32(p->addr, &ok);
                  p->v[2] = now;
                  p->done = ok ? now == b[3] : -1;
               }
            }
            break;
         case 0x46: /* EVENT_WRITE */
            if ((p = ps4_walk_add(w, va, op, depth))) {
               p->v[0] = b[0];
               if (body >= 3) {
                  p->addr = b[1] | ((uint64_t)(b[2] & 0xffff) << 32);
                  if ((b[0] & 0x3f) == 0x15 && !w->light) { /* ZPASS_DONE: the DBs' counts, bit 63 = written */
                     const uint64_t now = ps4_hw_read64(p->addr, &ok);
                     p->done = ok ? (int8_t)(now >> 63) : -1;
                  }
               }
            }
            break;
         case 0x47: /* EVENT_WRITE_EOP */
         case 0x48: /* EVENT_WRITE_EOS */
            if (body >= 4 && (p = ps4_walk_add(w, va, op, depth))) {
               p->v[0] = b[0];
               p->addr = b[1] | ((uint64_t)(b[2] & 0xffff) << 32);
               p->v[1] = b[3];
               p->v[3] = op == 0x47 ? b[2] >> 29 : 1; /* DATA_SEL (EOS: its data, 32 bits) */
               if ((p->v[3] == 1 || p->v[3] == 2) && !w->light) {
                  const uint32_t now = ps4_hw_read32(p->addr, &ok);
                  p->v[2] = now;
                  p->done = ok ? now == b[3] : -1;
               }
            }
            break;
         case 0x3c: /* WAIT_REG_MEM */
            if (body >= 5 && (p = ps4_walk_add(w, va, op, depth))) {
               p->v[0] = b[0] & 7;          /* function */
               p->v[3] = (b[0] >> 4) & 1;   /* memory (1) or register */
               p->v[1] = b[3];              /* reference */
               p->v[2] = b[4];              /* mask */
               if (p->v[3]) {
                  p->addr = (b[1] & ~3u) | ((uint64_t)b[2] << 32);
                  const uint32_t now = w->light ? 0 : ps4_hw_read32(p->addr, &ok) & b[4];
                  p->v[4] = now;
                  const uint32_t ref = b[3];
                  const bool met = p->v[0] == 0 ? true : p->v[0] == 1 ? now < ref
                                   : p->v[0] == 2 ? now <= ref : p->v[0] == 3 ? now == ref
                                   : p->v[0] == 4 ? now != ref : p->v[0] == 5 ? now >= ref
                                   : p->v[0] == 6 ? now > ref : false;
                  p->done = ok ? met : -1;
               } else {
                  p->v[4] = b[1]; /* the register */
               }
            }
            break;
         case 0x39: /* MEM_SEMAPHORE */
            if (body >= 3 && (p = ps4_walk_add(w, va, op, depth))) {
               p->addr = b[0] | ((uint64_t)(b[1] & 0xffff) << 32);
               p->v[0] = b[2];
            }
            break;
         case 0x43: /* SURFACE_SYNC */
         case 0x58: /* ACQUIRE_MEM */
         case 0x42: /* PFP_SYNC_ME */
         case 0x1e: /* ATOMIC_MEM */
         case 0x22: /* COND_EXEC */
         case 0x23: /* PRED_EXEC */
         case 0x20: /* SET_PREDICATION */
            if ((p = ps4_walk_add(w, va, op, depth)))
               p->v[0] = b[0];
            break;
         case 0x40: /* COPY_DATA */
            if (body >= 5 && (p = ps4_walk_add(w, va, op, depth))) {
               p->v[0] = b[0];
               p->v[1] = b[1];
               p->v[2] = b[2];
               p->addr = b[3] | ((uint64_t)b[4] << 32);
            }
            break;
         case 0x50: /* DMA_DATA */
            if (body >= 6 && (p = ps4_walk_add(w, va, op, depth))) {
               p->v[0] = b[0];
               p->v[1] = b[1];
               p->v[2] = b[2];
               p->v[3] = b[5] & 0x1fffff;
               p->addr = b[3] | ((uint64_t)b[4] << 32);
            }
            break;
         default:
            break;
         }
         if (next_va)
            break;
         i += 1 + body;
      }
      if (!next_va)
         return;
      ib_va = next_va;
      num_dw = next_dw;
   }
}

static const char *
ps4_event_name(unsigned type)
{
   switch (type) {
   case 0x04: return "CACHE_FLUSH_TS";
   case 0x06: return "CACHE_FLUSH";
   case 0x07: return "CS_PARTIAL_FLUSH";
   case 0x08: return "VGT_STREAMOUT_SYNC";
   case 0x0f: return "VS_PARTIAL_FLUSH";
   case 0x10: return "PS_PARTIAL_FLUSH";
   case 0x14: return "CACHE_FLUSH_AND_INV_TS";
   case 0x15: return "ZPASS_DONE";
   case 0x16: return "CACHE_FLUSH_AND_INV";
   case 0x19: return "PIPELINESTAT_START";
   case 0x1a: return "PIPELINESTAT_STOP";
   case 0x1e: return "SAMPLE_PIPELINESTAT";
   case 0x24: return "VGT_FLUSH";
   case 0x28: return "BOTTOM_OF_PIPE_TS";
   case 0x2c: return "FLUSH_AND_INV_DB_META";
   case 0x2d: return "FLUSH_AND_INV_CB_DATA_TS";
   case 0x2e: return "FLUSH_AND_INV_CB_META";
   case 0x2f: return "CS_DONE";
   case 0x30: return "PS_DONE";
   default: return NULL;
   }
}

/* Packets the CP waits at until earlier work has finished. */
static bool
ps4_pkt_is_wait(const struct ps4_pkt *p)
{
   if (p->op == 0x46) {
      const unsigned type = p->v[0] & 0x3f;
      return type == 0x07 || type == 0x0f || type == 0x10; /* CS, VS, PS partial flushes */
   }
   if (p->op == 0x43 || p->op == 0x58) /* SURFACE_SYNC, ACQUIRE_MEM: wait when DEST_BASE / CB / DB */
      return (p->v[0] & 0x7fc0) != 0 || (p->v[0] & (1u << 25)) || (p->v[0] & (1u << 26));
   return p->op == 0x3c || p->op == 0x39; /* WAIT_REG_MEM, MEM_SEMAPHORE */
}

static bool
ps4_pkt_is_work(const struct ps4_pkt *p)
{
   switch (p->op) {
   case 0x15: case 0x16: case 0x24: case 0x25: case 0x27: case 0x2c: case 0x2d: case 0x35: case 0x38:
   case 0x9c: case 0x9d:
      return true;
   default:
      return false;
   }
}

struct ps4_line {
   char s[400];
   size_t n;
};

static void
ps4_line_add(struct ps4_line *l, const char *fmt, ...)
{
   if (l->n + 1 >= sizeof(l->s))
      return;
   va_list ap;
   va_start(ap, fmt);
   const int r = vsnprintf(l->s + l->n, sizeof(l->s) - l->n, fmt, ap);
   va_end(ap);
   if (r > 0)
      l->n = MIN2(l->n + (size_t)r, sizeof(l->s) - 1);
}

static void
ps4_line_shaders(struct ps4_line *l, const struct ps4_pkt *p)
{
   for (unsigned s = 0; s < PS4_STAGES; s++)
      if (p->sh[s])
         ps4_line_add(l, ", %s %#llx", ps4_stage_name[s], (unsigned long long)p->sh[s]);
}

static const char *
ps4_done_text(int done, const char *yes, const char *no)
{
   return done < 0 ? "" : done ? yes : no;
}

static void
ps4_pkt_text(struct ps4_line *l, const struct ps4_pkt *p, uint64_t trace_va)
{
   const uint32_t *v = p->v;
   switch (p->op) {
   case 0x15:
   case 0x16:
      ps4_line_add(l, "%s ", p->op == 0x15 ? "DISPATCH_DIRECT" : "DISPATCH_INDIRECT");
      if (p->op == 0x16)
         ps4_line_add(l, "(arguments at %#llx) ", (unsigned long long)p->addr);
      if (v[6])
         ps4_line_add(l, "%ux%ux%u groups", v[0], v[1], v[2]);
      else
         ps4_line_add(l, "(arguments not readable)");
      ps4_line_add(l, " of %ux%ux%u", v[3] & 0xffff, v[4] & 0xffff, v[5] & 0xffff);
      ps4_line_shaders(l, p);
      break;
   case 0x27:
   case 0x2d:
   case 0x35:
      ps4_line_add(l, "%s %u %s x %u instances", p->op == 0x27 ? "DRAW_INDEX_2" : p->op == 0x2d ? "DRAW_INDEX_AUTO" : "DRAW_INDEX_OFFSET_2",
                   v[0], p->op == 0x2d ? "vertices" : "indices", v[1]);
      if (p->op == 0x27)
         ps4_line_add(l, " (indices at %#llx)", (unsigned long long)p->addr);
      ps4_line_shaders(l, p);
      break;
   case 0x24:
   case 0x25:
   case 0x2c:
   case 0x38:
   case 0x9c:
   case 0x9d: {
      static const char *const names[] = {"DRAW_INDIRECT", "DRAW_INDEX_INDIRECT"};
      const bool indexed = p->op == 0x25 || p->op == 0x38 || p->op == 0x9d;
      const bool multi = p->op != 0x24 && p->op != 0x25;
      ps4_line_add(l, "%s%s (arguments at %#llx", names[indexed],
                   p->op >= 0x9c ? "_COUNT_MULTI" : multi ? "_MULTI" : "", (unsigned long long)p->addr);
      if (multi)
         ps4_line_add(l, ", %u draws, stride %u", v[5], v[7]);
      ps4_line_add(l, ")");
      if (!v[6])
         ps4_line_add(l, " arguments not readable");
      else if (indexed)
         ps4_line_add(l, " %u indices x %u instances, first index %u, vertex offset %d, first instance %u",
                      v[0], v[1], v[2], (int)v[3], v[4]);
      else
         ps4_line_add(l, " %u vertices x %u instances, first vertex %u, first instance %u", v[0], v[1], v[2], v[3]);
      ps4_line_shaders(l, p);
      break;
   }
   case 0x37:
      if (trace_va && p->addr == trace_va) /* (passed or not: see where it is from the one in memory) */
         ps4_line_add(l, "trace id %u%s", v[0], p->done == 1 ? " (in memory now)" : "");
      else
         ps4_line_add(l, "WRITE_DATA %#llx <- %#x%s, memory %#x %s", (unsigned long long)p->addr, v[0],
                      v[1] > 1 ? " ..." : "", v[2], ps4_done_text(p->done, "done", "NOT DONE"));
      break;
   case 0x46: {
      const char *name = ps4_event_name(v[0] & 0x3f);
      if (name)
         ps4_line_add(l, "EVENT_WRITE %s", name);
      else
         ps4_line_add(l, "EVENT_WRITE event %#x", v[0] & 0x3f);
      if (p->addr)
         ps4_line_add(l, " -> %#llx %s", (unsigned long long)p->addr, ps4_done_text(p->done, "(written)", "(NOT written)"));
      if (ps4_pkt_is_wait(p))
         ps4_line_add(l, " (waits)");
      break;
   }
   case 0x47:
   case 0x48: {
      const char *name = ps4_event_name(v[0] & 0x3f);
      ps4_line_add(l, "%s %s -> %#llx", p->op == 0x47 ? "EVENT_WRITE_EOP" : "EVENT_WRITE_EOS",
                   name ? name : "?", (unsigned long long)p->addr);
      if (p->done >= 0)
         ps4_line_add(l, " <- %#x, memory %#x %s", v[1], v[2], p->done ? "done" : "NOT DONE");
      break;
   }
   case 0x3c: {
      static const char *const ops[] = {"always", "<", "<=", "==", "!=", ">=", ">", "?"};
      if (v[3])
         ps4_line_add(l, "WAIT_REG_MEM memory %#llx & %#x %s %#x: now %#x, %s", (unsigned long long)p->addr, v[2],
                      ops[v[0] & 7], v[1], v[4], ps4_done_text(p->done, "satisfied", "NOT SATISFIED (waits for it)"));
      else
         ps4_line_add(l, "WAIT_REG_MEM register %#x & %#x %s %#x", v[4], v[2], ops[v[0] & 7], v[1]);
      break;
   }
   case 0x39:
      ps4_line_add(l, "MEM_SEMAPHORE %#llx, %#x", (unsigned long long)p->addr, v[0]);
      break;
   case 0x43:
   case 0x58:
      ps4_line_add(l, "%s %#x%s", p->op == 0x43 ? "SURFACE_SYNC" : "ACQUIRE_MEM", v[0], ps4_pkt_is_wait(p) ? " (waits)" : "");
      break;
   case 0x79:
      ps4_line_add(l, "SET_UCONFIG_REG %#x (%u):", 0x30000 + v[0] * 4, v[1]);
      for (unsigned k = 0; k < v[1] && k < 5; k++)
         ps4_line_add(l, " %#x", v[2 + k]);
      ps4_line_add(l, " (GS/tessellation rings)");
      break;
   case 0x42: ps4_line_add(l, "PFP_SYNC_ME"); break;
   case 0x1e: ps4_line_add(l, "ATOMIC_MEM %#x", v[0]); break;
   case 0x22: ps4_line_add(l, "COND_EXEC"); break;
   case 0x23: ps4_line_add(l, "PRED_EXEC"); break;
   case 0x20: ps4_line_add(l, "SET_PREDICATION %#x", v[0]); break;
   case 0x40:
      ps4_line_add(l, "COPY_DATA %#x: %#llx -> %#llx", v[0], (unsigned long long)(v[1] | ((uint64_t)v[2] << 32)),
                   (unsigned long long)p->addr);
      break;
   case 0x50: {
      const uint64_t src = v[1] | ((uint64_t)v[2] << 32);
      if (src == p->addr)
         ps4_line_add(l, "DMA_DATA prefetch %u bytes at %#llx", v[3], (unsigned long long)p->addr);
      else
         ps4_line_add(l, "DMA_DATA %u bytes %#llx -> %#llx", v[3], (unsigned long long)src, (unsigned long long)p->addr);
      break;
   }
   case 0xfe: ps4_line_add(l, "not a packet (%#x): the walk stops", v[0]); break;
   case 0xff: ps4_line_add(l, "an IB of %u dwords that isn't mapped", v[0]); break;
   default: ps4_line_add(l, "packet %#x", p->op); break;
   }
}

static void
ps4_pkt_print(FILE *f, const struct ps4_pkt *p, uint64_t trace_va, const char *mark)
{
   struct ps4_line l = {{0}, 0};
   ps4_pkt_text(&l, p, trace_va);
   fprintf(f, "%s%*s%#llx: %s\n", mark, p->depth * 2, "", (unsigned long long)p->va, l.s);
}

/* The code of each shader a suspect runs, to `fd` (gpu/hang-shaders.txt, which the log bundle
 * takes along): a "==== shader <stage> <address> (<bytes> bytes, packet <n>)" line, then its
 * dwords in hex, 8 a line, up to its s_endpgm and 256 bytes past it (at most 64 KiB), for
 * llvm-mc --disassemble (eden-ps4/tools/gpu-hang-shaders.py). */
static void
ps4_dump_shader(int fd, uint64_t va, unsigned stage, unsigned packet, uint64_t *done, unsigned *num_done)
{
   if (fd < 0 || !va)
      return;
   for (unsigned d = 0; d < *num_done; d++)
      if (done[d] == va)
         return;
   if (*num_done < 64)
      done[(*num_done)++] = va;
   unsigned bytes = 0;
   bool end = false;
   for (; bytes < 65536 && !end; bytes += 256) {
      const uint32_t *chunk = ps4_hw_read(va + bytes, 256);
      if (!chunk)
         break;
      for (unsigned k = 0; k < 64; k++)
         end |= chunk[k] == 0xbf810000; /* s_endpgm */
   }
   if (end && bytes < 65536 && ps4_hw_read(va + bytes, 256))
      bytes += 256;
   char line[128];
   int n = snprintf(line, sizeof(line), "==== shader %s %#llx (%u bytes, packet %u)\n",
                    ps4_stage_name[stage < PS4_STAGES ? stage : PS4_CS], (unsigned long long)va, bytes, packet);
   (void)!write(fd, line, (size_t)n);
   for (unsigned at = 0; at < bytes; at += 32) {
      const uint32_t *d = ps4_hw_read(va + at, 32);
      if (!d)
         break;
      n = snprintf(line, sizeof(line), "%08x %08x %08x %08x %08x %08x %08x %08x\n", d[0], d[1], d[2], d[3], d[4],
                   d[5], d[6], d[7]);
      (void)!write(fd, line, (size_t)n);
   }
}

/* The analysis: `w` holds the unfinished submissions' packets (seqs[i] for sub i). */
static void
ps4_walk_analyze(FILE *f, const struct ps4_walk *w, const uint64_t *seqs, uint64_t trace_va, uint32_t trace_now,
                 int shader_fd)
{
   if (w->dropped)
      fprintf(f, "(only the first %u packets kept, %u more not)\n", w->count, w->dropped);
   int match = -1;
   unsigned matches = 0;
   for (unsigned i = 0; trace_va && i < w->count; i++) {
      const struct ps4_pkt *p = &w->pkts[i];
      if (p->op == 0x37 && p->addr == trace_va && p->v[0] == trace_now) {
         if (match < 0)
            match = (int)i;
         matches++;
      }
   }
   if (!trace_va) {
      fprintf(f, "no trace ids (RADV_PS4_TRACE unset): where the GPU stopped can't be told; waits that "
                 "aren't satisfied:\n");
      for (unsigned i = 0; i < w->count; i++)
         if (w->pkts[i].op == 0x3c && w->pkts[i].done == 0)
            ps4_pkt_print(f, &w->pkts[i], 0, "  ");
      return;
   }
   if (match < 0) {
      fprintf(f, "the last trace id the command processor wrote is %u, in none of the unfinished submissions: it "
                 "hasn't reached the first trace point of any (or is in one that isn't listed)\n", trace_now);
      unsigned shown = 0;
      for (unsigned i = 0; i < w->count && shown < 48; i++, shown++)
         ps4_pkt_print(f, &w->pkts[i], trace_va, "  ");
      return;
   }
   const struct ps4_pkt *m = &w->pkts[match];
   fprintf(f, "the command processor passed trace id %u at %#llx, in submission %llu%s\n", trace_now,
           (unsigned long long)m->va, (unsigned long long)seqs[m->sub],
           matches > 1 ? " (that id is there more than once: the first is taken)" : "");
   int next_trace = -1, next_wait = -1, stuck_wait = -1;
   for (unsigned i = match + 1; i < w->count; i++) {
      const struct ps4_pkt *p = &w->pkts[i];
      if (next_trace < 0 && p->op == 0x37 && p->addr == trace_va)
         next_trace = (int)i;
      if (next_wait < 0 && ps4_pkt_is_wait(p))
         next_wait = (int)i;
      if (stuck_wait < 0 && p->op == 0x3c && p->done == 0)
         stuck_wait = (int)i;
   }
   int stop = -1; /* the packet the CP is most likely on */
   if (next_wait >= 0 && (next_trace < 0 || next_wait < next_trace)) {
      stop = next_wait;
      struct ps4_line l = {{0}, 0};
      ps4_pkt_text(&l, &w->pkts[stop], trace_va);
      fprintf(f, "it waits at %#llx: %s\n", (unsigned long long)w->pkts[stop].va, l.s);
      if (w->pkts[stop].op == 0x3c && w->pkts[stop].done == 0)
         fprintf(f, "  - a wait on memory that isn't satisfied: nothing may ever write it\n");
      else
         fprintf(f, "  - for the work issued before it (the suspects below) to finish\n");
   } else if (next_trace >= 0) {
      stop = next_trace;
      fprintf(f, "it hasn't reached the next trace id (%u, at %#llx) and there is no wait between: it is stuck on "
                 "one of the packets in between\n", w->pkts[next_trace].v[0], (unsigned long long)w->pkts[next_trace].va);
   } else {
      fprintf(f, "no wait and no trace id after it: the CP got to the end; the work still running is the suspects "
                 "below\n");
   }
   if (stuck_wait >= 0 && stuck_wait != stop) {
      struct ps4_line l = {{0}, 0};
      ps4_pkt_text(&l, &w->pkts[stuck_wait], trace_va);
      fprintf(f, "a later wait isn't satisfied either: %#llx: %s\n", (unsigned long long)w->pkts[stuck_wait].va, l.s);
   }
   /* The suspects: the draws and dispatches from the last wait before the trace id the CP passed
    * (work before it had finished: the CP got past that wait) to where it stopped. */
   const unsigned end = stop >= 0 ? (unsigned)stop : w->count;
   unsigned from = 0;
   for (int i = match; i >= 0; i--)
      if (ps4_pkt_is_wait(&w->pkts[i])) {
         from = (unsigned)i;
         break;
      }
   unsigned suspects = 0, first_suspect = end;
   for (unsigned i = end; i-- > from && suspects < 24;)
      if (ps4_pkt_is_work(&w->pkts[i])) {
         suspects++;
         first_suspect = i;
      }
   fprintf(f, "\n-- the suspects: %u draws and dispatches%s, their shaders' code in hang-shaders.txt --\n", suspects,
           suspects == 24 ? " (the last 24)" : "");
   uint64_t dumped[64];
   unsigned num_dumped = 0;
   for (unsigned i = first_suspect; i < end; i++) {
      const struct ps4_pkt *p = &w->pkts[i];
      if (!ps4_pkt_is_work(p))
         continue;
      ps4_pkt_print(f, p, trace_va, "  ");
      for (unsigned s = 0; s < PS4_STAGES; s++)
         ps4_dump_shader(shader_fd, p->sh[s], s, i, dumped, &num_dumped);
   }
   /* The packets around it, in the CP's order. */
   const unsigned lo = (unsigned)MAX2(0, MIN2((int)first_suspect, match) - 16);
   const unsigned hi = MIN2(w->count, MAX2(end, (unsigned)match + 1) + 12);
   fprintf(f, "\n-- the packets around it (> the trace id the CP passed, * where it stopped) --\n");
   unsigned sub = 0xff;
   for (unsigned i = lo; i < hi; i++) {
      const struct ps4_pkt *p = &w->pkts[i];
      if (p->sub != sub) {
         sub = p->sub;
         fprintf(f, "submission %llu:\n", (unsigned long long)seqs[sub]);
      }
      ps4_pkt_print(f, p, trace_va, (int)i == match ? "> " : (int)i == stop ? "* " : "  ");
   }
}

/* ---- hang walker: end ---- */

static const void *
ps4_hw_read(uint64_t va, uint64_t bytes)
{
   return va && ps4_mapped((const void *)(uintptr_t)va, bytes) ? (const void *)(uintptr_t)va : NULL;
}

/* "Where the GPU stopped": the unfinished submissions walked, which of them the CP has started
 * (its timestamp at the start of each, ts_start), and the analysis above. */
static void
ps4_report_stop_point(ac_drm_device *dev, FILE *f, uint64_t completed, int shader_fd)
{
   fprintf(f, "==== where the GPU stopped ====\n");
   struct ps4_walk w;
   memset(&w, 0, sizeof(w));
   w.max = 16384;
   w.pkts = malloc(w.max * sizeof(*w.pkts));
   if (!w.pkts) {
      w.max = 2048;
      w.pkts = malloc(w.max * sizeof(*w.pkts));
   }
   if (!w.pkts) {
      fprintf(f, "(no memory for the walk)\n\n");
      return;
   }
   uint64_t seqs[ARRAY_SIZE(dev->recent)];
   unsigned num = 0;
   const uint64_t last = dev->last_seq;
   for (uint64_t s = completed + 1; s <= last && num < ARRAY_SIZE(seqs); s++) {
      const unsigned r = s % ARRAY_SIZE(dev->recent);
      const bool reached = dev->ts_start && dev->ts_start[s % FENCE_IB_COUNT] != 0;
      if (dev->recent[r].seq != s) {
         fprintf(f, "submission %llu: a flip (or not recorded)\n", (unsigned long long)s);
         continue;
      }
      fprintf(f, "submission %llu: %u IBs; the command processor %s it\n", (unsigned long long)s,
              dev->recent[r].num_ibs, reached ? "started" : "hasn't started");
      w.sub = (uint8_t)num;
      seqs[num++] = s;
      for (unsigned i = 0; i < dev->recent[r].num_ibs; i++)
         ps4_walk_collect(&w, dev->recent[r].va[i], dev->recent[r].bytes[i] / 4, 0);
   }
   /* (through the mapping check: a game's RADV device, and its trace buffer, may be gone) */
   const volatile uint32_t *trace = ps4_trace_va ? ps4_hw_read(ps4_trace_va, 4) : NULL;
   if (ps4_trace_va && !trace)
      fprintf(f, "(the trace id's memory at %#llx isn't mapped)\n", (unsigned long long)ps4_trace_va);
   ps4_walk_analyze(f, &w, seqs, trace ? ps4_trace_va : 0, trace ? *trace : 0, shader_fd);
   free(w.pkts);
   fprintf(f, "\n");
}

/* RADV_PS4_HANG_EXIT=1 (Eden for PS4 sets it): on a hang, end the app unless the GPU finishes
 * something within 2 s of the report's start. Waiting on hung GPU work, Eden stayed up ~40 s until
 * the PS4's own system software crashed (CE-36329-3, Eden 04.38: Zelda: Echoes of Wisdom after the
 * cliff scene), and Captain Toad's level 3 took the PS4 down within 10 s; ended while the GPU is
 * stuck, the system may reset the GPU and close just the app. A thread of its own decides, so the
 * long part of the report (the decode) doesn't hold it up: "where the GPU stopped" is on the disk
 * before it starts, the decode may be cut short. */
extern void ps4_boot_trace_sync(void) __attribute__((weak)); /* port/ps4_crashlog.cpp, 04.39 on */

struct ps4_hang_watch {
   ac_drm_device *dev;
   uint64_t completed;
   int64_t since;
   volatile int verdict; /* 0 deciding, 1 the GPU went on */
};

static bool
ps4_hang_exit_wanted(void)
{
   const char *e = getenv("RADV_PS4_HANG_EXIT");
   return e && e[0] && strcmp(e, "0") != 0 && strcmp(e, "off") != 0;
}

static void *
ps4_hang_watch_thread(void *arg)
{
   struct ps4_hang_watch *w = arg;
   while (ps4_completed(w->dev) == w->completed && os_time_get_nano() - w->since < 2000000000ll)
      sceKernelUsleep(50000);
   if (ps4_completed(w->dev) != w->completed) {
      ps4_log("radv/ps4: the GPU went on (completed %llu, was %llu): slow, not stuck - the app goes on\n",
              (unsigned long long)ps4_completed(w->dev), (unsigned long long)w->completed);
      w->verdict = 1;
      return NULL;
   }
   char line[240];
   snprintf(line, sizeof(line),
            "radv/ps4: GPU hang: submission %llu unfinished for %.0f s (gpu/gpu-hang.txt) - ending the app before "
            "the PS4's system software crashes",
            (unsigned long long)(w->completed + 1), (os_time_get_nano() - w->since) / 1e9 + 2.0);
   ps4_log("%s\n", line);
   if (ps4_log_fd >= 0)
      fsync(ps4_log_fd);
   ps4_boot_trace(line);
   if (ps4_boot_trace_sync)
      ps4_boot_trace_sync();
   abort();
   return NULL;
}

/* Writes where the GPU stopped, and the command buffers of the first submission the GPU hasn't
 * finished decoded by Mesa's IB parser, to gpu/gpu-hang.txt (once; again only after the GPU went
 * on); with RADV_PS4_HANG_EXIT, the app may end meanwhile (ps4_hang_watch_thread). */
static void
ps4_report_hang(ac_drm_device *dev, uint64_t waited_seq)
{
   if (__atomic_exchange_n(&dev->hang_reported, true, __ATOMIC_ACQ_REL))
      return;
   static struct ps4_hang_watch watch;
   watch.dev = dev;
   watch.since = os_time_get_nano();
   watch.completed = ps4_completed(dev);
   watch.verdict = 0;
   const int64_t since = watch.since;
   const uint64_t completed = watch.completed;
   const bool exit_wanted = ps4_hang_exit_wanted();
   ps4_log("radv/ps4: GPU hang? waiting for %llu, completed %llu, submitted %llu - writing "
           "%s\n",
           (unsigned long long)waited_seq, (unsigned long long)completed,
           (unsigned long long)dev->last_seq, ps4_gpu_file("gpu-hang.txt"));
   FILE *f = fopen(ps4_gpu_file("gpu-hang.txt"), "w");
   if (f) {
      fprintf(f, "waiting for %llu, GPU completed %llu, submitted %llu\n\n",
              (unsigned long long)waited_seq, (unsigned long long)completed,
              (unsigned long long)dev->last_seq);
      const int shader_fd = open(ps4_gpu_file("hang-shaders.txt"), O_WRONLY | O_CREAT | O_TRUNC, 0666);
      ps4_report_stop_point(dev, f, completed, shader_fd);
      if (shader_fd >= 0) {
         fsync(shader_fd);
         close(shader_fd);
      }
      fflush(f);
      fsync(fileno(f)); /* the summary on the disk before the long part */
   }
   {
      char line[200];
      snprintf(line, sizeof(line), "radv/ps4: GPU hang? submission %llu not finished in 2 s: where the GPU stopped "
               "in gpu/gpu-hang.txt (%.1f s to write)", (unsigned long long)(completed + 1),
               (os_time_get_nano() - since) / 1e9);
      ps4_boot_trace(line);
      if (ps4_boot_trace_sync)
         ps4_boot_trace_sync();
   }
   bool watching = false;
   if (exit_wanted) {
      pthread_t t;
      watching = pthread_create(&t, NULL, ps4_hang_watch_thread, &watch) == 0;
      if (watching)
         pthread_detach(t);
   }
   for (unsigned r = 0; f && r < ARRAY_SIZE(dev->recent); r++) {
      if (dev->recent[r].seq != completed + 1)
         continue;
      fprintf(f, "==== buffers ====\n");
      for (unsigned i = 0; i < dev->recent[r].num_ibs; i++)
         ps4_describe_va(dev, f, dev->recent[r].va[i]);
      ps4_check_overlaps(dev, f);
      simple_mtx_lock(&dev->lock);
      ps4_scan_tails(dev); /* logs to radv-ps4.log */
      simple_mtx_unlock(&dev->lock);
      fprintf(f, "==== execution check (memory writes the GPU did / didn't do) ====\n");
      for (unsigned i = 0; i < dev->recent[r].num_ibs; i++) {
         fprintf(f, "-- IB %u\n", i);
         ps4_walk_ib(dev, f, (const uint32_t *)(uintptr_t)dev->recent[r].va[i],
                     dev->recent[r].bytes[i] / 4, 0);
      }
      fprintf(f, "\n==== decoded command buffers ====\n");
      for (unsigned i = 0; i < dev->recent[r].num_ibs; i++) {
         char name[64];
         snprintf(name, sizeof(name), "submission %llu IB %u (%#llx, %u dwords)",
                  (unsigned long long)dev->recent[r].seq, i,
                  (unsigned long long)dev->recent[r].va[i], dev->recent[r].bytes[i] / 4);
         struct ac_ib_parser ib = {
            .f = f,
            .ib = (uint32_t *)(uintptr_t)dev->recent[r].va[i],
            .num_dw = dev->recent[r].bytes[i] / 4,
            .gfx_level = GFX7,
            .family = CHIP_BONAIRE,
            .ip_type = AMD_IP_GFX,
            .addr_callback = ps4_ib_addr,
            .addr_callback_data = dev,
         };
         ac_parse_ib(&ib, name);
      }
   }
   if (f) {
      fflush(f);
      fsync(fileno(f));
      fclose(f);
   }
   ps4_log("radv/ps4: hang report written (%.1f s)\n", (os_time_get_nano() - since) / 1e9);
   if (exit_wanted && !watching)
      ps4_hang_watch_thread(&watch); /* no thread: decide here, after the decode */
   /* wait for the verdict (an abort doesn't come back); the GPU went on: a later hang is reported too */
   while (exit_wanted && !watch.verdict)
      sceKernelUsleep(50000);
   static unsigned again;
   if (watch.verdict && again++ < 3)
      __atomic_store_n(&dev->hang_reported, false, __ATOMIC_RELEASE);
}

/* (Eden 04.40) A stall note, sooner than the hang report and whether or not anything waits:
 * Captain Toad's level 3 took the PS4 down within seconds with no GPU wait 2 s long, so 04.39's
 * report never started, and radv-ps4.log (not synced) lost its last minute. A thread looks at the
 * GPU's progress every 50 ms; when it has finished nothing for RADV_PS4_STALL_MS (300) with work
 * submitted, "where the GPU stopped" goes to gpu/gpu-stall.txt (its shaders: stall-shaders.txt) and
 * a line to the boot trace, both on the disk at once. Nothing else is done (a long submission is
 * no hang); at most 8 notes a session, one a stall. ac_ps4_gpu_progress: the numbers, for the app's
 * black box. */
static int64_t ps4_progress_ns; /* when the GPU last finished something, or was last idle */
static ac_drm_device *ps4_last_device; /* (defined with the flight recorder, below) */

/* (04.42) Geometry shaders and tessellation. On the PS4 the GS and tessellation rings are the
 * system's (Sony's Gnm sets their sizes with a system call, and hands apps one system tessellation
 * factor ring); RADV writes their registers itself (VGT_ESGS/GSVS_RING_SIZE, VGT_TF_RING_SIZE,
 * VGT_HS_OFFCHIP_PARAM, VGT_TF_MEMORY_BASE), and Crema saw the GS write past RADV's ring (the VGT
 * wraps it at a larger size). Captain Toad's level 3 takes the whole PS4 down within 0.1 s of its
 * first frame, the screen corrupted: whether that frame has such draws, and whether the PS4 survives
 * without them. Every submission's draws with a GS or tessellation stage enabled
 * (VGT_SHADER_STAGES_EN) and its ring register writes are counted, the first ones described in the
 * boot trace; with RADV_PS4_SKIP_GS_TESS=1 (Eden 04.42's default) they become NOPs before the GPU
 * gets them (the draws don't appear: a test, not a fix). */
static unsigned ps4_gs_tess_last_draws, ps4_gs_tess_last_rings; /* the last submission's (for its note) */

static void
ps4_filter_gs_tess(void *const *dcb, const uint32_t *sizes, unsigned num_ibs, uint64_t seq)
{
   static int skip = -1;
   if (skip < 0) {
      const char *e = getenv("RADV_PS4_SKIP_GS_TESS");
      skip = e && e[0] && strcmp(e, "0") != 0 && strcmp(e, "off") != 0;
   }
   static struct ps4_pkt pkts[8192];
   static uint64_t total_draws, total_rings, submissions;
   static unsigned described;
   struct ps4_walk w;
   memset(&w, 0, sizeof(w));
   w.pkts = pkts;
   w.max = ARRAY_SIZE(pkts);
   w.light = true;
   for (unsigned i = 0; i < num_ibs; i++)
      ps4_walk_collect(&w, (uintptr_t)dcb[i], sizes[i] / 4, 0);
   unsigned draws = 0, rings = 0;
   for (unsigned k = 0; k < w.count; k++) {
      struct ps4_pkt *p = &w.pkts[k];
      const bool draw = ps4_pkt_is_work(p) && p->op != 0x15 && p->op != 0x16 && (p->stages & 0x3f);
      const bool ring = p->op == 0x79;
      if (!draw && !ring)
         continue;
      draws += draw;
      rings += ring;
      const bool nop = skip && (draw || p->v[7]);
      if (described < 12) {
         described++;
         struct ps4_line l = {{0}, 0};
         ps4_pkt_text(&l, p, ps4_trace_va);
         char line[460];
         snprintf(line, sizeof(line), "radv/ps4: submission %llu: %s%s%s", (unsigned long long)seq, l.s,
                  draw ? "" : "", nop ? " - SKIPPED (a NOP)" : "");
         if (draw) {
            const size_t n = strlen(line);
            snprintf(line + n, sizeof(line) - n, " [VGT_SHADER_STAGES_EN %#x:%s%s%s%s]", p->stages,
                     (p->stages & 3) ? " LS" : "", (p->stages & 4) ? " HS" : "", (p->stages & 0x18) ? " ES" : "",
                     (p->stages & 0x20) ? " GS" : "");
         }
         ps4_boot_trace(line);
      }
      if (nop) {
         uint32_t *h = (uint32_t *)(uintptr_t)p->va;
         const unsigned body = ((*h >> 16) & 0x3fff) + 1;
         *h = 0xC0001000u | ((body - 1) << 16); /* PKT3(NOP) of the same length */
         ps4_flush_cpu_cache(h, 4);
      }
   }
   ps4_gs_tess_last_draws = draws;
   ps4_gs_tess_last_rings = rings;
   if (!draws && !rings)
      return;
   total_draws += draws;
   total_rings += rings;
   if (submissions++ % 600 == 0) {
      char line[200];
      snprintf(line, sizeof(line), "radv/ps4: geometry shader / tessellation so far: %llu draws, %llu ring register "
               "writes in %llu submissions%s%s", (unsigned long long)total_draws, (unsigned long long)total_rings,
               (unsigned long long)submissions, skip ? " (skipped: RADV_PS4_SKIP_GS_TESS)" : "",
               w.dropped ? "; a walk cut short" : "");
      ps4_boot_trace(line);
   }
}

/* ---- descriptor check: begin (also built on a PC: eden-ps4/tools/gpu-tests/desccheck.sh) ---- */
/* (04.44) The check of the first submissions after a loading screen, where Captain Toad's level 3
 * takes the whole PS4 down within 0.05 s (04.41-04.43: its first frame, 183 draws; no geometry shader
 * or tessellation). Mesa issue 3168 (2020): the same game hung a GCN GPU for good on a new stage in
 * yuzu, because its shaders bound more storage buffers than radeonsi had slots and the rest read
 * garbage descriptors. Here, for every draw and dispatch: its user data (32-bit pointers into the
 * 32-bit window, 64-bit ones into the GPU windows), the descriptors they point to (buffer V#s: base
 * and num_records x stride; image T#s: base), each checked against the live GPU mappings. A
 * descriptor or pointer outside every one ("wild"), or a buffer range running more than 64 KiB past
 * its mapping's end, is described in the boot trace and its draw or dispatch turned into a NOP
 * (RADV_PS4_SKIP_WILD=0: described only). A census of what touches the GPU's global state goes with
 * it: register writes and waits, config registers, the user-config registers written, GDS,
 * streamout, packets the walker doesn't know. dev->lock held. */
struct ps4_live_range {
   uint64_t start, end; /* [start, end): the mapping, its tail included when mapped */
   uint64_t size;       /* the buffer's own size */
   uint32_t handle;
};

static int
ps4_live_range_cmp(const void *a, const void *b)
{
   const struct ps4_live_range *x = a, *y = b;
   return x->start < y->start ? -1 : x->start > y->start;
}

struct ps4_check {
   ac_drm_device *dev;
   struct ps4_live_range *ranges;
   unsigned num_ranges;
   unsigned wild, overruns, skipped, described;
   unsigned config_regs, reg_writes, reg_waits, reg_copies, gds, streamout, loads, unknown;
   uint16_t uconfig[24];
   unsigned num_uconfig;
   uint8_t unknown_ops[12];
   unsigned num_unknown_ops;
};

static const struct ps4_live_range *
ps4_live_at(const struct ps4_check *c, uint64_t va)
{
   unsigned lo = 0, hi = c->num_ranges;
   while (lo < hi) {
      const unsigned mid = (lo + hi) / 2;
      if (c->ranges[mid].start <= va)
         lo = mid + 1;
      else
         hi = mid;
   }
   if (!lo)
      return NULL;
   const struct ps4_live_range *r = &c->ranges[lo - 1];
   return va < r->end ? r : NULL;
}

static bool
ps4_in_gpu_windows(const ac_drm_device *dev, uint64_t va)
{
   return (va >= dev->va_start && va < dev->va_start + PS4_VA_SIZE) ||
          (dev->va32_start && va >= dev->va32_start && va < dev->va32_start + PS4_VA32_SIZE);
}

/* One problem found: described (the first 12 of a session), counted. Returns true: skip the work. */
static bool
ps4_check_report(struct ps4_check *c, uint64_t seq, const void *packet, const char *work, const char *what,
                 uint64_t va, uint64_t bytes, const struct ps4_live_range *r)
{
   static unsigned session;
   if (session < 12) {
      session++;
      char line[300];
      if (r)
         snprintf(line, sizeof(line), "radv/ps4: submission %llu: %p: %s: %s %#llx + %#llx runs %#llx past bo %u (%#llx "
                  "bytes)", (unsigned long long)seq, packet, work, what, (unsigned long long)va,
                  (unsigned long long)bytes, (unsigned long long)(va + bytes - r->start - r->size), r->handle,
                  (unsigned long long)r->size);
      else
         snprintf(line, sizeof(line), "radv/ps4: submission %llu: %p: %s: %s %#llx%s is in no GPU buffer (WILD)",
                  (unsigned long long)seq, packet, work, what, (unsigned long long)va,
                  ps4_in_gpu_windows(c->dev, va) ? "" : ", outside the GPU windows too");
      ps4_boot_trace(line);
   }
   return true;
}

/* A descriptor set (or table) at `ptr`: its V#s and T#s, each one's base checked. A quad is taken
 * for a descriptor only when it looks like one (channel selects set; a V# with records and a data
 * format, a T# of an image type) and its base is inside the GPU windows; and a bad one only counts in
 * a set where others point into live buffers (one: described; two: the work is skipped), so that data
 * a stray user-data value points to isn't taken for a set. Returns true: skip the work. */
static bool
ps4_check_set(struct ps4_check *c, uint64_t seq, const void *packet, const char *work, uint64_t ptr)
{
   const struct ps4_live_range *set = ps4_live_at(c, ptr);
   if (!set)
      return false; /* not a pointer to our memory (a value, not an address) */
   struct amdgpu_bo *b = c->dev->bos[set->handle];
   if (!b || !b->cpu || ptr % 4)
      return false;
   /* (04.47) The range's end (past b->size when the tail is mapped: b->size alone wrapped), and the
    * memory checked before it is read: 04.46 read 0x44fdfac00c this way in Captain Toad, unmapped
    * though a live range held it, and crashed (SIGSEGV in here). */
   const uint64_t avail = set->end - ptr;
   const unsigned n = (unsigned)MIN2(avail / 4, 256);
   const uint32_t *d = (const uint32_t *)(uintptr_t)ptr;
   if (n < 4 || !ps4_mapped(d, n * 4ull))
      return false;
   unsigned valid = 0, bad_wild = 0, bad_over = 0;
   uint64_t first_va = 0, first_bytes = 0;
   const struct ps4_live_range *first_r = NULL;
   const char *first_what = NULL;
   for (unsigned j = 0; j + 4 <= n; j += 4) {
      const uint32_t d1 = d[j + 1], d2 = d[j + 2], d3 = d[j + 3];
      if ((d3 & 0xfff) == 0)
         continue; /* no channel selects: not a buffer or image descriptor (a sampler, data) */
      if ((d3 >> 30) == 0 && d2 && ((d3 >> 15) & 0xf)) { /* V#: a buffer (with a data format) */
         const uint64_t base = d[j] | ((uint64_t)(d1 & 0xffff) << 32);
         const unsigned stride = (d1 >> 16) & 0x3fff;
         const uint64_t bytes = stride ? (uint64_t)d2 * stride : d2;
         if (!base || !ps4_in_gpu_windows(c->dev, base))
            continue;
         const struct ps4_live_range *r = ps4_live_at(c, base);
         if (!r) {
            if (!bad_wild++ && !bad_over)
               first_va = base, first_bytes = bytes, first_r = NULL, first_what = "buffer descriptor at";
         } else if (base + bytes > r->end + (64u << 10) && bytes < (1ull << 32)) {
            if (!bad_over++ && !bad_wild)
               first_va = base, first_bytes = bytes, first_r = r, first_what = "buffer descriptor";
         } else {
            valid++;
         }
      } else if ((d3 >> 28) >= 9 && (d3 >> 28) <= 15 && j + 8 <= n) { /* T#: an image (2D and up) */
         const uint64_t base = (d[j] | ((uint64_t)(d1 & 0xff) << 32)) << 8;
         if (base && ps4_in_gpu_windows(c->dev, base)) {
            if (ps4_live_at(c, base))
               valid++;
            else if (!bad_wild++ && !bad_over)
               first_va = base, first_bytes = 0, first_r = NULL, first_what = "image descriptor at";
         }
         j += 4; /* (T#s are 8 dwords) */
      }
   }
   if (!valid || !(bad_wild + bad_over))
      return false;
   c->wild += bad_wild;
   c->overruns += bad_over;
   ps4_check_report(c, seq, packet, work, first_what, first_va, first_bytes, first_r);
   return valid >= 2;
}

/* A draw's or dispatch's user data: 32-bit pointers into the 32-bit window (descriptor sets, vertex
 * buffer tables, push constants; RADV's on gfx7) and 64-bit ones into the 16 GiB window. Values that
 * point into no live buffer are just values (push constants). */
static bool
ps4_check_user_data(struct ps4_check *c, uint64_t seq, const void *packet, const char *work, const uint32_t *ud)
{
   bool skip = false;
   for (unsigned u = 0; u < 16 && !skip; u++) {
      if (!ud[u])
         continue;
      if (u + 1 < 16 && (ud[u + 1] >> 8) == (c->dev->va_start >> 40) && !(ud[u] & 3)) {
         const uint64_t ptr = ud[u] | ((uint64_t)ud[u + 1] << 32);
         if (ptr >= c->dev->va_start && ptr < c->dev->va_start + PS4_VA_SIZE)
            skip = ps4_check_set(c, seq, packet, work, ptr);
      }
      if (!skip && c->dev->va32_start && !(ud[u] & 3))
         skip = ps4_check_set(c, seq, packet, work, c->dev->va32_start + ud[u]);
   }
   return skip;
}

static void
ps4_check_note_uconfig(struct ps4_check *c, unsigned reg)
{
   for (unsigned i = 0; i < c->num_uconfig; i++)
      if (c->uconfig[i] == reg)
         return;
   if (c->num_uconfig < ARRAY_SIZE(c->uconfig))
      c->uconfig[c->num_uconfig++] = (uint16_t)reg;
}

static void
ps4_check_ib(struct ps4_check *c, uint32_t *ib, unsigned num_dw, uint64_t seq, int depth, bool skip)
{
   static const unsigned stage_reg[6] = {0x0c, 0x4c, 0x8c, 0xcc, 0x10c, 0x14c};
   static const char *const stage_name[6] = {"draw (PS)", "draw (VS)", "draw (GS)", "draw (ES)", "draw (HS)", "draw (LS)"};
   uint32_t compute_ud[16] = {0};
   uint32_t gfx_ud[6][16] = {{0}};
   for (unsigned link = 0; link < 256 && num_dw; link++) {
      if (!ps4_mapped(ib, num_dw * 4ull))
         return;
      uint32_t *next = NULL;
      unsigned next_dw = 0;
      for (unsigned i = 0; i < num_dw;) {
         const uint32_t h = ib[i];
         if (h >> 30 == 2 || h == 0xffff1000u) { /* (and PKT3_NOP_PAD: a header-only NOP) */
            i++;
            continue;
         }
         if (h >> 30 != 3)
            return;
         const unsigned op = (h >> 8) & 0xff;
         const unsigned body = ((h >> 16) & 0x3fff) + 1;
         if (i + 1 + body > num_dw)
            return;
         uint32_t *b = &ib[i + 1];
         bool bad = false;
         switch (op) {
         case 0x76: /* SET_SH_REG */
            for (unsigned k = 1; k < body; k++) {
               const unsigned reg = b[0] + k - 1;
               if (reg >= 0x240 && reg < 0x250)
                  compute_ud[reg - 0x240] = b[k];
               for (unsigned st = 0; st < 6; st++)
                  if (reg >= stage_reg[st] && reg < stage_reg[st] + 16)
                     gfx_ud[st][reg - stage_reg[st]] = b[k];
            }
            break;
         case 0x27: case 0x2d: case 0x35: case 0x24: case 0x25: case 0x2c: case 0x38: case 0x9c: case 0x9d:
            for (unsigned st = 0; st < 6 && !bad; st++)
               bad = ps4_check_user_data(c, seq, &ib[i], stage_name[st], gfx_ud[st]);
            break;
         case 0x15: case 0x16: /* dispatches */
            bad = ps4_check_user_data(c, seq, &ib[i], "dispatch", compute_ud);
            break;
         case 0x68: /* SET_CONFIG_REG */
            c->config_regs++;
            break;
         case 0x79: /* SET_UCONFIG_REG */
            for (unsigned k = 1; k < body; k++) /* (b[0]'s top bits: the register index, not the register) */
               ps4_check_note_uconfig(c, (b[0] & 0xffff) + k - 1);
            break;
         case 0x37: /* WRITE_DATA */
            if (body >= 1 && ((b[0] >> 8) & 0xf) == 0)
               c->reg_writes++;
            if (body >= 1 && ((b[0] >> 8) & 0xf) == 6)
               c->gds++;
            break;
         case 0x3c: /* WAIT_REG_MEM */
            if (body >= 1 && !((b[0] >> 4) & 1))
               c->reg_waits++;
            break;
         case 0x40: /* COPY_DATA: register source or destination, GDS */
            if (body >= 1 && ((b[0] & 0xf) == 0 || ((b[0] >> 8) & 0xf) == 0))
               c->reg_copies++;
            if (body >= 1 && ((b[0] & 0xf) == 3 || ((b[0] >> 8) & 0xf) == 3))
               c->gds++;
            break;
         case 0x50: /* DMA_DATA: GDS or register ends */
            if (body >= 1 && (((b[0] >> 20) & 3) == 1 || ((b[0] >> 29) & 3) == 1))
               c->gds++;
            break;
         case 0x1d: /* ATOMIC_GDS */
            c->gds++;
            break;
         case 0x34: /* STRMOUT_BUFFER_UPDATE */
            c->streamout++;
            break;
         case 0x5f: case 0x60: case 0x61: case 0x63: /* LOAD_SH/CONFIG/CONTEXT/UCONFIG_REG */
            c->loads++;
            break;
         case 0x3f: { /* INDIRECT_BUFFER */
            uint32_t *t = (uint32_t *)(uintptr_t)(b[0] | ((uint64_t)(b[1] & 0xffff) << 32));
            if (body >= 3 && (b[2] & (1u << 20))) {
               next = t;
               next_dw = b[2] & 0xfffff;
            } else if (body >= 3 && depth < 3) {
               ps4_check_ib(c, t, b[2] & 0xfffff, seq, depth + 1, skip);
            }
            break;
         }
         /* the packets RADV emits all the time */
         case 0x10: case 0x11: case 0x12: case 0x13: case 0x26: case 0x28: case 0x2a: case 0x2f: case 0x33:
         case 0x39: case 0x42: case 0x43: case 0x46: case 0x47: case 0x48: case 0x58: case 0x69: case 0x1e:
         case 0x22: case 0x23: case 0x20:
            break;
         default:
            c->unknown++;
            if (c->num_unknown_ops < ARRAY_SIZE(c->unknown_ops)) {
               bool seen = false;
               for (unsigned u = 0; u < c->num_unknown_ops; u++)
                  seen |= c->unknown_ops[u] == op;
               if (!seen)
                  c->unknown_ops[c->num_unknown_ops++] = (uint8_t)op;
            }
            break;
         }
         if (bad && skip) {
            ib[i] = (h & ~0xff00u) | (0x10u << 8); /* NOP, same length */
            ps4_flush_cpu_cache(&ib[i], 4);
            c->skipped++;
         }
         if (next)
            break;
         i += 1 + body;
      }
      if (!next)
         return;
      ib = next;
      num_dw = next_dw;
   }
}

/* ---- descriptor check: end ---- */

/* Writes the census into `out` (for the submission's note). */
static void
ps4_check_submission(ac_drm_device *dev, uint64_t seq, void *const *dcb, const uint32_t *sizes, unsigned num_ibs,
                     char *out, size_t out_size)
{
   static int skip = -1;
   if (skip < 0) {
      const char *e = getenv("RADV_PS4_SKIP_WILD");
      skip = !(e && (!strcmp(e, "0") || !strcmp(e, "off")));
   }
   static struct ps4_live_range *ranges;
   static unsigned capacity;
   if (capacity < dev->num_bos) {
      free(ranges);
      capacity = dev->num_bos + 256;
      ranges = malloc(capacity * sizeof(*ranges));
      if (!ranges) {
         capacity = 0;
         snprintf(out, out_size, "; no memory for the check");
         return;
      }
   }
   struct ps4_check c;
   memset(&c, 0, sizeof(c));
   c.dev = dev;
   c.ranges = ranges;
   for (unsigned h = 1; h < dev->num_bos; h++) {
      const struct amdgpu_bo *b = dev->bos[h];
      if (!b || !b->cpu)
         continue;
      const uint64_t start = (uintptr_t)b->cpu;
      c.ranges[c.num_ranges++] = (struct ps4_live_range){start, start + b->size + (b->tail_mapped ? PS4_TAIL : 0),
                                                         b->size, b->handle};
   }
   qsort(c.ranges, c.num_ranges, sizeof(*c.ranges), ps4_live_range_cmp);
   for (unsigned i = 0; i < num_ibs; i++)
      ps4_check_ib(&c, dcb[i], sizes[i] / 4, seq, 0, skip);
   size_t n = (size_t)snprintf(out, out_size, "; check: %u wild, %u overruns, %u skipped; config regs %u, register "
                               "writes %u, waits %u, copies %u, GDS %u, streamout %u, register loads %u, other %u",
                               c.wild, c.overruns, c.skipped, c.config_regs, c.reg_writes, c.reg_waits, c.reg_copies,
                               c.gds, c.streamout, c.loads, c.unknown);
   for (unsigned u = 0; u < c.num_unknown_ops && n < out_size; u++)
      n += (size_t)snprintf(out + n, out_size - n, "%s%#x", u ? "," : " (", c.unknown_ops[u]);
   if (c.num_unknown_ops && n < out_size)
      n += (size_t)snprintf(out + n, out_size - n, ")");
   for (unsigned u = 0; u < c.num_uconfig && n < out_size; u++)
      n += (size_t)snprintf(out + n, out_size - n, "%s%#x", u ? "," : "; uconfig ", 0x30000 + c.uconfig[u] * 4);
}

/* ---- pre-flight record: begin (also built on a PC: eden-ps4/tools/gpu-tests/preflight.sh) ----
 *
 * (04.46) Captain Toad's level 3 takes the PS4 down within ~0.25 s of its first big submission
 * after the loading screen: too soon for the stall note (0.3 s) or the hang report (2 s). So the
 * big submissions noted after an idle GPU go to gpu/preflight.txt - every IB's dwords (the chained
 * and nested ones too) and the code of every shader they set - synced BEFORE they go to the GPU.
 * gpu/progress.txt (ps4_watch_thread) says, every 2 ms while they run, how far the command
 * processor got. The two name the packet the GPU was on when the PS4 went down
 * (eden-ps4/tools/gpu-preflight.py). One file of each per idle period, the previous period's in
 * preflight-previous.txt and progress-previous.txt. */

#define PS4_PF_LIMIT (6ull << 20) /* bytes of gpu/preflight.txt in one period */

struct ps4_pf {
   int fd;
   uint64_t bytes;          /* in the file so far */
   size_t n;                /* in buf */
   uint32_t lo[PS4_STAGES], hi[PS4_STAGES];
   unsigned ibs, dwords;
   bool cut;                /* the limit reached */
   uint64_t *shaders;       /* the period's, written once each */
   uint8_t *stages;
   unsigned *num_shaders, max_shaders;
   unsigned new_shaders;
   char buf[16384];         /* (last: everything before it is reset for each submission) */
};

static void
ps4_pf_flush(struct ps4_pf *pf)
{
   if (pf->n && pf->fd >= 0)
      (void)!write(pf->fd, pf->buf, pf->n);
   pf->bytes += pf->n;
   pf->n = 0;
}

static void
ps4_pf_printf(struct ps4_pf *pf, const char *fmt, ...)
{
   if (sizeof(pf->buf) - pf->n < 512)
      ps4_pf_flush(pf);
   va_list ap;
   va_start(ap, fmt);
   const int r = vsnprintf(pf->buf + pf->n, sizeof(pf->buf) - pf->n, fmt, ap);
   va_end(ap);
   if (r > 0)
      pf->n = MIN2(pf->n + (size_t)r, sizeof(pf->buf) - 1);
}

static bool
ps4_pf_full(struct ps4_pf *pf)
{
   if (!pf->cut && pf->bytes + pf->n >= PS4_PF_LIMIT) {
      pf->cut = true;
      ps4_pf_printf(pf, "(the rest left out: the record's %llu MiB for this period are used)\n",
                    (unsigned long long)(PS4_PF_LIMIT >> 20));
   }
   return pf->cut;
}

/* A shader address set by SET_SH_REG: noted once a period (its code goes after the IBs). */
static void
ps4_pf_note_shader(struct ps4_pf *pf, unsigned stage)
{
   const uint64_t va = ((uint64_t)pf->lo[stage] << 8) | ((uint64_t)(pf->hi[stage] & 0xff) << 40);
   if (!va)
      return;
   for (unsigned s = 0; s < *pf->num_shaders; s++)
      if (pf->shaders[s] == va)
         return;
   if (*pf->num_shaders >= pf->max_shaders)
      return;
   pf->stages[*pf->num_shaders] = (uint8_t)stage;
   pf->shaders[(*pf->num_shaders)++] = va;
   pf->new_shaders++;
}

/* One IB's dwords, "<address>: <8 dwords>" a line, then the IBs it calls (in the order it calls
 * them) and the one it chains to. */
static void
ps4_pf_ib(struct ps4_pf *pf, uint64_t va, unsigned num_dw, int depth)
{
   for (unsigned link = 0; link < 256 && num_dw; link++) {
      if (ps4_pf_full(pf))
         return;
      const uint32_t *ib = ps4_hw_read(va, num_dw * 4ull);
      pf->ibs++;
      ps4_pf_printf(pf, "== IB %#llx, %u dwords, depth %d%s\n", (unsigned long long)va, num_dw, depth,
                    ib ? "" : ": not mapped");
      if (!ib)
         return;
      pf->dwords += num_dw;
      for (unsigned i = 0; i < num_dw; i += 8) {
         if (i % 4096 == 0 && ps4_pf_full(pf))
            return;
         ps4_pf_printf(pf, "%010llx:", (unsigned long long)(va + 4ull * i));
         for (unsigned k = i; k < num_dw && k < i + 8; k++)
            ps4_pf_printf(pf, " %08x", ib[k]);
         ps4_pf_printf(pf, "\n");
      }
      uint64_t next_va = 0;
      unsigned next_dw = 0;
      for (unsigned i = 0; i < num_dw;) {
         const uint32_t h = ib[i];
         if (h >> 30 == 2 || h == 0xffff1000u) { /* type-2 and PKT3_NOP_PAD: one dword */
            i++;
            continue;
         }
         const unsigned op = (h >> 8) & 0xff;
         const unsigned body = ((h >> 16) & 0x3fff) + 1;
         if (h >> 30 != 3 || i + 1 + body > num_dw)
            break;
         const uint32_t *b = &ib[i + 1];
         if ((op == 0x3f || op == 0x33) && body >= 3) {
            const uint64_t target = b[0] | ((uint64_t)(b[1] & 0xffff) << 32);
            if (b[2] & (1u << 20)) {
               next_va = target;
               next_dw = b[2] & 0xfffff;
               break;
            }
            if (depth < 4)
               ps4_pf_ib(pf, target, b[2] & 0xfffff, depth + 1);
         } else if (op == 0x76 && body >= 2) { /* SET_SH_REG: the shader addresses */
            unsigned set = 0;
            for (unsigned k = 1; k < body; k++) {
               const unsigned reg = (b[0] & 0xffff) + k - 1;
               static const unsigned lo_reg[PS4_STAGES] = {0x148, 0x108, 0x0c8, 0x088, 0x048, 0x008, 0x20c};
               for (unsigned s = 0; s < PS4_STAGES; s++) {
                  if (reg == lo_reg[s]) {
                     pf->lo[s] = b[k];
                     set |= 1u << s;
                  } else if (reg == lo_reg[s] + 1) {
                     pf->hi[s] = b[k];
                     set |= 1u << s;
                  }
               }
            }
            for (unsigned s = 0; s < PS4_STAGES; s++)
               if (set & (1u << s))
                  ps4_pf_note_shader(pf, s);
         }
         i += 1 + body;
      }
      if (!next_va)
         return;
      va = next_va;
      num_dw = next_dw;
   }
}

/* Submission `seq` (its IBs dcb/sizes) into the record at `fd`; returns the bytes written. The
 * shaders it sets that the period's record doesn't have yet follow its IBs. */
static uint64_t
ps4_pf_submission(int fd, uint64_t seq, void *const *dcb, const uint32_t *sizes, unsigned num_ibs,
                  uint32_t first_trace, uint32_t last_trace, uint64_t *shaders, uint8_t *stages,
                  unsigned *num_shaders, unsigned max_shaders, uint64_t bytes_before, unsigned *ibs_out,
                  unsigned *new_shaders_out)
{
   static struct ps4_pf pf; /* (under dev->lock) */
   memset(&pf, 0, offsetof(struct ps4_pf, buf));
   pf.fd = fd;
   pf.bytes = bytes_before;
   pf.shaders = shaders;
   pf.stages = stages;
   pf.num_shaders = num_shaders;
   pf.max_shaders = max_shaders;
   const unsigned first_shader = *num_shaders;
   ps4_pf_printf(&pf, "\n==== submission %llu: %u IBs, trace ids %u-%u\n", (unsigned long long)seq, num_ibs,
                 first_trace, last_trace);
   for (unsigned i = 0; i < num_ibs && !ps4_pf_full(&pf); i++) {
      ps4_pf_printf(&pf, "=== top-level IB %u\n", i);
      ps4_pf_ib(&pf, (uintptr_t)dcb[i], sizes[i] / 4, 0);
   }
   ps4_pf_printf(&pf, "=== end of submission %llu (%u IBs in all, %u dwords; %u new shaders)\n",
                 (unsigned long long)seq, pf.ibs, pf.dwords, pf.new_shaders);
   ps4_pf_flush(&pf);
   uint64_t done[1];
   for (unsigned s = first_shader; s < *num_shaders && pf.bytes < PS4_PF_LIMIT; s++) {
      unsigned num_done = 0;
      ps4_dump_shader(fd, shaders[s], stages[s], s, done, &num_done);
      const off_t at = fd >= 0 ? lseek(fd, 0, SEEK_CUR) : -1;
      if (at > 0)
         pf.bytes = (uint64_t)at;
   }
   if (ibs_out)
      *ibs_out = pf.ibs;
   if (new_shaders_out)
      *new_shaders_out = pf.new_shaders;
   return pf.bytes - bytes_before;
}

/* ---- pre-flight record: end ---- */

static int ps4_pf_fd = -1;            /* gpu/preflight.txt of the current idle period (under dev->lock) */
static unsigned ps4_pf_count;         /* submissions in it (at most 16 a period) */
static uint64_t ps4_pf_bytes;         /* written to it */
static uint64_t ps4_pf_shaders[1024]; /* the shaders in it */
static uint8_t ps4_pf_stages[1024];
static unsigned ps4_pf_num_shaders;
static unsigned ps4_pf_period;        /* idle periods so far: the progress thread starts a new file on each */
static int64_t ps4_watch_until;       /* the progress record runs until then (ns) */
static uint64_t ps4_handed_seq;       /* the last submission handed to the system (the submission thread) */

static void
ps4_pf_begin(double idle_s)
{
   static int enabled = -1;
   if (enabled < 0) {
      const char *e = getenv("RADV_PS4_PREFLIGHT");
      enabled = !(e && (!strcmp(e, "0") || !strcmp(e, "off")));
   }
   if (!enabled)
      return;
   if (ps4_pf_fd >= 0)
      close(ps4_pf_fd);
   char old[256];
   snprintf(old, sizeof(old), "%s", ps4_gpu_file("preflight.txt"));
   rename(old, ps4_gpu_file("preflight-previous.txt"));
   ps4_pf_fd = open(old, O_WRONLY | O_CREAT | O_TRUNC, 0666);
   ps4_pf_bytes = 0;
   ps4_pf_count = 0;
   ps4_pf_num_shaders = 0;
   if (ps4_pf_fd >= 0) {
      char line[400];
      const int n = snprintf(line, sizeof(line),
                             "the GPU was idle %.1f s (idle period %u); the submissions after it with 8 or more "
                             "draws and dispatches, each written and synced before it went to the GPU: every IB "
                             "(address: dwords), then the code of the shaders it set that weren't here yet. "
                             "Trace ids at %#llx. gpu/progress.txt: how far the GPU got "
                             "(eden-ps4/tools/gpu-preflight.py)\n",
                             idle_s, ps4_pf_period + 1, (unsigned long long)ps4_trace_va);
      (void)!write(ps4_pf_fd, line, (size_t)n);
      ps4_pf_bytes = (uint64_t)n;
   }
   __atomic_add_fetch(&ps4_pf_period, 1, __ATOMIC_RELEASE);
}

static void
ps4_preflight(uint64_t seq, void *const *dcb, const uint32_t *sizes, unsigned num_ibs, uint32_t first_trace,
              uint32_t last_trace)
{
   if (ps4_pf_fd < 0 || ps4_pf_bytes >= PS4_PF_LIMIT || ps4_pf_count >= 16)
      return;
   ps4_pf_count++;
   const int64_t t0 = os_time_get_nano();
   unsigned ibs = 0, new_shaders = 0;
   const uint64_t bytes = ps4_pf_submission(ps4_pf_fd, seq, dcb, sizes, num_ibs, first_trace, last_trace,
                                            ps4_pf_shaders, ps4_pf_stages, &ps4_pf_num_shaders,
                                            ARRAY_SIZE(ps4_pf_shaders), ps4_pf_bytes, &ibs, &new_shaders);
   ps4_pf_bytes += bytes;
   fsync(ps4_pf_fd);
   char line[220];
   snprintf(line, sizeof(line), "radv/ps4: submission %llu in gpu/preflight.txt before the GPU got it: %u IBs, "
            "%u new shaders, %llu KiB (%.0f ms with the sync)", (unsigned long long)seq, ibs, new_shaders,
            (unsigned long long)(bytes >> 10), (os_time_get_nano() - t0) / 1e6);
   ps4_boot_trace(line);
}

/* (04.46) Every 2 ms while noted submissions run (synced every 10 ms): what completed, the last
 * submission handed to the system, the last one the CP started (its timestamp at the start), and
 * the trace id it passed last. A line when one of them moves, else every 50 ms. */
static void *
ps4_watch_thread(void *arg)
{
   ac_drm_device *dev = arg;
   int fd = -1;
   unsigned period = 0, lines = 0;
   uint64_t last_done = ~0ull, last_handed = ~0ull, last_started = ~0ull;
   uint32_t last_trace = ~0u;
   int64_t start = 0, last_line = 0, last_sync = 0, max_sync = 0;
   bool dirty = false;
   for (;;) {
      const int64_t now = os_time_get_nano();
      const unsigned p = __atomic_load_n(&ps4_pf_period, __ATOMIC_ACQUIRE);
      if (p != period) {
         if (fd >= 0) {
            fsync(fd);
            close(fd);
         }
         char path[256];
         snprintf(path, sizeof(path), "%s", ps4_gpu_file("progress.txt"));
         rename(path, ps4_gpu_file("progress-previous.txt"));
         fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
         period = p;
         lines = 0;
         start = now;
         last_done = last_handed = last_started = ~0ull;
         last_trace = ~0u;
         if (fd >= 0) {
            char line[300];
            const int n = snprintf(line, sizeof(line),
                                   "idle period %u: ms from its first noted submission; completed / handed to "
                                   "the system / last the CP started (of submitted); the trace id the CP passed "
                                   "last; the longest sync so far\n", p);
            (void)!write(fd, line, (size_t)n);
            dirty = true;
         }
      }
      if (fd < 0 || now >= __atomic_load_n(&ps4_watch_until, __ATOMIC_RELAXED)) {
         if (fd >= 0 && dirty) {
            fsync(fd);
            dirty = false;
         }
         sceKernelUsleep(10000);
         continue;
      }
      const uint64_t done = ps4_completed(dev);
      const uint64_t submitted = dev->last_seq;
      const uint64_t handed = __atomic_load_n(&ps4_handed_seq, __ATOMIC_ACQUIRE);
      uint64_t started = done;
      if (dev->ts_start)
         for (uint64_t s = handed; s > done && s + FENCE_IB_COUNT > handed; s--)
            if (dev->ts_start[s % FENCE_IB_COUNT]) {
               started = s;
               break;
            }
      const volatile uint32_t *t = ps4_trace_va ? ps4_hw_read(ps4_trace_va, 4) : NULL;
      const uint32_t trace = t ? *t : 0;
      const bool moved = done != last_done || handed != last_handed || started != last_started ||
                         trace != last_trace;
      if ((moved || now - last_line >= 50000000ll) && lines < 40000) {
         char line[200];
         const int n = snprintf(line, sizeof(line), "%9.1f: %llu / %llu / %llu (of %llu); trace %u; sync %.1f ms\n",
                                (now - start) / 1e6, (unsigned long long)done, (unsigned long long)handed,
                                (unsigned long long)started, (unsigned long long)submitted, trace, max_sync / 1e6);
         (void)!write(fd, line, (size_t)n);
         lines++;
         dirty = true;
         last_line = now;
         last_done = done;
         last_handed = handed;
         last_started = started;
         last_trace = trace;
      }
      if (dirty && now - last_sync >= 10000000ll) {
         const int64_t t0 = os_time_get_nano();
         fsync(fd);
         const int64_t t1 = os_time_get_nano();
         max_sync = MAX2(max_sync, t1 - t0);
         last_sync = t1;
         dirty = false;
      }
      sceKernelUsleep(2000);
   }
   return NULL;
}

/* (04.41) The first submissions after the GPU was idle a while (a game's loading screen), and their
 * completions, in the boot trace (synced within 50 ms by the app): Captain Toad's level 3 takes the
 * PS4 down within a quarter second of its loading, with the GPU idle and caught up just before;
 * this says whether the GPU got level 3's first work and how far it got.
 * (04.47) "Idle" is now a gap of 1 s (RADV_PS4_NOTE_GAP_MS) with nothing submitted. Before, it was
 * the stall thread finding the GPU done at its 50 ms looks for 0.5 s, which a game running at all
 * slowly (the GPU idle between frames) met every second or so: Zelda's 04.45 run noted 8762
 * submissions in 220 periods, and 04.46's record (MiBs written and synced for each) slowed Captain
 * Toad to 11 fps, which met it more often still. */
static int64_t ps4_last_submit_ns;   /* when the last submission was noted or passed by (under dev->lock) */
static int ps4_after_idle_left;      /* submissions still to note (under dev->lock) */
static uint64_t ps4_after_idle_last; /* the last one noted: completions are noted up to it */

static void
ps4_note_submission(ac_drm_device *dev, uint64_t seq, void *const *dcb, const uint32_t *sizes, unsigned num_ibs)
{
   static int64_t min_gap = -1;
   if (min_gap < 0) {
      const char *e = getenv("RADV_PS4_NOTE_GAP_MS");
      min_gap = (e && atoi(e) > 0 ? atoi(e) : 1000) * 1000000ll;
   }
   const int64_t now = os_time_get_nano();
   const int64_t gap = ps4_last_submit_ns ? now - ps4_last_submit_ns : 0;
   if (!ps4_after_idle_left && gap >= min_gap && ps4_completed(dev) + 1 >= seq) {
      ps4_after_idle_left = 40;
      char line[160];
      snprintf(line, sizeof(line), "radv/ps4: the GPU was idle %.1f s (nothing submitted): its next 40 "
               "submissions follow", gap / 1e9);
      ps4_boot_trace(line);
      ps4_pf_begin(gap / 1e9);
   }
   if (ps4_after_idle_left <= 0) {
      ps4_last_submit_ns = now;
      return;
   }
   ps4_after_idle_left--;
   __atomic_store_n(&ps4_after_idle_last, seq, __ATOMIC_RELEASE);
   static struct ps4_pkt pkts[16384]; /* (04.46: 4096 cut Toad's level 3 upload short) */
   struct ps4_walk w;
   memset(&w, 0, sizeof(w));
   w.pkts = pkts;
   w.max = ARRAY_SIZE(pkts);
   unsigned dwords = 0;
   for (unsigned i = 0; i < num_ibs; i++) {
      ps4_walk_collect(&w, (uintptr_t)dcb[i], sizes[i] / 4, 0);
      dwords += sizes[i] / 4;
   }
   unsigned draws = 0, indirect = 0, dispatches = 0, waits = 0, traces = 0;
   uint32_t first_trace = 0, last_trace = 0;
   for (unsigned i = 0; i < w.count; i++) {
      const struct ps4_pkt *p = &w.pkts[i];
      draws += p->op == 0x27 || p->op == 0x2d || p->op == 0x35 || p->op == 0x24 || p->op == 0x25 ||
               p->op == 0x2c || p->op == 0x38 || p->op == 0x9c || p->op == 0x9d;
      indirect += p->op == 0x24 || p->op == 0x25 || p->op == 0x2c || p->op == 0x38 || p->op == 0x9c ||
                  p->op == 0x9d || p->op == 0x16;
      dispatches += p->op == 0x15 || p->op == 0x16;
      waits += p->op == 0x3c;
      if (p->op == 0x37 && ps4_trace_va && p->addr == ps4_trace_va) {
         if (!traces++)
            first_trace = p->v[0];
         last_trace = p->v[0];
      }
   }
   char check[600];
   check[0] = 0;
   ps4_check_submission(dev, seq, dcb, sizes, num_ibs, check, sizeof(check));
   char line[1000];
   snprintf(line, sizeof(line), "radv/ps4: submit %llu: %u IBs, %u dwords, %u draws, %u dispatches (%u of them "
            "indirect), %u WAIT_REG_MEM, trace ids %u-%u; geometry shader / tessellation: %u draws, %u ring "
            "writes%s%s", (unsigned long long)seq, num_ibs, dwords, draws, dispatches, indirect, waits, first_trace,
            last_trace, ps4_gs_tess_last_draws, ps4_gs_tess_last_rings, w.dropped ? " (walk cut short)" : "", check);
   ps4_boot_trace(line);
   if (draws + dispatches >= 8)
      ps4_preflight(seq, dcb, sizes, num_ibs, first_trace, last_trace);
   __atomic_store_n(&ps4_watch_until, os_time_get_nano() + 3000000000ll, __ATOMIC_RELAXED);
   ps4_last_submit_ns = os_time_get_nano(); /* (the note's own time is no gap) */
}

void ac_ps4_gpu_progress(uint64_t *submitted, uint64_t *completed, uint32_t *trace_id, int64_t *stalled_ms);
void
ac_ps4_gpu_progress(uint64_t *submitted, uint64_t *completed, uint32_t *trace_id, int64_t *stalled_ms)
{
   ac_drm_device *dev = ps4_last_device;
   *submitted = dev ? dev->last_seq : 0;
   *completed = dev ? ps4_completed(dev) : 0;
   const volatile uint32_t *t = ps4_trace_va ? ps4_hw_read(ps4_trace_va, 4) : NULL;
   *trace_id = t ? *t : 0;
   const int64_t since = __atomic_load_n(&ps4_progress_ns, __ATOMIC_RELAXED);
   *stalled_ms = dev && *completed < *submitted && since ? (os_time_get_nano() - since) / 1000000 : 0;
}

static void *
ps4_stall_thread(void *arg)
{
   ac_drm_device *dev = arg;
   const char *e = getenv("RADV_PS4_STALL_MS");
   const int64_t limit = (e && atoi(e) > 0 ? atoi(e) : 300) * 1000000ll;
   uint64_t last = ps4_completed(dev), noted = ~0ull;
   int64_t moved = os_time_get_nano();
   unsigned notes = 0;
   __atomic_store_n(&ps4_progress_ns, moved, __ATOMIC_RELAXED);
   for (;;) {
      sceKernelUsleep(50000);
      const uint64_t done = ps4_completed(dev);
      const int64_t now = os_time_get_nano();
      if (done != last && last < __atomic_load_n(&ps4_after_idle_last, __ATOMIC_ACQUIRE)) {
         char line[120];
         snprintf(line, sizeof(line), "radv/ps4: GPU completed %llu (submitted %llu)", (unsigned long long)done,
                  (unsigned long long)dev->last_seq);
         ps4_boot_trace(line);
      }
      if (done != last || done >= dev->last_seq) {
         if (done != last && noted == last) {
            char line[160];
            snprintf(line, sizeof(line), "radv/ps4: the GPU went on after a %.1f s stall (completed %llu)",
                     (now - moved) / 1e9, (unsigned long long)done);
            ps4_log("%s\n", line);
            ps4_boot_trace(line);
         }
         last = done;
         moved = now;
         __atomic_store_n(&ps4_progress_ns, now, __ATOMIC_RELAXED);
         continue;
      }
      if (now - moved < limit || noted == done || notes >= 8)
         continue;
      noted = done;
      notes++;
      FILE *f = fopen(ps4_gpu_file("gpu-stall.txt"), "w");
      if (f) {
         fprintf(f, "the GPU finished nothing for %.0f ms: completed %llu, submitted %llu (stall note %u)\n\n",
                 (now - moved) / 1e6, (unsigned long long)done, (unsigned long long)dev->last_seq, notes);
         const int shader_fd = open(ps4_gpu_file("stall-shaders.txt"), O_WRONLY | O_CREAT | O_TRUNC, 0666);
         ps4_report_stop_point(dev, f, done, shader_fd);
         if (shader_fd >= 0) {
            fsync(shader_fd);
            close(shader_fd);
         }
         fflush(f);
         fsync(fileno(f));
         fclose(f);
      }
      char line[200];
      snprintf(line, sizeof(line), "radv/ps4: GPU stalled %.1f s at submission %llu (of %llu submitted): where it "
               "stopped in gpu/gpu-stall.txt", (os_time_get_nano() - moved) / 1e9, (unsigned long long)(done + 1),
               (unsigned long long)dev->last_seq);
      ps4_log("%s\n", line);
      ps4_boot_trace(line);
      if (ps4_boot_trace_sync)
         ps4_boot_trace_sync();
   }
   return NULL;
}

static void
ps4_start_stall_thread(ac_drm_device *dev)
{
   static bool started;
   const char *e = getenv("RADV_PS4_STALL_MS");
   if (started || !ps4_trace_option(NULL) || (e && !strcmp(e, "0")))
      return;
   pthread_t t;
   if (pthread_create(&t, NULL, ps4_stall_thread, dev) == 0) {
      pthread_detach(t);
      started = true;
   }
   if (pthread_create(&t, NULL, ps4_watch_thread, dev) == 0)
      pthread_detach(t);
}

/* Crema: how the CPU waits for the GPU, every 5 s in the boot trace (waits per second, their
 * average length, and how many sleeps ended by the 1 ms timeout instead of the interrupt). */
void ps4_boot_trace(const char *stage);
static struct {
   uint64_t waits, wait_ns, sleeps, timeouts, since_ns;
} s_wait_stats;

/* What the CPU spent waiting for the GPU's work in Vulkan fences and semaphores (ps4_wait_common),
 * for the launcher's statistics (ac_ps4_get_gpu_stats): waits that had to wait, and their time. */
static uint64_t s_fence_waits, s_fence_wait_ns;

static void
ps4_wait_stats_report(int64_t now)
{
   if (!s_wait_stats.since_ns) {
      s_wait_stats.since_ns = now;
      return;
   }
   const int64_t span = now - (int64_t)s_wait_stats.since_ns;
   if (span < 5000000000ll)
      return;
   char line[200];
   snprintf(line, sizeof(line), "gpu waits: %.0f/s, average %.0f us, %.0f%% of the time; %llu sleeps, %llu ended by the 1 ms timeout",
            s_wait_stats.waits * 1e9 / span, s_wait_stats.waits ? s_wait_stats.wait_ns / 1e3 / s_wait_stats.waits : 0.0,
            s_wait_stats.wait_ns * 100.0 / span, (unsigned long long)s_wait_stats.sleeps,
            (unsigned long long)s_wait_stats.timeouts);
   ps4_boot_trace(line);
   s_wait_stats.waits = s_wait_stats.wait_ns = s_wait_stats.sleeps = s_wait_stats.timeouts = 0;
   s_wait_stats.since_ns = now;
}

/* Waits for the next GPU end-of-pipe interrupt (any submission's fence write), at most 1 ms. */
static void
ps4_sleep(ac_drm_device *dev)
{
   if (dev->eop_queue) {
      uint64_t events[8][4];
      int count = 0;
      unsigned timeout = 1000;
      sceKernelWaitEqueue(dev->eop_queue, events, 8, &count, &timeout);
      s_wait_stats.sleeps++;
      if (count <= 0)
         s_wait_stats.timeouts++;
   } else {
      sceKernelUsleep(50);
   }
}

static bool
ps4_wait_seq(ac_drm_device *dev, uint64_t seq, int64_t abs_timeout_ns)
{
   unsigned spins = 0;
   const int64_t start = os_time_get_nano();
   bool reported = false;
   while (ps4_completed(dev) < seq) {
      const int64_t now = os_time_get_nano();
      if (abs_timeout_ns != INT64_MAX && now >= abs_timeout_ns)
         return false;
      /* A wait this long means the GPU stopped (hang) or the fence write never happened. */
      if (!reported && now - start > 2000000000ll) {
         ps4_report_hang(dev, seq);
         reported = true;
      }
      if (++spins > 64)
         ps4_sleep(dev);
   }
   if (spins) {
      const int64_t end = os_time_get_nano();
      s_wait_stats.waits++;
      s_wait_stats.wait_ns += end - start;
      ps4_wait_stats_report(end);
   }
   return true;
}

/* ---------------------------------------------------------------------------------------------
 * Device
 */

static bool
ps4_load_gnm(ac_drm_device *dev)
{
   char path[128];
   const char *word = sceKernelGetFsSandboxRandomWord();
   snprintf(path, sizeof(path), "/%s/common/lib/libSceGnmDriver.sprx", word ? word : "system");
   int module = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
   if (module < 0) {
      ps4_log("radv/ps4: loading %s failed (%#x)\n", path, module);
      return false;
   }
   if (sceKernelDlsym(module, "sceGnmAddEqEvent", (void **)&dev->add_eq_event))
      dev->add_eq_event = NULL;
   if (sceKernelDlsym(module, "sceGnmSubmitAndFlipCommandBuffers", (void **)&dev->submit_and_flip))
      dev->submit_and_flip = NULL;
   if (sceKernelDlsym(module, "sceGnmSubmitCommandBuffers", (void **)&dev->submit) ||
       sceKernelDlsym(module, "sceGnmSubmitDone", (void **)&dev->submit_done)) {
      ps4_log("radv/ps4: libSceGnmDriver symbols missing\n");
      return false;
   }
   dev->gnm_module = module;
   /* (Eden 04.43) The GS ring sizes as PS4 games set them: VGT_ESGS/GSVS_RING_SIZE are the
    * system's (RADV's own writes don't take: Crema found the GS writing past RADV's ring), and
    * Sony's sceGnmSetGsRingSizes takes 4-8 MiB (1 MiB aligned; shadPS4's reading of
    * libSceGnmDriver). 8 MiB each, which is what RADV programs on the PS4 (ac_ps4_gs_ring_min) and
    * half of what it allocates (PS4_RING_ALLOC): the VGT and RADV then agree on the rings.
    * RADV_PS4_GS_RINGS=0: the system's sizes as they are. */
   const char *gs = getenv("RADV_PS4_GS_RINGS");
   int (*set_gs_ring_sizes)(uint32_t, uint32_t) = NULL;
   if (!(gs && !strcmp(gs, "0")) &&
       !sceKernelDlsym(module, "sceGnmSetGsRingSizes", (void **)&set_gs_ring_sizes) && set_gs_ring_sizes) {
      const int r = set_gs_ring_sizes(8u << 20, 8u << 20);
      char line[120];
      snprintf(line, sizeof(line), "radv/ps4: sceGnmSetGsRingSizes(8 MiB, 8 MiB) = %#x", r);
      ps4_log("%s\n", line);
      ps4_boot_trace(line);
   }
   return true;
}

/* ---------------------------------------------------------------------------------------------
 * GPU fault capture. A GPU page/protection fault kills the app without a signal. Hooks found by
 * disassembling the system modules:
 *  - libkernel's libSceCoredump: sceCoredumpRegisterCoredumpHandler(handler, stack, arg) starts
 *    a thread that blocks in mdbg_service(0x14) until the kernel begins a core dump of this
 *    process, then calls `handler` - i.e. it runs on exactly that kill.
 *    sceCoredumpGetStopInfoCpu(buf, 16) / sceCoredumpGetStopInfoGpu(buf, 8) say why.
 *  - libSceGnmDriver maps 512 KiB of system memory named "SceGnmGpuInfo" and registers it with
 *    the kernel (sceKernelSetProcessProperty "Sce.Debug:Gnm"); the GPU driver records GPU state
 *    there (+0 valid, +0x90 protection fault timestamp, ...). Its pointer is GnmDriver data
 *    +0x10508 (firmware 12.00: sceGnmIsCoredumpValid at +0x690 reads it).
 *  - sceKernelAddGpuExceptionEvent(eq, udata): kevent filter -20 on GPU exceptions.
 * Everything is written to /data/Crema/gpu/gpu-fault.txt, with the recent submissions and every
 * live buffer, so a fault address can be mapped to its buffer.
 */
int sceKernelGetModuleList(int *handles, size_t num, size_t *actual);
static const uint8_t *ps4_gpu_info;
static int (*ps4_stop_info_cpu)(void *, size_t);
static int (*ps4_stop_info_gpu)(void *, size_t);
static ac_drm_device *ps4_fault_dev;

static void
ps4_dump_state(FILE *f, ac_drm_device *dev)
{
   uint64_t cpu[2] = {0}, gpu = 0;
   const int rc = ps4_stop_info_cpu ? ps4_stop_info_cpu(cpu, sizeof(cpu)) : -1;
   const int rg = ps4_stop_info_gpu ? ps4_stop_info_gpu(&gpu, sizeof(gpu)) : -1;
   fprintf(f, "stop info cpu %#x: %#llx %#llx; gpu %#x: %#llx\n", rc, (unsigned long long)cpu[0],
           (unsigned long long)cpu[1], rg, (unsigned long long)gpu);
   if (dev) {
      fprintf(f, "GPU completed %llu, submitted %llu\n", (unsigned long long)ps4_completed(dev),
              (unsigned long long)dev->last_seq);
      for (unsigned n = 0; n < ARRAY_SIZE(dev->recent); n++) {
         const unsigned r = (dev->last_seq + 1 + n) % ARRAY_SIZE(dev->recent);
         if (!dev->recent[r].seq)
            continue;
         fprintf(f, "submission %llu:", (unsigned long long)dev->recent[r].seq);
         for (unsigned i = 0; i < dev->recent[r].num_ibs; i++)
            fprintf(f, " %#llx+%u", (unsigned long long)dev->recent[r].va[i], dev->recent[r].bytes[i]);
         fprintf(f, "\n");
      }
      fprintf(f, "live buffers:\n");
      for (unsigned h = 1; h < dev->num_bos; h++) {
         const struct amdgpu_bo *b = dev->bos[h];
         if (b && b->last_va)
            fprintf(f, "  bo %u va %#llx..%#llx phys %#llx heap %#x flags %#llx%s%s\n", b->handle,
                    (unsigned long long)b->last_va, (unsigned long long)(b->last_va + b->size),
                    (unsigned long long)b->phys, b->heap, (unsigned long long)b->flags,
                    b->gpu_ro ? " gpu-read-only" : "", b->cpu ? "" : " (unmapped)");
      }
   }
   if (ps4_gpu_info) {
      fprintf(f, "SceGnmGpuInfo %p (non-zero 32-byte lines):\n", (const void *)ps4_gpu_info);
      unsigned lines = 0;
      for (unsigned off = 0; off < 0x80000 && lines < 3000; off += 32) {
         const uint32_t *w = (const uint32_t *)(ps4_gpu_info + off);
         if (!(w[0] | w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7]))
            continue;
         fprintf(f, "  +%#07x: %08x %08x %08x %08x %08x %08x %08x %08x\n", off, w[0], w[1], w[2],
                 w[3], w[4], w[5], w[6], w[7]);
         lines++;
      }
   }
}

static int
ps4_coredump_handler(void *arg)
{
   (void)arg;
   FILE *f = fopen(ps4_gpu_file("gpu-fault.txt"), "w");
   if (f) {
      fprintf(f, "==== core dump handler (the system is killing the app)\n");
      ps4_dump_state(f, ps4_fault_dev);
      fclose(f);
   }
   return 0;
}

static void *
ps4_gpu_exception_thread(void *eq)
{
   for (;;) {
      uint64_t ev[4][4];
      int n = 0;
      if (sceKernelWaitEqueue(eq, ev, 4, &n, NULL) || n <= 0)
         continue;
      FILE *f = fopen(ps4_gpu_file("gpu-fault.txt"), "a");
      for (int i = 0; i < n; i++) {
         ps4_log("radv/ps4: GPU EXCEPTION event: ident %#llx filter/flags/fflags %#llx data %#llx "
                 "udata %#llx\n", (unsigned long long)ev[i][0], (unsigned long long)ev[i][1],
                 (unsigned long long)ev[i][2], (unsigned long long)ev[i][3]);
         if (f)
            fprintf(f, "==== GPU exception event: ident %#llx filter/flags/fflags %#llx data %#llx\n",
                    (unsigned long long)ev[i][0], (unsigned long long)ev[i][1],
                    (unsigned long long)ev[i][2]);
      }
      if (f) {
         ps4_dump_state(f, ps4_fault_dev);
         fclose(f);
      }
   }
   return NULL;
}

/* A libkernel export not in the link stubs: look it up in every loaded module. */
static void *
ps4_find_symbol(const char *name)
{
   int handles[256];
   size_t count = 0;
   if (sceKernelGetModuleList(handles, ARRAY_SIZE(handles), &count))
      return NULL;
   for (size_t i = 0; i < count && i < ARRAY_SIZE(handles); i++) {
      void *p = NULL;
      if (!sceKernelDlsym(handles[i], name, &p) && p)
         return p;
   }
   return NULL;
}

/* ---------------------------------------------------------------------------------------------
 * Flight recorder (with RADV_PS4_TRACE other than "ids", ps4.ini radv_trace=on). Nothing reports
 * a GPU fault on a retail PS4 - the app is just gone, and its core dump is encrypted - but data
 * written with write() sits in the kernel's page cache and survives the kill. So:
 *  - /data/Crema/gpu/flight/ib-N.bin: the complete command stream of each submission (every IB,
 *    chained and nested), in a ring of 8 files (N = submission % 8);
 *  - /data/Crema/gpu/flight/progress.log: submissions, the GPU's completed submission, and the
 *    last RADV trace id (written by the CP as it reaches each draw), every millisecond it changes.
 * After a kill, the last trace id locates the draw the GPU died on inside the saved stream.
 */
#include <sys/stat.h>
static int ps4_progress_fd = -1;

void ac_ps4_set_trace_va(uint64_t va);
void
ac_ps4_set_trace_va(uint64_t va)
{
   ps4_trace_va = va;
   ps4_trace_id = (volatile const uint32_t *)(uintptr_t)va;
}

static void
ps4_progress(const char *fmt, ...)
{
   if (ps4_progress_fd < 0)
      return;
   char line[256];
   va_list ap;
   va_start(ap, fmt);
   const int n = vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   if (n > 0)
      write(ps4_progress_fd, line, MIN2(n, (int)sizeof(line) - 1));
}

static void *
ps4_progress_thread(void *arg)
{
   (void)arg;
   ac_drm_device *dev;
   uint64_t last_done = ~0ull;
   uint32_t last_trace = ~0u;
   int64_t last_write = 0;
   for (;;) {
      sceKernelUsleep(500);
      dev = ps4_fault_dev; /* the device in use (the last one created) */
      const uint64_t done = ps4_completed(dev);
      const uint32_t trace = ps4_trace_id ? *ps4_trace_id : 0;
      const int64_t now = os_time_get_nano();
      if (done == last_done && trace == last_trace)
         continue;
      if (done == last_done && now - last_write < 2000000)
         continue; /* trace ids alone: at most every 2 ms */
      ps4_progress("%lld.%06lld completed %llu submitted %llu trace %u\n",
                   (long long)(now / 1000000000), (long long)(now / 1000 % 1000000),
                   (unsigned long long)done, (unsigned long long)dev->last_seq, trace);
      last_done = done;
      last_trace = trace;
      last_write = now;
   }
   return NULL;
}

struct ps4_ib_record {
   uint32_t magic; /* 'IBRC' */
   uint32_t depth;
   uint64_t seq;
   uint64_t va;
   uint32_t num_dw;
   uint32_t top_index;
};

static void
ps4_record_ib(int fd, const uint32_t *ib, unsigned num_dw, uint64_t seq, unsigned top, int depth)
{
   for (unsigned link = 0; link < 256 && num_dw; link++) {
      if (!ps4_mapped(ib, num_dw * 4ull))
         return;
      const struct ps4_ib_record r = {0x43524249, depth, seq, (uintptr_t)ib, num_dw, top};
      write(fd, &r, sizeof(r));
      write(fd, ib, num_dw * 4ull);
      const uint32_t *next = NULL;
      unsigned next_dw = 0;
      for (unsigned i = 0; i < num_dw;) {
         const uint32_t h = ib[i];
         if (h >> 30 == 2 || h == 0xffff1000u) { /* (and PKT3_NOP_PAD: a header-only NOP) */
            i++;
            continue;
         }
         if (h >> 30 != 3)
            break;
         const unsigned op = (h >> 8) & 0xff;
         const unsigned body = ((h >> 16) & 0x3fff) + 1;
         if (i + 1 + body > num_dw)
            break;
         const uint32_t *b = &ib[i + 1];
         if ((op == 0x3f || op == 0x33) && body >= 3) {
            const uint32_t *t = (const uint32_t *)(uintptr_t)(b[0] | ((uint64_t)(b[1] & 0xffff) << 32));
            if (b[2] & (1u << 20)) {
               next = t;
               next_dw = b[2] & 0xfffff;
               break;
            }
            if (depth < 2)
               ps4_record_ib(fd, t, b[2] & 0xfffff, seq, top, depth + 1);
         }
         i += 1 + body;
      }
      if (!next)
         return;
      ib = next;
      num_dw = next_dw;
   }
}

static void
ps4_record_submission(void *const *dcb, const uint32_t *dcb_sizes, unsigned num_ibs, uint64_t seq);

/* The memory the submission's shader user data point to (descriptor sets, push constants, vertex
 * buffer tables), and the 32-bit pointers inside that, one level more: mem-N.bin, records of
 * 'DMEM' va length + bytes, to find offline what a draw reaches through descriptors (Donkey Kong
 * Country: Tropical Freeze: a GPU write into a freed 720p image no command referred to). */
struct ps4_mem_record {
   uint32_t magic;
   uint32_t length;
   uint64_t va;
};

static void
ps4_collect_user_data(uint32_t *ib, unsigned num_dw, uint32_t *values, unsigned *count, unsigned max,
                      int depth)
{
   for (unsigned link = 0; link < 256 && num_dw; link++) {
      if (!ps4_mapped(ib, num_dw * 4ull))
         return;
      uint32_t *next = NULL;
      unsigned next_dw = 0;
      for (unsigned i = 0; i < num_dw;) {
         const uint32_t h = ib[i];
         if (h >> 30 == 2 || h == 0xffff1000u) { /* (and PKT3_NOP_PAD: a header-only NOP) */
            i++;
            continue;
         }
         if (h >> 30 != 3)
            return;
         const unsigned op = (h >> 8) & 0xff;
         const unsigned body = ((h >> 16) & 0x3fff) + 1;
         if (i + 1 + body > num_dw)
            return;
         uint32_t *b = &ib[i + 1];
         if (op == 0x76) { /* SET_SH_REG: values in pairs (a 64-bit address) and alone */
            for (unsigned k = 1; k < body && *count + 2 <= max; k++) {
               values[(*count)++] = b[k];
               values[(*count)++] = k + 1 < body ? b[k + 1] : 0;
            }
         } else if (op == 0x3f && body >= 3) {
            uint32_t *t = (uint32_t *)(uintptr_t)(b[0] | ((uint64_t)(b[1] & 0xffff) << 32));
            if (b[2] & (1u << 20)) {
               next = t;
               next_dw = b[2] & 0xfffff;
            } else if (depth < 3) {
               ps4_collect_user_data(t, b[2] & 0xfffff, values, count, max, depth + 1);
            }
         }
         if (next)
            break;
         i += 1 + body;
      }
      if (!next)
         return;
      ib = next;
      num_dw = next_dw;
   }
}

static ac_drm_device *ps4_last_device;
static struct amdgpu_bo *ps4_bo_at(ac_drm_device *dev, uint64_t va);

static void
ps4_record_descriptors(void *const *dcb, const uint32_t *dcb_sizes, unsigned num_ibs, uint64_t seq)
{
   ac_drm_device *dev = ps4_last_device;
   if (!dev || ps4_progress_fd < 0)
      return;
   static uint32_t values[8192];
   unsigned count = 0;
   for (unsigned i = 0; i < num_ibs; i++)
      ps4_collect_user_data(dcb[i], dcb_sizes[i] / 4, values, &count, ARRAY_SIZE(values), 0);
   char path[64];
   snprintf(path, sizeof(path), "%s/flight/mem-%u.bin", ps4_gpu_dir(), (unsigned)(seq % 8));
   const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
   if (fd < 0)
      return;
   /* pointers to dump: 64-bit pairs and 32-bit window pointers; then 32-bit pointers inside */
   static uint64_t done[2048];
   unsigned num_done = 0;
   static uint64_t queue[4096];
   unsigned queued = 0;
   for (unsigned i = 0; i + 1 < count && queued + 2 <= ARRAY_SIZE(queue); i += 2) {
      queue[queued++] = values[i] | ((uint64_t)(values[i + 1] & 0xffff) << 32);
      if (dev->va32_start)
         queue[queued++] = dev->va32_start + values[i];
   }
   const unsigned first_level = queued;
   simple_mtx_lock(&dev->lock);
   for (unsigned q = 0; q < queued && num_done < ARRAY_SIZE(done); q++) {
      const uint64_t ptr = queue[q] & ~3ull;
      if (ptr < 0x4000000000ull || ptr >= PS4_GPU_VA_LIMIT)
         continue;
      bool seen = false;
      for (unsigned d = 0; d < num_done && !seen; d++)
         seen = done[d] == ptr;
      if (seen)
         continue;
      struct amdgpu_bo *t = ps4_bo_at(dev, ptr);
      if (!t)
         continue;
      done[num_done++] = ptr;
      const uint32_t length = (uint32_t)MIN2(t->last_va + t->size - ptr, 1024);
      const struct ps4_mem_record r = {0x4d454d44, length, ptr};
      write(fd, &r, sizeof(r));
      write(fd, (const void *)(uintptr_t)ptr, length);
      /* one level more: 32-bit pointers inside (only from the first level) */
      if (q < first_level && dev->va32_start) {
         const uint32_t *d = (const uint32_t *)(uintptr_t)ptr;
         for (unsigned j = 0; j < length / 4 && queued < ARRAY_SIZE(queue); j++)
            if (d[j] && d[j] % 4 == 0)
               queue[queued++] = dev->va32_start + d[j];
      }
   }
   simple_mtx_unlock(&dev->lock);
   close(fd);
}

static void
ps4_record_submission(void *const *dcb, const uint32_t *dcb_sizes, unsigned num_ibs, uint64_t seq)
{
   if (ps4_progress_fd < 0)
      return;
   char path[64];
   snprintf(path, sizeof(path), "%s/flight/ib-%u.bin", ps4_gpu_dir(), (unsigned)(seq % 8));
   const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
   if (fd < 0)
      return;
   for (unsigned i = 0; i < num_ibs; i++)
      ps4_record_ib(fd, dcb[i], dcb_sizes[i] / 4, seq, i, 0);
   close(fd);
   ps4_record_descriptors(dcb, dcb_sizes, num_ibs, seq);
   ps4_progress("submit %llu -> ib-%u.bin\n", (unsigned long long)seq, (unsigned)(seq % 8));
}

static void
ps4_start_flight_recorder(ac_drm_device *dev)
{
   bool flight = false;
   ps4_trace_option(&flight);
   if (!flight || ps4_progress_fd >= 0)
      return;
   mkdir(ps4_gpu_dir(), 0777);
   mkdir(ps4_gpu_file("flight"), 0777);
   ps4_progress_fd = open(ps4_gpu_file("flight/progress.log"), O_WRONLY | O_CREAT | O_TRUNC, 0666);
   if (ps4_progress_fd < 0)
      return;
   pthread_t t;
   pthread_create(&t, NULL, ps4_progress_thread, dev);
   ps4_log("radv/ps4: flight recorder on (%s)\n", ps4_gpu_file("flight"));
}

static void
ps4_install_fault_capture(ac_drm_device *dev, int gnm_module)
{
   ps4_fault_dev = dev;
   void *valid = NULL;
   if (!sceKernelDlsym(gnm_module, "sceGnmIsCoredumpValid", &valid) && valid) {
      const uint8_t *const *slot = (const uint8_t *const *)((uintptr_t)valid - 0x690 + 0x10508);
      struct ps4_vq_info vq;
      memset(&vq, 0, sizeof(vq));
      if (!sceKernelVirtualQuery(slot, 0, &vq, sizeof(vq)) && *slot &&
          !sceKernelVirtualQuery(*slot, 0, &vq, sizeof(vq)) && !strncmp(vq.name, "SceGnmGpuInfo", 13))
         ps4_gpu_info = *slot;
      ps4_log("radv/ps4: SceGnmGpuInfo area %p ('%.32s')\n", (const void *)ps4_gpu_info, vq.name);
   }
   ps4_stop_info_cpu = ps4_find_symbol("sceCoredumpGetStopInfoCpu");
   ps4_stop_info_gpu = ps4_find_symbol("sceCoredumpGetStopInfoGpu");
   int (*reg)(int (*)(void *), size_t, void *) = ps4_find_symbol("sceCoredumpRegisterCoredumpHandler");
   const int rr = reg ? reg(ps4_coredump_handler, 0x10000, NULL) : -1;
   int (*add_exc)(void *, void *) = ps4_find_symbol("sceKernelAddGpuExceptionEvent");
   void *eq = NULL;
   int re = -1;
   if (add_exc && !sceKernelCreateEqueue(&eq, "radv gpu exception")) {
      re = add_exc(eq, NULL);
      if (!re) {
         pthread_t t;
         pthread_create(&t, NULL, ps4_gpu_exception_thread, eq);
      }
   }
   ps4_log("radv/ps4: fault capture: core dump handler %#x, GPU exception event %#x, stop info %s/%s\n",
           rr, re, ps4_stop_info_cpu ? "cpu" : "-", ps4_stop_info_gpu ? "gpu" : "-");
}

static void *
ps4_map_new(size_t size, int type, int prot, off_t *phys_out)
{
   off_t phys;
   size = align64(size, PS4_PAGE);
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, PS4_PAGE, type,
                                     &phys))
      return NULL;
   void *addr = NULL;
   if (sceKernelMapDirectMemory(&addr, size, prot, 0, phys, PS4_PAGE)) {
      sceKernelReleaseDirectMemory(phys, size);
      return NULL;
   }
   if (phys_out)
      *phys_out = phys;
   return addr;
}

/* Reserves a free range of the address space (kernel's choice, `align` aligned). */
static uint64_t
ps4_reserve_window(uint64_t size, uint64_t align)
{
   void *a = (void *)(uintptr_t)PS4_VA_HINT;
   int r = sceKernelReserveVirtualRange(&a, size, 0, align);
   if (r || !a || (uint64_t)(uintptr_t)a + size > PS4_GPU_VA_LIMIT) {
      ps4_log("radv/ps4: reserving %#llx bytes failed (%#x, %p)\n", (unsigned long long)size, r, a);
      return 0;
   }
   return (uint64_t)(uintptr_t)a;
}

static bool
ps4_reserve(uint64_t addr, uint64_t size)
{
   void *a = (void *)(uintptr_t)addr;
   return sceKernelReserveVirtualRange(&a, size, PS4_MAP_FIXED, 0) == 0 && a == (void *)(uintptr_t)addr;
}

/* Something other than our reservation inside [addr, addr + size) of a GPU VA window? (Then a
 * MAP_FIXED would silently replace someone else's memory, and their writes would land in ours.)
 * Logs the first few, returns true if found. */
static unsigned ps4_foreign_logged;
static bool ps4_vq_ok;
static bool
ps4_check_va_free(uint64_t addr, uint64_t size, const char *what)
{
   if (!ps4_vq_ok)
      return false;
   struct ps4_vq_info info;
   memset(&info, 0, sizeof(info));
   /* flags 1 = find next: the first mapping at or after addr */
   if (sceKernelVirtualQuery((void *)(uintptr_t)addr, 1, &info, sizeof(info)))
      return false;
   const uint64_t start = (uintptr_t)info.start, end = (uintptr_t)info.end;
   if (start >= addr + size || end <= addr)
      return false;
   if (!info.is_direct && !info.is_flexible && !info.is_committed && !info.is_stack)
      return false; /* a reservation */
   if (ps4_foreign_logged < 32) {
      ps4_foreign_logged++;
      ps4_log("radv/ps4: %s %#llx+%#llx: address range already holds a mapping [%#llx, %#llx) "
              "offset %#llx prot %#x type %d direct %u flexible %u stack %u committed %u '%.32s'\n",
              what, (unsigned long long)addr, (unsigned long long)size, (unsigned long long)start,
              (unsigned long long)end, (unsigned long long)info.offset, info.protection,
              info.memory_type, info.is_direct, info.is_flexible, info.is_stack, info.is_committed,
              info.name);
   }
   return true;
}

static struct util_sync_provider *ps4_sync_provider_init(ac_drm_device *dev);
static ac_drm_device *ps4_last_device;

/* One device for the whole process. RADV asks for a device for each winsys it makes (one per
 * VkInstance), and Eden makes a new instance for every program it boots: a device each time
 * reserved new GPU address windows (16 + 8 GiB) and kept the last one's buffers in quarantine
 * (up to 96 MiB of direct memory) forever. Now the first device is handed out again, and when
 * its last user goes, the GPU has finished everything and the quarantine is released. */
static pthread_mutex_t ps4_device_lock = PTHREAD_MUTEX_INITIALIZER;
static ac_drm_device *ps4_device;
static unsigned ps4_device_users;

/* Direct memory the GPU's buffers hold now, frees still waiting for the GPU included (Eden's
 * caches size their garbage collection from it: Vulkan::Device::GetDeviceMemoryUsage). */
uint64_t ac_ps4_gpu_bytes(void);
uint64_t
ac_ps4_gpu_bytes(void)
{
   ac_drm_device *dev = __atomic_load_n(&ps4_device, __ATOMIC_ACQUIRE);
   return dev ? __atomic_load_n(&dev->bo_bytes, __ATOMIC_RELAXED) : 0;
}
static bool ps4_jobs_idle(void);

/* Logs the first memory operations (bring-up). */
static unsigned ps4_verbose_left = 64;
#define PS4_VERBOSE(...) do { if (ps4_verbose_left) { ps4_verbose_left--; ps4_log(__VA_ARGS__); } } while (0)

int
ac_drm_device_initialize(int fd, bool is_virtio, uint32_t *major_version, uint32_t *minor_version,
                         ac_drm_device **out)
{
   (void)fd;
   (void)is_virtio;
   pthread_mutex_lock(&ps4_device_lock);
   if (ps4_device) {
      ps4_device_users++;
      pthread_mutex_unlock(&ps4_device_lock);
      ps4_log("radv/ps4: device in use again (%u users)\n", ps4_device_users);
      *major_version = 3;
      *minor_version = 61;
      *out = ps4_device;
      return 0;
   }
   ac_drm_device *dev = calloc(1, sizeof(*dev));
   if (!dev) {
      pthread_mutex_unlock(&ps4_device_lock);
      return -ENOMEM;
   }
   simple_mtx_init(&dev->lock, mtx_plain);

   if (!ps4_load_gnm(dev))
      goto fail;
   dev->va_start = ps4_reserve_window(PS4_VA_SIZE, 0x200000);
   /* The kernel rejects a 4 GiB alignment (EINVAL): reserve twice the size and use the 4 GiB
    * aligned half inside it (the rest stays reserved, unused). */
   const uint64_t va32_range = ps4_reserve_window(PS4_VA32_SIZE * 2, 0x200000);
   dev->va32_start = va32_range ? align64(va32_range, PS4_VA32_SIZE) : 0;
   if (!dev->va_start || !dev->va32_start)
      goto fail;
   ps4_log("radv/ps4: GPU address windows %#llx (16 GiB), %#llx (32-bit)\n",
           (unsigned long long)dev->va_start, (unsigned long long)dev->va32_start);
   util_vma_heap_init(&dev->va_heap, dev->va_start, PS4_VA_SIZE);
   util_vma_heap_init(&dev->va32_heap, dev->va32_start, PS4_VA32_SIZE);

   off_t page_phys = -1;
   uint8_t *page =
      ps4_map_new(PS4_PAGE * 4, PS4_WB_ONION, PS4_PROT_CPU_RW | PS4_PROT_GPU_RW, &page_phys);
   if (!page)
      goto fail;
   {
      /* sceKernelVirtualQuery's struct is declared by hand: only trust it if it describes this
       * known mapping correctly. */
      struct ps4_vq_info vq;
      memset(&vq, 0, sizeof(vq));
      const int r = sceKernelVirtualQuery(page + 64, 0, &vq, sizeof(vq));
      ps4_vq_ok = r == 0 && vq.start == page && vq.end == page + PS4_PAGE * 4 && vq.is_direct &&
                  vq.offset == page_phys;
      ps4_log("radv/ps4: virtual query check %s (%#x: [%p, %p) offset %#llx direct %u, expected "
              "[%p, +%#llx) offset %#llx)\n", ps4_vq_ok ? "ok" : "FAILED - mapping checks off", r,
              vq.start, vq.end, (unsigned long long)vq.offset, vq.is_direct, page,
              (unsigned long long)(PS4_PAGE * 4), (unsigned long long)page_phys);
   }
   memset(page, 0, PS4_PAGE * 4);
   dev->fence = (volatile uint64_t *)page;
   if (dev->add_eq_event) {
      void *eq = NULL;
      int r = sceKernelCreateEqueue(&eq, "radv eop");
      int a = r == 0 ? dev->add_eq_event(eq, 0x40 /* GFX EOP */, NULL) : -1;
      ps4_log("radv/ps4: EOP event queue %#x, sceGnmAddEqEvent %#x\n", r, a);
      if (r == 0 && a == 0)
         dev->eop_queue = eq;
   }
   dev->fence_ibs = (uint32_t *)(page + 256);
   dev->flip_ibs = ps4_map_new(PS4_PAGE, PS4_WB_ONION, PS4_PROT_CPU_RW | PS4_PROT_GPU_RW, NULL);
   {
      /* Timestamps: [0, 4 KiB) start, [4, 8 KiB) end, then the start IBs (32 bytes each). */
      STATIC_ASSERT(FENCE_IB_COUNT * 8 * 2 + FENCE_IB_COUNT * TS_IB_DW * 4 <= PS4_PAGE * 2);
      uint8_t *ts = ps4_map_new(PS4_PAGE * 2, PS4_WB_ONION, PS4_PROT_CPU_RW | PS4_PROT_GPU_RW, NULL);
      if (ts) {
         memset(ts, 0, PS4_PAGE * 2);
         dev->ts_start = (volatile uint64_t *)ts;
         dev->ts_end = (volatile uint64_t *)(ts + FENCE_IB_COUNT * 8);
         dev->ts_ibs = (uint32_t *)(ts + FENCE_IB_COUNT * 16);
      }
   }
   STATIC_ASSERT(256 + FENCE_IB_COUNT * FENCE_IB_DW * 4 <= PS4_PAGE * 4);

   ps4_sync_provider_init(dev);

   /* Report a recent amdgpu kernel (3.61) so that Mesa enables its normal paths. */
   *major_version = 3;
   *minor_version = 61;
   *out = dev;
   ps4_last_device = dev;
   if (!ps4_fault_dev)
      ps4_install_fault_capture(dev, dev->gnm_module);
   ps4_fault_dev = dev; /* the last device created is the one in use */
   ps4_start_flight_recorder(dev);
   ps4_start_stall_thread(dev);
   ps4_device = dev;
   ps4_device_users = 1;
   pthread_mutex_unlock(&ps4_device_lock);
   ps4_log("radv/ps4: device initialized\n");
   return 0;

fail:
   pthread_mutex_unlock(&ps4_device_lock);
   free(dev);
   return -ENODEV;
}

struct util_sync_provider *
ac_drm_device_get_sync_provider(ac_drm_device *dev)
{
   return &dev->p;
}

uintptr_t
ac_drm_device_get_cookie(ac_drm_device *dev)
{
   return (uintptr_t)dev;
}

void
ac_drm_device_deinitialize(ac_drm_device *dev)
{
   /* The device lives for the whole process; when its last user goes (a program's renderer),
    * the buffers it left waiting are released once the GPU has finished with them. (Under the
    * lock: a new user waits until that's done.) */
   pthread_mutex_lock(&ps4_device_lock);
   if (dev != ps4_device || !ps4_device_users || --ps4_device_users) {
      pthread_mutex_unlock(&ps4_device_lock);
      return;
   }
   const int64_t deadline = os_time_get_nano() + 2000000000ll;
   while (!ps4_jobs_idle() && os_time_get_nano() < deadline)
      usleep(1000);
   const bool idle = ps4_jobs_idle() && ps4_wait_seq(dev, dev->last_seq, deadline);
   char line[200];
   if (idle) {
      simple_mtx_lock(&dev->lock);
      const unsigned waiting = dev->num_deferred;
      ps4_run_deferred_ex(dev, 0);
      simple_mtx_unlock(&dev->lock);
      snprintf(line, sizeof(line), "radv/ps4: no users left: %u released buffers freed, %llu MiB in %llu "
               "buffers still allocated", waiting, (unsigned long long)(dev->bo_bytes >> 20),
               (unsigned long long)dev->bo_count);
   } else {
      /* (the GPU may still write to them: they stay until it's done, as before) */
      snprintf(line, sizeof(line), "radv/ps4: no users left, but the GPU is still busy (submission %llu "
               "of %llu): the released buffers stay", (unsigned long long)ps4_completed(dev),
               (unsigned long long)dev->last_seq);
   }
   pthread_mutex_unlock(&ps4_device_lock);
   ps4_log("%s\n", line);
   ps4_boot_trace(line);
}

int
ac_drm_device_get_fd(ac_drm_device *dev)
{
   (void)dev;
   return -1;
}

/* ---------------------------------------------------------------------------------------------
 * Buffers and address space
 */

static struct amdgpu_bo *
ps4_bo(ac_drm_device *dev, uint32_t handle)
{
   const unsigned num = __atomic_load_n(&dev->num_bos, __ATOMIC_ACQUIRE);
   struct amdgpu_bo **bos = __atomic_load_n(&dev->bos, __ATOMIC_ACQUIRE);
   return handle && handle < num ? __atomic_load_n(&bos[handle], __ATOMIC_ACQUIRE) : NULL;
}

/* Gives a new buffer its handle. */
static void
ps4_register_bo(ac_drm_device *dev, struct amdgpu_bo *bo)
{
   simple_mtx_lock(&dev->lock);
   if (dev->num_bos + 1 >= dev->bos_cap) {
      const unsigned cap = MAX2(dev->bos_cap * 2, 1024);
      struct amdgpu_bo **bigger = calloc(cap, sizeof(*bigger));
      if (dev->num_bos)
         memcpy(bigger, dev->bos, dev->num_bos * sizeof(*bigger));
      /* The old table is deliberately kept (see struct ac_drm_device). */
      __atomic_store_n(&dev->bos, bigger, __ATOMIC_RELEASE);
      dev->bos_cap = cap;
   }
   if (dev->num_bos == 0)
      dev->num_bos = 1; /* handle 0 is invalid */
   bo->handle = dev->num_bos;
   dev->bos[bo->handle] = bo;
   __atomic_store_n(&dev->num_bos, bo->handle + 1, __ATOMIC_RELEASE);
   simple_mtx_unlock(&dev->lock);
}

int
ac_drm_bo_alloc(ac_drm_device *dev, struct amdgpu_bo_alloc_request *req, ac_drm_bo *out)
{
   struct amdgpu_bo *bo = calloc(1, sizeof(*bo));
   if (!bo)
      return -ENOMEM;
   bo->size = align64(MAX2(req->alloc_size, 1), PS4_PAGE);
   bo->heap = req->preferred_heap;
   bo->flags = req->flags;
   bo->pad_phys = -1;

   /* VRAM: write-combined garlic (the GPU's fast path); GTT: CPU-cached onion, coherent. */
   const int type = (req->preferred_heap & AMDGPU_GEM_DOMAIN_VRAM) ||
                          (req->flags & AMDGPU_GEM_CREATE_CPU_GTT_USWC)
                       ? PS4_WC_GARLIC
                       : PS4_WB_ONION;
   bo->write_combined = type == PS4_WC_GARLIC;
   const uint64_t align = MAX2(util_next_power_of_two64(MAX2(req->phys_alignment, 1)), PS4_PAGE);
   simple_mtx_lock(&dev->lock);
   ps4_run_deferred(dev);
   simple_mtx_unlock(&dev->lock);
   int r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bo->size + PS4_TAIL,
                                         align, type, &bo->phys);
   if (r && dev->num_deferred) {
      /* Memory still waiting for the GPU (or in quarantine): let it finish, then release the
       * quarantine's older half at a time (the newest stays protected longest) until it fits. */
      ps4_wait_seq(dev, dev->last_seq, INT64_MAX);
      for (uint64_t keep = PS4_QUARANTINE_BYTES / 2; r; keep /= 2) {
         simple_mtx_lock(&dev->lock);
         ps4_run_deferred_ex(dev, keep);
         simple_mtx_unlock(&dev->lock);
         r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bo->size + PS4_TAIL,
                                           align, type, &bo->phys);
         if (!keep)
            break;
      }
      static unsigned logged;
      if (logged++ < 8)
         ps4_log("radv/ps4: %llu bytes did not fit: quarantine released (%s)\n",
                 (unsigned long long)bo->size, r ? "still no memory" : "fits now");
   }
   if (r) {
      ps4_log("radv/ps4: allocating %llu bytes of direct memory failed (%#x)\n",
              (unsigned long long)bo->size, r);
      free(bo);
      return -ENOMEM;
   }

   ps4_register_bo(dev, bo);
   if (bo->size >= (4ull << 20)) { /* (04.41) the big ones in the boot trace */
      char line[160];
      snprintf(line, sizeof(line), "radv/ps4: GPU buffer %u: %llu KiB (%s), phys %#llx; %llu MiB in all", bo->handle,
               (unsigned long long)(bo->size >> 10), bo->write_combined ? "garlic" : "onion",
               (unsigned long long)bo->phys, (unsigned long long)((dev->bo_bytes + bo->size) >> 20));
      ps4_boot_trace(line);
   }

   out->abo = bo;
   __atomic_add_fetch(&dev->bo_bytes, bo->size, __ATOMIC_RELAXED);
   __atomic_add_fetch(&dev->bo_count, 1, __ATOMIC_RELAXED);
   PS4_VERBOSE("radv/ps4: bo %u: %llu bytes, heap %#x, flags %#llx, align %#llx, phys %#llx\n",
               bo->handle, (unsigned long long)bo->size, bo->heap, (unsigned long long)bo->flags,
               (unsigned long long)align, (unsigned long long)bo->phys);
   return 0;
}

int
ac_drm_bo_free(ac_drm_device *dev, ac_drm_bo bo)
{
   struct amdgpu_bo *b = bo.abo;
   if (!b)
      return 0;
   ps4_defer(dev, (struct ps4_deferred){.op = 'F', .bo = b});
   return 0;
}

/* A deferred free (dev->lock held). */
static void
ps4_do_free(ac_drm_device *dev, struct amdgpu_bo *b)
{
   if (b->user_mem) {
      /* the app's memory stays the app's: only the handle goes */
      ps4_record(dev, 'F', b, b->last_va, b->size);
      dev->bos[b->handle] = NULL;
      free(b);
      return;
   }
   if (b->size >= (4ull << 20)) { /* (04.41) the big ones in the boot trace */
      char line[120];
      snprintf(line, sizeof(line), "radv/ps4: GPU buffer %u freed (%llu KiB)", b->handle,
               (unsigned long long)(b->size >> 10));
      ps4_boot_trace(line);
   }
   if (b->cpu_owned)
      sceKernelMunmap(b->cpu, b->size);
   const int rr = sceKernelReleaseDirectMemory(b->phys, b->size + PS4_TAIL);
   if (rr) {
      static unsigned logged;
      if (logged++ < 16)
         ps4_log("radv/ps4: releasing bo %u (phys %#llx, %#llx bytes) failed (%#x)\n", b->handle,
                 (unsigned long long)b->phys, (unsigned long long)b->size, rr);
   }
   __atomic_sub_fetch(&dev->bo_bytes, b->size, __ATOMIC_RELAXED);
   __atomic_sub_fetch(&dev->bo_count, 1, __ATOMIC_RELAXED);
   if (b->pad_phys >= 0)
      sceKernelReleaseDirectMemory(b->pad_phys, PS4_PAGE);
   ps4_record(dev, 'F', b, b->last_va, b->size);
   dev->bos[b->handle] = NULL;
   free(b);
}

int
ac_drm_bo_export(ac_drm_device *dev, ac_drm_bo bo, enum amdgpu_bo_handle_type type,
                 uint32_t *shared_handle)
{
   (void)dev;
   if (type != amdgpu_bo_handle_type_kms && type != amdgpu_bo_handle_type_kms_noimport)
      return -ENOSYS;
   *shared_handle = bo.abo->handle;
   return 0;
}

int
ac_drm_bo_import(ac_drm_device *dev, enum amdgpu_bo_handle_type type, uint32_t shared_handle,
                 struct ac_drm_bo_import_result *output)
{
   return -ENOSYS;
}

/* User memory (VK_EXT_external_memory_host): the app's own direct memory, which it has already
 * made GPU-readable (Crema: the Wii U's memory, MemMapper::CremaSetGpuReadable). GPU addresses
 * are CPU addresses here and direct memory can't be mapped a second time, so the buffer's GPU
 * address must be the memory's own: RADV's next va_range_alloc on this thread
 * (radv_amdgpu_winsys_bo_from_ptr: create, va_range_alloc, export, MAP - in that order) is
 * answered with it, and the MAP, UNMAP and free leave the memory alone. */
static __thread uint64_t ps4_pending_user_va, ps4_pending_user_size;

int
ac_drm_create_bo_from_user_mem(ac_drm_device *dev, void *cpu, uint64_t size, ac_drm_bo *bo)
{
   const uint64_t addr = (uintptr_t)cpu;
   const uint64_t aligned = align64(size, PS4_PAGE);
   const bool in_window = (addr < dev->va_start + PS4_VA_SIZE && addr + aligned > dev->va_start) ||
                          (addr < dev->va32_start + PS4_VA32_SIZE * 2 && addr + aligned > dev->va32_start);
   if (!size || (addr % PS4_PAGE) || addr + aligned > PS4_GPU_VA_LIMIT || in_window) {
      ps4_log("radv/ps4: user memory %#llx (+%#llx) refused: %s\n", (unsigned long long)addr,
              (unsigned long long)size,
              !size || (addr % PS4_PAGE) ? "not page aligned"
              : in_window                ? "inside the GPU VA window"
                                         : "beyond the GPU's address range");
      return -EINVAL;
   }
   struct amdgpu_bo *b = calloc(1, sizeof(*b));
   if (!b)
      return -ENOMEM;
   b->size = aligned;
   b->phys = -1;
   b->pad_phys = -1;
   b->heap = AMDGPU_GEM_DOMAIN_GTT;
   b->cpu = cpu;
   b->last_va = addr;
   b->user_mem = true;
   ps4_register_bo(dev, b);
   bo->abo = b;
   ps4_pending_user_va = addr;
   ps4_pending_user_size = size;
   ps4_record(dev, 'A', b, addr, aligned);
   ps4_log("radv/ps4: bo %u: user memory at %#llx (%#llx bytes)\n", b->handle,
           (unsigned long long)addr, (unsigned long long)aligned);
   return 0;
}

int
ac_drm_bo_cpu_map(ac_drm_device *dev, ac_drm_bo bo, void **cpu)
{
   struct amdgpu_bo *b = bo.abo;
   if (!b->cpu) {
      void *addr = NULL;
      if (sceKernelMapDirectMemory(&addr, b->size, PS4_PROT_CPU_RW | PS4_PROT_GPU_RW, 0, b->phys,
                                   PS4_PAGE)) {
         ps4_log("radv/ps4: cpu map of bo %u failed\n", b->handle);
         return -ENOMEM;
      }
      if (b->write_combined)
         ps4_flush_cpu_cache(addr, b->size);
      b->cpu = addr;
      b->cpu_owned = true;
   }
   *cpu = b->cpu;
   return 0;
}

int
ac_drm_bo_cpu_unmap(ac_drm_device *dev, ac_drm_bo bo)
{
   return 0;
}

int
ac_drm_bo_set_metadata(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_metadata *info)
{
   return 0;
}

int
ac_drm_bo_query_info(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_info *info)
{
   struct amdgpu_bo *b = ps4_bo(dev, bo_handle);
   if (!b)
      return -EINVAL;
   memset(info, 0, sizeof(*info));
   info->alloc_size = b->size;
   info->phys_alignment = PS4_PAGE;
   info->preferred_heap = b->heap;
   info->alloc_flags = b->flags;
   return 0;
}

int
ac_drm_bo_wait_for_idle(ac_drm_device *dev, ac_drm_bo bo, uint64_t timeout_ns, bool *busy)
{
   /* No per-buffer tracking: wait for everything submitted so far. */
   *busy = !ps4_wait_seq(dev, dev->last_seq, os_time_get_absolute_timeout(timeout_ns));
   return 0;
}

/* Was anything written past the end of the buffer? `full`: look at the whole tail, else at the
 * first 64 bytes of each 4 KiB of it. Logs a buffer once. Called with dev->lock held. */
static void
ps4_check_tail(ac_drm_device *dev, struct amdgpu_bo *b, const char *when)
{
   if (!b->tail_mapped || b->tail_reported)
      return;
   const volatile uint32_t *t = (const volatile uint32_t *)(uintptr_t)(b->last_va + b->size);
   bool dirty = false;
   for (unsigned page = 0; page < PS4_TAIL / 4096 && !dirty; page++)
      for (unsigned i = 0; i < 16 && !dirty; i++)
         dirty = t[page * 1024 + i] != 0;
   if (!dirty)
      return;
   b->tail_reported = true;
   unsigned count = 0, first = ~0u, last = 0;
   char sample[512];
   int len = 0;
   for (unsigned i = 0; i < PS4_TAIL / 4; i++) {
      const uint32_t v = t[i];
      if (!v)
         continue;
      if (count < 24 && len < (int)sizeof(sample) - 24)
         len += snprintf(sample + len, sizeof(sample) - len, " +%#x:%08x", i * 4, v);
      count++;
      first = MIN2(first, i * 4);
      last = i * 4;
   }
   ps4_log("radv/ps4: WRITE PAST THE END of bo %u (%s): va %#llx size %#llx heap %#x flags %#llx "
           "phys %#llx, after submission %llu (GPU completed %llu): %u dwords written at "
           "+%#x..+%#x past the end:%s\n",
           b->handle, when, (unsigned long long)b->last_va, (unsigned long long)b->size, b->heap,
           (unsigned long long)b->flags, (unsigned long long)b->phys,
           (unsigned long long)dev->last_seq, (unsigned long long)ps4_completed(dev), count, first,
           last, sample);
}

/* All live buffers' tails (every 256 submissions; dev->lock held). */
static void
ps4_scan_tails(ac_drm_device *dev)
{
   for (unsigned h = 1; h < dev->num_bos; h++) {
      struct amdgpu_bo *b = dev->bos[h];
      if (b)
         ps4_check_tail(dev, b, "live");
   }
}

static int
ps4_va_op(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr,
          uint64_t flags, uint32_t ops)
{
   const uint64_t exact_size = size;
   size = align64(size, PS4_PAGE);
   if (flags & AMDGPU_VM_PAGE_PRT) {
      ps4_log("radv/ps4: sparse (PRT) mapping requested - unsupported\n");
      return -ENOSYS; /* no sparse residency */
   }

   switch (ops) {
   case AMDGPU_VA_OP_MAP: {
      struct amdgpu_bo *b = ps4_bo(dev, bo_handle);
      if (!b || offset + size > b->size) {
         ps4_log("radv/ps4: map of bo %u (+%#llx, %#llx bytes) out of range\n", bo_handle,
                 (unsigned long long)offset, (unsigned long long)size);
         return -EINVAL;
      }
      if (b->user_mem) {
         /* already mapped (by the app, GPU-readable): only its own address is right */
         if (offset == 0 && addr == (uintptr_t)b->cpu) {
            ps4_record(dev, 'M', b, addr, size);
            return 0;
         }
         ps4_log("radv/ps4: user memory bo %u mapped at %#llx (+%#llx) instead of %p - refused\n",
                 bo_handle, (unsigned long long)addr, (unsigned long long)offset, b->cpu);
         return -EINVAL;
      }
      void *a = (void *)(uintptr_t)addr;
      const uint64_t cpu = (uintptr_t)b->cpu;
      if (b->cpu && addr != cpu) {
         /* A second mapping of the same memory: RADEON_FLAG_VM_PAD_1PAGE maps the first page
          * again after the buffer, so that prefetches past the end hit valid memory. The PS4
          * can't map direct memory twice (EBUSY). Inside the buffer's own (16 KiB rounded)
          * mapping it's already backed; past it, back it with a page of its own. */
         /* Starting inside the buffer's mapping: the pad page RADV wants (4 KiB) is covered;
          * the length has been rounded to the 16 KiB system page by then (getpagesize). */
         (void)exact_size;
         if (addr >= cpu && addr < cpu + b->size + (b->tail_mapped ? PS4_TAIL : 0))
            return 0;
         if (addr != cpu + b->size || size != PS4_PAGE || b->pad_phys >= 0) {
            ps4_log("radv/ps4: unsupported alias mapping of bo %u at %#llx (+%#llx, %#llx bytes)\n",
                    b->handle, (unsigned long long)addr, (unsigned long long)offset,
                    (unsigned long long)size);
            return -ENOSYS;
         }
         off_t pad;
         if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), PS4_PAGE, PS4_PAGE,
                                           PS4_WB_ONION, &pad))
            return -ENOMEM;
         if (sceKernelMapDirectMemory(&a, PS4_PAGE, PS4_PROT_CPU_RW | PS4_PROT_GPU_RW,
                                      PS4_MAP_FIXED, pad, PS4_PAGE) ||
             a != (void *)(uintptr_t)addr) {
            sceKernelReleaseDirectMemory(pad, PS4_PAGE);
            return -ENOMEM;
         }
         b->pad_phys = pad;
         return 0;
      }
      /* The whole buffer: map its overflow tail too. */
      const bool whole = offset == 0 && size == b->size && !b->cpu;
      const uint64_t map_size = whole ? size + PS4_TAIL : size;
      ps4_check_va_free(addr, map_size, "map");
      /* GPU read-only when RADV asks for it (command buffers), as amdgpu enforces on Linux: there
       * a stray GPU write into one is dropped, here it would land. */
      const bool gpu_ro = flags && !(flags & AMDGPU_VM_PAGE_WRITEABLE);
      if (gpu_ro) {
         static unsigned logged;
         if (logged++ < 3)
            ps4_log("radv/ps4: bo %u mapped GPU read-only at %#llx\n", b->handle,
                    (unsigned long long)addr);
      }
      int r = sceKernelMapDirectMemory(&a, map_size,
                                       PS4_PROT_CPU_RW | (gpu_ro ? PS4_PROT_GPU_READ : PS4_PROT_GPU_RW),
                                       PS4_MAP_FIXED, b->phys + offset, PS4_PAGE);
      /* write-combined memory: no stale cached lines of an earlier owner (see ps4_flush_cpu_cache) */
      if (!r && whole && a == (void *)(uintptr_t)addr && b->write_combined)
         ps4_flush_cpu_cache(a, map_size);
      if (!r && whole && a == (void *)(uintptr_t)addr) {
         memset((uint8_t *)a + size, 0, PS4_TAIL);
         b->tail_mapped = true;
      }
      if (r || a != (void *)(uintptr_t)addr) {
         ps4_log("radv/ps4: mapping %#llx+%#llx at %#llx failed (%#x)\n",
                 (unsigned long long)b->phys, (unsigned long long)offset,
                 (unsigned long long)addr, r);
         return -ENOMEM;
      }
      if (offset == 0 && !b->cpu)
         b->cpu = a;
      if (offset == 0) {
         b->last_va = addr;
         b->gpu_ro = gpu_ro;
      }
      ps4_record(dev, 'M', b, addr, size);
      PS4_VERBOSE("radv/ps4: map bo %u +%#llx at %#llx (%#llx bytes)\n", b->handle,
                  (unsigned long long)offset, (unsigned long long)addr, (unsigned long long)size);
      return 0;
   }
   case AMDGPU_VA_OP_UNMAP:
   case AMDGPU_VA_OP_CLEAR: {
      struct amdgpu_bo *ub = ps4_bo(dev, bo_handle);
      if (ub && ub->user_mem) {
         /* never munmap the app's memory (ps4_do_unmap would, and reserve the hole) */
         ps4_record(dev, 'U', ub, addr, size);
         return 0;
      }
      ps4_defer(dev, (struct ps4_deferred){.op = 'U', .bo = ps4_bo(dev, bo_handle),
                                           .handle = bo_handle, .addr = addr, .size = size});
      return 0;
   }
   default:
      ps4_log("radv/ps4: va op %u unsupported\n", ops);
      return -ENOSYS;
   }
}

/* A deferred unmap (dev->lock held). */
static int
ps4_do_unmap(ac_drm_device *dev, struct amdgpu_bo *b, uint32_t bo_handle, uint64_t addr,
             uint64_t size)
{
   {
      if (b && b->tail_mapped && addr == b->last_va) {
         ps4_check_tail(dev, b, "unmapped");
         b->tail_mapped = false;
         size = MAX2(size, b->size + PS4_TAIL);
      }
      if (b && b->cpu == (void *)(uintptr_t)addr && !b->cpu_owned)
         b->cpu = NULL;
      if (b && addr == b->last_va)
         ps4_note_released(b->handle, addr, b->size, dev->last_seq);
      ps4_record(dev, 'U', b, addr, size);
      /* Unmap explicitly, then put the reservation back so the window stays ours. (Reserving
       * over a live mapping isn't guaranteed to replace it; a mapping left behind would vanish
       * when its direct memory is released, leaving a hole the kernel hands to other mappings
       * - which a later buffer at this address would then silently overwrite.) */
      const int ru = sceKernelMunmap((void *)(uintptr_t)addr, size);
      if (!ps4_reserve(addr, size)) {
         static unsigned logged;
         if (logged++ < 16)
            ps4_log("radv/ps4: unmap of bo %u at %#llx (%#llx bytes): munmap %#x, re-reserving "
                    "failed\n", bo_handle, (unsigned long long)addr, (unsigned long long)size, ru);
         return -EINVAL;
      }
      return 0;
   }
}

int
ac_drm_bo_va_op(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size,
                uint64_t addr, uint64_t flags, uint32_t ops)
{
   return ps4_va_op(dev, bo_handle, offset, size, addr, 0, ops);
}

int
ac_drm_bo_va_op_raw(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size,
                    uint64_t addr, uint64_t flags, uint32_t ops)
{
   return ps4_va_op(dev, bo_handle, offset, size, addr, flags, ops);
}

static int ps4_timeline_signal(struct util_sync_provider *p, const uint32_t *handles,
                               uint64_t *points, uint32_t count);

int
ac_drm_bo_va_op_raw2(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size,
                     uint64_t addr, uint64_t flags, uint32_t ops, uint32_t vm_timeline_syncobj_out,
                     uint64_t vm_timeline_point, uint64_t input_fence_syncobj_handles,
                     uint32_t num_syncobj_handles)
{
   int r = ps4_va_op(dev, bo_handle, offset, size, addr, flags, ops);
   /* Mapping is synchronous: signal the VM timeline point right away. */
   if (!r && vm_timeline_syncobj_out)
      ps4_timeline_signal(&dev->p, &vm_timeline_syncobj_out, &vm_timeline_point, 1);
   return r;
}

static void ps4_va_release_held(unsigned w, uint64_t keep);

int
ac_drm_va_range_alloc(ac_drm_device *dev, enum amdgpu_gpu_va_range va_range_type, uint64_t size,
                      uint64_t va_base_alignment, uint64_t va_base_required,
                      uint64_t *va_base_allocated, amdgpu_va_handle *va_range_handle,
                      uint64_t flags)
{
   if (ps4_pending_user_va) {
      /* the address of the user memory just created on this thread (ac_drm_create_bo_from_user_mem) */
      const uint64_t user_va = ps4_pending_user_va, user_size = ps4_pending_user_size;
      ps4_pending_user_va = 0;
      if (size == user_size && !va_base_required && !(flags & AMDGPU_VA_RANGE_32_BIT)) {
         *va_base_allocated = user_va;
         *va_range_handle = NULL; /* ac_drm_va_range_free(NULL): nothing to give back */
         return 0;
      }
   }
   struct util_vma_heap *heap = flags & AMDGPU_VA_RANGE_32_BIT ? &dev->va32_heap : &dev->va_heap;
   size = align64(size, PS4_PAGE) + PS4_TAIL;
   const uint64_t align = MAX2(util_next_power_of_two64(MAX2(va_base_alignment, 1)), PS4_PAGE);

   simple_mtx_lock(&ps4_va_lock);
   uint64_t addr = 0;
   for (int attempt = 0; attempt < 2 && !addr; attempt++) {
      if (attempt) /* no room: the held-back ranges of this window go back first (ps4_do_va_free) */
         ps4_va_release_held(heap == &dev->va32_heap ? 1 : 0, 0);
      if (va_base_required) {
         if (util_vma_heap_alloc_addr(heap, va_base_required, size))
            addr = va_base_required;
      } else {
         addr = util_vma_heap_alloc(heap, size, align);
      }
   }
   simple_mtx_unlock(&ps4_va_lock);
   if (!addr) {
      ps4_log("radv/ps4: va_range_alloc(%#llx, align %#llx, required %#llx, flags %#llx) failed\n",
              (unsigned long long)size, (unsigned long long)align,
              (unsigned long long)va_base_required, (unsigned long long)flags);
      return -ENOMEM;
   }
   PS4_VERBOSE("radv/ps4: va %#llx + %#llx (flags %#llx)\n", (unsigned long long)addr,
               (unsigned long long)size, (unsigned long long)flags);

   struct amdgpu_va *va = malloc(sizeof(*va));
   va->dev = dev;
   va->heap = heap;
   va->addr = addr;
   va->size = size;
   *va_base_allocated = addr;
   *va_range_handle = va;
   return 0;
}

int
ac_drm_va_range_free(amdgpu_va_handle va)
{
   if (!va)
      return 0;
   ps4_defer(va->dev, (struct ps4_deferred){.op = 'V', .va = va});
   return 0;
}

/* Released address ranges are held back before they are handed out again: up to PS4_VA_HOLD of
 * the 16 GiB window and PS4_VA32_HOLD of the 32-bit one, oldest returned first (all of a window's
 * when an allocation finds no room). Cemu uses resources through stale descriptors; when the
 * range had already gone to a new buffer, a stale write landed in that buffer - Donkey Kong
 * Country: Tropical Freeze (1.87) wrote into a command buffer (mapped GPU read-only: a fault that
 * ends the app) at the address of a 720p image freed shortly before. Held back, the range stays a
 * hole, which ps4_user_data_stale recognises (and the draw is skipped). */
#define PS4_VA_HOLD (4ull << 30)
#define PS4_VA32_HOLD (512ull << 20)
static struct {
   amdgpu_va_handle *items;
   unsigned head, count, cap;
   uint64_t bytes;
} ps4_va_held[2]; /* 0: the 16 GiB window, 1: the 32-bit one */

/* Returns the oldest held ranges of a window until at most `keep` bytes are held (ps4_va_lock held). */
static void
ps4_va_release_held(unsigned w, uint64_t keep)
{
   while (ps4_va_held[w].count && ps4_va_held[w].bytes > keep) {
      amdgpu_va_handle va = ps4_va_held[w].items[ps4_va_held[w].head];
      ps4_va_held[w].head = (ps4_va_held[w].head + 1) % ps4_va_held[w].cap;
      ps4_va_held[w].count--;
      ps4_va_held[w].bytes -= va->size;
      util_vma_heap_free(va->heap, va->addr, va->size);
      free(va);
   }
}

/* A deferred address range free. */
static void
ps4_do_va_free(amdgpu_va_handle va)
{
   simple_mtx_lock(&ps4_va_lock);
   const unsigned w = va->heap == &va->dev->va32_heap ? 1 : 0;
   if (ps4_va_held[w].count == ps4_va_held[w].cap) {
      /* grow the ring, keeping its order */
      const unsigned cap = MAX2(ps4_va_held[w].cap * 2, 256);
      amdgpu_va_handle *items = malloc(cap * sizeof(*items));
      if (!items) {
         util_vma_heap_free(va->heap, va->addr, va->size);
         simple_mtx_unlock(&ps4_va_lock);
         free(va);
         return;
      }
      for (unsigned i = 0; i < ps4_va_held[w].count; i++)
         items[i] = ps4_va_held[w].items[(ps4_va_held[w].head + i) % ps4_va_held[w].cap];
      free(ps4_va_held[w].items);
      ps4_va_held[w].items = items;
      ps4_va_held[w].head = 0;
      ps4_va_held[w].cap = cap;
   }
   ps4_va_held[w].items[(ps4_va_held[w].head + ps4_va_held[w].count) % ps4_va_held[w].cap] = va;
   ps4_va_held[w].count++;
   ps4_va_held[w].bytes += va->size;
   ps4_va_release_held(w, w ? PS4_VA32_HOLD : PS4_VA_HOLD);
   simple_mtx_unlock(&ps4_va_lock);
}

/* Runs the deferred operations whose submissions have completed, in order. dev->lock held. */
static void
ps4_run_deferred_ex(ac_drm_device *dev, uint64_t keep)
{
   /* Quarantine: buffers the GPU is done with stay mapped (and their address ranges allocated)
    * until more than PS4_QUARANTINE_BYTES wait, oldest released first. On Linux and Windows a
    * GPU write to an unmapped address is dropped and the app goes on; on the PS4 it is a GPU page
    * fault that ends the app, and Cemu does write through stale descriptors: Donkey Kong Country:
    * Tropical Freeze (1.84) wrote into a buffer freed ~500 submissions (8 s) earlier at its save
    * menu. Such writes now land in the old buffer's own memory. (A shared "sink" page can't back
    * the freed ranges instead: the PS4 maps direct memory only once.) */
   const uint64_t done = ps4_completed(dev);
   unsigned ready = 0;
   uint64_t held = 0;
   for (; ready < dev->num_deferred && dev->deferred[ready].seq <= done; ready++)
      if (dev->deferred[ready].op == 'F' && dev->deferred[ready].bo)
         held += dev->deferred[ready].bo->size;
   unsigned n = 0;
   for (; n < ready && (held > keep || !keep); n++) {
      struct ps4_deferred *d = &dev->deferred[n];
      switch (d->op) {
      case 'U':
         ps4_do_unmap(dev, d->bo, d->handle, d->addr, d->size);
         break;
      case 'F':
         if (d->bo)
            held -= MIN2(held, d->bo->size);
         ps4_do_free(dev, d->bo);
         break;
      case 'V':
         ps4_do_va_free(d->va);
         break;
      }
   }
   if (n) {
      dev->num_deferred -= n;
      memmove(dev->deferred, dev->deferred + n, dev->num_deferred * sizeof(*dev->deferred));
   }
}

static void
ps4_run_deferred(ac_drm_device *dev)
{
   ps4_run_deferred_ex(dev, PS4_QUARANTINE_BYTES);
}

static void
ps4_defer(ac_drm_device *dev, struct ps4_deferred d)
{
   simple_mtx_lock(&dev->lock);
   d.seq = dev->last_seq;
   if (dev->num_deferred == dev->deferred_cap) {
      dev->deferred_cap = MAX2(dev->deferred_cap * 2, 256);
      dev->deferred = realloc(dev->deferred, dev->deferred_cap * sizeof(*dev->deferred));
   }
   dev->deferred[dev->num_deferred++] = d;
   ps4_run_deferred(dev);
   simple_mtx_unlock(&dev->lock);
}

int
ac_drm_va_range_query(ac_drm_device *dev, enum amdgpu_gpu_va_range type, uint64_t *start,
                      uint64_t *end)
{
   *start = dev->va_start;
   *end = dev->va_start + PS4_VA_SIZE;
   return 0;
}

/* ---------------------------------------------------------------------------------------------
 * Contexts and submission
 */

int
ac_drm_cs_ctx_create2(ac_drm_device *dev, uint32_t priority, uint32_t *ctx_id)
{
   *ctx_id = 1;
   return 0;
}

int
ac_drm_cs_ctx_free(ac_drm_device *dev, uint32_t ctx_id)
{
   return 0;
}

int
ac_drm_cs_ctx_stable_pstate(ac_drm_device *dev, uint32_t ctx_id, uint32_t op, uint32_t flags,
                            uint32_t *out_flags)
{
   if (out_flags)
      *out_flags = 0;
   return 0;
}

int
ac_drm_cs_query_reset_state2(ac_drm_device *dev, uint32_t ctx_id, uint64_t *flags)
{
   *flags = 0;
   return 0;
}

int
ac_drm_cs_query_fence_status(ac_drm_device *dev, uint32_t ctx_id, uint32_t ip_type,
                             uint32_t ip_instance, uint32_t ring, uint64_t fence_seq_no,
                             uint64_t timeout_ns, uint64_t flags, uint32_t *expired)
{
   int64_t abs = flags & AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE
                    ? (int64_t)timeout_ns
                    : os_time_get_absolute_timeout(timeout_ns);
   *expired = ps4_wait_seq(dev, fence_seq_no, abs);
   /* RADV_DEBUG=hang checks each submission with a 1 s wait: report before it gives up. */
   if (!*expired && timeout_ns >= 500000000ull)
      ps4_report_hang(dev, fence_seq_no);
   return 0;
}

void
ac_drm_cs_chunk_fence_info_to_data(uint32_t bo_handle, uint64_t offset,
                                   struct drm_amdgpu_cs_chunk_data *data)
{
   data->fence_data.handle = bo_handle;
   data->fence_data.offset = offset * sizeof(uint64_t);
}

static struct ps4_syncobj *ps4_syncobj(ac_drm_device *dev, uint32_t handle);
static bool ps4_point_submitted(struct ps4_syncobj *s, uint64_t point);
static void ps4_add_point(struct ps4_syncobj *s, uint64_t point, uint64_t seq);

/* EVENT_WRITE_EOP: flush and invalidate CB/DB and the texture caches, then write a 64-bit value
 * at end of pipe. */
static bool
ps4_mapped(const void *p, uint64_t len)
{
   struct ps4_vq_info vq;
   memset(&vq, 0, sizeof(vq));
   return ps4_vq_ok && !sceKernelVirtualQuery(p, 0, &vq, sizeof(vq)) && vq.is_direct &&
          (uintptr_t)vq.end >= (uintptr_t)p + len;
}

/* Who wrote into a buffer? Lists the live buffers mapped next to `target`, and every reference in
 * the recent submissions' command streams to [lo, hi): 64-bit addresses in any packet, and
 * 256-byte-aligned base registers (render target / depth / CMASK / FMASK / HTILE bases). */
static unsigned ps4_scan_hits;
static void
ps4_scan_ib(ac_drm_device *dev, FILE *f, const uint32_t *ib, unsigned num_dw, uint64_t lo,
            uint64_t hi, int depth)
{
   for (unsigned link = 0; link < 64; link++) {
      if (!ps4_mapped(ib, num_dw * 4ull)) {
         fprintf(f, "      IB %p (%u dwords) no longer mapped\n", (void *)ib, num_dw);
         return;
      }
      const uint32_t *next = NULL;
      unsigned next_dw = 0;
      for (unsigned i = 0; i < num_dw && ps4_scan_hits < 300;) {
         const uint32_t h = ib[i];
         if (h >> 30 == 2 || h == 0xffff1000u) { /* (and PKT3_NOP_PAD: a header-only NOP) */
            i++;
            continue;
         }
         if (h >> 30 != 3) {
            fprintf(f, "      %p: non-type-3 header %#x, stop\n", (void *)&ib[i], h);
            return;
         }
         const unsigned op = (h >> 8) & 0xff;
         const unsigned body = ((h >> 16) & 0x3fff) + 1;
         if (i + 1 + body > num_dw)
            break;
         const uint32_t *b = &ib[i + 1];
         for (unsigned k = 0; k + 1 < body; k++) {
            const uint64_t a = b[k] | ((uint64_t)(b[k + 1] & 0xffff) << 32);
            if (a >= lo && a < hi) {
               fprintf(f, "      %p: op %#x (%u dwords) dword %u: address %#llx\n", (void *)&ib[i],
                       op, body, k, (unsigned long long)a);
               ps4_scan_hits++;
            }
         }
         if (op == 0x69 && body >= 2) { /* SET_CONTEXT_REG */
            for (unsigned k = 1; k < body; k++) {
               const uint64_t a = (uint64_t)b[k] << 8;
               if (a >= lo && a < hi) {
                  fprintf(f, "      %p: context reg %#x = %#x (base %#llx)\n", (void *)&ib[i],
                          0xa000 + b[0] + k - 1, b[k], (unsigned long long)a);
                  ps4_scan_hits++;
               }
            }
         }
         if ((op == 0x3f || op == 0x33) && body >= 3) { /* INDIRECT_BUFFER */
            const uint32_t *t = (const uint32_t *)(uintptr_t)(b[0] | ((uint64_t)(b[1] & 0xffff) << 32));
            if (b[2] & (1u << 20)) {
               next = t;
               next_dw = b[2] & 0xfffff;
               break;
            }
            if (depth < 2)
               ps4_scan_ib(dev, f, t, b[2] & 0xfffff, lo, hi, depth + 1);
         }
         i += 1 + body;
      }
      if (!next)
         return;
      ib = next;
      num_dw = next_dw;
   }
}

static void
ps4_find_writer(ac_drm_device *dev, FILE *f, uint64_t target)
{
   struct amdgpu_bo *below = NULL, *above = NULL;
   for (unsigned h = 1; h < dev->num_bos; h++) {
      struct amdgpu_bo *b = dev->bos[h];
      if (!b || !b->last_va || !b->cpu)
         continue;
      if (b->last_va + b->size <= target && (!below || b->last_va > below->last_va))
         below = b;
      if (b->last_va > target && (!above || b->last_va < above->last_va))
         above = b;
   }
   fprintf(f, "  neighbours:\n");
   if (below) {
      fprintf(f, "    below: bo %u va %#llx size %#llx (ends %#llx) phys %#llx heap %#x flags %#llx\n",
              below->handle, (unsigned long long)below->last_va, (unsigned long long)below->size,
              (unsigned long long)(below->last_va + below->size), (unsigned long long)below->phys,
              below->heap, (unsigned long long)below->flags);
      ps4_describe_va(dev, f, below->last_va + below->size - 4);
   }
   if (above)
      fprintf(f, "    above: bo %u va %#llx size %#llx phys %#llx heap %#x flags %#llx\n",
              above->handle, (unsigned long long)above->last_va, (unsigned long long)above->size,
              (unsigned long long)above->phys, above->heap, (unsigned long long)above->flags);

   /* References into [start of the buffer below, end of the target's first page). */
   const uint64_t lo = below ? below->last_va : target - 0x100000, hi = target + PS4_PAGE;
   fprintf(f, "  references to [%#llx, %#llx) in recent submissions (GPU completed %llu):\n",
           (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)ps4_completed(dev));
   for (unsigned n = 0; n < ARRAY_SIZE(dev->recent); n++) {
      const unsigned r = (dev->last_seq + 1 + n) % ARRAY_SIZE(dev->recent);
      if (!dev->recent[r].seq)
         continue;
      fprintf(f, "    submission %llu:\n", (unsigned long long)dev->recent[r].seq);
      for (unsigned i = 0; i < dev->recent[r].num_ibs; i++)
         ps4_scan_ib(dev, f, (const uint32_t *)(uintptr_t)dev->recent[r].va[i],
                     dev->recent[r].bytes[i] / 4, lo, hi, 0);
   }
}

/* The live buffer whose memory holds `va` (dev->lock held), or NULL. */
static struct amdgpu_bo *
ps4_bo_at(ac_drm_device *dev, uint64_t va)
{
   for (unsigned h = 1; h < dev->num_bos; h++) {
      struct amdgpu_bo *b = dev->bos[h];
      if (b && b->cpu && !b->cpu_owned && va >= b->last_va && va < b->last_va + b->size)
         return b;
   }
   return NULL;
}

/* Released buffers' address ranges (whole-buffer unmaps), for finding stale descriptors. */
struct ps4_released_range {
   uint64_t va, size, seq;
   uint32_t handle;
};
static struct ps4_released_range ps4_released[4096];
static unsigned ps4_num_released;

static void
ps4_note_released(uint32_t handle, uint64_t va, uint64_t size, uint64_t seq)
{
   ps4_released[ps4_num_released++ % ARRAY_SIZE(ps4_released)] =
      (struct ps4_released_range){va, size, seq, handle};
}

/* A released range holding va that no live buffer covers now (dev->lock held). */
static const struct ps4_released_range *
ps4_stale_at(ac_drm_device *dev, uint64_t va)
{
   if (va < 0x4000000000ull || va >= PS4_GPU_VA_LIMIT)
      return NULL;
   const struct ps4_released_range *hit = NULL;
   const unsigned n = MIN2(ps4_num_released, ARRAY_SIZE(ps4_released));
   for (unsigned i = 0; i < n && !hit; i++)
      if (va >= ps4_released[i].va && va < ps4_released[i].va + ps4_released[i].size)
         hit = &ps4_released[i];
   if (!hit)
      return NULL;
   for (unsigned h = 1; h < dev->num_bos; h++) {
      struct amdgpu_bo *b = dev->bos[h];
      if (b && b->cpu && !b->cpu_owned && va >= b->last_va && va < b->last_va + b->size + PS4_TAIL)
         return NULL;
   }
   return hit;
}

/* Do a shader stage's user data (or the descriptors they point to) hold the address of a released
 * buffer? Cemu uses resources through stale descriptors: on PC the GPU drops writes to freed
 * memory, on the PS4 they are a GPU page fault that ends the app (Donkey Kong Country: Tropical
 * Freeze, 1.86: a write into a 720p image released 5 submissions earlier, through a descriptor
 * set). User data hold 64-bit addresses and 32-bit descriptor set pointers (into the 32-bit
 * window); buffer descriptors carry an address in dwords 0-1 (48 bits), image descriptors as
 * address >> 8 (40 bits). A descriptor set is read again only after a release or every 64
 * submissions. dev->lock held. */
static struct {
   uint64_t ptr, epoch;
} ps4_checked_sets[2048];

static const struct ps4_released_range *
ps4_set_stale(ac_drm_device *dev, uint64_t ptr, uint64_t epoch, uint64_t *addr)
{
   if (ptr % 4)
      return NULL;
   /* the buffer found last time first: a frame's descriptor sets mostly lie in one upload buffer */
   static uint32_t last_handle;
   struct amdgpu_bo *t = last_handle < dev->num_bos ? dev->bos[last_handle] : NULL;
   if (!t || !t->cpu || t->cpu_owned || ptr < t->last_va || ptr >= t->last_va + t->size) {
      t = ps4_bo_at(dev, ptr);
      if (!t)
         return NULL;
      last_handle = t->handle;
   }
   const unsigned slot = (unsigned)((ptr >> 4) * 2654435761u) % ARRAY_SIZE(ps4_checked_sets);
   if (ps4_checked_sets[slot].ptr == ptr && ps4_checked_sets[slot].epoch == epoch)
      return NULL;
   const uint64_t avail = t->last_va + t->size - ptr;
   const unsigned n = (unsigned)MIN2(avail / 4, 256);
   const uint32_t *d = (const uint32_t *)(uintptr_t)ptr;
   for (unsigned j = 0; j + 1 < n; j++) {
      const uint64_t buffer = d[j] | ((uint64_t)(d[j + 1] & 0xffff) << 32);
      const uint64_t image = (d[j] | ((uint64_t)(d[j + 1] & 0xff) << 32)) << 8;
      const struct ps4_released_range *r;
      if ((r = ps4_stale_at(dev, buffer))) {
         *addr = buffer;
         return r;
      }
      if ((r = ps4_stale_at(dev, image))) {
         *addr = image;
         return r;
      }
   }
   ps4_checked_sets[slot].ptr = ptr;
   ps4_checked_sets[slot].epoch = epoch;
   return NULL;
}

static const struct ps4_released_range *
ps4_user_data_stale(ac_drm_device *dev, const uint32_t *ud, unsigned count, uint64_t seq,
                    unsigned *where, uint64_t *addr)
{
   const uint64_t epoch = ps4_num_released + (seq >> 6) * 0x100000000ull;
   for (unsigned u = 0; u < count; u++) {
      if (!ud[u])
         continue;
      const struct ps4_released_range *r;
      if (u + 1 < count) {
         const uint64_t ptr = ud[u] | ((uint64_t)(ud[u + 1] & 0xffff) << 32);
         if ((r = ps4_stale_at(dev, ptr))) {
            *where = u;
            *addr = ptr;
            return r;
         }
         if (ptr >= 0x4000000000ull && ptr < PS4_GPU_VA_LIMIT && (r = ps4_set_stale(dev, ptr, epoch, addr))) {
            *where = 0x100 + u;
            return r;
         }
      }
      if (dev->va32_start && ud[u] < PS4_VA32_SIZE &&
          (r = ps4_set_stale(dev, dev->va32_start + ud[u], epoch, addr))) {
         *where = 0x200 + u;
         return r;
      }
   }
   return NULL;
}

/* The dispatches and draws of a submission, checked with ps4_user_data_stale (always on: only packet headers
 * are walked, and nothing is read until a buffer has been released). dev->lock held. */
static void
ps4_scan_dispatches(ac_drm_device *dev, uint32_t *ib, unsigned num_dw, uint64_t seq, int depth)
{
   /* user data: COMPUTE_USER_DATA_0..15, and SPI_SHADER_USER_DATA_{PS,VS,GS,ES,HS,LS}_0..15 */
   static const unsigned stage_reg[6] = {0x0c, 0x4c, 0x8c, 0xcc, 0x10c, 0x14c};
   static const char *const stage_name[6] = {"PS", "VS", "GS", "ES", "HS", "LS"};
   uint32_t compute_ud[16] = {0};
   uint32_t gfx_ud[6][16] = {{0}};
   for (unsigned link = 0; link < 256 && num_dw; link++) {
      if (!ps4_mapped(ib, num_dw * 4ull))
         return;
      uint32_t *next = NULL;
      unsigned next_dw = 0;
      for (unsigned i = 0; i < num_dw;) {
         const uint32_t h = ib[i];
         if (h >> 30 == 2 || h == 0xffff1000u) { /* (and PKT3_NOP_PAD: a header-only NOP) */
            i++;
            continue;
         }
         if (h >> 30 != 3)
            return;
         const unsigned op = (h >> 8) & 0xff;
         const unsigned body = ((h >> 16) & 0x3fff) + 1;
         if (i + 1 + body > num_dw)
            return;
         uint32_t *b = &ib[i + 1];
         switch (op) {
         case 0x76: /* SET_SH_REG: COMPUTE_USER_DATA_n is SH register 0x240 + n */
            for (unsigned k = 1; k < body; k++) {
               const unsigned reg = b[0] + k - 1;
               if (reg >= 0x240 && reg < 0x250)
                  compute_ud[reg - 0x240] = b[k];
               for (unsigned s = 0; s < 6; s++)
                  if (reg >= stage_reg[s] && reg < stage_reg[s] + 16)
                     gfx_ud[s][reg - stage_reg[s]] = b[k];
            }
            break;
         case 0x27: /* DRAW_INDEX_2 */
         case 0x2d: /* DRAW_INDEX_AUTO */
         case 0x35: /* DRAW_INDEX_OFFSET_2 */
         case 0x24: /* DRAW_INDIRECT */
         case 0x25: /* DRAW_INDEX_INDIRECT */
         case 0x2c: /* DRAW_INDIRECT_MULTI */
         case 0x38: /* DRAW_INDEX_INDIRECT_MULTI */
         case 0x9c: /* DRAW_INDIRECT_COUNT_MULTI */
         case 0x9d: /* DRAW_INDEX_INDIRECT_COUNT_MULTI */
            for (unsigned s = 0; s < 6; s++) {
               unsigned where = 0;
               uint64_t addr = 0;
               const struct ps4_released_range *r = ps4_user_data_stale(dev, gfx_ud[s], 16, seq, &where, &addr);
               if (!r)
                  continue;
               static unsigned logged;
               if (logged++ < 40)
                  ps4_log("radv/ps4: submission %llu: %p: draw (%s stage) uses %#llx (user data %#x) "
                          "inside bo %u released at submission %llu (va %#llx, %#llx bytes) - skipped\n",
                          (unsigned long long)seq, (void *)&ib[i], stage_name[s], (unsigned long long)addr,
                          where, r->handle, (unsigned long long)r->seq, (unsigned long long)r->va,
                          (unsigned long long)r->size);
               ps4_progress("stale draw: submission %llu %s addr %#llx bo %u\n", (unsigned long long)seq,
                            stage_name[s], (unsigned long long)addr, r->handle);
               ib[i] = (h & ~0xff00u) | (0x10u << 8); /* NOP, same length */
               break;
            }
            break;
         case 0x15: /* DISPATCH_DIRECT */
         case 0x16: { /* DISPATCH_INDIRECT */
            unsigned where = 0;
            uint64_t addr = 0;
            const struct ps4_released_range *r = ps4_user_data_stale(dev, compute_ud, 16, seq, &where, &addr);
            if (r) {
               static unsigned logged;
               if (logged++ < 40)
                  ps4_log("radv/ps4: submission %llu: %p: dispatch uses %#llx (user data %#x) inside "
                          "bo %u released at submission %llu (va %#llx, %#llx bytes) - skipped\n",
                          (unsigned long long)seq, (void *)&ib[i], (unsigned long long)addr, where,
                          r->handle, (unsigned long long)r->seq, (unsigned long long)r->va,
                          (unsigned long long)r->size);
               ps4_progress("stale dispatch: submission %llu addr %#llx bo %u\n",
                            (unsigned long long)seq, (unsigned long long)addr, r->handle);
               ib[i] = (h & ~0xff00u) | (0x10u << 8); /* NOP, same length */
            }
            break;
         }
         case 0x3f: { /* INDIRECT_BUFFER */
            uint32_t *t = (uint32_t *)(uintptr_t)(b[0] | ((uint64_t)(b[1] & 0xffff) << 32));
            if (body >= 3 && (b[2] & (1u << 20))) {
               next = t;
               next_dw = b[2] & 0xfffff;
            } else if (body >= 3 && depth < 3) {
               ps4_scan_dispatches(dev, t, b[2] & 0xfffff, seq, depth + 1);
            }
            break;
         }
         default:
            break;
         }
         if (next)
            break;
         i += 1 + body;
      }
      if (!next)
         return;
      ib = next;
      num_dw = next_dw;
   }
}

/* Every command that writes memory (WRITE_DATA, EVENT_WRITE(_EOP), COPY_DATA, DMA_DATA,
 * ATOMIC_MEM) is checked before the GPU sees it: a destination inside a command buffer (mapped
 * GPU read-only, so the write would fault and the system kill the app) or outside every buffer is
 * reported and the command turned into a NOP of the same length. Render target / depth bases in a
 * command buffer are reported. dev->lock held. */
static unsigned ps4_bad_writes;
static unsigned long long ps4_writes_checked;
static void
ps4_scan_writes(ac_drm_device *dev, uint32_t *ib, unsigned num_dw, uint64_t seq, int depth)
{
   for (unsigned link = 0; link < 256 && num_dw; link++) {
      if (!ps4_mapped(ib, num_dw * 4ull))
         return;
      uint32_t *next = NULL;
      unsigned next_dw = 0;
      for (unsigned i = 0; i < num_dw;) {
         const uint32_t h = ib[i];
         if (h >> 30 == 2 || h == 0xffff1000u) { /* (and PKT3_NOP_PAD: a header-only NOP) */
            i++;
            continue;
         }
         if (h >> 30 != 3)
            return;
         const unsigned op = (h >> 8) & 0xff;
         const unsigned body = ((h >> 16) & 0x3fff) + 1;
         if (i + 1 + body > num_dw)
            return;
         uint32_t *b = &ib[i + 1];
         uint64_t dst = 0, len = 4;
         const char *name = NULL;
         switch (op) {
         case 0x37: /* WRITE_DATA */
            if (body >= 4 && (((b[0] >> 8) & 0xf) == 1 || ((b[0] >> 8) & 0xf) == 2 ||
                              ((b[0] >> 8) & 0xf) == 5)) {
               dst = b[1] | ((uint64_t)b[2] << 32);
               len = (body - 3) * 4ull;
               name = "WRITE_DATA";
            }
            break;
         case 0x47: /* EVENT_WRITE_EOP */
            if (body >= 5 && (b[2] >> 29)) {
               dst = b[1] | ((uint64_t)(b[2] & 0xffff) << 32);
               len = (b[2] >> 29) == 1 ? 4 : 8;
               name = "EVENT_WRITE_EOP";
            }
            break;
         case 0x46: /* EVENT_WRITE with an address (ZPASS_DONE, pipeline stats, ...) */
            if (body >= 3) {
               dst = b[1] | ((uint64_t)(b[2] & 0xffff) << 32);
               len = 8;
               name = "EVENT_WRITE";
            }
            break;
         case 0x40: /* COPY_DATA */
            if (body >= 5 && (((b[0] >> 8) & 0xf) == 1 || ((b[0] >> 8) & 0xf) == 2 ||
                              ((b[0] >> 8) & 0xf) == 5)) {
               dst = b[3] | ((uint64_t)b[4] << 32);
               len = (b[0] & (1u << 16)) ? 8 : 4;
               name = "COPY_DATA";
            }
            break;
         case 0x50: /* DMA_DATA */
            if (body >= 6 && (((b[0] >> 20) & 3) == 0 || ((b[0] >> 20) & 3) == 3)) {
               dst = b[3] | ((uint64_t)b[4] << 32);
               len = MAX2(b[5] & 0x1fffff, 1);
               name = "DMA_DATA";
            }
            break;
         case 0x1e: /* ATOMIC_MEM */
            if (body >= 3) {
               dst = b[1] | ((uint64_t)b[2] << 32);
               len = 8;
               name = "ATOMIC_MEM";
            }
            break;
         case 0x69: /* SET_CONTEXT_REG: CB_COLORn_BASE/CMASK/FMASK, DB bases, HTILE */
            for (unsigned k = 1; k < body; k++) {
               const unsigned reg = b[0] + k - 1;
               const bool base = (reg >= 0x318 && reg < 0x318 + 8 * 15 &&
                                  ((reg - 0x318) % 15 == 0 || (reg - 0x318) % 15 == 7 ||
                                   (reg - 0x318) % 15 == 9)) ||
                                 (reg >= 0x12 && reg <= 0x15) || reg == 0x5;
               if (!base || !b[k])
                  continue;
               struct amdgpu_bo *t = ps4_bo_at(dev, (uint64_t)b[k] << 8);
               if (t && t->gpu_ro && ps4_bad_writes++ < 40)
                  ps4_log("radv/ps4: submission %llu: %p: context reg %#x = %#x points into "
                          "command buffer bo %u (va %#llx)\n", (unsigned long long)seq,
                          (void *)&ib[i], 0xa000 + reg, b[k], t->handle,
                          (unsigned long long)t->last_va);
            }
            break;
         case 0x3f: { /* INDIRECT_BUFFER */
            uint32_t *t = (uint32_t *)(uintptr_t)(b[0] | ((uint64_t)(b[1] & 0xffff) << 32));
            if (body >= 3 && (b[2] & (1u << 20))) {
               next = t;
               next_dw = b[2] & 0xfffff;
            } else if (body >= 3 && depth < 3) {
               ps4_scan_writes(dev, t, b[2] & 0xfffff, seq, depth + 1);
            }
            break;
         }
         default:
            break;
         }
         if (name) {
            ps4_writes_checked++;
            struct amdgpu_bo *t = ps4_bo_at(dev, dst);
            struct amdgpu_bo *t_end = ps4_bo_at(dev, dst + len - 1);
            const char *why = !t ? "outside every buffer"
                              : t->gpu_ro ? "into a command buffer"
                              : t_end != t ? "past the end of its buffer"
                                           : NULL;
            if (why) {
               if (ps4_bad_writes++ < 40) {
                  char words[160];
                  int n = 0;
                  for (unsigned k = 0; k <= body && k < 10; k++)
                     n += snprintf(words + n, sizeof(words) - n, " %08x", ib[i + k]);
                  ps4_log("radv/ps4: submission %llu: %p: %s to %#llx (%llu bytes) %s - "
                          "disabled. packet:%s\n", (unsigned long long)seq, (void *)&ib[i], name,
                          (unsigned long long)dst, (unsigned long long)len, why, words);
                  if (t)
                     ps4_log("radv/ps4:   target bo %u: va %#llx size %#llx heap %#x flags %#llx\n",
                             t->handle, (unsigned long long)t->last_va,
                             (unsigned long long)t->size, t->heap, (unsigned long long)t->flags);
               }
               ib[i] = (h & ~0xff00u) | (0x10u << 8); /* NOP, same length */
            }
         }
         if (next)
            break;
         i += 1 + body;
      }
      if (!next)
         return;
      ib = next;
      num_dw = next_dw;
   }
}

/* Before submitting: does every IB of the chain still start with a packet header, and does every
 * chain link point into a live buffer? (A GPU hang showed a chained IB full of vertex-like float
 * data.) Follows only the chain link in each IB's last 4 dwords - a few reads per IB. On failure,
 * appends what it found to ib-corrupt.txt; returns false. */
static unsigned ps4_corrupt_reports;
static bool
ps4_check_ib_chain(ac_drm_device *dev, const uint32_t *ib, unsigned num_dw, uint64_t seq)
{
   const uint32_t *prev = NULL;
   unsigned prev_dw = 0;
   for (unsigned link = 0; link < 256 && num_dw; link++) {
      const char *problem = NULL;
      if (ib[0] >> 30 != 3 && ib[0] >> 30 != 2)
         problem = "does not start with a packet header";
      const uint32_t *tail = ib + num_dw - 4;
      uint64_t next = 0;
      unsigned next_dw = 0;
      if (!problem && num_dw >= 4 && tail[0] == 0xC0023F00 && (tail[3] & (1u << 20))) {
         next = tail[1] | ((uint64_t)(tail[2] & 0xffff) << 32);
         next_dw = tail[3] & 0xfffff;
         struct ac_addr_info info;
         ps4_ib_addr(dev, next, &info);
         struct ps4_vq_info vq;
         memset(&vq, 0, sizeof(vq));
         if (!info.valid)
            problem = "chains to an address outside the GPU windows";
         else if (ps4_vq_ok &&
                  (sceKernelVirtualQuery((void *)(uintptr_t)next, 0, &vq, sizeof(vq)) ||
                   !vq.is_direct || (uintptr_t)vq.end < next + next_dw * 4ull))
            problem = "chains to memory that isn't mapped";
      }
      if (problem) {
         if (ps4_corrupt_reports++ < 4) {
            ps4_log("radv/ps4: submission %llu: IB %p (%u dwords) %s (first dword %#x) - see "
                    "ib-corrupt.txt\n", (unsigned long long)seq, (void *)ib, num_dw, problem, ib[0]);
            FILE *f = fopen(ps4_gpu_file("ib-corrupt.txt"), "a");
            if (f) {
               fprintf(f, "==== submission %llu, chain link %u: IB %p, %u dwords, %s\n",
                       (unsigned long long)seq, link, (void *)ib, num_dw, problem);
               fprintf(f, "  first 48 dwords:");
               for (unsigned i = 0; i < 48 && i < num_dw; i++)
                  fprintf(f, "%s%08x", i % 8 ? " " : "\n    ", ib[i]);
               fprintf(f, "\n");
               if (prev && prev_dw >= 32) {
                  fprintf(f, "  last 32 dwords of the IB chaining to it (%p, %u dwords):", (void *)prev,
                          prev_dw);
                  for (unsigned i = prev_dw - 32; i < prev_dw; i++)
                     fprintf(f, "%s%08x", (i - (prev_dw - 32)) % 8 ? " " : "\n    ", prev[i]);
                  fprintf(f, "\n");
               }
               ps4_describe_va(dev, f, (uintptr_t)ib);
               if (next)
                  ps4_describe_va(dev, f, next);
               struct ps4_vq_info vq;
               memset(&vq, 0, sizeof(vq));
               if (!sceKernelVirtualQuery(ib, 0, &vq, sizeof(vq)))
                  fprintf(f, "  kernel: [%p, %p) offset %#llx prot %#x type %d direct %u flexible %u "
                             "'%.32s'\n", vq.start, vq.end, (unsigned long long)vq.offset,
                          vq.protection, vq.memory_type, vq.is_direct, vq.is_flexible, vq.name);
               ps4_check_overlaps(dev, f);
               ps4_find_writer(dev, f, (uintptr_t)ib);
               fclose(f);
            }
            simple_mtx_lock(&dev->lock);
            ps4_scan_tails(dev);
            simple_mtx_unlock(&dev->lock);
         }
         return false;
      }
      if (!next)
         return true;
      prev = ib;
      prev_dw = num_dw;
      ib = (const uint32_t *)(uintptr_t)next;
      num_dw = next_dw;
   }
   return true;
}

static uint32_t *
ps4_emit_eop(uint32_t *cs, uint64_t addr, uint64_t value, bool interrupt)
{
   *cs++ = 0xC0044700; /* PKT3(EVENT_WRITE_EOP, 4) */
   /* CACHE_FLUSH_AND_INV_TS_EVENT with TC_WB (bit 15), TCL1 and TC actions: Gnm's "write back and
    * invalidate L1 and L2". Without the write-back bit the fence could signal while shader stores
    * still sat dirty in the L2; the buffer was then released (unmapped) and the L2 wrote them back
    * later - a GPU page fault that ends the app (Donkey Kong Country: Tropical Freeze, 1.82-1.89:
    * a 720p image written through a storage buffer, released right after its submission
    * "finished"). */
   *cs++ = 0x14 | (5 << 8) | (1 << 15) | (1 << 16) | (1 << 17);
   *cs++ = (uint32_t)addr;
   /* DATA_SEL: 64-bit value; INT_SEL 2: interrupt once the write is confirmed. */
   *cs++ = ((addr >> 32) & 0xffff) | (2u << 29) | (interrupt ? 2u << 24 : 0);
   *cs++ = (uint32_t)value;
   *cs++ = (uint32_t)(value >> 32);
   return cs;
}

/* Adds the finished submissions' timestamps to the busy time (called with dev->lock held). */
static void
ps4_account_gpu_time(ac_drm_device *dev)
{
   if (!dev->ts_start)
      return;
   const uint64_t done = ps4_completed(dev);
   if (dev->ts_accounted + FENCE_IB_COUNT < done)
      dev->ts_accounted = done - FENCE_IB_COUNT; /* slots already reused */
   while (dev->ts_accounted < done) {
      const unsigned i = ++dev->ts_accounted % FENCE_IB_COUNT;
      const uint64_t start = dev->ts_start[i], end = dev->ts_end[i];
      if (!start || end <= start)
         continue; /* a flip (no timestamps) or not written */
      const uint64_t from = MAX2(start, dev->ts_prev_end);
      if (end > from)
         dev->ts_busy += end - from;
      dev->ts_prev_end = MAX2(dev->ts_prev_end, end);
      if (!dev->ts_span_first)
         dev->ts_span_first = start;
      dev->ts_span_last = MAX2(dev->ts_span_last, end);
   }
}

void ac_ps4_get_gpu_busy(uint64_t *busy_ticks, uint64_t *span_ticks);
void
ac_ps4_get_gpu_busy(uint64_t *busy_ticks, uint64_t *span_ticks)
{
   ac_drm_device *dev = ps4_last_device;
   *busy_ticks = *span_ticks = 0;
   if (!dev)
      return;
   simple_mtx_lock(&dev->lock);
   ps4_account_gpu_time(dev);
   *busy_ticks = dev->ts_busy;
   *span_ticks = dev->ts_span_last - dev->ts_span_first;
   simple_mtx_unlock(&dev->lock);
}

/* The GPU's statistics since the device was made, for the launcher to difference: busy and span
 * as ac_ps4_get_gpu_busy, submissions made, and the CPU's waits for the GPU (ps4_wait_common). */
void ac_ps4_get_gpu_stats(uint64_t *busy_ticks, uint64_t *span_ticks, uint64_t *submits,
                          uint64_t *waits, uint64_t *wait_ns);
void
ac_ps4_get_gpu_stats(uint64_t *busy_ticks, uint64_t *span_ticks, uint64_t *submits,
                     uint64_t *waits, uint64_t *wait_ns)
{
   ac_ps4_get_gpu_busy(busy_ticks, span_ticks);
   ac_drm_device *dev = ps4_last_device;
   *submits = dev ? __atomic_load_n(&dev->last_seq, __ATOMIC_RELAXED) : 0;
   *waits = __atomic_load_n(&s_fence_waits, __ATOMIC_RELAXED);
   *wait_ns = __atomic_load_n(&s_fence_wait_ns, __ATOMIC_RELAXED);
}

/* ---------------------------------------------------------------------------------------------
 * Submission thread. sceGnmSubmitCommandBuffers is a kernel call of ~0.4 ms, and the GPU thread
 * of an emulator submits ~230 times a second (Breath of the Wild: ~9% of Cemu's GPU thread). The
 * submitting thread now only queues the job - the sequence number is taken and the fence written
 * into the IB as before - and this thread makes the calls, in queue order (jobs are queued under
 * dev->lock, in sequence order; the thread never takes dev->lock). Flips go through the same
 * queue, so a flip can't overtake the work queued before it. A failed submission writes its
 * fence from the CPU, so nothing waits for it forever.
 */
#define PS4_JOB_RING 128
struct ps4_job {
   ac_drm_device *dev;
   bool flip;
   unsigned num_ibs;
   void *dcb[68];
   uint32_t dcb_sizes[68];
   uint64_t seq;
   int video;
   unsigned buffer, mode;
   int64_t arg;
};
static struct ps4_job ps4_jobs[PS4_JOB_RING];
static unsigned ps4_job_head, ps4_job_tail; /* next to run, next to fill */
static pthread_mutex_t ps4_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ps4_job_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t ps4_job_space = PTHREAD_COND_INITIALIZER;
static bool ps4_job_thread_started;
void ps4_boost_thread(void); /* port/ps4_runtime.cpp: above the default priority */

static void
ps4_fence_at_least(ac_drm_device *dev, uint64_t seq)
{
   volatile uint64_t *f = (volatile uint64_t *)dev->fence;
   uint64_t cur = *f;
   while (cur < seq && !__atomic_compare_exchange_n(f, &cur, seq, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      ;
}

static void *
ps4_submit_thread(void *arg)
{
   (void)arg;
   ps4_boost_thread();
   int64_t last_done = 0;
   for (;;) {
      pthread_mutex_lock(&ps4_job_lock);
      while (ps4_job_head == ps4_job_tail)
         pthread_cond_wait(&ps4_job_ready, &ps4_job_lock);
      struct ps4_job *job = &ps4_jobs[ps4_job_head % PS4_JOB_RING];
      pthread_mutex_unlock(&ps4_job_lock);

      ac_drm_device *dev = job->dev;
      void *ccb[68] = {0};
      uint32_t ccb_sizes[68] = {0};
      int r;
      if (job->flip)
         r = dev->submit_and_flip(job->num_ibs, job->dcb, job->dcb_sizes, ccb, ccb_sizes, job->video,
                                  job->buffer, job->mode, job->arg);
      else
         r = dev->submit(job->num_ibs, job->dcb, job->dcb_sizes, ccb, ccb_sizes);
      __atomic_store_n(&ps4_handed_seq, job->seq, __ATOMIC_RELEASE); /* (flips have one too) */
      /* sceGnmSubmitDone tells the system the frame's work is in (it wants one at least every
       * frame): after every flip, else at most every 4 ms */
      const int64_t now = os_time_get_nano();
      if (job->flip || now - last_done >= 4000000) {
         dev->submit_done();
         last_done = now;
      }
      if (r) {
         static unsigned logged;
         if (logged++ < 8)
            ps4_log("radv/ps4: %s of submission %llu = %#x (its fence written by the CPU)\n",
                    job->flip ? "sceGnmSubmitAndFlipCommandBuffers" : "sceGnmSubmitCommandBuffers",
                    (unsigned long long)job->seq, r);
         ps4_fence_at_least(dev, job->seq);
      }

      pthread_mutex_lock(&ps4_job_lock);
      ps4_job_head++;
      pthread_cond_signal(&ps4_job_space);
      pthread_mutex_unlock(&ps4_job_lock);
   }
   return NULL;
}

/* Every queued submission and flip handed to the system? */
static bool
ps4_jobs_idle(void)
{
   pthread_mutex_lock(&ps4_job_lock);
   const bool idle = ps4_job_head == ps4_job_tail;
   pthread_mutex_unlock(&ps4_job_lock);
   return idle;
}

/* Queues a submission (called with dev->lock held, in sequence order). */
static void
ps4_queue_job(const struct ps4_job *job)
{
   pthread_mutex_lock(&ps4_job_lock);
   if (!ps4_job_thread_started) {
      pthread_t thread;
      pthread_create(&thread, NULL, ps4_submit_thread, NULL);
      ps4_job_thread_started = true;
   }
   while (ps4_job_tail - ps4_job_head >= PS4_JOB_RING)
      pthread_cond_wait(&ps4_job_space, &ps4_job_lock);
   ps4_jobs[ps4_job_tail % PS4_JOB_RING] = *job;
   ps4_job_tail++;
   pthread_cond_signal(&ps4_job_ready);
   pthread_mutex_unlock(&ps4_job_lock);
}

int
ac_drm_cs_submit_raw2(ac_drm_device *dev, uint32_t ctx_id, uint32_t bo_list_handle,
                      int num_chunks, struct drm_amdgpu_cs_chunk *chunks, uint64_t *seq_no)
{
   void *dcb[64];
   uint32_t dcb_sizes[64];
   void *ccb[64] = {0};
   uint32_t ccb_sizes[64] = {0};
   unsigned num_ibs = 0;
   uint64_t user_fence_addr = 0;
   struct drm_amdgpu_cs_chunk *signal_chunk = NULL;

   for (int i = 0; i < num_chunks; i++) {
      struct drm_amdgpu_cs_chunk *c = &chunks[i];
      void *data = (void *)(uintptr_t)c->chunk_data;
      switch (c->chunk_id) {
      case AMDGPU_CHUNK_ID_IB: {
         struct drm_amdgpu_cs_chunk_ib *ib = data;
         if (ib->ip_type != AMDGPU_HW_IP_GFX || num_ibs >= ARRAY_SIZE(dcb) - 2)
            return -EINVAL;
         dcb[num_ibs] = (void *)(uintptr_t)ib->va_start;
         dcb_sizes[num_ibs] = ib->ib_bytes;
         num_ibs++;
         break;
      }
      case AMDGPU_CHUNK_ID_FENCE: {
         struct drm_amdgpu_cs_chunk_fence *f = data;
         struct amdgpu_bo *b = ps4_bo(dev, f->handle);
         if (b && b->cpu)
            user_fence_addr = (uintptr_t)b->cpu + f->offset;
         break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_IN:
      case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT: {
         /* One in-order queue: a wait only has to make sure the point was submitted (or
          * signalled by the CPU) before this submission; the GPU then runs them in order. */
         const bool timeline = c->chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT;
         const unsigned stride = timeline ? 4 : 1;
         for (unsigned j = 0; j + stride <= c->length_dw; j += stride) {
            const uint32_t *w = (const uint32_t *)data + j;
            const uint64_t point = timeline ? ((const struct drm_amdgpu_cs_chunk_syncobj *)w)->point : 0;
            for (;;) {
               simple_mtx_lock(&dev->lock);
               struct ps4_syncobj *s = ps4_syncobj(dev, w[0]);
               bool ready = !s || ps4_point_submitted(s, point);
               simple_mtx_unlock(&dev->lock);
               if (ready)
                  break;
               sceKernelUsleep(100);
            }
         }
         break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_OUT:
      case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL:
         signal_chunk = c;
         break;
      default:
         break;
      }
   }
   if (!num_ibs)
      return -EINVAL;

   /* A corrupted command buffer would hang the GPU (or fault it, and the system kills the app):
    * drop its work - the fences still signal - and report it once. */
   for (unsigned i = 0; i < num_ibs; i++) {
      if (!ps4_check_ib_chain(dev, dcb[i], dcb_sizes[i] / 4, dev->last_seq + 1)) {
         num_ibs = 0;
         break;
      }
   }
   ps4_filter_gs_tess(dcb, dcb_sizes, num_ibs, dev->last_seq + 1);
   ps4_record_submission(dcb, dcb_sizes, num_ibs, dev->last_seq + 1);

   /* Commands that would write where they mustn't: report and disable them (RADV_PS4_SCAN=1;
    * it found nothing in the boss-intro crash and costs CPU time). */
   static int scan = -1;
   if (scan < 0)
      scan = getenv("RADV_PS4_SCAN") != NULL;
   if (num_ibs && scan) {
      simple_mtx_lock(&dev->lock);
      for (unsigned i = 0; i < num_ibs; i++)
         ps4_scan_writes(dev, dcb[i], dcb_sizes[i] / 4, dev->last_seq + 1, 0);
      simple_mtx_unlock(&dev->lock);
   }
   /* Draws and dispatches through descriptors of released buffers: skipped (see
    * ps4_user_data_stale; RADV_PS4_SCAN=1 too: it never fired, the faults it hunted were the
    * fence's missing L2 write-back, ps4_emit_eop). */
   if (num_ibs && scan && ps4_num_released) {
      simple_mtx_lock(&dev->lock);
      for (unsigned i = 0; i < num_ibs; i++)
         ps4_scan_dispatches(dev, dcb[i], dcb_sizes[i] / 4, dev->last_seq + 1, 0);
      simple_mtx_unlock(&dev->lock);
   }

   simple_mtx_lock(&dev->lock);
   /* The fence IB slot for the next sequence number must be free (its previous user finished).
    * Waited for before taking the number, so submissions reach the GPU in sequence order (the
    * completed counter must never get ahead of work that hasn't been submitted yet). */
   while (dev->last_seq + 1 > FENCE_IB_COUNT &&
          ps4_completed(dev) < dev->last_seq + 1 - FENCE_IB_COUNT) {
      simple_mtx_unlock(&dev->lock);
      ps4_wait_seq(dev, dev->last_seq + 1 - FENCE_IB_COUNT, INT64_MAX);
      simple_mtx_lock(&dev->lock);
   }
   ps4_account_gpu_time(dev);
   const uint64_t seq = ++dev->last_seq;
   ps4_note_submission(dev, seq, dcb, dcb_sizes, num_ibs);
   uint32_t *fence_ib = dev->fence_ibs + (seq % FENCE_IB_COUNT) * FENCE_IB_DW;
   uint32_t *cs = fence_ib;
   if (user_fence_addr)
      cs = ps4_emit_eop(cs, user_fence_addr, seq, false);
   const unsigned ts_slot = seq % FENCE_IB_COUNT;
   if (dev->ts_start) {
      dev->ts_start[ts_slot] = 0;
      dev->ts_end[ts_slot] = 0;
      /* PKT3(EVENT_WRITE_EOP): BOTTOM_OF_PIPE_TS, DATA_SEL 3 = the 64-bit GPU clock. */
      const uint64_t addr = (uintptr_t)&dev->ts_end[ts_slot];
      *cs++ = 0xC0044700;
      *cs++ = 0x28 | (5 << 8);
      *cs++ = (uint32_t)addr;
      *cs++ = ((addr >> 32) & 0xffff) | (3u << 29);
      *cs++ = 0;
      *cs++ = 0;
   }
   cs = ps4_emit_eop(cs, (uintptr_t)dev->fence, seq, dev->eop_queue != NULL);
   while ((cs - fence_ib) % 8)
      *cs++ = 0xFFFF1000; /* PKT3_NOP_PAD: a header-only NOP */
   dcb[num_ibs] = fence_ib;
   dcb_sizes[num_ibs] = (cs - fence_ib) * 4;

   {
      const unsigned slot = seq % ARRAY_SIZE(dev->recent);
      dev->recent[slot].seq = seq;
      dev->recent[slot].num_ibs = MIN2(num_ibs, ARRAY_SIZE(dev->recent[slot].va));
      for (unsigned i = 0; i < dev->recent[slot].num_ibs; i++) {
         dev->recent[slot].va[i] = (uintptr_t)dcb[i];
         dev->recent[slot].bytes[i] = dcb_sizes[i];
      }
   }
   unsigned submit_ibs = num_ibs + 1;
   if (dev->ts_ibs) {
      /* In front: PKT3(COPY_DATA) of the GPU clock (SRC_SEL 9) to memory (DST_SEL 5), 64 bits,
       * write-confirmed - when the command processor reaches this submission. */
      uint32_t *ts_ib = dev->ts_ibs + ts_slot * TS_IB_DW, *t = ts_ib;
      const uint64_t addr = (uintptr_t)&dev->ts_start[ts_slot];
      *t++ = 0xC0044000;
      *t++ = 9 | (5 << 8) | (1 << 16) | (1 << 20);
      *t++ = 0;
      *t++ = 0;
      *t++ = (uint32_t)addr;
      *t++ = (uint32_t)(addr >> 32);
      while ((t - ts_ib) % 8)
         *t++ = 0xFFFF1000;
      memmove(dcb + 1, dcb, sizeof(dcb[0]) * submit_ibs);
      memmove(dcb_sizes + 1, dcb_sizes, sizeof(dcb_sizes[0]) * submit_ibs);
      dcb[0] = ts_ib;
      dcb_sizes[0] = (uint32_t)(t - ts_ib) * 4;
      submit_ibs++;
   }
   /* the submission thread makes the calls (ps4_queue_job) */
   struct ps4_job job;
   job.dev = dev;
   job.flip = false;
   job.num_ibs = submit_ibs;
   memcpy(job.dcb, dcb, sizeof(dcb[0]) * submit_ibs);
   memcpy(job.dcb_sizes, dcb_sizes, sizeof(dcb_sizes[0]) * submit_ibs);
   job.seq = seq;
   ps4_queue_job(&job);
   (void)ccb;
   (void)ccb_sizes;
   const int r = 0;
   if (seq <= 40 || seq % 1000 == 0)
      ps4_log("radv/ps4: submit %llu: %u IBs (first %p, %u bytes) = %#x, GPU completed %llu, "
              "%llu memory writes checked, %u disabled\n",
              (unsigned long long)seq, num_ibs, dcb[0], dcb_sizes[0], r,
              (unsigned long long)ps4_completed(dev), ps4_writes_checked, ps4_bad_writes);
   if (r) {
      ps4_log("radv/ps4: sceGnmSubmitCommandBuffers(%u IBs) = %#x\n", num_ibs + 1, r);
      dev->last_seq--;
      simple_mtx_unlock(&dev->lock);
      return -EINVAL;
   }

   /* Only with the flight recorder (Crema: GPU Fault Recorder): the tails are in write-combined
    * memory, slow to read, and every live buffer's took ~2% of Breath of the Wild's GPU thread. */
   static int trace_tails = -1;
   if (trace_tails < 0) {
      bool flight = false;
      ps4_trace_option(&flight);
      trace_tails = flight;
   }
   if (trace_tails && seq % 256 == 0)
      ps4_scan_tails(dev);
   ps4_run_deferred(dev);

   if (signal_chunk) {
      const bool timeline = signal_chunk->chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL;
      const unsigned stride = timeline ? 4 : 1;
      const uint32_t *data = (const uint32_t *)(uintptr_t)signal_chunk->chunk_data;
      for (unsigned j = 0; j + stride <= signal_chunk->length_dw; j += stride) {
         struct ps4_syncobj *s = ps4_syncobj(dev, data[j]);
         if (s)
            ps4_add_point(s, timeline ? ((const struct drm_amdgpu_cs_chunk_syncobj *)(data + j))->point : 0, seq);
      }
   }
   simple_mtx_unlock(&dev->lock);
   *seq_no = seq;
   return 0;
}

/* Present without the CPU waiting: queues a flip of VideoOut buffer `buffer` behind everything
 * submitted so far (sceGnmSubmitAndFlipCommandBuffers: the flip happens when the GPU gets there).
 * `arg` comes back in sceVideoOutGetFlipStatus().flipArg once the flip is done. */
int ac_ps4_submit_flip(int video, unsigned buffer, unsigned mode, int64_t arg);
int
ac_ps4_submit_flip(int video, unsigned buffer, unsigned mode, int64_t arg)
{
   ac_drm_device *dev = ps4_fault_dev;
   if (!dev || !dev->submit_and_flip || !dev->flip_ibs)
      return -1;
   enum { FLIP_IB_DW = 128 };
   STATIC_ASSERT(ARRAY_SIZE(dev->flip_seq) * FLIP_IB_DW * 4 <= PS4_PAGE);

   simple_mtx_lock(&dev->lock);
   const unsigned slot = dev->flip_count % ARRAY_SIZE(dev->flip_seq);
   /* The slot's previous flip IB, and the fence IB slot for the next sequence number, must be
    * free - waited for before taking the number, as in ac_drm_cs_submit_raw2. */
   while (ps4_completed(dev) < dev->flip_seq[slot] ||
          (dev->last_seq + 1 > FENCE_IB_COUNT &&
           ps4_completed(dev) < dev->last_seq + 1 - FENCE_IB_COUNT)) {
      const uint64_t wait = MAX2(dev->flip_seq[slot], dev->last_seq + 1 > FENCE_IB_COUNT
                                                         ? dev->last_seq + 1 - FENCE_IB_COUNT
                                                         : 0);
      simple_mtx_unlock(&dev->lock);
      ps4_wait_seq(dev, wait, INT64_MAX);
      simple_mtx_lock(&dev->lock);
   }
   const uint64_t seq = ++dev->last_seq;
   if (dev->ts_start) /* (no start timestamp for a flip: not an older submission's either) */
      dev->ts_start[seq % FENCE_IB_COUNT] = 0;
   uint32_t *ib = dev->flip_ibs + slot * FLIP_IB_DW, *cs = ib;
   cs = ps4_emit_eop(cs, (uintptr_t)dev->fence, seq, dev->eop_queue != NULL);
   while ((cs - ib) % 8)
      *cs++ = 0xFFFF1000;
   /* Gnm's prepareFlip: a 64-dword NOP with the flip label, rewritten by the driver. */
   *cs++ = 0xC03E1000;
   *cs++ = 0x68750777;
   for (int i = 0; i < 62; i++)
      *cs++ = 0;
   void *dcb[1] = {ib};
   uint32_t dcb_size[1] = {(uint32_t)(cs - ib) * 4};
   void *ccb[1] = {NULL};
   uint32_t ccb_size[1] = {0};
   /* behind the queued submissions (ps4_queue_job) */
   struct ps4_job job;
   job.dev = dev;
   job.flip = true;
   job.num_ibs = 1;
   job.dcb[0] = dcb[0];
   job.dcb_sizes[0] = dcb_size[0];
   job.seq = seq;
   job.video = video;
   job.buffer = buffer;
   job.mode = mode;
   job.arg = arg;
   ps4_queue_job(&job);
   (void)ccb;
   (void)ccb_size;
   const int r = 0;
   if (r) {
      dev->last_seq--;
      static unsigned logged;
      if (logged++ < 8)
         ps4_log("radv/ps4: sceGnmSubmitAndFlipCommandBuffers(buffer %u) = %#x\n", buffer, r);
   } else {
      dev->flip_seq[slot] = seq;
      dev->flip_count++;
      const unsigned rs = seq % ARRAY_SIZE(dev->recent);
      dev->recent[rs].seq = seq;
      dev->recent[rs].num_ibs = 1;
      dev->recent[rs].va[0] = (uintptr_t)ib;
      dev->recent[rs].bytes[0] = dcb_size[0];
      ps4_run_deferred(dev);
   }
   simple_mtx_unlock(&dev->lock);
   return r;
}

/* ---------------------------------------------------------------------------------------------
 * Sync objects (DRM syncobj semantics on top of sequence numbers)
 */

static struct ps4_syncobj *
ps4_syncobj(ac_drm_device *dev, uint32_t handle)
{
   return handle && handle < dev->num_syncobjs && dev->syncobjs[handle].used
             ? &dev->syncobjs[handle]
             : NULL;
}

static void
ps4_add_point(struct ps4_syncobj *s, uint64_t point, uint64_t seq)
{
   if (point == 0) {
      /* Binary: replace the fence. */
      s->num_points = 0;
   }
   if (s->num_points == s->max_points) {
      s->max_points = MAX2(s->max_points * 2, 4);
      s->points = realloc(s->points, s->max_points * sizeof(*s->points));
   }
   s->points[s->num_points++] = (struct ps4_point){point, seq};
}

static bool
ps4_point_submitted(struct ps4_syncobj *s, uint64_t point)
{
   for (unsigned i = 0; i < s->num_points; i++)
      if (s->points[i].point >= point)
         return true;
   return false;
}

/* Highest point whose submission has completed (timeline payload). */
static uint64_t
ps4_payload(ac_drm_device *dev, struct ps4_syncobj *s)
{
   const uint64_t done = ps4_completed(dev);
   uint64_t payload = 0;
   unsigned keep = 0;
   for (unsigned i = 0; i < s->num_points; i++) {
      if (s->points[i].seq <= done)
         payload = MAX2(payload, s->points[i].point);
   }
   /* Drop completed points below the payload (keep the latest one). */
   for (unsigned i = 0; i < s->num_points; i++) {
      if (s->points[i].seq > done || s->points[i].point == payload)
         s->points[keep++] = s->points[i];
   }
   s->num_points = keep;
   return payload;
}

static bool
ps4_binary_signalled(ac_drm_device *dev, struct ps4_syncobj *s)
{
   return s->num_points && s->points[s->num_points - 1].seq <= ps4_completed(dev);
}

static int
ps4_create(struct util_sync_provider *p, uint32_t flags, uint32_t *handle)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   unsigned h = 1;
   while (h < dev->num_syncobjs && dev->syncobjs[h].used)
      h++;
   if (h >= dev->num_syncobjs) {
      unsigned cap = MAX2(dev->num_syncobjs * 2, 64);
      dev->syncobjs = realloc(dev->syncobjs, cap * sizeof(*dev->syncobjs));
      memset(dev->syncobjs + dev->num_syncobjs, 0,
             (cap - dev->num_syncobjs) * sizeof(*dev->syncobjs));
      dev->num_syncobjs = cap;
   }
   struct ps4_syncobj *s = &dev->syncobjs[h];
   s->used = true;
   s->num_points = 0;
   if (flags & DRM_SYNCOBJ_CREATE_SIGNALED)
      ps4_add_point(s, 0, 0);
   simple_mtx_unlock(&dev->lock);
   *handle = h;
   return 0;
}

static int
ps4_destroy(struct util_sync_provider *p, uint32_t handle)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   struct ps4_syncobj *s = ps4_syncobj(dev, handle);
   if (s) {
      free(s->points);
      memset(s, 0, sizeof(*s));
   }
   simple_mtx_unlock(&dev->lock);
   return s ? 0 : -EINVAL;
}

static int
ps4_not_supported_fd(struct util_sync_provider *p, uint32_t handle, int *fd)
{
   return -ENOSYS;
}

static int
ps4_fd_to_handle(struct util_sync_provider *p, int fd, uint32_t *handle)
{
   return -ENOSYS;
}

static int
ps4_import_sync_file(struct util_sync_provider *p, uint32_t handle, int fd)
{
   return -ENOSYS;
}

static void
ps4_note_fence_wait(int64_t wait_start)
{
   if (!wait_start)
      return;
   __atomic_add_fetch(&s_fence_waits, 1, __ATOMIC_RELAXED);
   __atomic_add_fetch(&s_fence_wait_ns, (uint64_t)(os_time_get_nano() - wait_start), __ATOMIC_RELAXED);
}

/* Waits on handles/points; `points` NULL = binary. */
static int
ps4_wait_common(ac_drm_device *dev, uint32_t *handles, uint64_t *points, unsigned count,
                int64_t abs_timeout, unsigned flags, uint32_t *first)
{
   const bool all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
   const bool available = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
   const bool for_submit = flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
   unsigned spins = 0;
   int64_t wait_start = 0;
   for (;;) {
      unsigned done = 0, first_done = ~0u;
      simple_mtx_lock(&dev->lock);
      for (unsigned i = 0; i < count; i++) {
         struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
         if (!s) {
            simple_mtx_unlock(&dev->lock);
            return -EINVAL;
         }
         bool ok;
         if (points && points[i]) {
            ok = available ? ps4_point_submitted(s, points[i]) : ps4_payload(dev, s) >= points[i];
            if (!ok && !for_submit && !ps4_point_submitted(s, points[i])) {
               simple_mtx_unlock(&dev->lock);
               return -EINVAL;
            }
         } else {
            if (!s->num_points && !for_submit) {
               simple_mtx_unlock(&dev->lock);
               return -EINVAL;
            }
            ok = available ? s->num_points > 0 : ps4_binary_signalled(dev, s);
         }
         if (ok) {
            done++;
            if (first_done == ~0u)
               first_done = i;
         }
      }
      simple_mtx_unlock(&dev->lock);
      if (all ? done == count : done > 0) {
         if (first)
            *first = first_done == ~0u ? 0 : first_done;
         ps4_note_fence_wait(wait_start);
         return 0;
      }
      if (abs_timeout <= 0 || (abs_timeout != INT64_MAX && os_time_get_nano() >= abs_timeout)) {
         ps4_note_fence_wait(wait_start);
         return -ETIME;
      }
      if (!wait_start)
         wait_start = os_time_get_nano();
      else if (os_time_get_nano() - wait_start > 2000000000ll && ps4_completed(dev) < dev->last_seq)
         ps4_report_hang(dev, ps4_completed(dev) + 1);
      if (++spins > 64)
         ps4_sleep(dev);
   }
}

/* Failures also set errno, like the drmSyncobj ioctls: vk_drm_syncobj tells a timeout (fence not
 * signalled yet, e.g. vkGetFenceStatus) from an error by errno == ETIME, not by the return value. */
static int
ps4_wait(struct util_sync_provider *p, uint32_t *handles, unsigned count, int64_t timeout_nsec,
         unsigned flags, uint32_t *first)
{
   const int r = ps4_wait_common((ac_drm_device *)p, handles, NULL, count, timeout_nsec, flags, first);
   if (r)
      errno = -r;
   return r;
}

static int
ps4_timeline_wait(struct util_sync_provider *p, uint32_t *handles, uint64_t *points,
                  unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first)
{
   const int r = ps4_wait_common((ac_drm_device *)p, handles, points, count, timeout_nsec, flags, first);
   if (r)
      errno = -r;
   return r;
}

static int
ps4_reset(struct util_sync_provider *p, const uint32_t *handles, uint32_t count)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
      if (s)
         s->num_points = 0;
   }
   simple_mtx_unlock(&dev->lock);
   return 0;
}

static int
ps4_signal(struct util_sync_provider *p, const uint32_t *handles, uint32_t count)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
      if (s)
         ps4_add_point(s, 0, 0);
   }
   simple_mtx_unlock(&dev->lock);
   return 0;
}

static int
ps4_timeline_signal(struct util_sync_provider *p, const uint32_t *handles, uint64_t *points,
                    uint32_t count)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
      if (s)
         ps4_add_point(s, points[i], 0);
   }
   simple_mtx_unlock(&dev->lock);
   return 0;
}

static int
ps4_query(struct util_sync_provider *p, uint32_t *handles, uint64_t *points, uint32_t count,
          uint32_t flags)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
      if (!s) {
         points[i] = 0;
         continue;
      }
      if (flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) {
         uint64_t last = 0;
         for (unsigned j = 0; j < s->num_points; j++)
            last = MAX2(last, s->points[j].point);
         points[i] = last;
      } else {
         points[i] = ps4_payload(dev, s);
      }
   }
   simple_mtx_unlock(&dev->lock);
   return 0;
}

static int
ps4_transfer(struct util_sync_provider *p, uint32_t dst_handle, uint64_t dst_point,
             uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   struct ps4_syncobj *src = ps4_syncobj(dev, src_handle);
   struct ps4_syncobj *dst = ps4_syncobj(dev, dst_handle);
   int r = -EINVAL;
   if (src && dst) {
      /* The fence of the source: the first point >= src_point (binary: the current one). */
      uint64_t seq = 0;
      bool found = false;
      for (unsigned i = 0; i < src->num_points; i++) {
         if (src->points[i].point >= src_point && (!found || src->points[i].seq < seq)) {
            seq = src->points[i].seq;
            found = true;
         }
      }
      if (found) {
         ps4_add_point(dst, dst_point, seq);
         r = 0;
      }
   }
   simple_mtx_unlock(&dev->lock);
   return r;
}

static void
ps4_finalize(struct util_sync_provider *p)
{
}

static struct util_sync_provider *
ps4_clone(struct util_sync_provider *p)
{
   return p;
}

static struct util_sync_provider *
ps4_sync_provider_init(ac_drm_device *dev)
{
   dev->p = (struct util_sync_provider){
      .create = ps4_create,
      .destroy = ps4_destroy,
      .handle_to_fd = ps4_not_supported_fd,
      .fd_to_handle = ps4_fd_to_handle,
      .import_sync_file = ps4_import_sync_file,
      .export_sync_file = ps4_not_supported_fd,
      .wait = ps4_wait,
      .reset = ps4_reset,
      .signal = ps4_signal,
      .timeline_signal = ps4_timeline_signal,
      .timeline_wait = ps4_timeline_wait,
      .query = ps4_query,
      .transfer = ps4_transfer,
      .finalize = ps4_finalize,
      .clone = ps4_clone,
   };
   return &dev->p;
}

int
ac_drm_cs_create_syncobj2(ac_drm_device *dev, uint32_t flags, uint32_t *handle)
{
   return ps4_create(&dev->p, flags, handle);
}

int
ac_drm_cs_destroy_syncobj(ac_drm_device *dev, uint32_t handle)
{
   return ps4_destroy(&dev->p, handle);
}

int
ac_drm_cs_syncobj_wait(ac_drm_device *dev, uint32_t *handles, unsigned num_handles,
                       int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   return ps4_wait(&dev->p, handles, num_handles, timeout_nsec, flags, first_signaled);
}

int
ac_drm_cs_syncobj_query2(ac_drm_device *dev, uint32_t *handles, uint64_t *points,
                         unsigned num_handles, uint32_t flags)
{
   return ps4_query(&dev->p, handles, points, num_handles, flags);
}

int
ac_drm_cs_import_syncobj(ac_drm_device *dev, int shared_fd, uint32_t *handle)
{
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_export_sync_file(ac_drm_device *dev, uint32_t syncobj, int *sync_file_fd)
{
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_import_sync_file(ac_drm_device *dev, uint32_t syncobj, int sync_file_fd)
{
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_export_sync_file2(ac_drm_device *dev, uint32_t syncobj, uint64_t point,
                                    uint32_t flags, int *sync_file_fd)
{
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_transfer(ac_drm_device *dev, uint32_t dst_handle, uint64_t dst_point,
                           uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   return ps4_transfer(&dev->p, dst_handle, dst_point, src_handle, src_point, flags);
}

int
ac_drm_cs_syncobj_timeline_wait(ac_drm_device *dev, uint32_t *handles, uint64_t *points,
                                unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                                uint32_t *first_signaled)
{
   return ps4_timeline_wait(&dev->p, handles, points, num_handles, timeout_nsec, flags,
                            first_signaled);
}

/* ---------------------------------------------------------------------------------------------
 * Queries: answer as the amdgpu kernel would for a Liverpool-like gfx7 APU
 */

#define PS4_VRAM_SIZE (2048ull << 20)
#define PS4_GTT_SIZE (1024ull << 20)

int
ac_drm_query_gpu_info(ac_drm_device *dev, struct amdgpu_gpu_info *info)
{
   memset(info, 0, sizeof(*info));
   info->asic_id = 0x9920; /* Liverpool */
   info->chip_external_rev = 0x14; /* Bonaire range: identified as CHIP_BONAIRE (gfx7) */
   info->family_id = FAMILY_CI;
   info->ids_flags = AMDGPU_IDS_FLAGS_FUSION;
   info->max_engine_clk = 800000;
   info->max_memory_clk = 1375000;
   info->num_shader_engines = 2;
   info->num_shader_arrays_per_engine = 1;
   info->rb_pipes = 8;
   info->enabled_rb_pipes_mask = 0xff;
   info->gpu_counter_freq = 100000; /* kHz */
   info->mc_arb_ramcfg = PS4_MC_ARB_RAMCFG;
   info->gb_addr_cfg = PS4_GB_ADDR_CONFIG;
   memcpy(info->gb_tile_mode, ps4_tile_modes, sizeof(ps4_tile_modes));
   memcpy(info->gb_macro_tile_mode, ps4_macrotile_modes, sizeof(ps4_macrotile_modes));
   info->cu_bitmap[0][0] = 0x1ff; /* 9 CUs per shader engine, 18 total */
   info->cu_bitmap[1][0] = 0x1ff;
   info->vram_type = AMDGPU_VRAM_TYPE_GDDR5;
   info->vram_bit_width = 256;
   return 0;
}

static void
ps4_dev_info(ac_drm_device *dev, struct drm_amdgpu_info_device *d)
{
   memset(d, 0, sizeof(*d));
   d->device_id = 0x9920;
   d->external_rev = 0x14;
   d->family = FAMILY_CI;
   d->num_shader_engines = 2;
   d->num_shader_arrays_per_engine = 1;
   d->gpu_counter_freq = 100000;
   d->max_engine_clock = 800000;
   d->max_memory_clock = 1375000;
   d->cu_active_number = 18;
   d->cu_bitmap[0][0] = 0x1ff;
   d->cu_bitmap[1][0] = 0x1ff;
   d->enabled_rb_pipes_mask = 0xff;
   d->num_rb_pipes = 8;
   d->num_hw_gfx_contexts = 8;
   d->ids_flags = AMDGPU_IDS_FLAGS_FUSION;
   d->virtual_address_offset = dev->va_start;
   d->virtual_address_max = dev->va_start + PS4_VA_SIZE;
   d->virtual_address_alignment = PS4_PAGE;
   d->pte_fragment_size = PS4_PAGE;
   d->gart_page_size = 4096;
   d->vram_type = AMDGPU_VRAM_TYPE_GDDR5;
   d->vram_bit_width = 256;
   d->wave_front_size = 64;
   d->num_shader_visible_vgprs = 256;
   d->num_cu_per_sh = 9;
   d->num_tcc_blocks = 8;
   d->gs_vgt_table_depth = 32;
   d->gs_prim_buffer_depth = 1792;
   d->max_gs_waves_per_vgt = 32;
}

int
ac_drm_query_info(ac_drm_device *dev, unsigned info_id, unsigned size, void *value)
{
   memset(value, 0, size);
   switch (info_id) {
   case AMDGPU_INFO_DEV_INFO: {
      struct drm_amdgpu_info_device d;
      ps4_dev_info(dev, &d);
      memcpy(value, &d, MIN2(size, sizeof(d)));
      return 0;
   }
   case AMDGPU_INFO_MEMORY: {
      struct drm_amdgpu_memory_info m = {0};
      m.vram.total_heap_size = m.vram.usable_heap_size = PS4_VRAM_SIZE;
      m.vram.max_allocation = PS4_VRAM_SIZE;
      m.cpu_accessible_vram = m.vram;
      m.gtt.total_heap_size = m.gtt.usable_heap_size = PS4_GTT_SIZE;
      m.gtt.max_allocation = PS4_GTT_SIZE;
      memcpy(value, &m, MIN2(size, sizeof(m)));
      return 0;
   }
   case AMDGPU_INFO_VRAM_GTT: {
      struct drm_amdgpu_info_vram_gtt v = {PS4_VRAM_SIZE, PS4_VRAM_SIZE, PS4_GTT_SIZE};
      memcpy(value, &v, MIN2(size, sizeof(v)));
      return 0;
   }
   case AMDGPU_INFO_MAX_IBS: {
      uint32_t *max = value;
      if (size >= 4)
         max[AMDGPU_HW_IP_GFX] = 32;
      return 0;
   }
   case AMDGPU_INFO_TIMESTAMP:
      if (size >= 8)
         *(uint64_t *)value = os_time_get_nano() / 10; /* 100 MHz */
      return 0;
   case AMDGPU_INFO_VRAM_USAGE:
   case AMDGPU_INFO_GTT_USAGE:
   case AMDGPU_INFO_VIS_VRAM_USAGE:
   case AMDGPU_INFO_NUM_EVICTIONS:
   case AMDGPU_INFO_NUM_BYTES_MOVED:
   case AMDGPU_INFO_NUM_VRAM_CPU_PAGE_FAULTS:
      return 0;
   default:
      return -EINVAL;
   }
}

int
ac_drm_read_mm_registers(ac_drm_device *dev, unsigned dword_offset, unsigned count,
                         uint32_t instance, uint32_t flags, uint32_t *values)
{
   for (unsigned i = 0; i < count; i++) {
      const unsigned reg = dword_offset + i;
      if (reg >= 0x2644 && reg < 0x2644 + 32)
         values[i] = ps4_tile_modes[reg - 0x2644];
      else if (reg >= 0x2664 && reg < 0x2664 + 16)
         values[i] = ps4_macrotile_modes[reg - 0x2664];
      else if (reg == 0x263e)
         values[i] = PS4_GB_ADDR_CONFIG;
      else
         return -EINVAL;
   }
   return 0;
}

int
ac_drm_query_hw_ip_count(ac_drm_device *dev, unsigned type, uint32_t *count)
{
   *count = type == AMDGPU_HW_IP_GFX ? 1 : 0;
   return 0;
}

int
ac_drm_query_hw_ip_info(ac_drm_device *dev, unsigned type, unsigned ip_instance,
                        struct drm_amdgpu_info_hw_ip *info)
{
   /* Only the graphics ring: GNM's compute queues and SDMA aren't exposed. */
   if (type != AMDGPU_HW_IP_GFX)
      return -EINVAL;
   memset(info, 0, sizeof(*info));
   info->hw_ip_version_major = 7;
   info->hw_ip_version_minor = 0;
   info->ib_start_alignment = 32;
   info->ib_size_alignment = 32;
   info->available_rings = 1;
   return 0;
}

int
ac_drm_query_firmware_version(ac_drm_device *dev, unsigned fw_type, unsigned ip_instance,
                              unsigned index, uint32_t *version, uint32_t *feature)
{
   /* Recent CIK microcode. */
   *version = 0x1000;
   *feature = 0x40;
   return 0;
}

int
ac_drm_query_uq_fw_area_info(ac_drm_device *dev, unsigned type, unsigned ip_instance,
                             struct drm_amdgpu_info_uq_metadata *info)
{
   return -EINVAL;
}

int
ac_drm_query_heap_info(ac_drm_device *dev, uint32_t heap, uint32_t flags,
                       struct amdgpu_heap_info *info)
{
   memset(info, 0, sizeof(*info));
   info->heap_size = heap == AMDGPU_GEM_DOMAIN_VRAM ? PS4_VRAM_SIZE : PS4_GTT_SIZE;
   info->max_allocation = info->heap_size;
   return 0;
}

int
ac_drm_query_sensor_info(ac_drm_device *dev, unsigned sensor_type, unsigned size, void *value)
{
   return -EINVAL;
}

int
ac_drm_query_video_caps_info(ac_drm_device *dev, unsigned cap_type, unsigned size, void *value)
{
   return -EINVAL;
}

int
ac_drm_query_gpuvm_fault_info(ac_drm_device *dev, unsigned size, void *value)
{
   return -EINVAL;
}

int
ac_drm_vm_reserve_vmid(ac_drm_device *dev, uint32_t flags)
{
   return 0;
}

int
ac_drm_vm_unreserve_vmid(ac_drm_device *dev, uint32_t flags)
{
   return 0;
}

const char *
ac_drm_get_marketing_name(ac_drm_device *device)
{
   return "AMD Liverpool (PS4)";
}

int
ac_drm_query_sw_info(ac_drm_device *dev, enum amdgpu_sw_info info, void *value)
{
   if (info != amdgpu_sw_info_address32_hi)
      return -EINVAL;
   *(uint32_t *)value = dev->va32_start >> 32;
   return 0;
}

int
ac_drm_create_userqueue(ac_drm_device *dev, uint32_t ip_type, uint32_t doorbell_handle,
                        uint32_t doorbell_offset, uint64_t queue_va, uint64_t queue_size,
                        uint64_t wptr_va, uint64_t rptr_va, void *mqd_in, uint32_t flags,
                        uint32_t *queue_id)
{
   return -ENOSYS;
}

int
ac_drm_free_userqueue(ac_drm_device *dev, uint32_t queue_id)
{
   return -ENOSYS;
}

int
ac_drm_userq_signal(ac_drm_device *dev, struct drm_amdgpu_userq_signal *signal_data)
{
   return -ENOSYS;
}

int
ac_drm_userq_wait(ac_drm_device *dev, struct drm_amdgpu_userq_wait *wait_data)
{
   return -ENOSYS;
}

int
ac_drm_query_pci_bus_info(ac_drm_device *dev, struct radeon_info *info)
{
   info->pci.domain = 0;
   info->pci.bus = 0;
   info->pci.dev = 1;
   info->pci.func = 0;
   return 0;
}

void
ac_drm_query_has_vm_always_valid(ac_drm_device *dev, struct radeon_info *info)
{
   info->has_vm_always_valid = true;
}

/* ---------------------------------------------------------------------------------------------
 * libdrm entry points referenced by RADV's DRM-device paths, which never run on the PS4.
 */

#include <xf86drm.h>

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   return -EINVAL;
}

drmVersionPtr
drmGetVersion(int fd)
{
   return NULL;
}

void
drmFreeVersion(drmVersionPtr version)
{
}

char *
drmGetFormatModifierName(uint64_t modifier)
{
   return NULL;
}

/* Trace points in RADV's startup (PS4_TRACE in the patched Mesa sources). */
void
ac_ps4_trace(const char *fmt, ...)
{
   char line[256];
   va_list args;
   va_start(args, fmt);
   vsnprintf(line, sizeof(line), fmt, args);
   va_end(args);
   ps4_log("%s", line);
}

/* For the app's performance monitor: the last device's submissions and GPU memory. */

void
ac_ps4_get_stats(uint64_t *submitted, uint64_t *completed, uint64_t *bo_bytes, uint64_t *bo_count)
{
   ac_drm_device *dev = ps4_last_device;
   *submitted = dev ? dev->last_seq : 0;
   *completed = dev ? ps4_completed(dev) : 0;
   *bo_bytes = dev ? dev->bo_bytes : 0;
   *bo_count = dev ? dev->bo_count : 0;
}
