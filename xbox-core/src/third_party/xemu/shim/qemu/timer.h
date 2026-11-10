/* xbox-core: QEMU's millisecond timers, on one thread (shim/xb_qemu_timer.c) */
#ifndef XB_XEMU_TIMER_H
#define XB_XEMU_TIMER_H
#include "qemu/osdep.h"
enum { QEMU_CLOCK_VIRTUAL = 1 };
typedef void QEMUTimerCB(void *opaque);
typedef struct QEMUTimer {
    QEMUTimerCB *cb;
    void *opaque;
    int64_t scale;  /* ns per unit */
    int64_t expire; /* ns, or -1 */
    struct QEMUTimer *next;
} QEMUTimer;
void timer_init_ns(QEMUTimer *t, int clock, QEMUTimerCB *cb, void *opaque);
void timer_init_ms(QEMUTimer *t, int clock, QEMUTimerCB *cb, void *opaque);
QEMUTimer *timer_new_ms(int clock, QEMUTimerCB *cb, void *opaque);
QEMUTimer *timer_new_ns(int clock, QEMUTimerCB *cb, void *opaque);
void timer_mod(QEMUTimer *t, int64_t expire); /* in the timer's own unit (ms or ns) */
void timer_del(QEMUTimer *t);
void timer_free(QEMUTimer *t);
bool timer_pending(QEMUTimer *t);
static inline int64_t qemu_clock_get_ms(int clock) { return qemu_clock_get_us(clock) / 1000; }
static inline int64_t qemu_clock_get_ns(int clock) { return qemu_clock_get_us(clock) * 1000; }
#define NANOSECONDS_PER_SECOND 1000000000LL
#endif
