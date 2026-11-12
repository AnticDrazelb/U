/* xbox-core: where xemu's renderer thread spends its time (the stats line's "the GPU's work"): for
 * each kind of work, how many and how long, since the last report. */
#ifndef HW_XBOX_NV2A_XB_PROFILE_H
#define HW_XBOX_NV2A_XB_PROFILE_H

#include <stdint.h>
#include <time.h>

enum {
    XB_PROF_BUSY,         /* the PFIFO thread at work (not waiting for methods) */
    XB_PROF_DRAW,         /* a draw recorded (its state, dirty checks, uploads, the commands) */
    XB_PROF_GLSL,         /* a shader made from GLSL (glslang) */
    XB_PROF_SPV_CACHED,   /* ... found in the shader cache instead */
    XB_PROF_PIPELINE,     /* a pipeline made (the driver compiles the shaders) */
    XB_PROF_FINISH,       /* the commands so far submitted and waited for */
    XB_PROF_DOWNLOAD,     /* a surface written back to RAM */
    XB_PROF_SURF_UPLOAD,  /* a surface uploaded from RAM */
    XB_PROF_TEX_UPLOAD,   /* a texture uploaded */
    XB_PROF_COUNT
};

extern uint64_t xb_prof_n[XB_PROF_COUNT], xb_prof_ns[XB_PROF_COUNT];
/* surfaces written back to RAM so far (the picture is copied again only after one) */
extern uint64_t xb_surface_writebacks;

static inline uint64_t xb_prof_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static inline void xb_prof_add(int what, uint64_t since)
{
    __atomic_add_fetch(&xb_prof_n[what], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&xb_prof_ns[what], xb_prof_now() - since, __ATOMIC_RELAXED);
}

/* the report since the last one, over `seconds` (nv2a.c) */
int xb_nv2a_profile(char *out, int size, double seconds);

#endif
