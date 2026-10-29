/* dlmalloc 2.8.6 (Doug Lea, MIT-0) configured as the PS4 app heap: only the mspace API (no
 * global malloc symbols, so nothing clashes with the OpenOrbis libc), over a memory block we
 * map ourselves (no mmap/sbrk), with locks and footers. Corruption and bad frees are reported
 * through ps4_heap_error() in ps4_runtime.cpp with the operation and caller that hit them.
 *
 * Replaces Sony's sceLibcMspace, which stopped returning memory after ~800 small allocations
 * in Dolphin's static initialisation with 49 KB in use on a 1 GiB heap. */

#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
/* Blocking locks, not dlmalloc's spin-and-sched_yield ones: a waiter of higher priority only
 * yields to its equals, so a lower priority thread holding the lock never got to release it
 * (Crema 1.61: every busy thread stuck in mspace_malloc behind a recompiler worker). */
#define USE_SPIN_LOCKS 0
#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define HAVE_MORECORE 0
#define FOOTERS 1
#define INSECURE 0
#define USE_DEV_RANDOM 0
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define LACKS_SCHED_H 0
/* The PS4 page size; sysconf numbering differs between headers, so don't ask. */
#define malloc_getpagesize ((size_t)16384U)
#define DEFAULT_GRANULARITY ((size_t)64U * (size_t)1024U)

void ps4_heap_error(void* mspace, void* chunk, int corruption);
#define CORRUPTION_ERROR_ACTION(m) ps4_heap_error((m), 0, 1)
#define USAGE_ERROR_ACTION(m, p) ps4_heap_error((m), (p), 0)

#include "third_party/dlmalloc/malloc.c"

/* Integrity check for diagnostics: is the mspace's state still intact? */
int ps4_heap_ok(void* msp) {
  mstate ms = (mstate)msp;
  return ok_magic(ms);
}

/* Bytes in use in an mspace (chunks not free; dlmalloc's mallinfo uordblks, which NO_MALLINFO
 * leaves out), and its size. Walks every chunk: for diagnostics, a few times per program. */
void ps4_heap_in_use(void* msp, size_t* in_use, size_t* size) {
  mstate m = (mstate)msp;
  *in_use = *size = 0;
  if (!ok_magic(m) || PREACTION(m))
    return;
  if (is_initialized(m)) {
    size_t mfree = m->topsize + TOP_FOOT_SIZE;
    msegmentptr s = &m->seg;
    while (s != 0) {
      mchunkptr q = align_as_chunk(s->base);
      while (segment_holds(s, q) && q != m->top && q->head != FENCEPOST_HEAD) {
        if (!is_inuse(q))
          mfree += chunksize(q);
        q = next_chunk(q);
      }
      s = s->next;
    }
    *in_use = m->footprint - mfree;
    *size = m->footprint;
  }
  POSTACTION(m);
}
