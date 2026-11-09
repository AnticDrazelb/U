// The front end's screen (1920x1080), light and friendly: a title and the clock at the top, the
// collection as a carousel of covers (the chosen one raised and glowing, a caption under each), a dock
// of the four places (Collection, Settings, Tools, About) at the bottom, the controller's hints in
// the corners.
// Drawn in software into any RGBA buffer: the PS4's scan-out, or a PNG (tools/ui-preview).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "canvas.h"
#include "library.h"
#include "tools.h"

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
    LibraryPaths library;          // to scan again after a copy
    std::vector<std::string> usb;  // where USB drives may be mounted
    // the diagnostics report: the app's own lines, the logs to quote (file, last lines), where to
    // write it (each USB drive gets a copy too, as <drive>/xbox-core-diagnostics.txt)
    std::vector<std::string> system;
    std::vector<std::pair<std::string, size_t>> logs;
    std::string report;
    std::string data_dir;
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
    enum Tab { kLibrary, kSettings, kTools, kAbout, kTabs };
    struct Covers
    {
        Image card, large;
    };

    void Sort();
    const Covers& CoversFor(const Game& g);
    Image GeneratedCover(const Game& g, int w, int h);
    void BuildBackground();
    void DrawHeader(Canvas& c, const std::string& clock, const std::string& user);
    void DrawDock(Canvas& c);
    void DrawIcon(Canvas& c, int tab, int cx, int cy, Color color, Color back);
    void DrawLibrary(Canvas& c, double seconds);
    void DrawInfo(Canvas& c, const Game& g, int y);
    void DrawSettings(Canvas& c);
    void DrawAbout(Canvas& c);
    void DrawTools(Canvas& c);
    void DrawCopyProgress(Canvas& c, int x, int y, int w);
    void UpdateTools(const Input& in);
    void Rescan();
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
    int first = 0;      // the first card the carousel shows
    float scroll = 0;   // where it is drawn from: easing towards `first`
    int setting = 0;
    bool options_open = false;
    int option = 0;
    std::string toast;
    double toast_until = 0, now = 0;
    std::map<std::string, Covers> covers;
    std::vector<uint32_t> background;
    std::string autostart_game; // the autostart file's game, or ""
    // tools
    int tool = 0;
    bool usb_view = false;
    size_t usb_selected = 0;
    std::vector<std::string> usb_roots;
    std::vector<UsbGame> usb_games;
    Copier copier;
    Copier::Status copy;
    std::vector<std::string> report_written;
};

} // namespace fe
