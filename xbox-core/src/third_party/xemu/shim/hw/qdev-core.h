/* xbox-core: QEMU's qdev core, reduced (see qom/object.h) */
#ifndef XB_XEMU_QDEV_CORE_H
#define XB_XEMU_QDEV_CORE_H
#include "qom/object.h"
#include "qemu/queue.h"
#include "qapi/error.h"

typedef struct BusState BusState;
typedef struct DeviceState {
    Object parent_obj;
    const char *id;
    bool realized;
    BusState *parent_bus;
} DeviceState;
typedef struct Property { const char *name; } Property;
typedef void (*DeviceRealize)(DeviceState *dev, Error **errp);
typedef void (*DeviceUnrealize)(DeviceState *dev);
typedef struct DeviceClass {
    ObjectClass parent_class;
    const char *desc;
    const void *vmsd;
    unsigned long categories[1];
    DeviceRealize realize;
    DeviceUnrealize unrealize;
    const char *bus_type;
    bool user_creatable;
    const char *fw_name;
} DeviceClass;
struct BusState {
    Object obj;
    DeviceState *parent;
    char *name;
};
enum { DEVICE_CATEGORY_BRIDGE, DEVICE_CATEGORY_USB, DEVICE_CATEGORY_INPUT, DEVICE_CATEGORY_NETWORK, DEVICE_CATEGORY_MISC };
#define DEVICE(obj) ((DeviceState *)(obj))
#define DEVICE_CLASS(k) ((DeviceClass *)(k))
#define BUS(obj) ((BusState *)(obj))
#define set_bit(nr, addr) ((addr)[(nr) / (8 * sizeof(unsigned long))] |= 1UL << ((nr) % (8 * sizeof(unsigned long))))
#define device_class_set_props(dc, props) ((void)(props))
#define device_class_set_legacy_reset(dc, fn) ((void)(fn))
static inline char *qdev_get_dev_path(DeviceState *dev) { (void)dev; return NULL; }
#define DEFINE_PROP_END_OF_LIST() {}
/* device properties: xbox-core sets the fields itself */
#define DEFINE_PROP_UINT32(n, ...) { .name = (n) }
#define DEFINE_PROP_BOOL(n, ...) { .name = (n) }
#define DEFINE_PROP_UINT8(n, ...) { .name = (n) }
#define DEFINE_PROP_STRING(n, ...) { .name = (n) }
#define DEFINE_PROP_LINK(n, ...) { .name = (n) }
/* the machine is always ready (devices are made at start) */
#define phase_check(phase) true
#define PHASE_MACHINE_READY 0
#endif
