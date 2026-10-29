// PS4 runtime fixes, based on love-ps4 (src/common/ps4_heap.cpp and ps4.cpp), which runs on the
// test console:
//
// - The OpenOrbis libc creates its heap on the first malloc by mapping 2.5 GiB of system
//   flexible memory; that fails on retail consoles and malloc then crashes at address 0x38,
//   before main(). The malloc family is redirected here with the linker's --wrap
//   (toolchain/ps4-love-style.cmake).
// - The heap is dlmalloc (port/ps4_dlmalloc.c) over a block of memory mapped here. Sony's
//   sceLibcMspace (what love-ps4 uses) stopped returning memory in Dolphin's static
//   initialisation after ~800 small allocations; dlmalloc also checks for corruption and
//   foreign frees, which are logged with the caller.
// - libc++abi runs thread_local destructors through __cxa_thread_atexit_impl, which the PS4
//   libc lacks (and create-fself refuses unresolved imports).

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include <atomic>
#include <thread>
#include <memory>
#include <immintrin.h>
#include <sys/types.h>

// Declared by hand: the toolchain headers give these the wrong (or no) prototypes.
extern "C" {
int32_t sceKernelDebugOutText(int32_t channel, const char* fmt, ...);
int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
int32_t sceKernelReserveVirtualRange(void** addr, size_t len, int32_t flags, size_t alignment);
int32_t sceKernelMunmap(void* addr, size_t len);
int32_t sceKernelMapNamedFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags, const char* name);
int32_t sceKernelMapNamedSystemFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags,
                                              const char* name);
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int32_t type,
                                      off_t* physOut);
int32_t sceKernelMapDirectMemory(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys,
                                 size_t align);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t len);
struct SceKernelSwVersion {
    size_t size;
    char text[0x1C];
    uint32_t version;
};
int32_t sceKernelGetSystemSwVersion(SceKernelSwVersion* version);

// dlmalloc mspace API (port/ps4_dlmalloc.c).
typedef void* mspace;
mspace create_mspace_with_base(void* base, size_t capacity, int locked);
void* mspace_malloc(mspace msp, size_t bytes);
void mspace_free(mspace msp, void* mem);
void* mspace_calloc(mspace msp, size_t n_elements, size_t elem_size);
void* mspace_realloc(mspace msp, void* mem, size_t newsize);
void* mspace_memalign(mspace msp, size_t alignment, size_t bytes);
size_t mspace_usable_size(const void* mem);
size_t mspace_footprint(mspace msp);
size_t mspace_max_footprint(mspace msp);
void* memcpy(void* dst, const void* src, size_t n);

// port/ps4_crashlog.cpp (weak: a program may link without it).
__attribute__((weak)) void ps4_boot_trace(const char* stage);
int ps4_heap_ok(void* msp);  // port/ps4_dlmalloc.c

extern char __text_start[];  // OpenOrbis link.x: start of .text (ELF address 0)
}

namespace {

const size_t MB = 1024 * 1024;
const size_t PAGE = 16 * 1024;
// Regular flexible memory left for Piglet (OpenGL ES) when the heap has to come from there.
const size_t RESERVE_FOR_SYSTEM = 320 * MB;
#ifndef PS4_SYSTEM_HEAP_MAX_MB
#define PS4_SYSTEM_HEAP_MAX_MB 512  // the largest heap taken from the system flexible pool
#endif
const int32_t PROT_CPU_RW = 0x3;
const int32_t MAP_FIXED_FLAG = 0x10;

mspace g_heap = nullptr;
const char* g_heapPool = "none";
uintptr_t g_heapBase = 0;
size_t g_heapSize = 0;

// sceKernelDebugOutText doesn't format its arguments; vsnprintf doesn't allocate.
__attribute__((format(printf, 1, 2))) void heapLog(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    sceKernelDebugOutText(0, line);
    if (ps4_boot_trace)
        ps4_boot_trace(line);
}

unsigned long long elf(const void* address) {
    return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                           reinterpret_cast<uintptr_t>(__text_start));
}

// Logs the code addresses on the current stack (no frame pointers in release builds).
void logStack() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(__text_start);
    const auto* stack = static_cast<const uintptr_t*>(__builtin_frame_address(0));
    int found = 0;
    for (int i = 0; i < 512 && found < 16; i++) {
        if (stack[i] >= base && stack[i] < base + 0x1000000) {
            heapLog("[cemu] heap:   stack elf 0x%llx\n", static_cast<unsigned long long>(stack[i] - base));
            found++;
        }
    }
}

mspace createMspace(const char* pool, void* base, size_t size) {
    mspace msp = create_mspace_with_base(base, size, 1);
    if (!msp) {
        heapLog("[cemu] heap: create_mspace_with_base on %zu MiB of %s failed\n", size / MB, pool);
        return nullptr;
    }
    void* test = mspace_malloc(msp, 4096);
    if (!test) {
        heapLog("[cemu] heap: %zu MiB of %s at %p: self-test allocation failed\n", size / MB, pool, base);
        return nullptr;
    }
    mspace_free(msp, test);
    g_heapPool = pool;
    g_heapBase = reinterpret_cast<uintptr_t>(base);
    g_heapSize = size;
    heapLog("[cemu] heap: dlmalloc on %zu MiB of %s at %p\n", size / MB, pool, base);
    return msp;
}

// The PS4 kernel library allocates its own objects (pthread attributes, ...) at 0x200000000,
// where a kernel-placed reservation of ours also lands; those allocations wiped the heap's
// control block. Ask for an address well above that instead.
void* reserveAwayFromKernel(size_t size) {
    const uintptr_t hints[] = {0x1000000000ull, 0x800000000ull, 0x600000000ull};
    for (uintptr_t hint : hints) {
        void* address = reinterpret_cast<void*>(hint);
        if (sceKernelReserveVirtualRange(&address, size, 0, PAGE) == 0) {
            if (reinterpret_cast<uintptr_t>(address) >= 0x400000000ull)
                return address;
            heapLog("[cemu] heap: reservation hint %p gave %p, trying another\n", reinterpret_cast<void*>(hint),
                    address);
        }
    }
    return nullptr;
}

mspace mapAndCreate(bool systemPool, size_t size) {
    // Reserve one extra page in front of the heap and leave it inaccessible: a stray write
    // just before the heap's control block then faults where it happens (see crash.log).
    void* guard = reserveAwayFromKernel(size + PAGE);
    if (!guard)
        return nullptr;
    void* base = static_cast<char*>(guard) + PAGE;
    int32_t ret = systemPool
                      ? sceKernelMapNamedSystemFlexibleMemory(&base, size, PROT_CPU_RW, MAP_FIXED_FLAG, "cemu heap")
                      : sceKernelMapNamedFlexibleMemory(&base, size, PROT_CPU_RW, MAP_FIXED_FLAG, "cemu heap");
    if (ret != 0) {
        heapLog("[cemu] heap: mapping %zu MiB of %s flexible memory failed (0x%x)\n", size / MB,
                systemPool ? "system" : "regular", ret);
        return nullptr;  // the leaked VA reservation doesn't matter in a 47-bit address space
    }
    return createMspace(systemPool ? "system flexible memory" : "flexible memory", base, size);
}

