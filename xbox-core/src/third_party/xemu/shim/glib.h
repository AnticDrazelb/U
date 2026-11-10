/* xbox-core: what xemu's code uses of glib */
#ifndef XB_XEMU_GLIB_H
#define XB_XEMU_GLIB_H
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char gchar;
typedef unsigned char guint8;
typedef unsigned int guint;
typedef int gint;
typedef int gboolean;
typedef void *gpointer;
typedef size_t gsize;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define g_malloc(n) malloc(n)
#define g_malloc0(n) calloc(1, (n))
#define g_malloc_n(n, size) malloc((size_t)(n) * (size))
#define g_malloc0_n(n, size) calloc((n), (size))
#define g_realloc(p, n) realloc((p), (n))
#define g_new(type, n) ((type *)malloc((size_t)(n) * sizeof(type)))
#define g_new0(type, n) ((type *)calloc((n), sizeof(type)))
#define g_free(p) free(p)
#define g_strdup(s) strdup(s)

static inline char *g_strdup_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = NULL;
    if (vasprintf(&s, fmt, ap) < 0) {
        s = NULL;
    }
    va_end(ap);
    return s;
}

static inline char *g_strdup_vprintf(const char *fmt, va_list ap)
{
    char *s = NULL;
    if (vasprintf(&s, fmt, ap) < 0) {
        s = NULL;
    }
    return s;
}
static inline void xb_g_autofree_cleanup(void *p) { free(*(void **)p); }
#define g_autofree __attribute__((cleanup(xb_g_autofree_cleanup)))

typedef struct GArray {
    char *data;
    unsigned int len;
    unsigned int elt_size, alloc;
    bool zero_terminated, clear;
} GArray;

static inline GArray *g_array_sized_new(gboolean zero_terminated, gboolean clear, guint elt_size, guint reserved)
{
    GArray *a = calloc(1, sizeof(GArray));
    a->elt_size = elt_size;
    a->zero_terminated = zero_terminated;
    a->clear = clear;
    a->alloc = reserved ? reserved : 1;
    a->data = calloc(a->alloc + 1, elt_size);
    return a;
}
static inline GArray *g_array_new(gboolean zero_terminated, gboolean clear, guint elt_size)
{
    return g_array_sized_new(zero_terminated, clear, elt_size, 0);
}
static inline void xb_g_array_reserve(GArray *a, guint len)
{
    if (len + 1 > a->alloc) {
        guint n = a->alloc * 2 > len + 1 ? a->alloc * 2 : len + 1;
        a->data = realloc(a->data, (size_t)n * a->elt_size);
        memset(a->data + (size_t)a->alloc * a->elt_size, 0, (size_t)(n - a->alloc) * a->elt_size);
        a->alloc = n;
    }
}
static inline GArray *g_array_set_size(GArray *a, guint len)
{
    xb_g_array_reserve(a, len);
    if (len > a->len && a->clear) {
        memset(a->data + (size_t)a->len * a->elt_size, 0, (size_t)(len - a->len) * a->elt_size);
    }
    a->len = len;
    return a;
}
static inline GArray *g_array_append_vals(GArray *a, const void *data, guint n)
{
    xb_g_array_reserve(a, a->len + n);
    memcpy(a->data + (size_t)a->len * a->elt_size, data, (size_t)n * a->elt_size);
    a->len += n;
    if (a->zero_terminated) {
        memset(a->data + (size_t)a->len * a->elt_size, 0, a->elt_size);
    }
    return a;
}
#define g_array_append_val(a, v) g_array_append_vals((a), &(v), 1)
#define g_array_index(a, type, i) (((type *)(void *)(a)->data)[(i)])
static inline char *g_array_free(GArray *a, gboolean free_segment)
{
    char *d = a->data;
    if (free_segment) {
        free(d);
        d = NULL;
    }
    free(a);
    return d;
}

typedef struct GByteArray {
    guint8 *data;
    guint len;
} GByteArray;
static inline GByteArray *g_byte_array_new_take(guint8 *data, gsize len)
{
    GByteArray *b = malloc(sizeof(GByteArray));
    b->data = data;
    b->len = (guint)len;
    return b;
}
static inline void g_byte_array_unref(GByteArray *b)
{
    if (b) {
        free(b->data);
        free(b);
    }
}
#define G_DEFINE_AUTOPTR_CLEANUP_FUNC(T, fn) \
    static inline void xb_g_autoptr_##T(T **p) { if (*p) fn(*p); }
#define XB_G_GLUE2(a, b) a##b
#define XB_G_GLUE(a, b) XB_G_GLUE2(a, b)
#define g_autoptr(T) __attribute__((cleanup(XB_G_GLUE(xb_g_autoptr_, T)))) T *
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GByteArray, g_byte_array_unref)
static inline void xb_g_array_unref(GArray *a) { g_array_free(a, TRUE); }
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GArray, xb_g_array_unref)
#define G_GNUC_PRINTF(a, b) __attribute__((format(printf, a, b)))
#define G_GNUC_UNUSED __attribute__((unused))
typedef struct GString {
    char *str;
    size_t len, allocated_len;
} GString;
static inline void xb_g_string_reserve(GString *s, size_t len)
{
    if (len + 1 > s->allocated_len) {
        size_t n = s->allocated_len * 2 > len + 1 ? s->allocated_len * 2 : len + 1;
        s->str = realloc(s->str, n);
        s->allocated_len = n;
    }
}
static inline GString *g_string_new(const char *init)
{
    GString *s = calloc(1, sizeof(GString));
    size_t n = init ? strlen(init) : 0;
    xb_g_string_reserve(s, n + 64);
    if (n) {
        memcpy(s->str, init, n);
    }
    s->len = n;
    s->str[n] = 0;
    return s;
}
static inline GString *g_string_new_take(char *init)
{
    GString *s = calloc(1, sizeof(GString));
    if (init) {
        s->str = init;
        s->len = strlen(init);
        s->allocated_len = s->len + 1;
    } else {
        xb_g_string_reserve(s, 64);
        s->str[0] = 0;
    }
    return s;
}
static inline GString *g_string_append_len(GString *s, const char *v, size_t n)
{
    xb_g_string_reserve(s, s->len + n);
    memcpy(s->str + s->len, v, n);
    s->len += n;
    s->str[s->len] = 0;
    return s;
}
static inline GString *g_string_append(GString *s, const char *v) { return g_string_append_len(s, v, strlen(v)); }
static inline void g_string_append_vprintf(GString *s, const char *fmt, va_list ap)
{
    char *t = NULL;
    if (vasprintf(&t, fmt, ap) >= 0) {
        g_string_append(s, t);
        free(t);
    }
}
static inline void g_string_append_printf(GString *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_string_append_vprintf(s, fmt, ap);
    va_end(ap);
}
static inline char *g_string_free(GString *s, gboolean free_segment)
{
    char *d = s->str;
    if (free_segment) {
        free(d);
        d = NULL;
    }
    free(s);
    return d;
}
#endif
