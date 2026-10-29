// The fonts, included whole (EDEN_PS4_UI_FONTS: the ui-fonts folder, set by the build).

#include "ui/fonts.h"

#ifndef EDEN_PS4_UI_FONTS
#error "EDEN_PS4_UI_FONTS (the ui-fonts folder) must be set"
#endif

#define EDEN_PS4_INCLUDE_FONT(name, file)                                                                         \
    __asm__(".pushsection .rodata\n"                                                                              \
            ".balign 16\n"                                                                                         \
            ".global " #name "\n" #name ":\n"                                                                      \
            ".incbin \"" EDEN_PS4_UI_FONTS "/" file "\"\n"                                                         \
            ".global " #name "_end\n" #name "_end:\n"                                                              \
            ".popsection\n")

EDEN_PS4_INCLUDE_FONT(eden_ps4_font_regular, "Inter-Regular.otf");
EDEN_PS4_INCLUDE_FONT(eden_ps4_font_semibold, "Inter-SemiBold.otf");

extern "C" const unsigned char eden_ps4_font_regular[], eden_ps4_font_regular_end[];
extern "C" const unsigned char eden_ps4_font_semibold[], eden_ps4_font_semibold_end[];

namespace Ui {

FontData UiFont(Weight weight)
{
    if (weight == Weight::SemiBold)
        return {eden_ps4_font_semibold, size_t(eden_ps4_font_semibold_end - eden_ps4_font_semibold)};
    return {eden_ps4_font_regular, size_t(eden_ps4_font_regular_end - eden_ps4_font_regular)};
}

} // namespace Ui
