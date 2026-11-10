/* xbox-core: QEMU's DMA to guest physical memory (xbox-core's xb_phys_read/write) */
#ifndef XB_XEMU_DMA_H
#define XB_XEMU_DMA_H
#include "hw/pci/pci.h"
typedef enum { DMA_DIRECTION_TO_DEVICE = 0, DMA_DIRECTION_FROM_DEVICE = 1 } DMADirection;
typedef struct MemTxAttrs { int unused; } MemTxAttrs;
typedef int MemTxResult;
#define MEMTX_OK 0
#define MEMTXATTRS_UNSPECIFIED ((MemTxAttrs){0})
static inline MemTxResult dma_memory_read(AddressSpace *as, dma_addr_t addr, void *buf, dma_addr_t len, MemTxAttrs attrs)
{
    (void)as; (void)attrs;
    xb_phys_read(addr, buf, len);
    return MEMTX_OK;
}
static inline MemTxResult dma_memory_write(AddressSpace *as, dma_addr_t addr, const void *buf, dma_addr_t len, MemTxAttrs attrs)
{
    (void)as; (void)attrs;
    xb_phys_write(addr, buf, len);
    return MEMTX_OK;
}
static inline MemTxResult dma_memory_rw(AddressSpace *as, dma_addr_t addr, void *buf, dma_addr_t len, DMADirection dir, MemTxAttrs attrs)
{
    return dir == DMA_DIRECTION_FROM_DEVICE ? dma_memory_write(as, addr, buf, len, attrs)
                                            : dma_memory_read(as, addr, buf, len, attrs);
}
#endif