// Last resort (PSChrome's heap): direct memory.
mspace createDirectHeap(size_t size) {
    off_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 2 * MB, 0, &phys) != 0)
        return nullptr;
    void* base = nullptr;
    if (sceKernelMapDirectMemory(&base, size, PROT_CPU_RW, 0, phys, 2 * MB) != 0)
        return nullptr;
    return createMspace("direct memory", base, size);
}

mspace createHeap() {
    size_t available = 0;
    int32_t ret = sceKernelAvailableFlexibleMemorySize(&available);
    heapLog("[cemu] heap: flexible memory available %zu MiB (0x%x)\n", available / MB, ret);
    // Where does the kernel library put its own objects? (8 bytes reserved even if the
    // headers' pthread_mutexattr_t is the 4-byte musl one.)
    void* attr[2] = {nullptr, nullptr};
    if (pthread_mutexattr_init(reinterpret_cast<pthread_mutexattr_t*>(attr)) == 0) {
        heapLog("[cemu] heap: kernel pthread objects live around %p\n", attr[0]);
        pthread_mutexattr_destroy(reinterpret_cast<pthread_mutexattr_t*>(attr));
    }
    // Preferred: the system flexible memory pool, leaving regular flexible memory to Piglet.
    // Not all of it: the kernel's own objects ("ScePthread internal memory") and the system libc
    // heap that Sony modules (Piglet's shader compiler) grow on demand come from the same pool.
    // With a 1 GiB heap, compiling Dolphin's larger shaders ran the system out of memory.
    // An app that needs that pool's executable memory too caps the heap lower (PS4_SYSTEM_HEAP_MAX_MB:
    // Eden's code caches).
    const size_t systemSizes[] = {512 * MB, 384 * MB, 256 * MB, 192 * MB, 128 * MB};
    for (size_t size : systemSizes) {
        if (size > PS4_SYSTEM_HEAP_MAX_MB * MB)
            continue;
        if (mspace msp = mapAndCreate(true, size))
            return msp;
    }
    size_t size = 0;
    if (ret == 0 && available > RESERVE_FOR_SYSTEM + 32 * MB)
        size = (available - RESERVE_FOR_SYSTEM) & ~(PAGE - 1);
    for (; size >= 32 * MB; size = (size / 2) & ~(PAGE - 1)) {
        if (mspace msp = mapAndCreate(false, size))
            return msp;
    }
    const size_t directSizes[] = {256 * MB, 128 * MB};
    for (size_t direct : directSizes) {
        if (mspace msp = createDirectHeap(direct))
            return msp;
    }
    heapLog("[cemu] heap: could not create a heap, out of memory\n");
    return nullptr;
}

bool g_creatingHeap = false;
// The overflow heaps' lock (below): a blocking one, as dlmalloc's (port/ps4_dlmalloc.c: a spinning
// waiter of higher priority starves the holder), initialized with the heap (initializing it on
// first use could allocate from the full heap it is there to extend).
pthread_mutex_t g_overflowLock;

inline mspace getHeap() {
    // The first allocation happens in a static constructor, before any threads exist. While the
    // heap is being created, nested allocations (if a kernel call allocates) just fail.
    if (g_heap == nullptr && !g_creatingHeap) {
        g_creatingHeap = true;
        g_heap = createHeap();
        g_creatingHeap = false;
        if (g_heap)
            pthread_mutex_init(&g_overflowLock, nullptr);
    }
    return g_heap;
}

// Declared by hand: the SDK passes its output arguments by value.
extern "C" int32_t ps4QueryDirectMemory(off_t start, off_t end, size_t align, off_t* physOut, size_t* sizeOut)
    asm("sceKernelAvailableDirectMemorySize");

// Overflow heaps: mapped when the main heap can't serve a request. Spider-Man 2 fills most of the
// 512 MiB main heap; a save state then needed one 99 MiB block (148 MiB in Mario Kart: Double
// Dash!!), the allocation failed and the app crashed. With FOOTERS, free and realloc find each
// chunk's heap from the chunk itself.
//  - By default (Crema): one heap of direct memory, a separate 4.5 GiB pool (the GPU's), as large
//    as it can be.
//  - With a GPU reserve (ps4_heap_set_overflow_gpu_reserve: Eden for PS4, whose 4 GiB of guest RAM
//    leaves about 480 MiB of direct memory for the GPU): heaps added in small steps, from regular
//    flexible memory (which nothing else there uses) first, then from direct memory only while
//    the reserve stays free. One large direct heap left Eden4's Vulkan driver 320 KiB of direct
//    memory in Clubhouse Games, and it failed.
struct OverflowHeap {
    mspace msp;
    uintptr_t base;
    size_t size;
};
constexpr int MAX_OVERFLOW_HEAPS = 32;
constexpr size_t FLEXIBLE_KEEP = 64 * MB;  // regular flexible memory left for the system's libraries
OverflowHeap g_overflows[MAX_OVERFLOW_HEAPS];
int g_overflowCount = 0;  // published with release once its entry is written
bool g_overflowTried = false;  // the default's one heap
size_t g_overflowGpuReserve = 0;
__attribute__((tls_model("initial-exec"))) thread_local bool t_inOverflow = false;

bool addOverflow(void* base, size_t size, const char* pool) {
    mspace msp = create_mspace_with_base(base, size, 1);
    if (!msp)
        return false;
    g_overflows[g_overflowCount] = {msp, reinterpret_cast<uintptr_t>(base), size};
    __atomic_store_n(&g_overflowCount, g_overflowCount + 1, __ATOMIC_RELEASE);
    heapLog("[cemu] heap: overflow heap %d: %zu MiB of %s at %p\n", g_overflowCount, size / MB, pool, base);
    return true;
}

// Direct memory for an overflow heap, mapped well above 0x200000000 (where the kernel library
// puts its own objects), each heap in its own 1 GiB.
bool addDirectOverflow(size_t size) {
    off_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 2 * MB, 0, &phys) != 0)
        return false;
    void* base = reinterpret_cast<void*>(0x1800000000ull + static_cast<uintptr_t>(g_overflowCount) * 0x40000000ull);
    const int32_t ret = sceKernelMapDirectMemory(&base, size, PROT_CPU_RW, 0, phys, 2 * MB);
    if (ret != 0 || reinterpret_cast<uintptr_t>(base) < 0x400000000ull) {
        heapLog("[cemu] heap: overflow heap: mapping %zu MiB of direct memory failed (0x%x, %p)\n", size / MB,
                ret, base);
        if (ret == 0)
            sceKernelMunmap(base, size);
        sceKernelReleaseDirectMemory(phys, size);
        return false;
    }
    if (addOverflow(base, size, "direct memory"))
        return true;
    sceKernelMunmap(base, size);
    sceKernelReleaseDirectMemory(phys, size);
    return false;
}

