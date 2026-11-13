// The HOME menu's fonts (ui-fonts/, both under the SIL Open Font License), in the executable: Inter,
// and Pixelify Sans for the Retro theme.

#pragma once

#include <cstddef>

#include "ui/canvas.h"

namespace Ui {

struct FontData
{
    const unsigned char* data;
    size_t size;
};
FontData UiFont(Weight weight, FontFamily family = FontFamily::Inter);

} // namespace Ui
