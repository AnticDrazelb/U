/* Host test of ac_ps4_drm.c's user-memory buffers (VK_EXT_external_memory_host), driven in the
 * order radv_amdgpu_winsys_bo_from_ptr / _bo_destroy call the winsys. Sony calls are stubbed. */
#include "ac_ps4_drm.c"
#include <assert.h>

static int n_alloc, n_release, n_map, n_munmap, n_reserve;
static void *last_munmap;
int sceKernelAllocateDirectMemory(off_t s, off_t e, size_t len, size_t align, int type, off_t *phys)
{ static off_t next = 0x1000000; *phys = next; next += (len + align - 1) / align * align + align; n_alloc++; return 0; }
int sceKernelReleaseDirectMemory(off_t start, size_t len) { n_release++; return 0; }
#include <sys/mman.h>
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, off_t phys, size_t align)
{
   n_map++;
   if (!(flags & PS4_MAP_FIXED)) { *addr = aligned_alloc(16384, (len + 16383) & ~16383ull); return 0; }
   void *m = mmap(*addr, len, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   return m == *addr ? 0 : -1;
}
int sceKernelReserveVirtualRange(void **addr, size_t len, int flags, size_t align) { n_reserve++; return 0; }
int sceKernelMunmap(void *addr, size_t len) { n_munmap++; last_munmap = addr; return munmap(addr, len); }
size_t sceKernelGetDirectMemorySize(void) { return 8ull << 30; }
int sceKernelUsleep(unsigned int usec) { return 0; }
const char *sceKernelGetFsSandboxRandomWord(void) { return "test"; }
int sceKernelLoadStartModule(const char *p, size_t a, const void *b, unsigned f, void *o, int *r) { return -1; }
int sceKernelDlsym(int h, const char *s, void **a) { return -1; }
int sceKernelDebugOutText(int c, const char *t) { return 0; }
int sceKernelCreateEqueue(void **eq, const char *n) { return -1; }
int sceKernelWaitEqueue(void *eq, void *ev, int num, int *out, unsigned *t) { return -1; }
int sceKernelVirtualQuery(const void *a, int f, struct ps4_vq_info *i, size_t s) { return -1; }

void ac_parse_ib(struct ac_ib_parser *ib, const char *name) { }
void ps4_boost_thread(void) { }
void ps4_boot_trace(const char *s) { }
int sceKernelGetModuleList(int *a, size_t n, size_t *o) { if (o) *o = 0; return -1; }

int main(void)
{
   static uint64_t fence = ~0ull >> 1; /* everything submitted has completed */
   ac_drm_device *dev = calloc(1, sizeof(*dev));
   simple_mtx_init(&dev->lock, mtx_plain);
   dev->fence = &fence;
   /* the GPU VA windows (on the console below 1 TiB; here above AddressSanitizer's shadow) */
   dev->va_start = 0x200000000000ull;
   dev->va32_start = 0x300000000000ull;
   util_vma_heap_init(&dev->va_heap, dev->va_start, PS4_VA_SIZE);
   util_vma_heap_init(&dev->va32_heap, dev->va32_start, PS4_VA32_SIZE);
   int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

   /* --- 1. a normal buffer still works (refactored handle registration) --- */
   struct amdgpu_bo_alloc_request req = {.alloc_size = 65536, .preferred_heap = AMDGPU_GEM_DOMAIN_GTT};
   ac_drm_bo nb;
   CHECK(ac_drm_bo_alloc(dev, &req, &nb) == 0);
   uint64_t nva; amdgpu_va_handle nvh;
   CHECK(ac_drm_va_range_alloc(dev, amdgpu_gpu_va_range_general, 65536, 4096, 0, &nva, &nvh, AMDGPU_VA_RANGE_HIGH) == 0);
   CHECK(nva >= dev->va_start && nva < dev->va_start + PS4_VA_SIZE && nvh != NULL);
   uint32_t nh; CHECK(ac_drm_bo_export(dev, nb, amdgpu_bo_handle_type_kms, &nh) == 0 && nh == nb.abo->handle);
   const int maps_before = n_map;
   CHECK(ac_drm_bo_va_op(dev, nh, 0, 65536, nva, 0, AMDGPU_VA_OP_MAP) == 0);
   CHECK(n_map == maps_before + 1); /* a real mapping */

   /* --- 2. user memory: the Wii U's MEM2 at 0x40_1000_0000 (MemMapper's placement), 1 GiB --- */
   void *guest = (void *)(uintptr_t)0x4010000000ull;
   const uint64_t gsize = 0x40000000ull;
   ac_drm_bo ub;
   CHECK(ac_drm_create_bo_from_user_mem(dev, guest, gsize, &ub) == 0);
   CHECK(ub.abo && ub.abo->user_mem && ub.abo->cpu == guest && ub.abo->phys == -1);
   CHECK(ub.abo->handle != nb.abo->handle);
   const uint64_t bytes_before = dev->bo_bytes;
   uint64_t uva = 0; amdgpu_va_handle uvh = (amdgpu_va_handle)1;
   /* radv: va_range_alloc(size, optimal alignment, 0, ..., AMDGPU_VA_RANGE_HIGH) */
   CHECK(ac_drm_va_range_alloc(dev, amdgpu_gpu_va_range_general, gsize, 1 << 20, 0, &uva, &uvh, AMDGPU_VA_RANGE_HIGH) == 0);
   CHECK(uva == (uintptr_t)guest);   /* GPU address = CPU address */
   CHECK(uvh == NULL);
   uint32_t uh; CHECK(ac_drm_bo_export(dev, ub, amdgpu_bo_handle_type_kms, &uh) == 0);
   const int maps_before_user = n_map, munmaps_before = n_munmap;
   CHECK(ac_drm_bo_va_op(dev, uh, 0, gsize, uva, 0, AMDGPU_VA_OP_MAP) == 0);
   CHECK(n_map == maps_before_user);  /* nothing mapped: the memory is already the app's */
   void *cpu = NULL; CHECK(ac_drm_bo_cpu_map(dev, ub, &cpu) == 0 && cpu == guest);
   /* a MAP at any other address is refused */
   CHECK(ac_drm_bo_va_op(dev, uh, 0, gsize, uva + 0x10000, 0, AMDGPU_VA_OP_MAP) != 0);
   /* the next normal allocation isn't given the user address */
   uint64_t nva2; amdgpu_va_handle nvh2;
   CHECK(ac_drm_va_range_alloc(dev, amdgpu_gpu_va_range_general, gsize, 1 << 20, 0, &nva2, &nvh2, AMDGPU_VA_RANGE_HIGH) == 0);
   CHECK(nva2 != (uintptr_t)guest && nvh2 != NULL);
   CHECK(dev->bo_bytes == bytes_before);

   /* --- 3. destroy (radv_amdgpu_winsys_bo_destroy): UNMAP, va_range_free, bo_free --- */
   const int releases_before = n_release;
   CHECK(ac_drm_bo_va_op(dev, uh, 0, gsize, uva, 0, AMDGPU_VA_OP_UNMAP) == 0);
   CHECK(ac_drm_va_range_free(uvh) == 0);
   CHECK(ac_drm_bo_free(dev, ub) == 0);
   simple_mtx_lock(&dev->lock);
   ps4_run_deferred_ex(dev, 0); /* past the quarantine */
   simple_mtx_unlock(&dev->lock);
   CHECK(n_munmap == munmaps_before);       /* the guest memory was never unmapped */
   CHECK(n_release == releases_before);     /* nor its (not ours) direct memory released */
   CHECK(ps4_bo(dev, uh) == NULL);          /* the handle is gone */

   /* --- 4. the normal buffer's destroy still unmaps and releases --- */
   CHECK(ac_drm_bo_va_op(dev, nh, 0, 65536, nva, 0, AMDGPU_VA_OP_UNMAP) == 0);
   CHECK(ac_drm_va_range_free(nvh) == 0);
   CHECK(ac_drm_bo_free(dev, nb) == 0);
   simple_mtx_lock(&dev->lock);
   ps4_run_deferred_ex(dev, 0);
   simple_mtx_unlock(&dev->lock);
   CHECK(n_munmap == munmaps_before + 1 && last_munmap == (void *)(uintptr_t)nva);
   CHECK(n_release == releases_before + 1);

   /* --- 5. refusals: unaligned, inside the GPU VA window, beyond the GPU's range --- */
   ac_drm_bo bad;
   CHECK(ac_drm_create_bo_from_user_mem(dev, (void *)0x4010000100ull, 65536, &bad) == -EINVAL);
   CHECK(ac_drm_create_bo_from_user_mem(dev, (void *)(uintptr_t)(dev->va_start + 0x100000), 65536, &bad) == -EINVAL);
   CHECK(ac_drm_create_bo_from_user_mem(dev, (void *)(uintptr_t)(PS4_GPU_VA_LIMIT - 16384), 65536, &bad) == -EINVAL);
   /* a refused import leaves no pending address behind */
   uint64_t nva3; amdgpu_va_handle nvh3;
   CHECK(ac_drm_va_range_alloc(dev, amdgpu_gpu_va_range_general, 65536, 4096, 0, &nva3, &nvh3, AMDGPU_VA_RANGE_HIGH) == 0 && nvh3 != NULL);

   printf("%s (%d failures)\n", fails ? "FAILED" : "all checks passed", fails);
   return fails != 0;
}
