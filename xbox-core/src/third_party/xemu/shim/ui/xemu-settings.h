/* xbox-core: the audio settings xemu's APU reads, at xemu's defaults (DSP option off, HRTF on) */
#ifndef XB_XEMU_SETTINGS_H
#define XB_XEMU_SETTINGS_H
#include <stdbool.h>
struct XbXemuConfig {
    struct {
        bool use_dsp, use_dsp_jit, hrtf;
        float volume_limit;
        struct { int num_workers; } vp;
    } audio;
};
extern struct XbXemuConfig g_config;
#endif
