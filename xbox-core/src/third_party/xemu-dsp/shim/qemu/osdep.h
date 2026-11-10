/* xbox-core's stand-in for QEMU's osdep.h: what xemu's DSP code uses of QEMU and glib. */
#ifndef XB_XEMU_OSDEP_H
#define XB_XEMU_OSDEP_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
#define g_new0(type, n) ((type *)calloc((n), sizeof(type)))
#define g_free(p) free(p)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x) __builtin_expect(!!(x), 1)
#endif
