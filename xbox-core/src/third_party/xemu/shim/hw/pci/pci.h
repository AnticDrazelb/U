/* xbox-core: what xemu's APU uses of QEMU's memory regions and PCI devices */
#ifndef XB_XEMU_PCI_H
#define XB_XEMU_PCI_H
#include "qemu/osdep.h"
typedef struct MemoryRegion { int unused; } MemoryRegion;
typedef struct MemoryRegionOps {
    uint64_t (*read)(void *opaque, hwaddr addr, unsigned int size);
    void (*write)(void *opaque, hwaddr addr, uint64_t val, unsigned int size);
} MemoryRegionOps;
typedef struct PCIDevice { int unused; } PCIDevice;
typedef struct PCIBus PCIBus;
void xb_apu_irq(int level);
#define PCI_DEVICE(d) ((PCIDevice *)(d))
#define pci_irq_assert(d) xb_apu_irq(1)
#define pci_irq_deassert(d) xb_apu_irq(0)
uint64_t xb_ram_size(void);
#define memory_region_size(mr) xb_ram_size()
#define memory_region_set_dirty(mr, a, n) ((void)0)
#endif
