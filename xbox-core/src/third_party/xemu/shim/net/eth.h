/* xbox-core: what xemu's nvnet uses of QEMU's eth.h */
#ifndef XB_XEMU_ETH_H
#define XB_XEMU_ETH_H
#include "qemu/osdep.h"
#define ETH_ALEN 6
static inline bool is_broadcast_ether_addr(const uint8_t *a)
{
    return (a[0] & a[1] & a[2] & a[3] & a[4] & a[5]) == 0xff;
}
static inline bool is_multicast_ether_addr(const uint8_t *a) { return a[0] & 1; }
#endif
