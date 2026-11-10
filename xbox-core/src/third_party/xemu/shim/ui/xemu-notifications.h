/* xbox-core: xemu's on-screen notifications go to the log */
#ifndef XB_XEMU_NOTIFICATIONS_H
#define XB_XEMU_NOTIFICATIONS_H
void xb_xemu_log(const char *fmt, ...);
#define xemu_queue_notification(msg) xb_xemu_log("%s", (msg))
#define xemu_queue_error_message(msg) xb_xemu_log("%s", (msg))
#endif
