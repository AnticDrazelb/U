// The front end's screen (1920x1080): a header with the tabs (Library, Settings, About), the library
// as a grid of covers with the chosen game's details beside it, and the controller's hints below.
// Drawn in software into any RGBA buffer: the PS4's scan-out, or a PNG (tools/ui-preview).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "canvas.h"
#include "library.h"

namespace fe {

// the buttons pressed since the last update
struct Input
{
    bool up = false, down = false, left = false, right = false;
    bool cross = false, circle = false, square = false, triangle = false, l1 = false, r1 = false;
};

struct UiPaths
{
    std::string assets;      // the fonts
    std::string settings;    // a file
    std::string autostart;   // a file: the game to run without the library
    std::vector<std::string> help; // lines for the About tab: where games and covers go
};

class LibraryUi
{
public:
    static constexpr int kWidth = 1920, kHeight = 1080;

    LibraryUi(std::vector<Game> games, Settings& settings, UiPaths paths, std::string version);
    // false when a font is missing (the caller falls back to something plainer)
    bool Ready() const { return ready; }

    // the game to run, once the player chooses one
    const Game* Update(const Input& in);
    void Draw(Canvas& c, double seconds, const std::string& clock, const std::string& user);

private:
    enum Tab { kLibrary, kSettings, kAbout, kTabs };
    struct Covers
    {
        Image card, large;
    };

    void Sort();
    const Covers& CoversFor(const Game& g);
    Image GeneratedCover(const Game& g, int w, int h);
    void BuildBackground();
    void DrawHeader(Canvas& c, const std::string& clock);
    void DrawLibrary(Canvas& c, double seconds);
    void DrawDetails(Canvas& c, const Game& g);
    void DrawSettings(Canvas& c);
    void DrawAbout(Canvas& c);
    void DrawOptions(Canvas& c);
    void DrawHints(Canvas& c, const std::string& user);
    void Glyph(Canvas& c, int cx, int cy, char button, int radius = 20);
    void Toast(const std::string& text);
    bool IsAutostart(const Game& g) const;

    std::vector<Game> games;
    Settings& settings;
    UiPaths paths;
    std::string version;
    Font regular, semibold, bold;
    bool ready = false;
    Tab tab = kLibrary;
    size_t selected = 0;
    int first_row = 0;
    int setting = 0;
    bool options_open = false;
    int option = 0;
    std::string toast;
    double toast_until = 0, now = 0;
    std::map<std::string, Covers> covers;
    std::vector<uint32_t> background;
    std::string autostart_game; // the autostart file's game, or ""
};

} // namespace fe
