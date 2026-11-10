/* xbox-core: QEMU's Error (shim/qemu/osdep.h has the type and error_setg) */
#ifndef XB_XEMU_QAPI_ERROR_H
#define XB_XEMU_QAPI_ERROR_H
#include "qemu/osdep.h"
extern Error *error_fatal;
static inline const char *error_get_pretty(const Error *e) { return e ? e->message : ""; }
static inline void error_free(Error *e) { free(e); }
static inline void error_propagate(Error **dst, Error *src)
{
    if (dst && !*dst) {
        *dst = src;
    } else {
        free(src);
    }
}
#define error_prepend(errp, ...) ((void)0)
void xb_xemu_log(const char *fmt, ...);
#define error_report_err(err) (xb_xemu_log("%s", error_get_pretty(err)), free(err))
#endif
