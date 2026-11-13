// The loading screen from the moment a game is chosen until its first picture (New Eden's; drawn by
// Midway's presenter, ps4/main_ps4.cpp).

#pragma once

#include <string>
#include <vector>

#include "ui/canvas.h"

namespace Ui {

struct LoadingScreen
{
    std::string name, publisher;
    Image icon;              // the game's tile (its cover), else a plain tile
    bool white_theme = false; // the HOME menu's theme: Light (black on white), else Dark
    std::vector<std::string> lines; // the log's last lines, small, along the bottom (none: no log)
};

// One frame of it, `now` seconds into the loading (its bar moves with it). Stage 1: loading the
// game; 2: building its shaders, `done` of `total`; 3: starting it. `failed`: it stopped with an
// error: said so, and how to leave; past 3 minutes without that or a first picture, a hint that it
// may not be working.
void DrawLoadingScreen(Canvas& c, const LoadingScreen& screen, int stage, unsigned done, unsigned total, double now,
                       bool failed = false);

} // namespace Ui
