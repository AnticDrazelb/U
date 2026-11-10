/* xbox-core: no system bus (the OHCI is reached through xbox-core's device table) */
#ifndef XB_XEMU_SYSBUS_H
#define XB_XEMU_SYSBUS_H
#include "hw/qdev-core.h"
#include "hw/pci/pci.h"
typedef struct SysBusDevice { DeviceState parent_obj; } SysBusDevice;
#define SYS_BUS_DEVICE(obj) ((SysBusDevice *)(obj))
#endif
