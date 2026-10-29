// What the HOME menu draws besides text: its icons, the PS4's button glyphs, the built-in
// programs' tiles and the covers of titles without an icon.

#pragma once

#include <string>
#include <string_view>

#include "ui/canvas.h"

namespace Ui {

// The built-in programs' tile art
enum class Art { None, Breakout, Blocks, Controller, Gpu, Terminal };

// A title's tile, size x size pixels: the built-in art, or a cover made from its name (its
// colours from the name, so each title keeps its own)
Image MakeTileArt(Art art, int size);
Image MakeCover(std::string_view name, std::string_view kind, int size);

// The round buttons' icons, centred at (cx, cy), about `size` units across
enum class Icon { News, Games, Controllers, Settings, Power, AllSoftware, User, WiFi, Check, Cross };
void DrawIcon(Canvas& canvas, Icon icon, float cx, float cy, float size, uint32_t color);

// A PS4 button as the menu's hints show it: a disc in `color` with the button's symbol cut out
// in `background`
enum class Glyph { Cross, Circle, Triangle, Square, Options, L1, R1 };
void DrawGlyph(Canvas& canvas, Glyph glyph, float cx, float cy, uint32_t color, uint32_t background);
// The glyph's width (the Options button is wider than the face buttons)
float GlyphWidth(Glyph glyph);

// A PS4 controller, seen from above, `width` units wide
void DrawController(Canvas& canvas, float cx, float cy, float width, uint32_t color, uint32_t background);

} // namespace Ui
