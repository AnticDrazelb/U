/* xbox-core: QEMU's threads, mutexes and condition variables on pthreads */
#ifndef XB_XEMU_THREAD_H
#define XB_XEMU_THREAD_H
#include <pthread.h>
#include <errno.h>
#include "qemu/osdep.h"
typedef struct QemuMutex { pthread_mutex_t m; } QemuMutex;
typedef struct QemuCond { pthread_cond_t c; } QemuCond;
typedef struct QemuThread { pthread_t t; } QemuThread;
#define QEMU_THREAD_JOINABLE 0
static inline void qemu_mutex_init(QemuMutex *m) { pthread_mutex_init(&m->m, NULL); }
static inline void qemu_mutex_lock(QemuMutex *m) { pthread_mutex_lock(&m->m); }
static inline void qemu_mutex_unlock(QemuMutex *m) { pthread_mutex_unlock(&m->m); }
static inline void qemu_cond_init(QemuCond *c) { pthread_cond_init(&c->c, NULL); }
static inline void qemu_cond_signal(QemuCond *c) { pthread_cond_signal(&c->c); }
static inline void qemu_cond_broadcast(QemuCond *c) { pthread_cond_broadcast(&c->c); }
static inline void qemu_cond_wait(QemuCond *c, QemuMutex *m) { pthread_cond_wait(&c->c, &m->m); }
static inline bool qemu_cond_timedwait(QemuCond *c, QemuMutex *m, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    return pthread_cond_timedwait(&c->c, &m->m, &ts) != ETIMEDOUT;
}
static inline void qemu_thread_create(QemuThread *t, const char *name, void *(*fn)(void *), void *arg, int mode)
{
    (void)name;
    (void)mode;
    pthread_create(&t->t, NULL, fn, arg);
}
static inline void qemu_thread_join(QemuThread *t) { pthread_join(t->t, NULL); }
#endif
#ifndef XB_XEMU_EVENT
#define XB_XEMU_EVENT
/* QEMU's events: a flag with waiters */
typedef struct QemuEvent { pthread_mutex_t m; pthread_cond_t c; bool set; } QemuEvent;
static inline void qemu_event_init(QemuEvent *e, bool init)
{
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->set = init;
}
static inline void qemu_event_set(QemuEvent *e)
{
    pthread_mutex_lock(&e->m);
    e->set = true;
    pthread_cond_broadcast(&e->c);
    pthread_mutex_unlock(&e->m);
}
static inline void qemu_event_reset(QemuEvent *e)
{
    pthread_mutex_lock(&e->m);
    e->set = false;
    pthread_mutex_unlock(&e->m);
}
static inline void qemu_event_wait(QemuEvent *e)
{
    pthread_mutex_lock(&e->m);
    while (!e->set) {
        pthread_cond_wait(&e->c, &e->m);
    }
    pthread_mutex_unlock(&e->m);
}
static inline void qemu_event_destroy(QemuEvent *e) { (void)e; }
static inline void qemu_mutex_destroy(QemuMutex *m) { pthread_mutex_destroy(&m->m); }
static inline void qemu_cond_destroy(QemuCond *c) { pthread_cond_destroy(&c->c); }
#endif
