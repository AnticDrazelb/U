// The fonts, included whole (XB_UI_FONTS: frontend/ui/fonts, set by the build).

#include "ui/fonts.h"

#ifndef XB_UI_FONTS
#error "XB_UI_FONTS (frontend/ui/fonts) must be set"
#endif

#define XB_UI_INCLUDE_FONT(name, file)                                                                         \
    __asm__(".pushsection .rodata\n"                                                                              \
            ".balign 16\n"                                                                                         \
            ".global " #name "\n" #name ":\n"                                                                      \
            ".incbin \"" XB_UI_FONTS "/" file "\"\n"                                                         \
            ".global " #name "_end\n" #name "_end:\n"                                                              \
            ".popsection\n")

XB_UI_INCLUDE_FONT(xb_ui_font_regular, "Inter-Regular.otf");
XB_UI_INCLUDE_FONT(xb_ui_font_semibold, "Inter-SemiBold.otf");
XB_UI_INCLUDE_FONT(xb_ui_font_pixel_regular, "PixelifySans-Regular.ttf");
XB_UI_INCLUDE_FONT(xb_ui_font_pixel_semibold, "PixelifySans-SemiBold.ttf");

extern "C" const unsigned char xb_ui_font_regular[], xb_ui_font_regular_end[];
extern "C" const unsigned char xb_ui_font_semibold[], xb_ui_font_semibold_end[];
extern "C" const unsigned char xb_ui_font_pixel_regular[], xb_ui_font_pixel_regular_end[];
extern "C" const unsigned char xb_ui_font_pixel_semibold[], xb_ui_font_pixel_semibold_end[];

namespace Ui {

FontData UiFont(Weight weight, FontFamily family)
{
    if (family == FontFamily::Pixel)
    {
        if (weight == Weight::SemiBold)
            return {xb_ui_font_pixel_semibold,
                    size_t(xb_ui_font_pixel_semibold_end - xb_ui_font_pixel_semibold)};
        return {xb_ui_font_pixel_regular, size_t(xb_ui_font_pixel_regular_end - xb_ui_font_pixel_regular)};
    }
    if (weight == Weight::SemiBold)
        return {xb_ui_font_semibold, size_t(xb_ui_font_semibold_end - xb_ui_font_semibold)};
    return {xb_ui_font_regular, size_t(xb_ui_font_regular_end - xb_ui_font_regular)};
}

} // namespace Ui
