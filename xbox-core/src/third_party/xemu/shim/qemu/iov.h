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
#endif