// With a GPU reserve: one more heap of `size` bytes.
bool addOverflowStep(size_t size) {
    size_t flexible = 0;
    if (sceKernelAvailableFlexibleMemorySize(&flexible) == 0 && flexible >= FLEXIBLE_KEEP + size) {
        void* base = nullptr;
        if (sceKernelMapNamedFlexibleMemory(&base, size, PROT_CPU_RW, 0, "heap overflow") == 0) {
            if (addOverflow(base, size, "regular flexible memory"))
                return true;
            sceKernelMunmap(base, size);
        }
    }
    off_t start = 0;
    size_t largest = 0;
    const int32_t query = ps4QueryDirectMemory(0, sceKernelGetDirectMemorySize(), 2 * MB, &start, &largest);
    if (query != 0 || largest < g_overflowGpuReserve + size) {
        heapLog("[cemu] heap: no memory for a %zu MiB overflow heap (regular flexible %zu MiB free, largest "
                "free direct block %zu MiB, %zu MiB of it kept for the GPU)\n",
                size / MB, flexible / MB, largest / MB, g_overflowGpuReserve / MB);
        return false;
    }
    return addDirectOverflow(size);
}

// `alloc` on each overflow heap in turn; when they are all full, one more is added (by default
// only the first time).
template <typename F>
void* overflowAlloc(size_t need, F alloc) {
    const int count = __atomic_load_n(&g_overflowCount, __ATOMIC_ACQUIRE);
    for (int i = count - 1; i >= 0; i--) {  // the newest has the most room
        if (void* p = alloc(g_overflows[i].msp))
            return p;
    }
    // An allocation made while this thread adds a heap (a kernel call's) must not wait for itself.
    if (t_inOverflow)
        return nullptr;
    t_inOverflow = true;
    pthread_mutex_lock(&g_overflowLock);
    void* result = nullptr;
    // Another thread may have added one meanwhile.
    for (int i = count; i < g_overflowCount && !result; i++)
        result = alloc(g_overflows[i].msp);
    if (!result && g_overflowGpuReserve) {
        // Room for the request and dlmalloc's own bookkeeping: at least 32 MiB.
        const size_t step = need > 29 * MB ? (need + 3 * MB + 2 * MB - 1) & ~(2 * MB - 1) : 32 * MB;
        if (g_overflowCount >= MAX_OVERFLOW_HEAPS)
            heapLog("[cemu] heap: all %d overflow heaps full\n", MAX_OVERFLOW_HEAPS);
        else if (addOverflowStep(step))
            result = alloc(g_overflows[g_overflowCount - 1].msp);
    } else if (!result && !g_overflowTried) {
        g_overflowTried = true;
        const size_t sizes[] = {512 * MB, 384 * MB, 256 * MB, 128 * MB};
        bool added = false;
        for (size_t size : sizes) {
            if ((added = addDirectOverflow(size)))
                break;
        }
        if (added)
            result = alloc(g_overflows[g_overflowCount - 1].msp);
        else
            heapLog("[cemu] heap: main heap full and no direct memory for an overflow heap\n");
    }
    pthread_mutex_unlock(&g_overflowLock);
    t_inOverflow = false;
    return result;
}

bool ownsPointer(const void* p) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    if (address >= g_heapBase && address < g_heapBase + g_heapSize)
        return true;
    const int count = __atomic_load_n(&g_overflowCount, __ATOMIC_ACQUIRE);
    for (int i = 0; i < count; i++) {
        if (address >= g_overflows[i].base && address < g_overflows[i].base + g_overflows[i].size)
            return true;
    }
    return false;
}

// The operation in progress, for error reports (thread-local: allocations happen on any thread).
// initial-exec: the executable's own TLS, so access never goes through __tls_get_addr.
__attribute__((tls_model("initial-exec"))) thread_local const char* t_op = "?";
__attribute__((tls_model("initial-exec"))) thread_local const void* t_caller = nullptr;
int g_errors = 0;

// Per-thread arenas. One locked heap made Dolphin's busy threads (CPU, video, Vulkan submission,
// shader compilers) contend: dlmalloc's spin lock then yields to the kernel, and in Ultimate
// Spider-Man the video thread spent ~9% (likely more) of its time there. Each of those threads
// gets a private dlmalloc arena carved out of the main heap; with FOOTERS every chunk records
// its arena, so free/realloc from any thread return memory to the right one. A full arena falls
// back to the main heap.
constexpr size_t ARENA_SIZE = 10 * MB;
constexpr int MAX_ARENAS = 20;
__attribute__((tls_model("initial-exec"))) thread_local mspace t_arena = nullptr;
std::atomic<int> g_arenaCount{0};
// Arenas of threads that have exited, for the next busy thread (the audio thread is recreated
// every time the in-game menu closes, the shader compilers when their count changes): without
// reuse each restart took a fresh 8 MiB until MAX_ARENAS ran out.
mspace g_freeArenas[MAX_ARENAS];
int g_freeArenaCount = 0;
std::atomic_flag g_freeArenaLock = ATOMIC_FLAG_INIT;
pthread_key_t g_arenaKey;
std::atomic<bool> g_arenaKeyReady{false};

void lockFreeArenas() {
    while (g_freeArenaLock.test_and_set(std::memory_order_acquire))
        sched_yield();
}
void unlockFreeArenas() { g_freeArenaLock.clear(std::memory_order_release); }

// Thread exit (pthread key destructor): its arena goes back on the free list. Chunks other
// threads still hold stay valid - FOOTERS returns them to this arena when freed.
void releaseArena(void* arena) {
    if (!arena)
        return;
    lockFreeArenas();
    if (g_freeArenaCount < MAX_ARENAS)
        g_freeArenas[g_freeArenaCount++] = static_cast<mspace>(arena);
    unlockFreeArenas();
}

// Checks the heap's control block before every operation; the first time it is found
// overwritten, logs when (operation number, current and previous caller) and what it now holds.
// (per thread: shared counters written by every allocation made the cores fight over one cache
// line, Crema 1.50 and before)
__attribute__((tls_model("initial-exec"))) thread_local unsigned long long g_ops = 0;
__attribute__((tls_model("initial-exec"))) thread_local const char* g_prevOp = "none";
__attribute__((tls_model("initial-exec"))) thread_local const void* g_prevCaller = nullptr;
bool g_heapBroken = false;

