/* xbox-core: what xemu's APU uses of QEMU's memory regions and PCI devices */
#ifndef XB_XEMU_PCI_H
#define XB_XEMU_PCI_H
#include "qemu/osdep.h"
/* a memory region: RAM xbox-core gives (its pointer and size), or an I/O window (no RAM) */
struct MemoryRegionOps;
typedef struct MemoryRegion {
    uint8_t *ram;
    uint64_t size;
    const struct MemoryRegionOps *ops; /* an I/O window: its handlers (memory_region_init_io) */
    void *opaque;
} MemoryRegion;
enum { DIRTY_MEMORY_VGA, DIRTY_MEMORY_CODE, DIRTY_MEMORY_MIGRATION, DIRTY_MEMORY_NV2A, DIRTY_MEMORY_NV2A_TEX, DIRTY_MEMORY_NUM };
static inline uint64_t memory_region_size(MemoryRegion *mr) { return mr->size; }
static inline uint8_t *memory_region_get_ram_ptr(MemoryRegion *mr) { return mr->ram; }
static inline void memory_region_set_log(MemoryRegion *mr, bool log, unsigned client) { (void)mr; (void)log; (void)client; }
/* dirty pages (shim/xb_memory.c): QEMU's per-page bitmaps came from its CPU translator; nothing tracks
 * the natively run CPU's writes here, so a page is dirty for a client when its contents changed since
 * that client last asked (a hash per 4 KiB page), or something marked it */
void memory_region_set_dirty(MemoryRegion *mr, uint64_t addr, uint64_t size);
void memory_region_set_client_dirty(MemoryRegion *mr, uint64_t addr, uint64_t size, unsigned client);
bool memory_region_test_and_clear_dirty(MemoryRegion *mr, uint64_t addr, uint64_t size, unsigned client);
static inline bool tcg_enabled(void) { return false; } /* the CPU runs natively (as under KVM) */
enum device_endian { DEVICE_NATIVE_ENDIAN, DEVICE_BIG_ENDIAN, DEVICE_LITTLE_ENDIAN };
typedef struct MemoryRegionOps {
    uint64_t (*read)(void *opaque, hwaddr addr, unsigned int size);
    void (*write)(void *opaque, hwaddr addr, uint64_t val, unsigned int size);
    enum device_endian endianness;
    struct { unsigned min_access_size, max_access_size; bool unaligned; } valid, impl;
} MemoryRegionOps;
static inline void memory_region_init_io(MemoryRegion *mr, void *owner, const MemoryRegionOps *ops, void *opaque,
                                         const char *name, uint64_t size)
{
    (void)owner; (void)name;
    mr->ram = NULL;
    mr->size = size;
    mr->ops = ops;
    mr->opaque = opaque;
}
typedef struct PCIDevice { int irq; uint8_t config[256]; } PCIDevice; /* xbox-core: the device's IRQ line */
typedef struct PCIBus PCIBus;
typedef void *qemu_irq;
/* TCG's memory access callbacks: unused here (tcg_enabled() is false) */
typedef struct MemAccessCallback MemAccessCallback;
#define qemu_get_cpu(n) NULL
#define mem_access_callback_insert(...) NULL
#define mem_access_callback_remove_by_ref(...) ((void)0)
/* OpenGL names in renderer state (the GL interop for xemu's own UI isn't built) */
typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned int GLenum;
#define GL_RGBA8 0x8058
/* QEMU's run-on-CPU (the renderer switch at run time, which xbox-core never asks for) */
typedef struct CPUState CPUState;
typedef union { void *host_ptr; } run_on_cpu_data;
#define RUN_ON_CPU_HOST_PTR(p) ((run_on_cpu_data){.host_ptr = (p)})
#define async_safe_run_on_cpu(cpu, fn, data) ((void)0)
#define PCI_VENDOR_ID 0x00
#define PCI_COMMAND 0x04
#define PCI_CLASS_REVISION 0x08
#define PCI_INTERRUPT_PIN 0x3d
static inline uint32_t pci_get_long(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void pci_set_long(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void pci_set_word(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
void xb_pci_irq(int irq, int level);
#define PCI_DEVICE(d) ((PCIDevice *)(d))
#define pci_irq_assert(d) xb_pci_irq(((PCIDevice *)(d))->irq, 1)
#define pci_irq_deassert(d) xb_pci_irq(((PCIDevice *)(d))->irq, 0)
typedef uint64_t dma_addr_t;
void xb_phys_read(uint64_t addr, void *buf, size_t len);
void xb_phys_write(uint64_t addr, const void *buf, size_t len);
#define pci_dma_read(d, addr, buf, len) (xb_phys_read((addr), (buf), (len)), 0)
#define pci_dma_write(d, addr, buf, len) (xb_phys_write((addr), (buf), (len)), 0)

#endif
