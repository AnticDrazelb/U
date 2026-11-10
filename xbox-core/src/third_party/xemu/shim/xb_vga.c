/* xbox-core: QEMU's VGA register protocol (hw/display/vga.c, GPL-2.0-or-later: vga_ioport_read and
 * vga_ioport_write), over the reduced state in shim/hw/display/vga_int.h. Retrace is QEMU's "dumb"
 * toggle; the memory-access and VBE updates QEMU makes on writes have nothing to update here. */
#include "hw/display/vga_int.h"

static const uint8_t sr_mask[8] = {0x03, 0x3d, 0x0f, 0x3f, 0x0e, 0x00, 0x00, 0xff};
static const uint8_t gr_mask[16] = {0x0f, 0x0f, 0x0f, 0x1f, 0x03, 0x7b, 0x0f, 0x0f,
                                    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static int vga_ioport_invalid(VGACommonState *s, uint32_t addr)
{
    if (s->msr & VGA_MIS_COLOR) {
        return (addr >= 0x3b0 && addr <= 0x3bf);
    } else {
        return (addr >= 0x3d0 && addr <= 0x3df);
    }
}

void vga_common_reset(VGACommonState *s)
{
    s->sr_index = s->gr_index = s->ar_index = s->cr_index = 0;
    memset(s->sr, 0, sizeof(s->sr));
    memset(s->gr, 0, sizeof(s->gr));
    memset(s->ar, 0, sizeof(s->ar));
    memset(s->cr, 0, sizeof(s->cr));
    s->ar_flip_flop = 0;
    s->msr = s->fcr = s->st00 = s->st01 = 0;
    s->dac_state = s->dac_sub_index = s->dac_read_index = s->dac_write_index = 0;
    memset(s->dac_cache, 0, sizeof(s->dac_cache));
    s->dac_8bit = 0;
    memset(s->palette, 0, sizeof(s->palette));
}

uint32_t vga_ioport_read(void *opaque, uint32_t addr)
{
    VGACommonState *s = opaque;
    int val, index;

    if (vga_ioport_invalid(s, addr)) {
        val = 0xff;
    } else {
        switch (addr) {
        case VGA_ATT_W:
            val = s->ar_flip_flop == 0 ? s->ar_index : 0;
            break;
        case VGA_ATT_R:
            index = s->ar_index & 0x1f;
            val = index < VGA_ATT_C ? s->ar[index] : 0;
            break;
        case VGA_MIS_W:
            val = s->st00;
            break;
        case VGA_SEQ_I:
            val = s->sr_index;
            break;
        case VGA_SEQ_D:
            val = s->sr[s->sr_index];
            break;
        case VGA_PEL_IR:
            val = s->dac_state;
            break;
        case VGA_PEL_IW:
            val = s->dac_write_index;
            break;
        case VGA_PEL_D:
            val = s->palette[s->dac_read_index * 3 + s->dac_sub_index];
            if (++s->dac_sub_index == 3) {
                s->dac_sub_index = 0;
                s->dac_read_index++;
            }
            break;
        case VGA_FTC_R:
            val = s->fcr;
            break;
        case VGA_MIS_R:
            val = s->msr;
            break;
        case VGA_GFX_I:
            val = s->gr_index;
            break;
        case VGA_GFX_D:
            val = s->gr[s->gr_index];
            break;
        case VGA_CRT_IM:
        case VGA_CRT_IC:
            val = s->cr_index;
            break;
        case VGA_CRT_DM:
        case VGA_CRT_DC:
            val = s->cr[s->cr_index];
            break;
        case VGA_IS1_RM:
        case VGA_IS1_RC:
            /* just toggle to fool polling */
            val = s->st01 = s->st01 ^ (ST01_V_RETRACE | ST01_DISP_ENABLE);
            s->ar_flip_flop = 0;
            break;
        default:
            val = 0x00;
            break;
        }
    }
    return val;
}

void vga_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    VGACommonState *s = opaque;
    int index;

    if (vga_ioport_invalid(s, addr)) {
        return;
    }
    switch (addr) {
    case VGA_ATT_W:
        if (s->ar_flip_flop == 0) {
            val &= 0x3f;
            s->ar_index = val;
        } else {
            index = s->ar_index & 0x1f;
            switch (index) {
            case VGA_ATC_PALETTE0 ... VGA_ATC_PALETTEF:
                s->ar[index] = val & 0x3f;
                break;
            case VGA_ATC_MODE:
                s->ar[index] = val & ~0x10;
                break;
            case VGA_ATC_OVERSCAN:
                s->ar[index] = val;
                break;
            case VGA_ATC_PLANE_ENABLE:
                s->ar[index] = val & ~0xc0;
                break;
            case VGA_ATC_PEL:
                s->ar[index] = val & ~0xf0;
                break;
            case VGA_ATC_COLOR_PAGE:
                s->ar[index] = val & ~0xf0;
                break;
            default:
                break;
            }
        }
        s->ar_flip_flop ^= 1;
        break;
    case VGA_MIS_W:
        s->msr = val & ~0x10;
        break;
    case VGA_SEQ_I:
        s->sr_index = val & 7;
        break;
    case VGA_SEQ_D:
        s->sr[s->sr_index] = val & sr_mask[s->sr_index];
        break;
    case VGA_PEL_IR:
        s->dac_read_index = val;
        s->dac_sub_index = 0;
        s->dac_state = 3;
        break;
    case VGA_PEL_IW:
        s->dac_write_index = val;
        s->dac_sub_index = 0;
        s->dac_state = 0;
        break;
    case VGA_PEL_D:
        s->dac_cache[s->dac_sub_index] = val;
        if (++s->dac_sub_index == 3) {
            memcpy(&s->palette[s->dac_write_index * 3], s->dac_cache, 3);
            s->dac_sub_index = 0;
            s->dac_write_index++;
        }
        break;
    case VGA_GFX_I:
        s->gr_index = val & 0x0f;
        break;
    case VGA_GFX_D:
        s->gr[s->gr_index] = val & gr_mask[s->gr_index];
        break;
    case VGA_CRT_IM:
    case VGA_CRT_IC:
        s->cr_index = val;
        break;
    case VGA_CRT_DM:
    case VGA_CRT_DC:
        /* handle CR0-7 protection */
        if ((s->cr[VGA_CRTC_V_SYNC_END] & VGA_CR11_LOCK_CR0_CR7) && s->cr_index <= VGA_CRTC_OVERFLOW) {
            /* can always write bit 4 of CR7 */
            if (s->cr_index == VGA_CRTC_OVERFLOW) {
                s->cr[VGA_CRTC_OVERFLOW] = (s->cr[VGA_CRTC_OVERFLOW] & ~0x10) | (val & 0x10);
            }
            return;
        }
        s->cr[s->cr_index] = val;
        break;
    case VGA_IS1_RM:
    case VGA_IS1_RC:
        s->fcr = val & 0x10;
        break;
    }
}
