// Midway's HOME menu, New Eden's (eden-ps4/src/ui, laid out as the Switch's): the user's icon and
// the time at the top, a row of title tiles (the selected one framed, its name above it) ending in
// All Software, the round system buttons below with the selected one's name, and the buttons'
// hints along the bottom. Its other screens: All Software (every title), System Settings, News,
// Games and USB, Controllers; and Switch-style dialogs. It knows nothing of the PS4: the launcher
// (frontend/home.cpp) gives it the titles, settings and status, and the buttons held each frame,
// and gets back what to do.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
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
    std::string location;  // "PS4", "USB 0", "in the package"
    std::string size;      // "5.2 GB"
    std::string kind;      // "Disc image", "Game folder", "Sample"
    std::string details;   // more lines for its Options dialog (title ID, region, last run)
    bool on_usb = false;   // on a USB drive: "Copy to the PS4" offered
    bool autostart = false; // the app starts it without the menu
    Art art = Art::None;   // the built-in programs' art
    Image icon;            // its cover made square, when there is one (else its art, else a cover)
};

// The Switch's limit for a user's nickname
constexpr size_t kNicknameLength = 10;

struct MenuSettings
{
    bool white_theme = false;
    bool retro_theme = false; // Retro: pixel font, a starry navy background (white_theme off)
    int nv2a = 0;             // the GPU's emulation: 0 xemu's NV2A (the PS4's GPU), 1 Midway's own
    int picture = 0;          // 0 sharp (whole-number scale), 1 fit 4:3, 2 fill the screen
    bool show_log = true;     // the log while a game loads and its first seconds
    int dsp = 0;              // the APU's DSPs (sound effects): 0 off, 1 on
    int sort = 0;             // the titles: 0 by name, 1 recently played first
    // the user on the HOME screen: the nickname (at most kNicknameLength characters) and the icon, a
    // character on a background colour (ui/avatars.h)
    std::string nickname = "Player";
    int avatar = 0, avatar_color = 0;
};

// One line of the Games and USB screen: something found (or not), and where
struct StatusRow
{
    std::string label, detail;
    bool ok = false;
};

struct MenuAction
{
    enum class Kind { None, Boot, Quit, SettingsChanged, ImportUsb, CopyToPs4, Autostart, Diagnostics } kind = Kind::None;
    // Boot: what to boot; CopyToPs4: the game on a USB drive to copy to the PS4; Autostart: the title
    // the app starts without the menu ("" none)
    std::string path;
};

class HomeMenu
{
public:
    HomeMenu();

    // The titles in the order shown (the row shows the first 12)
    void SetTitles(std::vector<Title> titles);
    void SetSettings(const MenuSettings& settings);
    const MenuSettings& Settings() const { return settings; }
    // The About page: the version, and a line on where it runs
    void SetInfo(std::string version, std::string system);
    void SetClock(std::string time) { clock = std::move(time); }
    void SetController(bool connected) { controller = connected; }
    // News: headings and their text
    void SetNews(std::vector<std::pair<std::string, std::string>> sections) { news = std::move(sections); }
    void SetGamesStatus(std::vector<StatusRow> rows, std::string help);
    void ShowMessage(std::string title, std::string text);
    // Changes the message on top (a copy's progress, then its result); false if none is open
    bool UpdateMessage(std::string title, std::string text);
    // The selection kept on this title (after the titles are read again)
    void Select(const std::string& path);

    // Buttons held when the menu comes back (from a game) don't count as pressed
    void SetHeld(uint32_t held) { held_before = held; }
    // The buttons held now; returns what the launcher should do
    MenuAction Update(uint32_t held, double now);
    void Draw(Canvas& canvas, double now);
    // the Retro theme's backgrounds freed (drawn again when next needed): before a game
    void ReleaseBackgrounds()
    {
        for (Image& image : retro_background)
            image = Image{};
    }

private:
    enum class Screen { Home, AllSoftware, Settings, News, Games, Controllers, User, Icon, Keyboard };
    enum class DialogKind { Message, Quit, TitleInfo };
    struct Dialog
    {
        DialogKind kind = DialogKind::Message;
        std::string title, text;
        std::vector<std::string> buttons;
        int selected = 0;
        size_t title_index = 0;
        // TitleInfo: its middle button, "Copy to the PS4" for a game on a USB drive, else "Start at
        // Launch" (or "Don't Start at Launch")
        bool offers_copy = false;
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
    // OPTIONS on a title: what it is and where, Start, and "Copy to the PS4" for a game on a drive,
    // else "Start at Launch"
    void OpenTitleInfo(size_t index);
    MenuAction Navigate(uint32_t pressed, uint32_t moves);
    MenuAction ChooseDialog(int button);

    void DrawHome(Canvas& c, double now);
    // the Retro theme: its background (with the stars on the HOME screen, plain on the others, drawn
    // once a size and kept) and its HOME screen
    void DrawRetroBackground(Canvas& c, bool stars);
    void DrawRetroHome(Canvas& c, double now);
    void DrawAllSoftware(Canvas& c, double now);
    void DrawSettings(Canvas& c, double now);
    void DrawNews(Canvas& c);
    void DrawGames(Canvas& c);
    void DrawControllers(Canvas& c);
    void DrawDialog(Canvas& c, double now);
    // the User Page, its icon's choice (a character, then its colour) and the keyboard
    void DrawUser(Canvas& c, double now);
    void DrawIconPicker(Canvas& c, double now);
    void DrawKeyboard(Canvas& c, double now);
    MenuAction KeyboardPress(uint32_t pressed, uint32_t moves);
    // the user's icon, size x size units, round (made once for each size and colour)
    const Image& Avatar(int character, int color, float size, float scale);
    void DrawHeader(Canvas& c, Icon icon, uint32_t icon_color, const std::string& text);
    void DrawBottomBar(Canvas& c, const std::vector<std::pair<Glyph, std::string>>& hints);
    uint32_t Cursor(double now) const;

    std::vector<Title> titles;
    std::map<int, std::vector<Image>> tiles; // by size in pixels: each title's tile, made when first drawn
    Image retro_background[2];               // plain, starry: the size of the last canvas
    MenuSettings settings;
    std::string version, system_line, clock = "0:00", games_help;
    bool controller = true;
    std::vector<std::pair<std::string, std::string>> news;
    std::vector<StatusRow> games_status;

    Screen screen = Screen::Home;
    std::vector<Dialog> dialogs;
    // Home: the user's icon (-1), the row (0, tiles) or the buttons (1); the tile (HomeCount() is All
    // Software) and button
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
    // the category's list scrolled to keep the selection in view: its first row shown, and the drawn
    // offset easing towards it
    int settings_first = 0;
    float settings_scroll = 0, settings_scroll_target = 0;
    // News and Games: scrolled this far
    float page_scroll = 0;
    // the User Page's item (0 nickname, 1 icon); the icon's choice: its step (0 the
    // character, 1 the colour) and what's chosen so far; the keyboard: the text, the key (row, column),
    // whether the next letter is a capital
    int user_item = 0;
    int icon_step = 0, icon_character = 0, icon_color = 0;
    std::string keyboard_text;
    int key_row = 1, key_column = 0;
    bool key_shift = true;
    std::map<std::tuple<int, int, int>, Image> avatars;
    // Buttons held before, and when a held direction repeats
    uint32_t held_before = 0;
    double repeat_at = 0, last_draw = 0;
};

} // namespace Ui
