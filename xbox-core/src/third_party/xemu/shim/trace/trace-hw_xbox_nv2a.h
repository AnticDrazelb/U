/* xbox-core: xemu's NV2A trace points, compiled out (functions: some are used as pointers) */
#include <stdint.h>
#include <stdbool.h>
static inline void trace_nv2a_reg_read(const char *block, uint32_t addr, unsigned int size, uint64_t val) {}
static inline void trace_nv2a_reg_write(const char *block, uint32_t addr, unsigned int size, uint64_t val) {}
static inline void trace_nv2a_irq(uint32_t pending) {}
static inline void trace_nv2a_dma_map(uint32_t obj_address, uint32_t dma_class, uint32_t dma_target, uint32_t dma_addr, uint32_t dma_limit) {}
static inline void trace_nv2a_pgraph_method(uint32_t subchannel, uint32_t graphics_class, uint32_t method, const char *name, uint32_t offset, uint32_t parameter) {}
static inline void trace_nv2a_pgraph_method_abbrev(uint32_t subchannel, uint32_t graphics_class, uint32_t method, const char *name, unsigned int count) {}
static inline void trace_nv2a_pgraph_method_unhandled(uint32_t subchannel, uint32_t graphics_class, uint32_t method, uint32_t parameter) {}
static inline void trace_nv2a_pgraph_surface_compare_mismatch(const char *field, long int a, long int b) {}
static inline void trace_nv2a_pgraph_surface_cpu_read(uint32_t addr, uint32_t offset) {}
static inline void trace_nv2a_pgraph_surface_cpu_write(uint32_t addr, uint32_t offset) {}
static inline void trace_nv2a_pgraph_surface_create_color(uint32_t addr, uint32_t width, uint32_t height, const char *layout, uint32_t anti_aliasing, uint32_t clip_x, uint32_t clip_width, uint32_t clip_y, uint32_t clip_height, uint32_t pitch) {}
static inline void trace_nv2a_pgraph_surface_create_zeta(uint32_t addr, uint32_t width, uint32_t height, const char *layout, uint32_t anti_aliasing, uint32_t clip_x, uint32_t clip_width, uint32_t clip_y, uint32_t clip_height, uint32_t pitch) {}
static inline void trace_nv2a_pgraph_surface_download(const char *binding, const char *layout, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bytes_per_pixel) {}
static inline void trace_nv2a_pgraph_surface_evict_overlapping(uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch) {}
static inline void trace_nv2a_pgraph_surface_evict_reason(const char *reason, uint32_t addr) {}
static inline void trace_nv2a_pgraph_surface_hit_color(uint32_t addr, uint32_t width, uint32_t height, const char *layout, uint32_t anti_aliasing, uint32_t clip_x, uint32_t clip_width, uint32_t clip_y, uint32_t clip_height, uint32_t pitch) {}
static inline void trace_nv2a_pgraph_surface_hit_zeta(uint32_t addr, uint32_t width, uint32_t height, const char *layout, uint32_t anti_aliasing, uint32_t clip_x, uint32_t clip_width, uint32_t clip_y, uint32_t clip_height, uint32_t pitch) {}
static inline void trace_nv2a_pgraph_surface_invalidated(uint32_t addr) {}
static inline void trace_nv2a_pgraph_surface_match_color(uint32_t addr, uint32_t width, uint32_t height, const char *layout, uint32_t anti_aliasing, uint32_t clip_x, uint32_t clip_width, uint32_t clip_y, uint32_t clip_height, uint32_t pitch) {}
static inline void trace_nv2a_pgraph_surface_match_zeta(uint32_t addr, uint32_t width, uint32_t height, const char *layout, uint32_t anti_aliasing, uint32_t clip_x, uint32_t clip_width, uint32_t clip_y, uint32_t clip_height, uint32_t pitch) {}
static inline void trace_nv2a_pgraph_surface_migrate_type(const char *new_type) {}
static inline void trace_nv2a_pgraph_surface_render_to_texture(uint32_t addr, uint32_t width, uint32_t height) {}
static inline void trace_nv2a_pgraph_surface_target(const char *binding, uint32_t addr, const char *layout, uint32_t anti_aliasing, uint32_t clip_x, uint32_t clip_width, uint32_t clip_y, uint32_t clip_height) {}
static inline void trace_nv2a_pgraph_surface_texture_compat_failed(int32_t surface_fmt, int32_t texture_fmt) {}
static inline void trace_nv2a_pgraph_surface_upload(const char *binding, const char *layout, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bytes_per_pixel) {}
static inline void trace_nv2a_pgraph_flip_stall(void) {}
static inline void trace_nv2a_pgraph_flip_increment_write(uint32_t write3d_old, uint32_t write3d_new) {}
