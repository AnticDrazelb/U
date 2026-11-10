/* xbox-core: QEMU's iov helpers */
#ifndef XB_XEMU_IOV_H
#define XB_XEMU_IOV_H
#include "qemu/osdep.h"
#include <sys/uio.h>
static inline size_t iov_size(const struct iovec *iov, unsigned int n)
{
    size_t s = 0;
    for (unsigned int i = 0; i < n; i++) {
        s += iov[i].iov_len;
    }
    return s;
}
static inline size_t iov_to_buf(const struct iovec *iov, unsigned int n, size_t offset, void *buf, size_t bytes)
{
    size_t done = 0;
    for (unsigned int i = 0; i < n && done < bytes; i++) {
        size_t len = iov[i].iov_len;
        if (offset >= len) {
            offset -= len;
            continue;
        }
        size_t c = MIN(len - offset, bytes - done);
        memcpy((char *)buf + done, (char *)iov[i].iov_base + offset, c);
        done += c;
        offset = 0;
    }
    return done;
}
static inline size_t iov_from_buf(const struct iovec *iov, unsigned int n, size_t offset, const void *buf, size_t bytes)
{
    size_t done = 0;
    for (unsigned int i = 0; i < n && done < bytes; i++) {
        size_t len = iov[i].iov_len;
        if (offset >= len) {
            offset -= len;
            continue;
        }
        size_t c = MIN(len - offset, bytes - done);
        memcpy((char *)iov[i].iov_base + offset, (const char *)buf + done, c);
        done += c;
        offset = 0;
    }
    return done;
}
static inline size_t iov_memset(const struct iovec *iov, unsigned int n, size_t offset, int fill, size_t bytes)
{
    size_t done = 0;
    for (unsigned int i = 0; i < n && done < bytes; i++) {
        size_t len = iov[i].iov_len;
        if (offset >= len) {
            offset -= len;
            continue;
        }
        size_t c = MIN(len - offset, bytes - done);
        memset((char *)iov[i].iov_base + offset, fill, c);
        done += c;
        offset = 0;
    }
    return done;
}
/* QEMU's I/O vectors */
typedef struct QEMUIOVector {
    struct iovec *iov;
    int niov;
    int nalloc;
    size_t size;
} QEMUIOVector;
static inline void qemu_iovec_init(QEMUIOVector *q, int alloc_hint)
{
    q->iov = calloc(alloc_hint > 0 ? alloc_hint : 1, sizeof(struct iovec));
    q->niov = 0;
    q->nalloc = alloc_hint > 0 ? alloc_hint : 1;
    q->size = 0;
}
static inline void qemu_iovec_add(QEMUIOVector *q, void *base, size_t len)
{
    if (q->niov == q->nalloc) {
        q->nalloc = 2 * q->nalloc + 1;
        q->iov = realloc(q->iov, q->nalloc * sizeof(struct iovec));
    }
    q->iov[q->niov].iov_base = base;
    q->iov[q->niov].iov_len = len;
    q->size += len;
    q->niov++;
}
static inline void qemu_iovec_reset(QEMUIOVector *q)
{
    q->niov = 0;
    q->size = 0;
}
static inline void qemu_iovec_destroy(QEMUIOVector *q)
{
    free(q->iov);
    q->iov = NULL;
    q->niov = q->nalloc = 0;
    q->size = 0;
}
typedef struct QEMUSGList { int unused; } QEMUSGList; /* DMA scatter lists: not used by the devices built */
#endif
