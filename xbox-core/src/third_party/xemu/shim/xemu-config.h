/* xbox-core: the settings xemu's devices read (xemu generates this from its config spec) */
#ifndef XB_XEMU_CONFIG_H
#define XB_XEMU_CONFIG_H
#include <stdbool.h>
typedef enum CONFIG_DISPLAY_RENDERER {
    CONFIG_DISPLAY_RENDERER_NULL,
    CONFIG_DISPLAY_RENDERER_OPENGL,
    CONFIG_DISPLAY_RENDERER_VULKAN,
    CONFIG_DISPLAY_RENDERER__COUNT
} CONFIG_DISPLAY_RENDERER;
struct XbXemuConfig {
    struct {
        bool use_dsp, use_dsp_jit, hrtf;
        float volume_limit;
        struct { int num_workers; } vp;
    } audio;
    struct {
        CONFIG_DISPLAY_RENDERER renderer;
        struct { int surface_scale; } quality;
        struct {
            bool validation_layers, debug_shaders, assert_on_validation_msg;
            const char *preferred_physical_device;
        } vulkan;
    } display;
    struct { bool cache_shaders; } perf;
};
extern struct XbXemuConfig g_config;
static inline void xemu_settings_set_string(const char **dst, const char *s) { *dst = s; }
#endif
