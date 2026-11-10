/* xbox-core: an IRQ line is its number (shim/hw/pci/pci.h: xb_pci_irq) */
#include "hw/pci/pci.h"
#define qemu_set_irq(irq, level) xb_pci_irq((int)(intptr_t)(irq), (level))
#define qemu_irq_raise(irq) qemu_set_irq((irq), 1)
#define qemu_irq_lower(irq) qemu_set_irq((irq), 0)
