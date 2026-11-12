/* xbox-core: QEMU's dirty-page tracking for xemu's NV2A. QEMU learns which pages the CPU wrote from
 * its translator (or a hypervisor's dirty log); the CPU here runs natively with nothing recording its
 * writes. So a page counts as dirty for a client when its contents differ from what they were the
 * last time that client asked (an XXH3 hash per 4 KiB page and client), or when something marked it
 * (memory_region_set_dirty / set_client_dirty: the GPU's own writes, or a new mapping). */
#include "hw/pci/pci.h"
#include <pthread.h>
#define XXH_INLINE_ALL
#include "xxhash.h"

/* the contiguous memory's dirty log (dirty.cpp), where the CPU's self-test switched it on: the
 * pages written since a reader last looked, from write faults, without reading them */
bool xb_dirty_on(void);
bool xb_dirty_test_and_clear(uint64_t offset, uint64_t size, unsigned reader);
void xb_dirty_mark(uint64_t offset, uint64_t size, unsigned reader_mask);
#define XB_CONTIGUOUS ((uint8_t *)(uintptr_t)0x80000000u)

static bool logged(MemoryRegion *mr)
{
    return mr->ram == XB_CONTIGUOUS && xb_dirty_on();
}

#define PAGE 4096
#define CLIENTS DIRTY_MEMORY_NUM

typedef struct Tracked {
    MemoryRegion *mr;
    uint64_t pages;
    uint64_t *hash[CLIENTS];
    uint8_t *marked[CLIENTS];
} Tracked;

static Tracked g_tracked[4];
static int g_count;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static Tracked *tracked(MemoryRegion *mr)
{
    for (int i = 0; i < g_count; i++) {
        if (g_tracked[i].mr == mr) {
            return &g_tracked[i];
        }
    }
    if (g_count == 4 || !mr->ram) {
        return NULL;
    }
    Tracked *t = &g_tracked[g_count++];
    t->mr = mr;
    t->pages = (mr->size + PAGE - 1) / PAGE;
    for (int c = 0; c < CLIENTS; c++) {
        t->hash[c] = calloc(t->pages, sizeof(uint64_t));
        t->marked[c] = malloc(t->pages);
        memset(t->marked[c], 1, t->pages); /* all dirty at first */
    }
    return t;
}

static void range(Tracked *t, uint64_t addr, uint64_t size, uint64_t *first, uint64_t *end)
{
    *first = addr / PAGE;
    *end = size ? (addr + size + PAGE - 1) / PAGE : *first;
    if (*end > t->pages) {
        *end = t->pages;
    }
}

void memory_region_set_client_dirty(MemoryRegion *mr, uint64_t addr, uint64_t size, unsigned client)
{
    if (logged(mr)) {
        xb_dirty_mark(addr, size, 1u << client);
        return;
    }
    pthread_mutex_lock(&g_lock);
    Tracked *t = tracked(mr);
    if (t && client < CLIENTS) {
        uint64_t p, end;
        range(t, addr, size, &p, &end);
        for (; p < end; p++) {
            t->marked[client][p] = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void memory_region_set_dirty(MemoryRegion *mr, uint64_t addr, uint64_t size)
{
    for (unsigned c = 0; c < CLIENTS; c++) {
        memory_region_set_client_dirty(mr, addr, size, c);
    }
}

bool memory_region_test_and_clear_dirty(MemoryRegion *mr, uint64_t addr, uint64_t size, unsigned client)
{
    if (logged(mr)) {
        return xb_dirty_test_and_clear(addr, size, client);
    }
    bool dirty = false;
    pthread_mutex_lock(&g_lock);
    Tracked *t = tracked(mr);
    if (!t || client >= CLIENTS) {
        pthread_mutex_unlock(&g_lock);
        return true;
    }
    uint64_t p, end;
    range(t, addr, size, &p, &end);
    for (; p < end; p++) {
        const uint64_t h = XXH3_64bits(mr->ram + p * PAGE, PAGE);
        if (t->marked[client][p] || t->hash[client][p] != h) {
            dirty = true;
        }
        t->marked[client][p] = 0;
        t->hash[client][p] = h;
    }
    pthread_mutex_unlock(&g_lock);
    return dirty;
}
