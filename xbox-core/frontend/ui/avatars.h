// The user icons: original characters (ui-art/avatars.png, drawn by tools/avatars), each on a
// background colour of the user's choosing, as the Switch's user icons are. The HOME menu shows the
// icon; games get it too, as the Switch user's icon (a 256x256 JPEG).

#pragma once

#include <cstdint>
#include <vector>

#include "ui/canvas.h"

namespace Ui {

int AvatarCount();
const char* AvatarName(int character);
int AvatarColorCount();
uint32_t AvatarColor(int color);

// The character on its colour, size x size pixels; round: clear outside the circle (as the HOME
// menu shows it). An index out of range is the first.
Image MakeAvatar(int character, int color, int size, bool round);
// The art, decoded when first needed, freed (a game needs the memory more)
void ReleaseAvatarArt();
// The image as a JPEG (its alpha left out)
std::vector<uint8_t> EncodeJpeg(const Image& image, int quality = 92);

} // namespace Ui
