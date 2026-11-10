/* xbox-core's stand-in for QEMU's bswap.h: little-endian loads and stores (the hosts are x86-64) */
#ifndef XB_XEMU_BSWAP_H
#define XB_XEMU_BSWAP_H
#include <stdint.h>
#include <string.h>
static inline uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void stl_le_p(void *p, uint32_t v) { memcpy(p, &v, 4); }
#endif
#ifndef XB_XEMU_BSWAP_LE
#define XB_XEMU_BSWAP_LE
#define cpu_to_le16(x) ((uint16_t)(x))
#define cpu_to_le32(x) ((uint32_t)(x))
#define le16_to_cpu(x) ((uint16_t)(x))
#define le32_to_cpu(x) ((uint32_t)(x))
static inline uint32_t lduw_le_p(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline void stw_le_p(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline uint64_t ldq_le_p(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void stq_le_p(void *p, uint64_t v) { memcpy(p, &v, 8); }
static inline int ldub_p(const void *p) { return *(const uint8_t *)p; }
static inline void stb_p(void *p, uint8_t v) { *(uint8_t *)p = v; }
#endif