void checkHeap(const char* op, const void* caller) {
    g_ops++;
    if (g_heap && !g_heapBroken && !ps4_heap_ok(g_heap)) {
        g_heapBroken = true;
        heapLog("[cemu] heap: CONTROL BLOCK OVERWRITTEN, noticed at operation #%llu (%s from elf 0x%llx); "
                "previous operation %s from elf 0x%llx\n",
                g_ops, op, elf(caller), g_prevOp, elf(g_prevCaller));
        const auto* words = reinterpret_cast<const unsigned long long*>(g_heapBase);
        for (int i = 0; i < 16; i += 4)
            heapLog("[cemu] heap:   +0x%02x: %016llx %016llx %016llx %016llx\n", i * 8, words[i], words[i + 1],
                    words[i + 2], words[i + 3]);
        logStack();
    }
    g_prevOp = op;
    g_prevCaller = caller;
}

void reportFailure(const char* what, size_t size) {
    if (g_errors++ < 8) {
        heapLog("[cemu] heap: %s(%zu) failed, caller elf 0x%llx\n", what, size, elf(t_caller));
        logStack();
    }
}

struct ThreadDtor {
    void (*dtor)(void*);
    void* obj;
    ThreadDtor* next;
};
pthread_key_t g_threadDtorKey;
pthread_once_t g_threadDtorOnce = PTHREAD_ONCE_INIT;

void runThreadDtors(void* p) {
    auto* head = static_cast<ThreadDtor*>(p);
    while (head) {
        ThreadDtor* next = head->next;
        head->dtor(head->obj);
        free(head);
        head = next;
    }
}

void createThreadDtorKey() { pthread_key_create(&g_threadDtorKey, runThreadDtors); }

}  // namespace

