/* xbox-core: no snapshots. QEMU's state descriptions compile to inert data. */
#ifndef XB_XEMU_VMSTATE_H
#define XB_XEMU_VMSTATE_H
#include <stdbool.h>
typedef struct VMStateField { const char *name; } VMStateField;
typedef struct VMStateDescription {
    const char *name;
    int version_id, minimum_version_id;
    int (*pre_load)(void *opaque);
    int (*post_load)(void *opaque, int version_id);
    int (*pre_save)(void *opaque);
    int (*post_save)(void *opaque);
    bool (*needed)(void *opaque);
    const VMStateField *fields;
    const struct VMStateDescription *const *subsections;
    bool unmigratable;
} VMStateDescription;
#define XB_VMS(...) { 0 }
#define VMSTATE_UINT32(...) XB_VMS()
#define VMSTATE_UINT16(...) XB_VMS()
#define VMSTATE_UINT8(...) XB_VMS()
#define VMSTATE_INT32(...) XB_VMS()
#define VMSTATE_INT64(...) XB_VMS()
#define VMSTATE_BOOL(...) XB_VMS()
#define VMSTATE_UINT8_ARRAY(...) XB_VMS()
#define VMSTATE_UINT32_ARRAY(...) XB_VMS()
#define VMSTATE_STRUCT_ARRAY(...) XB_VMS()
#define VMSTATE_STRUCT(...) XB_VMS()
#define VMSTATE_TIMER_PTR(...) XB_VMS()
#define VMSTATE_USB_DEVICE(...) XB_VMS()
#define VMSTATE_PCI_DEVICE(...) XB_VMS()
#define VMSTATE_UNUSED(...) XB_VMS()
#define VMSTATE_END_OF_LIST() XB_VMS()
#endif
