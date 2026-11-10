/* xbox-core: QEMU's VGA state, reduced to what the NV2A uses (its CRTC/sequencer/graphics/attribute
 * registers, the DAC; no legacy memory windows, no text modes, no VBE). shim/xb_vga.c: QEMU's
 * register protocol (vga_ioport_read/write). */
#ifndef XB_XEMU_VGA_INT_H
#define XB_XEMU_VGA_INT_H
#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/display/vga_regs.h"

#define ST01_V_RETRACE 0x08
#define ST01_DISP_ENABLE 0x01
#define VGA_MAX_HEIGHT 2048

typedef struct VGADisplayParams {
    uint32_t line_offset;
    uint32_t start_addr;
    uint32_t line_compare;
    uint8_t hpel;
    bool hpel_split;
} VGADisplayParams;

typedef struct GraphicHwOps {
    void (*gfx_update)(void *opaque);
} GraphicHwOps;
typedef struct QemuConsole QemuConsole;

typedef struct VGACommonState {
    uint8_t *vram_ptr;
    MemoryRegion vram;
    uint32_t vram_size_mb;
    uint8_t sr_index;
    uint8_t sr[256];
    uint8_t gr_index;
    uint8_t gr[256];
    uint8_t ar_index;
    uint8_t ar[21];
    int ar_flip_flop;
    uint8_t cr_index;
    uint8_t cr[256];
    uint8_t msr, fcr, st00, st01;
    uint8_t dac_state, dac_sub_index, dac_read_index, dac_write_index;
    uint8_t dac_cache[3];
    int dac_8bit;
    uint8_t palette[768];
    int (*get_bpp)(struct VGACommonState *s);
    void (*get_params)(struct VGACommonState *s, VGADisplayParams *params);
    void (*get_resolution)(struct VGACommonState *s, int *pwidth, int *pheight);
    QemuConsole *con;
    const GraphicHwOps *hw_ops;
} VGACommonState;

uint32_t vga_ioport_read(void *opaque, uint32_t addr);
void vga_ioport_write(void *opaque, uint32_t addr, uint32_t val);
void vga_common_reset(VGACommonState *s);
static inline void vga_invalidate_scanlines(VGACommonState *s, int y1, int y2) { (void)s; (void)y1; (void)y2; }
static inline void vga_display_params(VGACommonState *s, VGADisplayParams *p) { s->get_params(s, p); }
#endif
