// The HOME menu's fonts: Inter (ui-fonts/, SIL Open Font License), in the executable.

#pragma once

#include <cstddef>

#include "ui/canvas.h"

namespace Ui {

struct FontData
{
    const unsigned char* data;
    size_t size;
};
FontData UiFont(Weight weight);

} // namespace Ui
