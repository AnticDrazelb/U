// Midway's HOME menu, a dashboard in black and white (or white and black): Midway's name, the time,
// the drives and the user along the top; under them the tabs, Games, Library, USB and Settings (L1
// and R1 move between them). Games is a row of covers where the selected one stands larger, its name
// and how to start it beside it, and a panel under the row with what Midway knows of it: when it was
// last played and how often, how its last run went, its facts, the last picture it showed. Library
// is every title in a grid; USB, what the drives hold (and copying from them, and the logs onto
// them); Settings, the settings, the user, the controls, what's new and about. Dialogs are drawn
// over it all. Started from New Eden's menu (eden-ps4/src/ui: its canvas, fonts, dialogs and
// keyboard).
//
// It knows nothing of the PS4: the launcher (frontend/home.cpp) gives it the titles, settings and
// status, and the buttons held each frame, and gets back what to do.

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
    std::string location;  // "on the PS4", "on /mnt/usb0", "in the package"
    std::string size;      // "5.2 GB"
    std::string kind;      // "Disc image", "Game folder", "Sample"
    std::string details;   // its title ID and region, or ""
    std::string last_played; // "30 minutes ago", "Never"
    int plays = 0;
    std::string last_result; // how its last run ended ("Showed its picture"), or ""
    bool on_usb = false;   // on a USB drive: "Copy to the PS4" offered
    bool autostart = false; // the app starts it without the menu
    Art art = Art::None;   // the samples' art
    Image icon;            // its cover made square, when there is one (else its art, else a cover)
    Image picture;         // the last picture it showed (its last run), or none
};

// A nickname's longest
constexpr size_t kNicknameLength = 12;

struct MenuSettings
{
    bool white_theme = false; // Light: black on white; off, Dark: white on black
    int nv2a = 0;             // the GPU's emulation: 0 xemu's NV2A (the PS4's GPU), 1 Midway's own
    int picture = 0;          // 0 sharp (whole-number scale), 1 fit 4:3, 2 fill the screen
    bool show_log = true;     // the log while a game loads and its first seconds
    int dsp = 0;              // the APU's DSPs (sound effects): 0 off, 1 on
    int sort = 0;             // the titles: 0 by name, 1 recently played first
    // the user at the top right: the nickname (at most kNicknameLength characters) and the icon, a
    // character on a shade (ui/avatars.h, drawn in grey)
    std::string nickname = "Player";
    int avatar = 0, avatar_color = 0;
};

// One line of the USB tab: something found (or not), and where
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

    // The titles in the order shown (the Games row shows the first 12, then "All games")
    void SetTitles(std::vector<Title> titles);
    void SetSettings(const MenuSettings& settings);
    const MenuSettings& Settings() const { return settings; }
    // About: the version, and a line on where it runs
    void SetInfo(std::string version, std::string system);
    void SetClock(std::string time) { clock = std::move(time); }
    void SetController(bool connected) { controller = connected; }
    // A USB drive is there (its icon at the top); some of its games aren't on the PS4 (a dot on USB)
    void SetUsb(bool present, bool new_games)
    {
        usb_present = present;
        usb_new = new_games;
    }
    // What's New: headings and their text
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
    void Draw(Canvas& c, double now);

private:
    enum class Tab { Games, Library, Usb, Settings };
    static constexpr int kTabCount = 4;
    // a full screen over the tabs: the nickname's keyboard, the icon's choice
    enum class Sub { None, Keyboard, Icon };
    enum class DialogKind { Message, Quit, TitleInfo };
    struct Dialog
    {
        DialogKind kind = DialogKind::Message;
        std::string title, text;
        std::vector<std::string> buttons;
        int selected = 0;
        size_t title_index = 0;
        // TitleInfo: its middle button, "Copy to the PS4" for a game on a USB drive, else "Start at
        // Launch" (or "Menu at Launch")
        bool offers_copy = false;
        double opened = 0;
    };
    struct Theme;
    // A setting: changed in place (change), or opening something (the keyboard, the icon's choice,
    // the Close dialog)
    struct SettingItem
    {
        enum class Does { Change, Keyboard, Icon, Close } does = Does::Change;
        std::string label, value, description;
        void (*change)(MenuSettings&) = nullptr;
    };
    std::vector<SettingItem> SettingItems(int category) const;

    const Theme& CurrentTheme() const;
    size_t RowCount() const; // the titles in the Games row (an "All games" tile after them when there are more)
    bool RowHasAll() const { return titles.size() > RowCount(); }
    const Image& Tile(size_t index, int pixels);
    MenuAction Start(size_t index);
    void OpenDialog(Dialog dialog);
    // OPTIONS on a title: what it is and where, Start, and "Copy to the PS4" for a game on a drive,
    // else "Start at Launch"
    void OpenTitleInfo(size_t index);
    void OpenClose();
    void SwitchTab(int tab);
    MenuAction Navigate(uint32_t pressed, uint32_t moves);
    MenuAction ChooseDialog(int button);

    void DrawBackground(Canvas& c);
    void DrawTop(Canvas& c, double now);
    void DrawGames(Canvas& c, double now);
    void DrawPanel(Canvas& c, const Title& title, float top);
    void DrawLibrary(Canvas& c, double now);
    void DrawUsb(Canvas& c);
    void DrawSettings(Canvas& c, double now);
    void DrawControls(Canvas& c, float left, float top);
    void DrawDialog(Canvas& c, double now);
    void DrawIconPicker(Canvas& c, double now);
    void DrawKeyboard(Canvas& c, double now);
    void DrawSubHeader(Canvas& c, const std::string& text);
    MenuAction KeyboardPress(uint32_t pressed, uint32_t moves);
    // the user's icon, size x size units, round, in grey (made once for each size and shade)
    const Image& Avatar(int character, int color, float size, float scale);
    void DrawBottomBar(Canvas& c, const std::vector<std::pair<Glyph, std::string>>& hints);
    uint32_t Cursor(double now) const;

    std::vector<Title> titles;
    std::map<int, std::vector<Image>> tiles; // by size in pixels: each title's tile, made when first drawn
    Image background;                        // the gradient, the size of the last canvas, by theme
    bool background_white = false;
    MenuSettings settings;
    std::string version, system_line, clock = "0:00", games_help;
    bool controller = true, usb_present = false, usb_new = false;
    std::vector<std::pair<std::string, std::string>> news;
    std::vector<StatusRow> games_status;

    Tab tab = Tab::Games;
    Sub sub = Sub::None;
    bool on_tabs = false; // the cursor on the tabs (Up from the top of a tab's content)
    std::vector<Dialog> dialogs;
    // Games: the selected tile (RowCount() is "All games"), the row's scroll, each tile's growth
    // (0 small, 1 the selected one's size)
    size_t row_index = 0;
    float row_scroll = 0, row_scroll_target = 0;
    std::vector<float> grow;
    // Library
    size_t grid_index = 0;
    float grid_scroll = 0, grid_scroll_target = 0;
    // Settings: the category, and whether its items have the cursor (and which); the list's first
    // row shown, and the drawn offset easing towards it
    int settings_category = 0, settings_item = 0;
    bool settings_items = false;
    int settings_first = 0;
    float settings_scroll = 0, settings_scroll_target = 0;
    // USB, What's New: scrolled this far
    float page_scroll = 0;
    // the icon's choice: its step (0 the character, 1 the shade) and what's chosen so far; the
    // keyboard: the text, the key (row, column), whether the next letter is a capital
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