extern "C" {

// dlmalloc found a corrupted heap (corruption = 1) or was asked to free/realloc something it
// didn't allocate (corruption = 0).
void ps4_heap_error(void* msp, void* chunk, int corruption) {
    heapLog("[cemu] heap: %s during %s from elf 0x%llx (chunk %p)\n",
            corruption ? "CORRUPTION DETECTED" : "invalid pointer", t_op, elf(t_caller), chunk);
    logStack();
    if (corruption)
        abort();
}

void* __wrap_malloc(size_t size) {
    t_op = "malloc";
    t_caller = __builtin_return_address(0);
    checkHeap(t_op, t_caller);
    void* p = t_arena ? mspace_malloc(t_arena, size) : nullptr;
    if (!p)
        p = mspace_malloc(getHeap(), size);
    if (!p)
        p = overflowAlloc(size, [&](mspace msp) { return mspace_malloc(msp, size); });
    if (!p)
        reportFailure("malloc", size);
    return p;
}

void __wrap_free(void* ptr) {
    if (!ptr)
        return;
    t_op = "free";
    t_caller = __builtin_return_address(0);
    checkHeap(t_op, t_caller);
    if (!ownsPointer(ptr)) {
        if (g_errors++ < 8) {
            heapLog("[cemu] heap: free(%p) of memory not from this heap, caller elf 0x%llx (ignored)\n", ptr,
                    elf(t_caller));
            logStack();
        }
        return;
    }
    mspace_free(getHeap(), ptr);
}

void* __wrap_calloc(size_t nelem, size_t size) {
    t_op = "calloc";
    t_caller = __builtin_return_address(0);
    checkHeap(t_op, t_caller);
    void* p = t_arena ? mspace_calloc(t_arena, nelem, size) : nullptr;
    if (!p)
        p = mspace_calloc(getHeap(), nelem, size);
    if (!p)
        p = overflowAlloc(nelem * size, [&](mspace msp) { return mspace_calloc(msp, nelem, size); });
    if (!p)
        reportFailure("calloc", nelem * size);
    return p;
}

void* __wrap_realloc(void* ptr, size_t size) {
    t_op = "realloc";
    t_caller = __builtin_return_address(0);
    checkHeap(t_op, t_caller);
    if (ptr && !ownsPointer(ptr)) {
        heapLog("[cemu] heap: realloc(%p) of memory not from this heap, caller elf 0x%llx\n", ptr,
                elf(t_caller));
        logStack();
        return nullptr;
    }
    if (ptr && size == 0) {
        mspace_free(getHeap(), ptr);
        return nullptr;
    }
    // With FOOTERS this grows the chunk inside the arena it came from (the mspace argument is
    // only used for new allocations); when that arena is full, move it to this thread's.
    void* p = ptr ? mspace_realloc(getHeap(), ptr, size) : nullptr;
    if (!p) {
        p = t_arena ? mspace_malloc(t_arena, size) : nullptr;
        if (!p)
            p = mspace_malloc(getHeap(), size);
        if (!p)
            p = overflowAlloc(size, [&](mspace msp) { return mspace_malloc(msp, size); });
        if (p && ptr) {
            const size_t old_size = mspace_usable_size(ptr);
            memcpy(p, ptr, old_size < size ? old_size : size);
            mspace_free(getHeap(), ptr);
        }
    }
    if (!p)
        reportFailure("realloc", size);
    return p;
}

void* __wrap_memalign(size_t alignment, size_t size) {
    t_op = "memalign";
    t_caller = __builtin_return_address(0);
    checkHeap(t_op, t_caller);
    void* p = t_arena ? mspace_memalign(t_arena, alignment, size) : nullptr;
    if (!p)
        p = mspace_memalign(getHeap(), alignment, size);
    if (!p)
        p = overflowAlloc(size + alignment, [&](mspace msp) { return mspace_memalign(msp, alignment, size); });
    if (!p)
        reportFailure("memalign", size);
    return p;
}

// Direct memory the overflow heaps leave free for the GPU (see overflowAlloc); 0, the default,
// takes what they can.
void ps4_heap_set_overflow_gpu_reserve(size_t bytes) { g_overflowGpuReserve = bytes; }

// Gives the calling thread a private arena if it is one of the emulator's busy threads (called
// when a thread names itself: SetThreadName -> ps4_watch_thread). Crema's (Cemu's) names: until
// 1.51 only DolphinPS4's were listed, so every Cemu thread shared the one locked heap and the
// recompiler (allocation heavy) took seconds per function.
void ps4_heap_private_arena(const char* name) {
    // (and Eden for PS4's: its emulated CPU cores, in multi-core and single-core mode, and its GPU
    // thread)
    static const char* const busy[] = {"PPCRecompiler", "OSSched[core=0]", "OSSched[core=1]", "OSSched[core=2]",
                                       "LatteThread", "compilePl", "vkShaderComp", "plCacheCompiler",
                                       "vkDriverPlCache", "fileCache", "PS4AudioOut", "H264Decoder",
                                       "CPUCore_0", "CPUCore_1", "CPUCore_2", "CPUCore_3", "CPUThread", "GPU"};
    if (t_arena || !name || !getHeap())
        return;
    bool wanted = false;
    for (const char* b : busy)
        wanted |= strcmp(name, b) == 0;
    if (!wanted)
        return;
    if (!g_arenaKeyReady.load(std::memory_order_acquire)) {
        static std::atomic_flag s_creating = ATOMIC_FLAG_INIT;
        if (!s_creating.test_and_set()) {
            pthread_key_create(&g_arenaKey, releaseArena);
            g_arenaKeyReady.store(true, std::memory_order_release);
        }
        while (!g_arenaKeyReady.load(std::memory_order_acquire))
            sched_yield();
    }
    // An exited thread's arena first.
    lockFreeArenas();
    if (g_freeArenaCount > 0)
        t_arena = g_freeArenas[--g_freeArenaCount];
    unlockFreeArenas();
    if (t_arena) {
        pthread_setspecific(g_arenaKey, t_arena);
        heapLog("[cemu] heap: reused arena for '%s'\n", name);
        return;
    }
    if (g_arenaCount.fetch_add(1) >= MAX_ARENAS)
        return;
    void* block = mspace_malloc(getHeap(), ARENA_SIZE);
    if (!block)
        return;
    t_arena = create_mspace_with_base(block, ARENA_SIZE, 1);
    if (t_arena)
        pthread_setspecific(g_arenaKey, t_arena);
    heapLog("[cemu] heap: private %zu MiB arena for '%s'%s\n", ARENA_SIZE / MB, name,
            t_arena ? "" : " FAILED");
}

// posix_memalign and aligned_alloc in libc.a are built on __memalign.
void* __wrap___memalign(size_t alignment, size_t size) { return __wrap_memalign(alignment, size); }

// System libc malloc replacement (port/ps4_crt1.S, _sceLibcMallocReplace): the system libc
// forwards every malloc of the process here - Sony's system modules too (Piglet, its shader
// compiler) - instead of serving it from its own ~12.7 MiB heap, which they exhausted.
int ps4_replace_malloc_init(void) {
    getHeap();
    return 0;
}
int ps4_replace_malloc_finalize(void) { return 0; }
void* ps4_replace_malloc(size_t size) { return __wrap_malloc(size); }
void ps4_replace_free(void* ptr) { __wrap_free(ptr); }
void* ps4_replace_calloc(size_t nelem, size_t size) { return __wrap_calloc(nelem, size); }
void* ps4_replace_realloc(void* ptr, size_t size) { return __wrap_realloc(ptr, size); }
void* ps4_replace_memalign(size_t boundary, size_t size) { return __wrap_memalign(boundary, size); }
void* ps4_replace_reallocalign(void* ptr, size_t size, size_t boundary) {
    if (!ptr)
        return __wrap_memalign(boundary, size);
    if (size == 0) {
        __wrap_free(ptr);
        return nullptr;
    }
    void* p = __wrap_memalign(boundary, size);
    if (!p)
        return nullptr;
    const size_t old_size = mspace_usable_size(ptr);
    memcpy(p, ptr, old_size < size ? old_size : size);
    __wrap_free(ptr);
    return p;
}
int ps4_replace_posix_memalign(void** out, size_t boundary, size_t size) {
    void* p = __wrap_memalign(boundary, size);
    if (!p)
        return 12;  // ENOMEM
    *out = p;
    return 0;
}
// SceLibcMallocManagedSize: u16 size, u16 version, u32 reserved, then 4 sizes. Report the heap.
struct MallocManagedSize {
    uint16_t size;
    uint16_t version;
    uint32_t reserved;
    size_t max_system_size, current_system_size, max_inuse_size, current_inuse_size;
};
int ps4_replace_malloc_stats(void* out) {
    auto* stats = static_cast<MallocManagedSize*>(out);
    if (!stats)
        return 0x80020016;  // EINVAL
    const size_t footprint = mspace_footprint(getHeap());
    stats->max_system_size = mspace_max_footprint(getHeap());
    stats->current_system_size = footprint;
    stats->max_inuse_size = footprint;
    stats->current_inuse_size = footprint;
    return 0;
}
int ps4_replace_malloc_stats_fast(void* out) { return ps4_replace_malloc_stats(out); }
size_t ps4_replace_malloc_usable_size(void* ptr) { return ptr ? mspace_usable_size(ptr) : 0; }

// Large anonymous mmaps (--wrap=mmap): Dolphin's JIT code buffers, DSP ARAM, FIFO, ... would
// otherwise come out of the regular flexible memory pool (255 MiB), which Piglet (OpenGL ES)
// needs for all GPU memory - it ran out of it entirely. Take them from the system flexible pool
// (like the heap) instead; fall back to a normal mmap if that fails (e.g. for executable memory).
// Port switches, set by the app before emulation starts (DolphinPS4 ps4.ini).
int ps4_jit_in_system_pool = 1;           // executable mappings from the system pool too
unsigned long long ps4_fault_count = 0;   // fastmem faults seen by Dolphin's handler

void* __real_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset);

