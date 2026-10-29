// The launcher's HOME menu, laid out as the Switch's: the user's icon and the time at the top, a
// row of title tiles (the selected one framed, its name above it) ending in All Software, the
// round system buttons below with the selected one's name, and the buttons' hints along the
// bottom. Its other screens: All Software (every title), System Settings, News, Games and Keys,
// Controllers, and the report of a run that didn't finish; and Switch-style dialogs. It knows
// nothing of the PS4: the launcher gives it the titles, settings and status, and the buttons held
// each frame, and gets back what to do.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ui/art.h"
#include "ui/canvas.h"

namespace Ui {

// The PS4 controller's buttons, as the menu reads them
enum Button : uint32_t
{
    kUp = 1 << 0,
    kDown = 1 << 1,
    kLeft = 1 << 2,
    kRight = 1 << 3,
    kCross = 1 << 4,
    kCircle = 1 << 5,
    kTriangle = 1 << 6,
    kSquare = 1 << 7,
    kOptions = 1 << 8,
    kL1 = 1 << 9,
    kR1 = 1 << 10,
};

struct Title
{
    std::string path;      // what boots
    std::string name;      // its title
    std::string publisher; // or empty
    std::string location;  // "data folder", "USB 0", "in the package"
    std::string size;      // "5.2 GB"
    std::string kind;      // "NSP", "XCI", "NRO", "NSO"
    bool game = false;     // needs the keys of the Switch it was dumped from
    Art art = Art::None;   // the built-in programs' art
    Image icon;            // its own icon, when known (else its art, else a cover)
};

struct MenuSettings
{
    bool white_theme = false;
    bool software_renderer = false;
    bool docked = false;
    int resolution = 100;      // what games render at, in percent of the console mode's: 100, 75 or 50
    bool shader_cache = true;  // shaders kept on disk, built while a game loads the next time
    bool async_shaders = true; // shaders built in the background (a draw waits for none)
    bool cpu_unsafe = false;   // Eden's Unsafe CPU accuracy instead of Auto
    bool gpu_high = false;     // Eden's High GPU accuracy (and reactive flushing) instead of Normal
    bool seventh_core = true;  // the app's threads may also run on the 7th core (shared with the system)
};

// One line of the Games and Keys screen: something found (or not), and where
struct StatusRow
{
    std::string label, detail;
    bool ok = false;
};

// What a run that didn't finish left behind
struct Report
{
    std::vector<std::string> log, crash, trace;
};

struct MenuAction
{
    enum class Kind { None, Boot, Quit, SettingsChanged, ImportUsb } kind = Kind::None;
    std::string path; // Boot: what to boot
};

class HomeMenu
{
public:
    HomeMenu();

    // The titles, most recently played first (the row shows the first 12)
    void SetTitles(std::vector<Title> titles);
    void SetSettings(const MenuSettings& settings);
    const MenuSettings& Settings() const { return settings; }
    // The About page, and Graphics' status line
    void SetInfo(std::string version, std::string renderer, bool radv_linked);
    void SetClock(std::string time) { clock = std::move(time); }
    void SetController(bool connected) { controller = connected; }
    void SetKeysPresent(bool present) { keys_present = present; }
    // News: headings and their text
    void SetNews(std::vector<std::pair<std::string, std::string>> sections) { news = std::move(sections); }
    void SetGamesStatus(std::vector<StatusRow> rows, std::string help);
    // Controllers' "Test Input Devices" boots this (none: no such button)
    void SetControllerTest(std::string path) { controller_test = std::move(path); }
    // Shown first, until dismissed
    void ShowReport(Report report);
    void ShowMessage(std::string title, std::string text);
    // Changes the message on top (a copy's progress, then its result); false if none is open
    bool UpdateMessage(std::string title, std::string text);
    // "No controller: the test program starts in N s" (negative: hidden)
    void SetCountdown(int seconds) { countdown = seconds; }

    // Buttons held when the menu comes back (from a game) don't count as pressed
    void SetHeld(uint32_t held) { held_before = held; }
    // The buttons held now; returns what the launcher should do
    MenuAction Update(uint32_t held, double now);
    void Draw(Canvas& canvas, double now);

private:
    enum class Screen { Home, AllSoftware, Settings, News, Games, Controllers, Report };
    enum class DialogKind { Message, Quit, TitleInfo, NoKeys };
    struct Dialog
    {
        DialogKind kind = DialogKind::Message;
        std::string title, text;
        std::vector<std::string> buttons;
        int selected = 0;
        size_t title_index = 0;
        double opened = 0;
    };
    struct Theme;
    struct SettingItem
    {
        std::string label, value, description;
        void (*change)(MenuSettings&);
    };
    std::vector<SettingItem> SettingItems(int category) const;

    const Theme& CurrentTheme() const;
    size_t HomeCount() const;
    const Image& Tile(size_t index, int pixels);
    MenuAction Start(size_t index);
    void OpenDialog(Dialog dialog);
    MenuAction Navigate(uint32_t pressed, uint32_t moves);
    MenuAction ChooseDialog(int button);

    void DrawHome(Canvas& c, double now);
    void DrawAllSoftware(Canvas& c, double now);
    void DrawSettings(Canvas& c, double now);
    void DrawNews(Canvas& c);
    void DrawGames(Canvas& c);
    void DrawControllers(Canvas& c, double now);
    void DrawReport(Canvas& c);
    void DrawDialog(Canvas& c, double now);
    void DrawHeader(Canvas& c, Icon icon, uint32_t icon_color, const std::string& text);
    void DrawBottomBar(Canvas& c, const std::vector<std::pair<Glyph, std::string>>& hints);
    uint32_t Cursor(double now) const;

    std::vector<Title> titles;
    std::map<int, std::vector<Image>> tiles; // by size in pixels: each title's tile, made when first drawn
    MenuSettings settings;
    std::string version, renderer, clock = "0:00", controller_test, games_help;
    bool radv_linked = false, controller = true, keys_present = false;
    std::vector<std::pair<std::string, std::string>> news;
    std::vector<StatusRow> games_status;
    Report report;
    int countdown = -1;

    Screen screen = Screen::Home;
    std::vector<Dialog> dialogs;
    // Home: the row (0, tiles) or the buttons (1); the tile (HomeCount() is All Software) and button
    int home_row = 0;
    size_t home_tile = 0;
    int home_button = 0;
    float scroll = 0, scroll_target = 0;
    // All Software
    size_t grid_index = 0;
    float grid_scroll = 0, grid_scroll_target = 0;
    // System Settings: the category, and whether its items have the cursor (and which)
    int settings_category = 0, settings_item = 0;
    bool settings_items = false;
    // News and Games: scrolled this far
    float page_scroll = 0;
    // Buttons held before, and when a held direction repeats
    uint32_t held_before = 0;
    double repeat_at = 0, last_draw = 0;
};

} // namespace Ui
