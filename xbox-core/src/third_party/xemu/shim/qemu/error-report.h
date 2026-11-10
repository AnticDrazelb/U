/* xbox-core: QEMU's error reporting into xbox-core's log */
#ifndef XB_XEMU_ERROR_REPORT_H
#define XB_XEMU_ERROR_REPORT_H
void xb_xemu_log(const char *fmt, ...);
#define error_report(...) xb_xemu_log(__VA_ARGS__)
#define warn_report(...) xb_xemu_log(__VA_ARGS__)
#define info_report(...) xb_xemu_log(__VA_ARGS__)
#endif