// Executable memory (the recompiler's code blocks). On GoldHEN 12.00/13.52 consoles system
// flexible memory mapped RWX runs code; on a firmware 11.00 console that mapping succeeded but its
// pages did not execute: every game crashed at its first recompiled block (SIGSEGV code 2 with rip
// = the block). So each source is tried once with a one-instruction function (ret) before it
// serves executable mappings; a fault there is caught and the next source is tried.
namespace {

enum ExecSource { kExecSystemPool, kExecRegular, kExecDirect, kExecNone };
const char* const kExecSourceNames[] = {"system flexible memory", "regular flexible memory", "direct memory",
                                        "nothing"};
constexpr int kSigBusFreeBsd = 10, kSigSegvFreeBsd = 11, kSaSiginfoFreeBsd = 0x40;
constexpr int kMcontextOffset = 0x40, kRipIndex = 20, kRspIndex = 23;  // as port/ps4_crashlog.cpp
const int32_t PROT_CPU_RWX = 0x7;

volatile uintptr_t g_probeCode = 0;
volatile int g_probeFaulted = 0;
struct sigaction g_oldSegv, g_oldBus;

void execProbeHandler(int, siginfo_t*, void* context) {
    auto* regs = reinterpret_cast<uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
    if (g_probeCode && regs[kRipIndex] == g_probeCode) {
        // as if the ret ran: back to the caller
        regs[kRipIndex] = *reinterpret_cast<const uint64_t*>(regs[kRspIndex]);
        regs[kRspIndex] += 8;
        g_probeFaulted = 1;
        return;
    }
    // another fault: the previous handlers get it when the instruction runs again
    sigaction(kSigSegvFreeBsd, &g_oldSegv, nullptr);
    sigaction(kSigBusFreeBsd, &g_oldBus, nullptr);
}

// Whether code placed at `code` (mapped RWX) runs.
bool runsCode(void* code) {
    static_cast<volatile uint8_t*>(code)[0] = 0xC3;  // ret
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    void (*handler)(int, siginfo_t*, void*) = execProbeHandler;
    memcpy(&action, &handler, sizeof(handler));  // the handler union is the first member
    action.sa_flags = kSaSiginfoFreeBsd;
    sigemptyset(&action.sa_mask);
    g_probeFaulted = 0;
    g_probeCode = reinterpret_cast<uintptr_t>(code);
    sigaction(kSigSegvFreeBsd, &action, &g_oldSegv);
    sigaction(kSigBusFreeBsd, &action, &g_oldBus);
    void (*volatile function)() = reinterpret_cast<void (*)()>(code);
    function();
    sigaction(kSigSegvFreeBsd, &g_oldSegv, nullptr);
    sigaction(kSigBusFreeBsd, &g_oldBus, nullptr);
    g_probeCode = 0;
    return !g_probeFaulted;
}

void* mapSystemPoolExecutable(size_t size) {
    void* base = reserveAwayFromKernel(size);
    if (!base)
        return nullptr;
    const int32_t ret = sceKernelMapNamedSystemFlexibleMemory(&base, size, PROT_CPU_RWX, MAP_FIXED_FLAG, "cemu code");
    if (ret != 0) {
        heapLog("[cemu] mmap: %zu KiB of executable system flexible memory not possible (0x%x)\n", size / 1024,
                ret);
        sceKernelMunmap(base, size);
        return nullptr;
    }
    return base;
}

// Direct memory isn't given back by munmap: code blocks are kept for the whole run anyway.
void* mapDirectExecutable(size_t size, off_t* physOut = nullptr) {
    off_t phys = 0;
    int32_t ret = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, PAGE, 0, &phys);
    if (ret != 0) {
        heapLog("[cemu] mmap: %zu KiB of direct memory not available (0x%x)\n", size / 1024, ret);
        return nullptr;
    }
    void* base = reserveAwayFromKernel(size);
    ret = base ? sceKernelMapDirectMemory(&base, size, PROT_CPU_RWX, MAP_FIXED_FLAG, phys, PAGE) : -1;
    if (ret != 0) {
        heapLog("[cemu] mmap: %zu KiB of executable direct memory not possible (0x%x)\n", size / 1024, ret);
        if (base)
            sceKernelMunmap(base, size);
        sceKernelReleaseDirectMemory(phys, size);
        return nullptr;
    }
    if (physOut)
        *physOut = phys;
    return base;
}

ExecSource chooseExecSource() {
    SceKernelSwVersion version;
    memset(&version, 0, sizeof(version));
    version.size = sizeof(version);
    if (sceKernelGetSystemSwVersion(&version) == 0) {
        version.text[sizeof(version.text) - 1] = 0;
        heapLog("[cemu] firmware %s (0x%08x)\n", version.text, version.version);
    }
    if (ps4_jit_in_system_pool) {
        void* code = mapSystemPoolExecutable(PAGE);
        if (code) {
            const bool ok = runsCode(code);
            sceKernelMunmap(code, PAGE);
            heapLog("[cemu] executable memory: system flexible memory %s\n", ok ? "runs code" : "does not run code");
            if (ok)
                return kExecSystemPool;
        }
    }
    void* code = __real_mmap(nullptr, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (code != MAP_FAILED) {
        const bool ok = runsCode(code);
        munmap(code, PAGE);
        heapLog("[cemu] executable memory: regular flexible memory %s\n", ok ? "runs code" : "does not run code");
        if (ok)
            return kExecRegular;
    } else {
        heapLog("[cemu] executable memory: regular flexible memory cannot be mapped RWX\n");
    }
    off_t phys = 0;
    code = mapDirectExecutable(PAGE, &phys);
    if (code) {
        const bool ok = runsCode(code);
        sceKernelMunmap(code, PAGE);
        sceKernelReleaseDirectMemory(phys, PAGE);
        heapLog("[cemu] executable memory: direct memory %s\n", ok ? "runs code" : "does not run code");
        if (ok)
            return kExecDirect;
    }
    heapLog("[cemu] executable memory: NONE runs code - this console's jailbreak does not let homebrew run "
            "generated code; games will crash\n");
    return kExecNone;
}

}  // namespace

void* __wrap_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    const bool anonymous = fd == -1 && (flags & MAP_ANON) && !(flags & MAP_FIXED);
    if (anonymous && (prot & PROT_EXEC)) {
        static const ExecSource source = chooseExecSource();
        const size_t size = (len + PAGE - 1) & ~(PAGE - 1);
        void* base = nullptr;
        if (source == kExecSystemPool)
            base = mapSystemPoolExecutable(size);
        else if (source == kExecDirect)
            base = mapDirectExecutable(size);
        if (base) {
            heapLog("[cemu] mmap: %zu KiB prot %d from %s at %p, caller elf 0x%llx\n", size / 1024, prot,
                    kExecSourceNames[source], base, elf(__builtin_return_address(0)));
            return base;
        }
        void* result = __real_mmap(addr, len, prot, flags, fd, offset);
        heapLog("[cemu] mmap: %zu KiB prot %d from regular flexible memory = %p, caller elf 0x%llx\n", len / 1024,
                prot, result, elf(__builtin_return_address(0)));
        return result;
    }
    if (anonymous && len >= MB) {
        const size_t size = (len + PAGE - 1) & ~(PAGE - 1);
        void* base = reserveAwayFromKernel(size);
        if (base) {
            const int32_t ret = sceKernelMapNamedSystemFlexibleMemory(&base, size, prot & 0x7, MAP_FIXED_FLAG,
                                                                      "cemu mmap");
            if (ret == 0) {
                heapLog("[cemu] mmap: %zu KiB prot %d from system flexible memory at %p, caller elf 0x%llx\n",
                        size / 1024, prot, base, elf(__builtin_return_address(0)));
                return base;
            }
            heapLog("[cemu] mmap: %zu KiB prot %d not possible from system flexible memory (0x%x)\n",
                    size / 1024, prot, ret);
            sceKernelMunmap(base, size);
        }
    }
    void* result = __real_mmap(addr, len, prot, flags, fd, offset);
    if (anonymous && len >= MB) {
        size_t available = 0;
        sceKernelAvailableFlexibleMemorySize(&available);
        heapLog("[cemu] mmap: %zu KiB prot %d from regular flexible memory = %p, caller elf 0x%llx, "
                "%zu MiB flexible left\n",
                len / 1024, prot, result, elf(__builtin_return_address(0)), available / MB);
    }
    return result;
}

