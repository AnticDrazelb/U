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
// game; 2: building its shaders, `done` of `total`; 3: starting it. (04.69) `failed`: the game hit
// an error (its own check failing, its abort): said so, and how to leave; past 3 minutes without
// that or a first frame, a hint that it may not be working.
void DrawLoadingScreen(Canvas& c, const LoadingScreen& screen, int stage, unsigned done, unsigned total, double now,
                       bool failed = false);

} // namespace Ui
