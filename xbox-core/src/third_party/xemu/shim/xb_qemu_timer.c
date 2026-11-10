/* xbox-core: QEMU's timers for xemu's devices: one thread runs each due callback (as QEMU's main loop
 * would), with no device lock held; the callbacks take the devices' own. */
#include "qemu/timer.h"
#include <pthread.h>

struct QEMUTimer {
    QEMUTimerCB *cb;
    void *opaque;
    int64_t scale;   /* ns per unit */
    int64_t expire;  /* ns, or -1 */
    QEMUTimer *next;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static QEMUTimer *g_timers;
static pthread_t g_thread;
static bool g_started;

static int64_t now_ns(void) { return qemu_clock_get_us(0) * 1000; }

static void *timer_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        int64_t now = now_ns(), soonest = -1;
        QEMUTimer *due = NULL;
        for (QEMUTimer *t = g_timers; t; t = t->next) {
            if (t->expire < 0) {
                continue;
            }
            if (t->expire <= now) {
                due = t;
                break;
            }
            if (soonest < 0 || t->expire < soonest) {
                soonest = t->expire;
            }
        }
        if (due) {
            due->expire = -1;
            pthread_mutex_unlock(&g_lock);
            due->cb(due->opaque);
            pthread_mutex_lock(&g_lock);
            continue;
        }
        if (soonest < 0) {
            pthread_cond_wait(&g_cond, &g_lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int64_t wait = soonest - now;
            ts.tv_sec += wait / 1000000000;
            ts.tv_nsec += wait % 1000000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&g_cond, &g_lock, &ts);
        }
    }
    return NULL;
}

static QEMUTimer *timer_new(int64_t scale, QEMUTimerCB *cb, void *opaque)
{
    QEMUTimer *t = calloc(1, sizeof(QEMUTimer));
    t->cb = cb;
    t->opaque = opaque;
    t->scale = scale;
    t->expire = -1;
    pthread_mutex_lock(&g_lock);
    t->next = g_timers;
    g_timers = t;
    if (!g_started) {
        g_started = true;
        pthread_create(&g_thread, NULL, timer_thread, NULL);
    }
    pthread_mutex_unlock(&g_lock);
    return t;
}

QEMUTimer *timer_new_ms(int clock, QEMUTimerCB *cb, void *opaque) { (void)clock; return timer_new(1000000, cb, opaque); }
QEMUTimer *timer_new_ns(int clock, QEMUTimerCB *cb, void *opaque) { (void)clock; return timer_new(1, cb, opaque); }

void timer_mod(QEMUTimer *t, int64_t expire)
{
    pthread_mutex_lock(&g_lock);
    t->expire = expire * t->scale;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

void timer_del(QEMUTimer *t)
{
    pthread_mutex_lock(&g_lock);
    t->expire = -1;
    pthread_mutex_unlock(&g_lock);
}

bool timer_pending(QEMUTimer *t)
{
    pthread_mutex_lock(&g_lock);
    bool pending = t->expire >= 0;
    pthread_mutex_unlock(&g_lock);
    return pending;
}

void timer_free(QEMUTimer *t)
{
    pthread_mutex_lock(&g_lock);
    for (QEMUTimer **p = &g_timers; *p; p = &(*p)->next) {
        if (*p == t) {
            *p = t->next;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    free(t);
}