// clock_gettime for every caller (--wrap): CLOCK_MONOTONIC from the CPU's time stamp counter.
// The kernel's clock_gettime is a system call (FreeBSD 9 has no user-space time page), and
// Dolphin reads the monotonic clock thousands of times a second on both emulation threads
// (CoreTiming's throttle, std::chrono timeouts, the port's wait timers): ~6-9% of each thread in
// FIFA Street 2. The counter is invariant on the PS4's Jaguar cores; it is anchored to the
// kernel's clock on first use, so absolute times from both agree. Other clocks go to the kernel.
int __real_clock_gettime(clockid_t id, struct timespec* ts);
uint64_t sceKernelGetTscFrequency_ps4() __asm__("sceKernelGetTscFrequency");
unsigned long long ps4_clock_reads;  // DolphinNoGUI monitor (approximate, unsynchronised)

int __wrap_clock_gettime(clockid_t id, struct timespec* ts) {
    if (id != CLOCK_MONOTONIC)
        return __real_clock_gettime(id, ts);
    struct Anchor {
        uint64_t tsc = 0, ns = 0, mult = 0;  // ns per tick as 32.32 fixed point
        bool ok = false;
    };
    static const Anchor anchor = [] {
        Anchor a;
        const uint64_t freq = static_cast<uint64_t>(sceKernelGetTscFrequency_ps4());
        struct timespec now;
        if (freq < 1000000 || __real_clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return a;
        a.tsc = __builtin_ia32_rdtsc();
        a.ns = static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec);
        a.mult = (1000000000ull << 32) / freq;
        a.ok = true;
        return a;
    }();
    if (!anchor.ok)
        return __real_clock_gettime(id, ts);
    ps4_clock_reads++;
    const uint64_t ticks = __builtin_ia32_rdtsc() - anchor.tsc;
    const uint64_t ns =
        anchor.ns + static_cast<uint64_t>((static_cast<unsigned __int128>(ticks) * anchor.mult) >> 32);
    ts->tv_sec = static_cast<time_t>(ns / 1000000000ull);
    ts->tv_nsec = static_cast<long>(ns % 1000000000ull);
    return 0;
}

// pthread_once for every caller (--wrap): the kernel's (FreeBSD) version treats pthread_once_t
// as a 16-byte struct, but the OpenOrbis headers - and the prebuilt libc.a (newlocale,
// call_once, ...) - use a 4-byte int, which it overflowed. This one only uses those 4 bytes:
// 0 = not run (PTHREAD_ONCE_INIT), 1 = running, 2 = done.
int __wrap_pthread_once(pthread_once_t* once, void (*init)(void)) {
    auto* state = reinterpret_cast<int*>(once);
    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) == 2)
        return 0;
    int expected = 0;
    if (__atomic_compare_exchange_n(state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        init();
        __atomic_store_n(state, 2, __ATOMIC_RELEASE);
        return 0;
    }
    while (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2)
        sched_yield();
    return 0;
}

// pthread_create for every caller (--wrap): a PS4 thread inherits its creator's CPU affinity, so a
// worker started from an emulator thread that runs on one core (Cemu's H.264 decoder, started from
// an emulated CPU core's thread; DolphinPS4's shader compilers) would share that busy core and
// starve. New threads start on all of the app's cores (0-5) instead; a thread that wants one core
// sets it itself.
extern "C" int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);
extern "C" void* scePthreadSelf();
extern "C" void scePthreadSetaffinity();  // libkernel: (ScePthread, SceKernelCpumask)
extern "C" void scePthreadGetaffinity();  // libkernel: (ScePthread, SceKernelCpumask*)
extern "C" void ps4_boot_trace(const char* stage);
extern "C" void ps4_watch_thread(const char* name);  // port/ps4_sampler.cpp
extern "C" void ps4_install_signal_stack();        // port/ps4_crashlog.cpp

// Crema setting core_layout (src/gui/ps4/launcher/CremaSettings.cpp), set before the game's threads
// start. The PS4's CPU is two Jaguar modules of four cores, each module with its own L2 cache;
// a cache line moving between the modules costs several times what it does inside one.
//   0 Spread:    every thread on any of the app's cores (the scheduler's choice)
//   1 Clustered: the emulation's busiest threads (ps4_boost_thread: the emulated CPU cores and
//                the GPU thread, which hand work to each other constantly) on module 0
//   2 Split:     as 1, and threads started afterwards on module 1, so shader compilers, the
//                recompiler's workers and the driver's threads don't evict their cache
extern "C" {
int g_crema_core_layout = 0;
}

namespace {
constexpr uint64_t kAppCores = 0x7F;  // every core the app may use (the 7th is shared with the system)
constexpr uint64_t kModule0Cores = 0x0F; // cores 0-3: the first Jaguar module, all the app's
constexpr uint64_t kModule1Cores = 0x70; // cores 4-6 of the second (the 7th is the system's)
// Every thread gets at least this much stack. The system's default is far smaller than a PC's
// (logged at the first thread), and shader translation (glslang) and the driver's shader
// compiler recurse deeply on large shaders; a thread out of stack is ended by the system with
// error CE-34878-0 (suspected when Minecraft's menus appear).
constexpr size_t kMinThreadStack = 2 * 1024 * 1024;
using SetAffinityFn = int (*)(void*, uint64_t);
using GetAffinityFn = int (*)(void*, uint64_t*);

struct ThreadStart {
    void* (*start)(void*);
    void* arg;
};

void* startOnAppCores(void* raw) {
    const ThreadStart start = *static_cast<ThreadStart*>(raw);
    free(raw);
    reinterpret_cast<SetAffinityFn>(&scePthreadSetaffinity)(scePthreadSelf(),
                                                            g_crema_core_layout == 2 ? kModule1Cores : kAppCores);
    ps4_install_signal_stack();
    // in the stack dumps (crash.log, hang.log) from the start; a thread that names itself later
    // (SetThreadName) updates the name
    ps4_watch_thread("(unnamed)");
    return start.start(start.arg);
}
}  // namespace

