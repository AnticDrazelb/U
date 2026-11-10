/* xbox-core: QEMU's object model, reduced to what its USB devices use: casts, a class pointer on each
 * object, and class structs filled by the devices' own class_init functions (called by xbox-core). */
#ifndef XB_XEMU_QOM_OBJECT_H
#define XB_XEMU_QOM_OBJECT_H
#include "qemu/osdep.h"

typedef struct ObjectClass { const char *type_name; } ObjectClass;
typedef struct Object { ObjectClass *klass; const char *type_name; } Object;
typedef struct InterfaceInfo { const char *type; } InterfaceInfo;
typedef struct TypeInfo {
    const char *name, *parent;
    size_t instance_size, class_size;
    void (*instance_init)(Object *obj);
    void (*class_init)(ObjectClass *klass, const void *data);
    bool abstract;
    InterfaceInfo *interfaces;
} TypeInfo;

#define OBJECT(obj) ((Object *)(obj))
#define OBJECT_CLASS(k) ((ObjectClass *)(k))
#define OBJECT_CHECK(type, obj, name) ((type *)(obj))
#define OBJECT_CLASS_CHECK(type, klass, name) ((type *)(klass))
#define OBJECT_GET_CLASS(type, obj, name) ((type *)OBJECT(obj)->klass)
#define type_register_static(info) ((void)(info))
#define type_init(fn)
#define object_unparent(obj) ((void)(obj))
static inline const char *object_get_typename(Object *o) { return o->type_name ? o->type_name : "?"; }

#define OBJECT_DECLARE_TYPE(InstanceType, ClassType, MODULE_OBJ_NAME) \
    typedef struct InstanceType InstanceType; \
    typedef struct ClassType ClassType; \
    static inline G_GNUC_UNUSED InstanceType *MODULE_OBJ_NAME(const void *obj) { return (InstanceType *)obj; } \
    static inline G_GNUC_UNUSED ClassType *MODULE_OBJ_NAME##_CLASS(const void *klass) { return (ClassType *)klass; } \
    static inline G_GNUC_UNUSED ClassType *MODULE_OBJ_NAME##_GET_CLASS(const void *obj) { return (ClassType *)OBJECT(obj)->klass; }
#define OBJECT_DECLARE_SIMPLE_TYPE(InstanceType, MODULE_OBJ_NAME) \
    typedef struct InstanceType InstanceType; \
    static inline G_GNUC_UNUSED InstanceType *MODULE_OBJ_NAME(const void *obj) { return (InstanceType *)obj; }
#endif
