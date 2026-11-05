// The loading screen from the moment a game is chosen until its first frame (drawn by the launcher on
// its own screen, then by the game display into its buffers: Common::PS4Diagnostics::draw_loading).

#pragma once

#include <string>

#include "ui/canvas.h"

namespace Ui {

struct LoadingScreen
{
    std::string name, publisher;
    Image icon;              // the game's, when its last boot left one (else a plain tile)
    bool white_theme = false; // the HOME menu's theme
};

// One frame of it, `now` seconds into the loading (its bar moves with it). Stage 1: loading the
// game; 2: building its shaders, `done` of `total`; 3: starting it.
void DrawLoadingScreen(Canvas& c, const LoadingScreen& screen, int stage, unsigned done, unsigned total, double now);

} // namespace Ui