// Do the cores' time stamp counters agree? Cemu's Wii U clock (PPCTimer.cpp) and parts of Crema
// read the TSC on whatever core a thread runs on, and threads move between the app's cores. A
// thread pinned to core 0 and one pinned to core N pass a counter value back and forth; the
// smallest differences each way give the offset (core N minus core 0) and the round trip. In a
// detached thread, every wait limited to 0.2 s: a core the app can't run on (2.10 waited forever
// for one, and the game never started) is reported as "no answer".
namespace {
struct TscShared {
    std::atomic<uint64_t> value{0};
    std::atomic<int> turn{0};
    std::atomic<bool> abandon{false};
    uint64_t limit = 0;  // TSC ticks a wait may take
};

bool tscWait(TscShared& s, int turn) {
    const uint64_t start = __builtin_ia32_rdtsc();
    while (s.turn.load(std::memory_order_acquire) != turn) {
        if (s.abandon.load(std::memory_order_relaxed))
            return false;
        // the start was read on this core: the limit doesn't depend on other cores' counters
        if (__builtin_ia32_rdtsc() - start > s.limit) {
            s.abandon.store(true);
            return false;
        }
        _mm_pause();
    }
    return true;
}

void tscProbeCore(int core, double ticksPerUs, char* out, size_t size) {
    auto* shared = new TscShared;  // leaked if a side never ends (it would still read it)
    shared->limit = (uint64_t)(ticksPerUs * 200000);
    auto minAB = std::make_shared<std::atomic<uint64_t>>(~0ull);
    auto minBA = std::make_shared<std::atomic<uint64_t>>(~0ull);
    constexpr int kRounds = 200;
    auto pin = [](int c) {
        reinterpret_cast<SetAffinityFn>(&scePthreadSetaffinity)(scePthreadSelf(), 1ull << c);
        sched_yield();
    };
    std::thread a([shared, minBA, pin] {
        pin(0);
        for (int k = 0; k < kRounds; k++) {
            shared->value.store(__builtin_ia32_rdtsc(), std::memory_order_release);
            shared->turn.store(4 * k + 1, std::memory_order_release);
            if (!tscWait(*shared, 4 * k + 3))
                return;
            const uint64_t d = __builtin_ia32_rdtsc() - shared->value.load(std::memory_order_acquire);
            if ((int64_t)d < (int64_t)minBA->load() || minBA->load() == ~0ull)
                minBA->store(d);
            shared->turn.store(4 * k + 4, std::memory_order_release);
        }
    });
    std::thread b([shared, minAB, core, pin] {
        pin(core);
        for (int k = 0; k < kRounds; k++) {
            if (!tscWait(*shared, 4 * k + 1))
                return;
            const uint64_t d = __builtin_ia32_rdtsc() - shared->value.load(std::memory_order_acquire);
            if ((int64_t)d < (int64_t)minAB->load() || minAB->load() == ~0ull)
                minAB->store(d);
            shared->value.store(__builtin_ia32_rdtsc(), std::memory_order_release);
            shared->turn.store(4 * k + 3, std::memory_order_release);
            if (!tscWait(*shared, 4 * k + 4))
                return;
        }
    });
    a.join();
    if (shared->abandon.load())
        b.detach(); // it may never get to run on that core
    else
        b.join();
    if (shared->abandon.load()) {
        snprintf(out, size, " core%d no answer", core);
        return;
    }
    const double ab = (double)(int64_t)minAB->load(), ba = (double)(int64_t)minBA->load();
    snprintf(out, size, " core%d %+.2f (trip %.2f)", core, (ab - ba) / 2.0 / ticksPerUs, (ab + ba) / 2.0 / ticksPerUs);
}
}  // namespace

extern "C" void ps4_tsc_probe() {
    std::thread([] {
        char line[400];
        int len = snprintf(line, sizeof(line), "tsc: offsets from core 0 (us):");
        const double ticksPerUs = (double)sceKernelGetTscFrequency_ps4() / 1e6;
        for (int core = 1; core < 7 && len < (int)sizeof(line) - 40; core++) {
            char part[64];
            tscProbeCore(core, ticksPerUs, part, sizeof(part));
            len += snprintf(line + len, sizeof(line) - len, "%s", part);
        }
        ps4_boot_trace(line);
    }).detach();
}

// The emulation's busiest threads (the emulated CPU cores, the GPU thread) above the default
// priority (256 highest .. 767 lowest, 700 default): logging, loading and shader compiling never
// take their time. Audio stays higher (400).
extern "C" int scePthreadSetprio(void* thread, int prio);
extern "C" void ps4_boost_thread() {
    scePthreadSetprio(scePthreadSelf(), 650);
    if (g_crema_core_layout != 0)
        reinterpret_cast<SetAffinityFn>(&scePthreadSetaffinity)(scePthreadSelf(), kModule0Cores);
}

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    // the stack: at least kMinThreadStack
    pthread_attr_t local;
    bool ownAttr = false;
    size_t stack = 0;
    if (attr) {
        pthread_attr_getstacksize(attr, &stack);
        if (stack < kMinThreadStack)
            pthread_attr_setstacksize(const_cast<pthread_attr_t*>(attr), kMinThreadStack);
    } else if (pthread_attr_init(&local) == 0) {
        ownAttr = true;
        pthread_attr_getstacksize(&local, &stack);
        if (stack < kMinThreadStack)
            pthread_attr_setstacksize(&local, kMinThreadStack);
        attr = &local;
    }
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true)) {
        uint64_t mask = 0;
        reinterpret_cast<GetAffinityFn>(&scePthreadGetaffinity)(scePthreadSelf(), &mask);
        char line[160];
        snprintf(line, sizeof(line),
                 "threads: first creator's cores %#llx, new threads get %#llx; stack %zu KiB by default, at least %zu KiB",
                 static_cast<unsigned long long>(mask), static_cast<unsigned long long>(kAppCores), stack / 1024,
                 kMinThreadStack / 1024);
        ps4_boot_trace(line);
    }
    int result;
    auto* wrapped = static_cast<ThreadStart*>(malloc(sizeof(ThreadStart)));
    if (!wrapped) {
        result = __real_pthread_create(thread, attr, start, arg);
    } else {
        *wrapped = {start, arg};
        result = __real_pthread_create(thread, attr, startOnAppCores, wrapped);
        if (result != 0)
            free(wrapped);
    }
    if (ownAttr)
        pthread_attr_destroy(&local);
    return result;
}

int __cxa_thread_atexit_impl(void (*dtor)(void*), void* obj, void* /*dso_symbol*/) {
    pthread_once(&g_threadDtorOnce, createThreadDtorKey);
    auto* entry = static_cast<ThreadDtor*>(malloc(sizeof(ThreadDtor)));
    if (!entry)
        return -1;
    // Destructors run in reverse order of registration, so push to the front.
    entry->dtor = dtor;
    entry->obj = obj;
    entry->next = static_cast<ThreadDtor*>(pthread_getspecific(g_threadDtorKey));
    pthread_setspecific(g_threadDtorKey, entry);
    return 0;
}

// Diagnostics.
const char* ps4rt_heap_source(void) {
    getHeap();
    return g_heapPool;
}
size_t ps4rt_heap_size(void) {
    getHeap();
    return g_heapSize;
}
// How much of the heap has been touched: an upper bound of what is in use (freed chunks below the
// top count too).
size_t ps4rt_heap_footprint(void) {
    return mspace_footprint(getHeap());
}

}  // extern "C"
