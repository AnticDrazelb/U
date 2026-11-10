/* xbox-core: what xemu's nvnet uses of QEMU's net layer. There is no network: the link is down (no
 * cable), as a console with nothing plugged in; packets sent are dropped (xb_nic_send logs them). */
#ifndef XB_XEMU_NET_H
#define XB_XEMU_NET_H
#include "qemu/osdep.h"
#include <sys/uio.h>
typedef struct MACAddr { uint8_t a[6]; } MACAddr;
typedef struct NICConf { MACAddr macaddr; } NICConf;
typedef struct NetClientState { bool link_down; void *opaque; } NetClientState;
typedef struct NICState { NetClientState nc; } NICState;
typedef struct NetClientInfo {
    int type;
    size_t size;
    bool (*can_receive)(NetClientState *);
    ssize_t (*receive)(NetClientState *, const uint8_t *, size_t);
    ssize_t (*receive_iov)(NetClientState *, const struct iovec *, int);
    void (*link_status_changed)(NetClientState *);
} NetClientInfo;
#define NET_CLIENT_DRIVER_NIC 0
static inline NetClientState *qemu_get_queue(NICState *nic) { return &nic->nc; }
static inline void *qemu_get_nic_opaque(NetClientState *nc) { return nc->opaque; }
void xb_nic_send(const uint8_t *buf, size_t size);
#define qemu_send_packet(nc, buf, size) xb_nic_send((buf), (size))
#endif
