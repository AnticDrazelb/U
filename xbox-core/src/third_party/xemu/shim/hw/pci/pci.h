/* xbox-core: what xemu's APU uses of QEMU's memory regions and PCI devices */
#ifndef XB_XEMU_PCI_H
#define XB_XEMU_PCI_H
#include "qemu/osdep.h"
typedef struct MemoryRegion { int unused; } MemoryRegion;
typedef struct MemoryRegionOps {
    uint64_t (*read)(void *opaque, hwaddr addr, unsigned int size);
    void (*write)(void *opaque, hwaddr addr, uint64_t val, unsigned int size);
} MemoryRegionOps;
typedef struct PCIDevice { int irq; uint8_t config[256]; } PCIDevice; /* xbox-core: the device's IRQ line */
typedef struct PCIBus PCIBus;
void xb_pci_irq(int irq, int level);
#define PCI_DEVICE(d) ((PCIDevice *)(d))
#define pci_irq_assert(d) xb_pci_irq(((PCIDevice *)(d))->irq, 1)
#define pci_irq_deassert(d) xb_pci_irq(((PCIDevice *)(d))->irq, 0)
typedef uint64_t dma_addr_t;
void xb_phys_read(uint64_t addr, void *buf, size_t len);
void xb_phys_write(uint64_t addr, const void *buf, size_t len);
#define pci_dma_read(d, addr, buf, len) (xb_phys_read((addr), (buf), (len)), 0)
#define pci_dma_write(d, addr, buf, len) (xb_phys_write((addr), (buf), (len)), 0)
uint64_t xb_ram_size(void);
#define memory_region_size(mr) xb_ram_size()
#define memory_region_set_dirty(mr, a, n) ((void)0)
#endif
