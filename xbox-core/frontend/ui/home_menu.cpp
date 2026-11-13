// The HOME menu (home_menu.h). Positions are in 1280x720 units (the PS4's 1920x1080 is 1.5 pixels a
// unit).

#include "ui/home_menu.h"

#include "ui/avatars.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string_view>

namespace Ui {

namespace {

constexpr float kPi = 3.14159265f;

// The top: the name, the time, the drives, the user; then the tabs
constexpr float kTopBaseline = 52, kTabBaseline = 128, kTabSize = 40, kTabLeft = 118, kTabGap = 36;
const char* const kTabs[] = {"Games", "Library", "USB", "Settings"};
// Games: the row (small tiles, the selected one big, its top edge level with theirs), the selected
// one kept at kAnchor once the row scrolls; the panel under it
constexpr float kRowTop = 166, kSmall = 150, kBig = 288, kRowGap = 14, kRowLeft = 40, kAnchor = 214;
constexpr float kPanelTop = 482;
constexpr size_t kRowTitles = 12;
// Library
constexpr int kGridColumns = 6;
constexpr float kGridTile = 160, kGridGapX = 34, kGridRow = 226, kGridTop = 176, kContentTop = 150;
// Settings
constexpr float kCategoryLeft = 70, kCategoryTop = 192, kCategoryStep = 50, kCategoryWidth = 330;
constexpr float kItemLeft = 420, kItemRight = 1230, kItemHeight = 62, kListTop = 196;
// The bottom line and the hints on it
constexpr float kBottomLine = 650, kHintY = 686;

const char* const kCategories[] = {"Themes", "Graphics", "Sound", "System", "User", "Controls", "What's New", "About"};
constexpr int kCategoryCount = 8;
constexpr int kUserCategory = 4, kControlsCategory = 5, kNewsCategory = 6, kAboutCategory = 7;

// The icon's choice: the characters in a grid, then the colours in a row
constexpr int kIconColumns = 10;
constexpr float kIconCell = 96, kIconGapX = 18, kIconRow = 136, kIconTop = 140;
constexpr float kSwatchRadius = 30, kSwatchGap = 22, kSwatchY = 548;
// The keyboard: four rows of 11 keys, and Shift, Space, Delete and OK under them
const char* const kKeyRows[4] = {"1234567890-", "qwertyuiop_", "asdfghjkl'.", "zxcvbnm,!?@"};
constexpr int kKeyColumns = 11, kKeyRowCount = 5;
constexpr float kKeyW = 84, kKeyH = 62, kKeyGap = 10, kKeyTop = 266;
constexpr float kKeyLeft = (1280 - (kKeyColumns * kKeyW + (kKeyColumns - 1) * kKeyGap)) / 2;
struct WideKey
{
    const char* label;
    int first, span; // the columns it spans
};
constexpr WideKey kWideKeys[] = {{"Shift", 0, 2}, {"Space", 2, 5}, {"Delete", 7, 2}, {"OK", 9, 2}};
constexpr int kWideKeyCount = 4;
int WideKeyAt(int column)
{
    for (int i = kWideKeyCount - 1; i >= 0; i--)
        if (column >= kWideKeys[i].first)
            return i;
    return 0;
}

std::string Trimmed(const std::string& text)
{
    const size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

float Smooth(float current, float target, float dt)
{
    const float k = 1 - std::exp(-dt * 14);
    const float next = current + (target - current) * k;
    return std::fabs(next - target) < 0.25f ? target : next;
}

// A USB stick, about 24 units tall
void DrawUsbIcon(Canvas& c, float cx, float cy, uint32_t color, uint32_t hole)
{
    c.Rect(cx - 5, cy - 12, 10, 8, color);
    c.Rect(cx - 2.8f, cy - 10, 1.8f, 2.6f, hole);
    c.Rect(cx + 1.0f, cy - 10, 1.8f, 2.6f, hole);
    c.RoundRect(cx - 7.5f, cy - 4.5f, 15, 17, 3, color);
}

} // namespace

struct HomeMenu::Theme
{
    uint32_t bg_top, bg_bottom, text, dim, faint, line, cursor, cursor_light, button, shadow, dialog, dialog_line,
        panel;
};

// Dark: white on black; Light: black on white. Nothing else has a colour but the covers.
const HomeMenu::Theme& HomeMenu::CurrentTheme() const
{
    static const Theme dark{Rgb(0x2C2C2C), Rgb(0x080808), Rgb(0xFFFFFF), Rgb(0xA0A0A0), Rgb(0xFFFFFF, 92),
                            Rgb(0xFFFFFF, 40), Rgb(0xFFFFFF), Rgb(0x8C8C8C), Rgb(0x2A2A2A), Rgb(0x000000, 130),
                            Rgb(0x1C1C1C), Rgb(0x3A3A3A), Rgb(0xFFFFFF, 20)};
    static const Theme light{Rgb(0xFBFBFB), Rgb(0xD9D9D9), Rgb(0x111111), Rgb(0x666666), Rgb(0x111111, 92),
                             Rgb(0x000000, 40), Rgb(0x111111), Rgb(0x8A8A8A), Rgb(0xFFFFFF), Rgb(0x000000, 40),
                             Rgb(0xFFFFFF), Rgb(0xD6D6D6), Rgb(0x000000, 14)};
    return settings.white_theme ? light : dark;
}

HomeMenu::HomeMenu() = default;

void HomeMenu::SetTitles(std::vector<Title> new_titles)
{
    titles = std::move(new_titles);
    tiles.clear();
    const size_t slots = RowCount() + (RowHasAll() ? 1 : 0);
    row_index = slots ? std::min(row_index, slots - 1) : 0;
    grid_index = titles.empty() ? 0 : std::min(grid_index, titles.size() - 1);
    grow.assign(RowCount() + 1, 0.0f);
    if (row_index < grow.size())
        grow[row_index] = 1;
}

void HomeMenu::SetSettings(const MenuSettings& new_settings)
{
    settings = new_settings;
}

void HomeMenu::SetInfo(std::string version_, std::string system_)
{
    version = std::move(version_);
    system_line = std::move(system_);
}

void HomeMenu::Select(const std::string& path)
{
    for (size_t i = 0; i < titles.size(); i++)
    {
        if (titles[i].path != path)
            continue;
        grid_index = i;
        if (i < RowCount())
            row_index = i;
        return;
    }
}

void HomeMenu::ShowBoxArt(const std::string& path, std::vector<BoxArtChoice> choices)
{
    box_path = path;
    box_choices = std::move(choices);
    box_index = 0;
    box_scroll = box_scroll_target = 0;
    sub = Sub::BoxArt;
}

void HomeMenu::SetGamesStatus(std::vector<StatusRow> rows, std::string help)
{
    games_status = std::move(rows);
    games_help = std::move(help);
}

void HomeMenu::ShowMessage(std::string title, std::string text)
{
    Dialog dialog;
    dialog.kind = DialogKind::Message;
    dialog.title = std::move(title);
    dialog.text = std::move(text);
    dialog.buttons = {"OK"};
    OpenDialog(std::move(dialog));
}

bool HomeMenu::UpdateMessage(std::string title, std::string text)
{
    if (dialogs.empty() || dialogs.back().kind != DialogKind::Message)
        return false;
    dialogs.back().title = std::move(title);
    dialogs.back().text = std::move(text);
    return true;
}

size_t HomeMenu::RowCount() const
{
    return std::min(titles.size(), kRowTitles);
}

const Image& HomeMenu::Tile(size_t index, int pixels)
{
    std::vector<Image>& list = tiles[pixels];
    if (list.size() != titles.size())
        list.assign(titles.size(), Image{});
    Image& tile = list[index];
    if (tile.Empty())
    {
        const Title& title = titles[index];
        if (!title.icon.Empty())
            tile = ResizeImage(title.icon, pixels, pixels);
        else
            tile = title.art != Art::None ? MakeTileArt(title.art, pixels) : MakeCover(title.name, title.kind, pixels);
    }
    return tile;
}

void HomeMenu::OpenDialog(Dialog dialog)
{
    dialog.opened = last_draw;
    dialogs.push_back(std::move(dialog));
}

MenuAction HomeMenu::Start(size_t index)
{
    if (index >= titles.size())
        return {};
    MenuAction action;
    action.kind = MenuAction::Kind::Boot;
    action.path = titles[index].path;
    return action;
}

MenuAction HomeMenu::ChooseDialog(int button)
{
    const Dialog dialog = dialogs.back();
    dialogs.pop_back();
    switch (dialog.kind)
    {
    case DialogKind::Quit:
        if (button == 1)
            return {MenuAction::Kind::Quit, {}};
        break;
    case DialogKind::TitleInfo: {
        if (dialog.title_index >= titles.size() || button < 0 || button >= int(dialog.buttons.size()))
            break;
        const Title& title = titles[dialog.title_index];
        const std::string& label = dialog.buttons[size_t(button)];
        if (label == "Start")
            return Start(dialog.title_index);
        if (label == "Box Art")
            return {MenuAction::Kind::BoxArt, title.path};
        if (label == "Copy to the PS4")
            return {MenuAction::Kind::CopyToPs4, title.path};
        if (label == "Start at Launch" || label == "Menu at Launch")
            return {MenuAction::Kind::Autostart, title.autostart ? std::string() : title.path};
        if (label == "Delete" || label == "Remove")
        {
            // asked first: Cancel is where the cursor starts
            Dialog confirm;
            confirm.kind = DialogKind::ConfirmRemove;
            confirm.path = title.path;
            confirm.removal = title.removal;
            if (title.removal == Title::Removal::Delete)
            {
                confirm.title = "Delete " + title.name + "?";
                confirm.text = title.remove_detail + "\nThis can't be undone. The game's saves on Midway's Xbox hard disk stay.";
                confirm.buttons = {"Cancel", "Delete"};
            }
            else
            {
                confirm.title = "Remove " + title.name + " from the list?";
                confirm.text = "It's part of Midway's package, so it isn't deleted: System Settings, System, Hidden "
                               "Titles brings it back.";
                confirm.buttons = {"Cancel", "Remove"};
            }
            confirm.selected = 0;
            OpenDialog(std::move(confirm));
        }
        break;
    }
    case DialogKind::ConfirmRemove:
        if (button == 1)
            return {dialog.removal == Title::Removal::Delete ? MenuAction::Kind::Delete : MenuAction::Kind::Hide,
                    dialog.path};
        break;
    case DialogKind::Message:
        break;
    }
    return {};
}

void HomeMenu::OpenTitleInfo(size_t index)
{
    Dialog dialog;
    dialog.kind = DialogKind::TitleInfo;
    dialog.title_index = index;
    // a game on a USB drive can be copied to the PS4's own storage; any other can start at launch
    dialog.offers_copy = index < titles.size() && titles[index].on_usb;
    const bool autostart = index < titles.size() && titles[index].autostart;
    dialog.buttons = {"Close", "Box Art", dialog.offers_copy ? "Copy to the PS4" : autostart ? "Menu at Launch" : "Start at Launch"};
    if (index < titles.size() && titles[index].removal != Title::Removal::None)
        dialog.buttons.push_back(titles[index].removal == Title::Removal::Delete ? "Delete" : "Remove");
    dialog.buttons.push_back("Start");
    dialog.selected = int(dialog.buttons.size()) - 1;
    OpenDialog(std::move(dialog));
}

void HomeMenu::OpenClose()
{
    Dialog dialog;
    dialog.kind = DialogKind::Quit;
    dialog.title = "Close Midway?";
    dialog.text = "The PS4's home screen comes back. (A game runs until the app is closed: the PS button, then "
                  "Close Application, to choose another.)";
    dialog.buttons = {"Cancel", "Close"};
    OpenDialog(std::move(dialog));
}

void HomeMenu::SwitchTab(int next)
{
    tab = Tab((next + kTabCount) % kTabCount);
    page_scroll = 0;
    if (tab == Tab::Settings)
    {
        settings_items = false;
        settings_first = 0;
        settings_scroll = settings_scroll_target = 0;
    }
}

MenuAction HomeMenu::Update(uint32_t held, double now)
{
    const uint32_t pressed = held & ~held_before;
    const uint32_t directions = kUp | kDown | kLeft | kRight;
    uint32_t moves = pressed & directions;
    if (moves)
    {
        repeat_at = now + 0.4;
    }
    else if ((held & directions) && (held & directions) == (held_before & directions) && now >= repeat_at)
    {
        moves = held & directions;
        repeat_at = now + 0.085;
    }
    held_before = held;
    if (!(pressed & ~directions) && !moves)
        return {};
    return Navigate(pressed, moves);
}

MenuAction HomeMenu::Navigate(uint32_t pressed, uint32_t moves)
{
    if (!dialogs.empty())
    {
        Dialog& dialog = dialogs.back();
        if (moves & kLeft)
            dialog.selected = std::max(0, dialog.selected - 1);
        if (moves & kRight)
            dialog.selected = std::min(int(dialog.buttons.size()) - 1, dialog.selected + 1);
        if (pressed & kCross)
            return ChooseDialog(dialog.selected);
        if (pressed & kCircle)
            return ChooseDialog(-1);
        return {};
    }

    if (sub == Sub::Keyboard)
        return KeyboardPress(pressed, moves);
    if (sub == Sub::BoxArt)
    {
        // the pictures in a grid: Cross uses the one selected, Square removes the imported one
        constexpr size_t kColumns = 5;
        const size_t count = box_choices.size();
        if (pressed & kCircle)
        {
            CloseBoxArt();
            return {};
        }
        if (count)
        {
            if ((moves & kLeft) && box_index % kColumns > 0)
                box_index--;
            if ((moves & kRight) && box_index % kColumns < kColumns - 1 && box_index + 1 < count)
                box_index++;
            if ((moves & kUp) && box_index >= kColumns)
                box_index -= kColumns;
            if ((moves & kDown) && box_index / kColumns < (count - 1) / kColumns)
                box_index = std::min(count - 1, box_index + kColumns);
            if (pressed & kCross)
            {
                MenuAction action{MenuAction::Kind::SetBoxArt, box_path, box_choices[box_index].path};
                return action;
            }
        }
        if (pressed & kSquare)
            for (const Title& title : titles)
                if (title.path == box_path && title.imported_art)
                    return {MenuAction::Kind::RemoveBoxArt, box_path};
        return {};
    }
    if (sub == Sub::Icon)
    {
        if (icon_step == 0)
        {
            const int count = AvatarCount();
            if ((moves & kLeft) && icon_character % kIconColumns > 0)
                icon_character--;
            if ((moves & kRight) && icon_character % kIconColumns < kIconColumns - 1 && icon_character + 1 < count)
                icon_character++;
            if ((moves & kUp) && icon_character >= kIconColumns)
                icon_character -= kIconColumns;
            if ((moves & kDown) && icon_character + kIconColumns < count)
                icon_character += kIconColumns;
            if (pressed & kCross)
                icon_step = 1;
            if (pressed & kCircle)
                sub = Sub::None;
            return {};
        }
        if (moves & kLeft)
            icon_color = std::max(0, icon_color - 1);
        if (moves & kRight)
            icon_color = std::min(AvatarColorCount() - 1, icon_color + 1);
        if (pressed & kCircle)
            icon_step = 0;
        if (pressed & kCross)
        {
            settings.avatar = icon_character;
            settings.avatar_color = icon_color;
            sub = Sub::None;
            return {MenuAction::Kind::SettingsChanged, {}};
        }
        return {};
    }

    // L1 and R1: the tab before or after, from anywhere
    if (pressed & (kL1 | kR1))
    {
        SwitchTab(int(tab) + ((pressed & kR1) ? 1 : -1));
        return {};
    }
    if (on_tabs)
    {
        if (moves & kLeft)
            SwitchTab(int(tab) - 1);
        if (moves & kRight)
            SwitchTab(int(tab) + 1);
        if ((moves & kDown) || (pressed & kCross))
            on_tabs = false;
        if ((pressed & kCircle) && tab != Tab::Games)
            SwitchTab(int(Tab::Games));
        return {};
    }

    switch (tab)
    {
    case Tab::Games: {
        const size_t slots = RowCount() + (RowHasAll() ? 1 : 0);
        if (moves & kUp)
            on_tabs = true;
        if ((moves & kLeft) && row_index > 0)
            row_index--;
        if ((moves & kRight) && row_index + 1 < slots)
            row_index++;
        if (pressed & kCross)
        {
            if (row_index == RowCount() && RowHasAll())
            {
                SwitchTab(int(Tab::Library));
                grid_index = std::min(RowCount(), titles.size() - 1);
                return {};
            }
            return Start(row_index);
        }
        if ((pressed & kOptions) && row_index < RowCount())
            OpenTitleInfo(row_index);
        return {};
    }

    case Tab::Library: {
        if (pressed & kCircle)
        {
            SwitchTab(int(Tab::Games));
            return {};
        }
        if (titles.empty())
        {
            if (moves & kUp)
                on_tabs = true;
            return {};
        }
        const size_t count = titles.size();
        if ((moves & kLeft) && grid_index % kGridColumns > 0)
            grid_index--;
        if ((moves & kRight) && grid_index % kGridColumns < kGridColumns - 1 && grid_index + 1 < count)
            grid_index++;
        if (moves & kUp)
        {
            if (grid_index >= size_t(kGridColumns))
                grid_index -= kGridColumns;
            else
                on_tabs = true;
        }
        if ((moves & kDown) && grid_index / kGridColumns < (count - 1) / kGridColumns)
            grid_index = std::min(count - 1, grid_index + kGridColumns);
        if (pressed & kCross)
            return Start(grid_index);
        if (pressed & kOptions)
            OpenTitleInfo(grid_index);
        return {};
    }

    case Tab::Usb:
        if (pressed & kCircle)
            SwitchTab(int(Tab::Games));
        if (pressed & kSquare)
            return {MenuAction::Kind::ImportUsb, {}};
        if (pressed & kTriangle)
            return {MenuAction::Kind::Diagnostics, {}};
        if (moves & kDown)
            page_scroll += 60;
        if (moves & kUp)
        {
            if (page_scroll <= 0)
                on_tabs = true;
            page_scroll = std::max(0.0f, page_scroll - 60);
        }
        return {};

    case Tab::Settings: {
        const std::vector<SettingItem> list = SettingItems(settings_category);
        const int items = int(list.size());
        // What's New is read in place: Right or Cross, then Up and Down scroll it
        const bool reading = settings_category == kNewsCategory;
        if (pressed & kCircle)
        {
            if (settings_items)
                settings_items = false;
            else
                SwitchTab(int(Tab::Games));
            return {};
        }
        if (!settings_items)
        {
            const int category = settings_category;
            if (moves & kUp)
            {
                if (settings_category == 0)
                    on_tabs = true;
                settings_category = std::max(0, settings_category - 1);
            }
            if (moves & kDown)
                settings_category = std::min(kCategoryCount - 1, settings_category + 1);
            if (settings_category != category)
            {
                settings_first = 0;
                settings_scroll = settings_scroll_target = 0;
                page_scroll = 0;
            }
            if (((moves & kRight) || (pressed & kCross)) && (items > 0 || reading))
            {
                settings_items = true;
                settings_item = 0;
                settings_first = 0;
                settings_scroll_target = 0;
            }
            return {};
        }
        if (moves & kLeft)
        {
            settings_items = false;
            return {};
        }
        if (reading)
        {
            if (moves & kDown)
                page_scroll += 60;
            if (moves & kUp)
                page_scroll = std::max(0.0f, page_scroll - 60);
            return {};
        }
        if (moves & kUp)
            settings_item = std::max(0, settings_item - 1);
        if (moves & kDown)
            settings_item = std::min(items - 1, settings_item + 1);
        if ((pressed & kCross) && settings_item < items)
        {
            const SettingItem& item = list[settings_item];
            switch (item.does)
            {
            case SettingItem::Does::Change:
                item.change(settings);
                return {MenuAction::Kind::SettingsChanged, {}};
            case SettingItem::Does::Keyboard:
                sub = Sub::Keyboard;
                keyboard_text = settings.nickname;
                key_shift = keyboard_text.empty();
                key_row = 1;
                key_column = 0;
                return {};
            case SettingItem::Does::Icon:
                sub = Sub::Icon;
                icon_step = 0;
                icon_character = std::clamp(settings.avatar, 0, AvatarCount() - 1);
                icon_color = std::clamp(settings.avatar_color, 0, AvatarColorCount() - 1);
                return {};
            case SettingItem::Does::Close:
                OpenClose();
                return {};
            case SettingItem::Does::ShowHidden:
                if (hidden_count > 0)
                    return {MenuAction::Kind::ShowHidden, {}};
                return {};
            }
        }
        return {};
    }
    }
    return {};
}

uint32_t HomeMenu::Cursor(double now) const
{
    const Theme& t = CurrentTheme();
    const float wave = 0.5f + 0.5f * std::sin(float(now) * 2 * kPi * 0.85f);
    return Mix(t.cursor, t.cursor_light, wave * 0.6f);
}

void HomeMenu::DrawBackground(Canvas& c)
{
    // a soft fall from grey to black (or white to grey), drawn once a size and kept
    if (background.width != c.Width() || background.height != c.Height() || background_white != settings.white_theme)
    {
        const Theme& t = CurrentTheme();
        Canvas b(c.Width(), c.Height(), c.Scale());
        b.Gradient(0, 0, 1280, 720, t.bg_top, t.bg_bottom);
        background.width = c.Width();
        background.height = c.Height();
        background.pixels.assign(b.Pixels(), b.Pixels() + size_t(c.Width()) * c.Height());
        background_white = settings.white_theme;
    }
    std::memcpy(c.Pixels(), background.pixels.data(), background.pixels.size() * sizeof(uint32_t));
}

void HomeMenu::Draw(Canvas& c, double now)
{
    const float dt = last_draw > 0 ? float(std::min(now - last_draw, 0.1)) : 0.1f;
    last_draw = now;
    row_scroll = Smooth(row_scroll, row_scroll_target, dt);
    grid_scroll = Smooth(grid_scroll, grid_scroll_target, dt);
    settings_scroll = Smooth(settings_scroll, settings_scroll_target, dt);
    {
        const float k = 1 - std::exp(-dt * 16);
        if (grow.size() != RowCount() + 1)
            grow.assign(RowCount() + 1, 0.0f);
        for (size_t i = 0; i < grow.size(); i++)
        {
            const float target = i == row_index ? 1.0f : 0.0f;
            grow[i] += (target - grow[i]) * k;
            if (std::fabs(grow[i] - target) < 0.002f)
                grow[i] = target;
        }
    }
    c.SetFontFamily(FontFamily::Inter);
    DrawBackground(c);
    if (sub == Sub::Keyboard)
        DrawKeyboard(c, now);
    else if (sub == Sub::Icon)
        DrawIconPicker(c, now);
    else if (sub == Sub::BoxArt)
        DrawBoxArt(c, now);
    else
    {
        DrawTop(c, now);
        switch (tab)
        {
        case Tab::Games:
            DrawGames(c, now);
            break;
        case Tab::Library:
            DrawLibrary(c, now);
            break;
        case Tab::Usb:
            DrawUsb(c);
            break;
        case Tab::Settings:
            DrawSettings(c, now);
            break;
        }
    }
    if (!dialogs.empty())
        DrawDialog(c, now);
}

void HomeMenu::DrawTop(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    // the name
    const float name_width = c.Text(40, kTopBaseline, "MIDWAY", 26, Weight::SemiBold, t.text);
    c.Text(40 + name_width + 12, kTopBaseline, "original Xbox", 21, Weight::Regular, t.dim);
    // the user, its name, the network, the drives and the time, from the right
    constexpr float kAvatarRadius = 21, kAvatarX = 1218, kAvatarY = 44;
    c.DrawImage(kAvatarX - kAvatarRadius, kAvatarY - kAvatarRadius, kAvatarRadius * 2, kAvatarRadius * 2,
                Avatar(settings.avatar, settings.avatar_color, kAvatarRadius * 2, c.Scale()));
    c.Ring(kAvatarX, kAvatarY, kAvatarRadius, 1.5f, Fade(t.text, 0.5f));
    float x = kAvatarX - kAvatarRadius - 16;
    x -= c.Text(x, kTopBaseline, settings.nickname, 21, Weight::Regular, t.text, Align::Right) + 30;
    DrawIcon(c, Icon::WiFi, x, kTopBaseline - 9, 24, t.text);
    x -= 30;
    if (usb_present)
    {
        DrawUsbIcon(c, x, kTopBaseline - 8, t.text, t.bg_top);
        x -= 24;
    }
    c.Text(x, kTopBaseline, clock, 23, Weight::SemiBold, t.text, Align::Right);

    // the tabs, between L1 and R1
    DrawGlyph(c, Glyph::L1, kTabLeft - 32, kTabBaseline - 14, t.dim, t.bg_top);
    float tx = kTabLeft;
    for (int i = 0; i < kTabCount; i++)
    {
        const bool current = int(tab) == i;
        const float w = c.Text(tx, kTabBaseline, kTabs[i], kTabSize, Weight::SemiBold, current ? t.text : t.faint);
        if (current && on_tabs)
            c.RoundRect(tx, kTabBaseline + 10, w, 4, 2, Cursor(now));
        if (i == int(Tab::Usb) && usb_new)
            c.Circle(tx + w + 7, kTabBaseline - 30, 5, t.text); // games on a drive, not on the PS4 yet
        tx += w + kTabGap;
    }
    DrawGlyph(c, Glyph::R1, tx - kTabGap + 32, kTabBaseline - 14, t.dim, t.bg_top);
}

void HomeMenu::DrawGames(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    const size_t count = RowCount(), slots = count + (RowHasAll() ? 1 : 0);
    if (slots == 0)
    {
        c.Text(kRowLeft + 6, kRowTop + 60, "No games yet", 34, Weight::SemiBold, t.text);
        const char* help = "Put them in /data/xbox-core/games (a folder with its default.xbe, or an .iso), or on a USB "
                           "drive: its top folder, or games/, xbox/ or xbox-core/games/. Covers: cover.jpg beside a game.";
        float y = kRowTop + 100;
        for (const std::string& line : c.Wrap(help, 19, Weight::Regular, 900))
        {
            c.Text(kRowLeft + 6, y, line, 19, Weight::Regular, t.dim);
            y += 28;
        }
        return;
    }
    // the selected one kept at kAnchor once the row has scrolled
    row_scroll_target = std::max(0.0f, kRowLeft + float(row_index) * (kSmall + kRowGap) - kAnchor);
    const int small_pixels = int(std::lround(kSmall * c.Scale())), big_pixels = int(std::lround(kBig * c.Scale()));
    const uint32_t cursor = Cursor(now);
    float x = kRowLeft - row_scroll, selected_x = x, selected_size = kBig;
    for (size_t i = 0; i < slots; i++)
    {
        const float g = i < grow.size() ? grow[i] : 0.0f;
        const float size = kSmall + (kBig - kSmall) * g;
        if (i == row_index)
            selected_x = x, selected_size = size;
        if (x < 1280 && x + size > 0)
        {
            if ((t.shadow >> 24) != 0 && g > 0.05f)
                c.RoundRect(x + 4, kRowTop + 8, size, size, 10, Fade(t.shadow, g));
            if (i < count)
                c.DrawImage(x, kRowTop, size, size, Tile(i, size > kSmall + 1 ? big_pixels : small_pixels));
            else
            {
                // "All games": the Library tab
                c.RoundRect(x, kRowTop, size, size, 6, t.button);
                DrawIcon(c, Icon::AllSoftware, x + size / 2, kRowTop + size / 2 - 14, 34 + 20 * g, t.text);
                c.Text(x + size / 2, kRowTop + size / 2 + 34 + 20 * g, "All games", 17 + 5 * g, Weight::SemiBold,
                       t.text, Align::Center);
            }
            if (i == row_index && !on_tabs)
                c.RoundRectOutline(x - 7, kRowTop - 7, size + 14, size + 14, 8, 3.5f, cursor);
        }
        x += size + kRowGap;
    }

    // the selected one's name, what it is, and how to start it, beside it under the row
    const float tx = selected_x + selected_size + 28, max_width = 1250 - tx;
    const bool all = row_index >= count;
    const std::string name = all ? "All Games" : titles[row_index].name;
    float y = kRowTop + kSmall + 52;
    const std::vector<std::string> name_lines = c.Wrap(name, 40, Weight::SemiBold, max_width);
    for (size_t i = 0; i < name_lines.size() && i < 2; i++) // two lines at most
    {
        c.Text(tx, y, name_lines[i], 40, Weight::SemiBold, t.text, Align::Left, max_width);
        y += 46;
    }
    std::string what;
    if (all)
        what = std::to_string(titles.size()) + " titles in the library";
    else
    {
        const Title& title = titles[row_index];
        what = title.kind + (title.size.empty() ? "" : "  \xC2\xB7  " + title.size) + "  \xC2\xB7  " + title.location;
    }
    c.Text(tx + 2, y - 12, what, 19, Weight::Regular, t.dim, Align::Left, max_width);
    float hx = tx + 14;
    DrawGlyph(c, Glyph::Cross, hx, y + 30, t.text, t.bg_bottom);
    hx += 20 + c.Text(hx + 20, y + 38, all ? "Open the Library" : "Start", 24, Weight::Regular, t.text);
    if (!all)
    {
        hx += 42;
        DrawGlyph(c, Glyph::Options, hx, y + 30, t.dim, t.bg_bottom);
        c.Text(hx + 24, y + 38, "Options", 24, Weight::Regular, t.dim);
    }

    if (!all)
        DrawPanel(c, titles[row_index], kPanelTop);
    else
    {
        // the library at a glance
        c.RoundRect(120, kPanelTop, 1040, 720 - kPanelTop + 30, 18, t.panel);
        size_t samples = 0, usb = 0, played = 0;
        for (const Title& title : titles)
            samples += title.kind == "Sample", usb += title.on_usb, played += title.plays > 0;
        const std::pair<std::string, std::string> stats[] = {
            {"Your Games", std::to_string(titles.size() - samples)},
            {"On USB Drives", std::to_string(usb)},
            {"Samples", std::to_string(samples)},
            {"Played", std::to_string(played)},
        };
        for (int i = 0; i < 4; i++)
        {
            const float sx = 160 + i * 250;
            c.Text(sx, kPanelTop + 50, stats[i].first, 20, Weight::SemiBold, t.text);
            c.Text(sx, kPanelTop + 104, stats[i].second, 44, Weight::SemiBold, t.text);
        }
    }
}

// What Midway knows of the selected game: when it was played and how often, how its last run went;
// what it is and where; the last picture it showed
void HomeMenu::DrawPanel(Canvas& c, const Title& title, float top)
{
    const Theme& t = CurrentTheme();
    c.RoundRect(120, top, 1040, 720 - top + 30, 18, t.panel); // (on past the bottom of the screen)

    // played
    const float ax = 160;
    c.Text(ax, top + 44, "Last Played", 20, Weight::SemiBold, t.text);
    c.Text(ax, top + 90, title.last_played.empty() ? "Never" : title.last_played, 34, Weight::SemiBold, t.text,
           Align::Left, 270);
    c.Text(ax, top + 140, "Times Played", 20, Weight::SemiBold, t.text);
    c.Text(ax, top + 186, std::to_string(title.plays), 34, Weight::SemiBold, t.text);
    if (!title.last_result.empty())
        c.Text(ax, top + 222, "Last run: " + title.last_result, 15, Weight::Regular, t.dim, Align::Left, 280);

    // about
    const float bx = 470, bw = 330;
    c.Text(bx, top + 44, "About", 20, Weight::SemiBold, t.text);
    std::vector<std::string> lines;
    lines.push_back(title.kind + (title.size.empty() ? "" : ", " + title.size));
    if (title.location == "in the package")
        lines.push_back("In Midway's package");
    else if (!title.location.empty())
        lines.push_back(std::string(1, char(std::toupper(static_cast<unsigned char>(title.location[0])))) +
                        title.location.substr(1));
    if (!title.details.empty())
        lines.push_back(title.details);
    if (title.autostart)
        lines.push_back("Starts when Midway opens");
    if (title.on_usb)
        lines.push_back("OPTIONS: copy it to the PS4");
    float y = top + 80;
    for (const std::string& line : lines)
    {
        for (const std::string& part : c.Wrap(line, 18, Weight::Regular, bw))
        {
            c.Text(bx, y, part, 18, Weight::Regular, t.dim);
            y += 27;
        }
    }
    const size_t slash = title.path.find_last_of('/');
    const std::string file = slash == std::string::npos ? title.path : title.path.substr(slash + 1);
    const std::string folder = slash == std::string::npos ? title.path : title.path.substr(0, slash);
    c.Text(bx, std::max(y + 8, top + 200), file == "default.xbe" ? folder : title.path, 14, Weight::Regular,
           Fade(t.dim, 0.8f), Align::Left, bw);

    // the last picture it showed
    const float px = 846, pw = 272, ph = 204, py = top + 64;
    c.Text(px, top + 44, "Last Seen", 20, Weight::SemiBold, t.text);
    if (!title.picture.Empty())
    {
        float w = pw, h = pw * title.picture.height / std::max(1, title.picture.width);
        if (h > ph)
            h = ph, w = ph * title.picture.width / std::max(1, title.picture.height);
        c.DrawImage(px, py, w, h, title.picture);
    }
    else
    {
        c.RoundRect(px, py, pw, ph, 8, t.panel);
        float ly = py + ph / 2 - 6;
        for (const std::string& line : c.Wrap("Its picture shows here after it has run", 16, Weight::Regular, pw - 40))
        {
            c.Text(px + pw / 2, ly, line, 16, Weight::Regular, t.dim, Align::Center);
            ly += 22;
        }
    }
}

void HomeMenu::DrawLibrary(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    if (titles.empty())
    {
        c.Text(640, 380, "No games yet.", 24, Weight::Regular, t.dim, Align::Center);
        DrawBottomBar(c, {{Glyph::Circle, "Back"}});
        return;
    }
    const float left = (1280 - (kGridColumns * kGridTile + (kGridColumns - 1) * kGridGapX)) / 2;
    const float row_y = float(grid_index / kGridColumns) * kGridRow;
    const float view = kBottomLine - kGridTop - 8;
    if (row_y - grid_scroll_target < 0)
        grid_scroll_target = row_y;
    if (row_y + kGridRow - grid_scroll_target > view)
        grid_scroll_target = row_y + kGridRow - view;
    const int pixels = int(std::lround(kGridTile * c.Scale()));
    const uint32_t cursor = Cursor(now);
    c.Clip(0, kContentTop, 1280, kBottomLine - kContentTop - 1);
    for (size_t i = 0; i < titles.size(); i++)
    {
        const float x = left + float(i % kGridColumns) * (kGridTile + kGridGapX);
        const float y = kGridTop + float(i / kGridColumns) * kGridRow - grid_scroll;
        if (y > kBottomLine || y + kGridRow < kContentTop)
            continue;
        c.DrawImage(x, y, kGridTile, kGridTile, Tile(i, pixels));
        const bool selected = i == grid_index && !on_tabs;
        if (selected)
            c.RoundRectOutline(x - 7, y - 7, kGridTile + 14, kGridTile + 14, 8, 3.5f, cursor);
        c.Text(x + kGridTile / 2, y + kGridTile + 30, titles[i].name, 16, selected ? Weight::SemiBold : Weight::Regular,
               selected ? t.text : t.dim, Align::Center, kGridTile + 24);
    }
    c.ResetClip();
    c.Text(1236, kTabBaseline, std::to_string(titles.size()) + (titles.size() == 1 ? " title" : " titles"), 19,
           Weight::Regular, t.dim, Align::Right);
    DrawBottomBar(c, {{Glyph::Circle, "Back"}, {Glyph::Options, "Options"}, {Glyph::Cross, "Start"}});
}

void HomeMenu::DrawUsb(Canvas& c)
{
    const Theme& t = CurrentTheme();
    c.Clip(0, kContentTop, 1280, kBottomLine - kContentTop - 1);
    float y = 200 - page_scroll;
    for (const StatusRow& row : games_status)
    {
        // a filled disc with a tick, or a ring with a cross
        if (row.ok)
        {
            c.Circle(88, y + 4, 15, t.text);
            DrawIcon(c, Icon::Check, 88, y + 4, 20, t.bg_bottom);
        }
        else
        {
            c.Ring(88, y + 4, 15, 2, t.dim);
            DrawIcon(c, Icon::Cross, 88, y + 4, 16, t.dim);
        }
        c.Text(118, y + 11, row.label, 21, Weight::SemiBold, t.text);
        float dy = 38;
        for (const std::string& line : c.Wrap(row.detail, 18, Weight::Regular, 1080))
        {
            c.Text(118, y + dy, line, 18, Weight::Regular, t.dim);
            dy += 26;
        }
        y += dy + 24;
    }
    y += 6;
    for (const std::string& line : c.Wrap(games_help, 18, Weight::Regular, 1110))
    {
        c.Text(80, y, line, 18, Weight::Regular, t.text);
        y += 27;
    }
    const float end = y + page_scroll;
    if (end - page_scroll < kBottomLine - 40 && page_scroll > 0)
        page_scroll = std::max(0.0f, end - (kBottomLine - 40));
    c.ResetClip();
    DrawBottomBar(c, {{Glyph::Triangle, "Write the Logs"}, {Glyph::Square, "Copy All to the PS4"}, {Glyph::Circle, "Back"}});
}

std::vector<HomeMenu::SettingItem> HomeMenu::SettingItems(int category) const
{
    using Does = SettingItem::Does;
    const auto on_off = [](bool on) { return std::string(on ? "On" : "Off"); };
    switch (category)
    {
    case 0:
        return {{Does::Change, "Theme", settings.white_theme ? "Light" : "Dark",
                 "The menu's own colours: Dark, white on black; Light, black on white. Covers, the games' art and "
                 "your icon keep theirs. The loading screen follows it.",
                 [](MenuSettings& s) { s.white_theme = !s.white_theme; }}};
    case 1:
        return {
            {Does::Change, "Picture", settings.picture == 2 ? "Fill the Screen" : settings.picture == 1 ? "Fit 4:3" : "Sharp",
             "Sharp: the game's picture scaled by a whole number (2x for 640x480), centred: every pixel the "
             "same size. Fit 4:3: the full height of the screen, at the Xbox's 4:3. Fill the Screen: stretched "
             "to 16:9 (for games set to widescreen).",
             [](MenuSettings& s) { s.picture = (s.picture + 1) % 3; }},
            {Does::Change, "Loading Log", on_off(settings.show_log),
             "What Midway is doing, in a few lines under the loading screen and over the game's first 15 "
             "seconds. L3 + R3 in a game shows or hides it. Every line is in the log files either way.",
             [](MenuSettings& s) { s.show_log = !s.show_log; }},
            {Does::Change, "GPU Emulation", settings.nv2a == 1 ? "Midway's Own (Less Complete)" : "xemu's NV2A",
             "xemu's NV2A: the Xbox's GPU as xemu emulates it, drawn by the PS4's GPU through Vulkan (RADV): "
             "the most complete, and the fast one. Midway's own: a simpler NV2A of Midway's, which covers far "
             "less (for comparing when a game draws wrongly). If xemu's didn't get started on this console the "
             "last time, Midway's own takes over by itself. From the next game.",
             [](MenuSettings& s) { s.nv2a ^= 1; }},
        };
    case 2:
        return {
            {Does::Change, "Sound Effects", settings.dsp == 1 ? "On (the DSPs)" : "Off",
             "The Xbox's sound chip has two DSPs for effects (reverb, echo, a voice's filters): On runs them as "
             "xemu does, which costs a large share of a CPU core, and turns itself off again if it can't keep "
             "up. Off: the voices are mixed without their effects: every sound plays, drier. From the next game.",
             [](MenuSettings& s) { s.dsp ^= 1; }},
        };
    case 3:
        return {
            {Does::Change, "Title Order", settings.sort == 1 ? "Recently Played" : "By Name",
             "The order of the Games row and the Library: by name, or the games played last first. Your games "
             "come before the samples in the package.",
             [](MenuSettings& s) { s.sort ^= 1; }},
            {Does::Change, "Logs", settings.logs ? "On" : "Off",
             "On: while a game runs, Midway writes what it's doing to its log files (and live to a USB drive), "
             "samples where its time goes (the profiler) and keeps a picture of the screen every 10 s: what to "
             "send when something goes wrong. Off: none of that during a game, a little faster and no disk "
             "work at all. Crashes are still recorded either way, and the menu's logs too.",
             [](MenuSettings& s) { s.logs = !s.logs; }},
            {Does::ShowHidden, "Hidden Titles", hidden_count ? std::to_string(hidden_count) + " Hidden" : "None",
             "Samples taken off the list (OPTIONS on one, Remove): X puts every one back.", nullptr},
            {Does::Close, "Close Midway", "",
             "Back to the PS4's home screen. (While a game runs, the PS button and Close Application end it; "
             "Midway opens on this menu the next time.)",
             nullptr},
        };
    case kUserCategory:
        return {
            {Does::Keyboard, "Nickname", settings.nickname, "Your name at the top right.", nullptr},
            {Does::Icon, "Icon", AvatarName(std::clamp(settings.avatar, 0, AvatarCount() - 1)),
             "Your icon at the top right: a character, on a colour of your choosing.", nullptr},
        };
    default:
        return {};
    }
}

// The PS4 controller as the Xbox's: each button and what it is to a game
void HomeMenu::DrawControls(Canvas& c, float left, float top)
{
    const Theme& t = CurrentTheme();
    struct Row
    {
        Glyph glyph;
        bool has_glyph;
        const char* ps4;
        const char* xbox;
    };
    const Row rows[] = {
        {Glyph::Cross, true, "", "A"},
        {Glyph::Circle, true, "", "B"},
        {Glyph::Square, true, "", "X"},
        {Glyph::Triangle, true, "", "Y"},
        {Glyph::L1, false, "L1 / R1", "White / Black"},
        {Glyph::L1, false, "L2 / R2", "Left / Right trigger"},
        {Glyph::Options, true, "", "Start"},
        {Glyph::L1, false, "Touchpad click", "Back"},
        {Glyph::L1, false, "Sticks, L3 / R3", "Sticks, their clicks"},
        {Glyph::L1, false, "L3 + R3 together", "The log, shown or hidden"},
    };
    c.Text(left, top, "PS4 button", 16, Weight::Regular, t.dim);
    c.Text(left + 230, top, "Xbox button", 16, Weight::Regular, t.dim);
    float y = top + 40;
    for (const Row& row : rows)
    {
        if (row.has_glyph)
            DrawGlyph(c, row.glyph, left + 14, y - 7, t.text, t.bg_bottom);
        else
            c.Text(left, y, row.ps4, 18, Weight::Regular, t.text);
        c.Text(left + 230, y, row.xbox, 19, Weight::SemiBold, t.text);
        y += 36;
    }
    c.Text(left, y + 6, controller ? "The PS4 controller is Player 1's Xbox controller; its rumble is the Xbox controller's."
                                   : "No controller connected.", 16, Weight::Regular, t.dim);
}

void HomeMenu::DrawSettings(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    const uint32_t cursor = Cursor(now);
    // the categories
    for (int i = 0; i < kCategoryCount; i++)
    {
        const float y = kCategoryTop + i * kCategoryStep;
        const bool current = i == settings_category;
        if (current && !settings_items && !on_tabs)
            c.RoundRectOutline(kCategoryLeft - 22, y - 34, kCategoryWidth - 30, 48, 6, 3.5f, cursor);
        if (current)
            c.Rect(kCategoryLeft - 12, y - 22, 4, 24, t.text);
        c.Text(kCategoryLeft, y, kCategories[i], 21, current ? Weight::SemiBold : Weight::Regular, current ? t.text : t.dim);
    }
    c.Line(kCategoryLeft + kCategoryWidth - 20, kContentTop + 20, kCategoryLeft + kCategoryWidth - 20, kBottomLine - 12,
           1.2f, t.line);

    const float wrap_width = kItemRight - kItemLeft - 16;
    if (settings_category == kControlsCategory)
    {
        DrawControls(c, kItemLeft + 8, kListTop);
        DrawBottomBar(c, {{Glyph::Circle, "Back"}});
        return;
    }
    if (settings_category == kNewsCategory || settings_category == kAboutCategory)
    {
        // text to read: What's New (scrolled with Up and Down once entered), About
        std::vector<std::pair<std::string, std::string>> sections = news;
        if (settings_category == kAboutCategory)
            sections = {{"Midway " + version,
                         "An original Xbox emulator for the PS4: the Xbox's code runs on the PS4's CPU as it is, with a "
                         "kernel of Midway's own and xemu's GPU, sound, USB and network chips (xemu.app)."},
                        {"This console", system_line + "\nData folder: /data/xbox-core"},
                        {"Licences",
                         "GPL-2.0-or-later. The menu was begun from New Eden's. Its fonts are Inter (rsms.me/inter) "
                         "and Pixelify Sans (github.com/eifetx/Pixelify-Sans), both under the SIL Open Font License."}};
        c.Clip(kItemLeft - 10, kContentTop + 10, kItemRight - kItemLeft + 20, kBottomLine - kContentTop - 12);
        float y = kListTop - page_scroll;
        for (const auto& [heading, body] : sections)
        {
            c.Text(kItemLeft, y, heading, 22, Weight::SemiBold, t.text);
            y += 32;
            for (const std::string& line : c.Wrap(body, 17, Weight::Regular, wrap_width))
            {
                c.Text(kItemLeft, y, line, 17, Weight::Regular, t.dim);
                y += 25;
            }
            y += 22;
        }
        // no scrolling past the end
        const float end = y + page_scroll;
        if (end < kBottomLine)
            page_scroll = 0;
        else
            page_scroll = std::min(page_scroll, end - (kBottomLine - 20));
        c.ResetClip();
        if (settings_items && settings_category == kNewsCategory && !on_tabs)
            c.RoundRect(kItemRight + 12, kListTop - 20, 4, 60, 2, cursor); // reading: Up and Down scroll
        DrawBottomBar(c, settings_category == kNewsCategory
                             ? std::vector<std::pair<Glyph, std::string>>{{Glyph::Circle, "Back"}, {Glyph::Cross, "Read"}}
                             : std::vector<std::pair<Glyph, std::string>>{{Glyph::Circle, "Back"}});
        return;
    }

    // the category's items, and what the selected one means; the rows that fit above the longest
    // description (so the layout doesn't move as the selection does), scrolled to keep the selection
    const std::vector<SettingItem> items = SettingItems(settings_category);
    size_t description_lines = 0;
    for (const SettingItem& item : items)
        description_lines = std::max(description_lines, c.Wrap(item.description, 17, Weight::Regular, wrap_width).size());
    const float list_bottom = kBottomLine - 12 - (description_lines * 26.0f + 30);
    const int count = int(items.size());
    const int visible = std::max(1, std::min(count, int((list_bottom - kListTop) / kItemHeight)));
    if (settings_items)
    {
        if (settings_item < settings_first)
            settings_first = settings_item;
        if (settings_item >= settings_first + visible)
            settings_first = settings_item - visible + 1;
    }
    settings_first = std::clamp(settings_first, 0, std::max(0, count - visible));
    settings_scroll_target = settings_first * kItemHeight;
    const float rows_bottom = kListTop + visible * kItemHeight;
    c.Clip(kItemLeft - 14, kListTop - 1, kItemRight - kItemLeft + 28, rows_bottom - kListTop + 2);
    float y = kListTop - settings_scroll;
    for (size_t i = 0; i < items.size(); i++)
    {
        if (y + kItemHeight >= kListTop - 1 && y <= rows_bottom + 1)
        {
            const bool selected = settings_items && int(i) == settings_item;
            c.Line(kItemLeft, y, kItemRight, y, 1.0f, t.line);
            if (selected)
                c.RoundRectOutline(kItemLeft - 8, y + 4, kItemRight - kItemLeft + 16, kItemHeight - 8, 6, 3.5f, cursor);
            c.Text(kItemLeft + 16, y + 40, items[i].label, 21, Weight::Regular, t.text);
            const float value_width =
                c.Text(kItemRight - 16, y + 40, items[i].value, 21, Weight::SemiBold, t.text, Align::Right);
            if (items[i].does == SettingItem::Does::Icon)
                c.DrawImage(kItemRight - 16 - value_width - 46, y + 13, 36, 36,
                            Avatar(settings.avatar, settings.avatar_color, 36, c.Scale()));
            c.Line(kItemLeft, y + kItemHeight, kItemRight, y + kItemHeight, 1.0f, t.line);
        }
        y += kItemHeight;
    }
    c.ResetClip();
    y = rows_bottom + 36;
    if (!items.empty())
        for (const std::string& line : c.Wrap(items[std::min(size_t(settings_items ? settings_item : 0), items.size() - 1)].description,
                                              17, Weight::Regular, wrap_width))
        {
            c.Text(kItemLeft + 8, y, line, 17, Weight::Regular, t.dim);
            y += 26;
        }
    DrawBottomBar(c, {{Glyph::Circle, "Back"}, {Glyph::Cross, settings_items ? "Change" : "OK"}});
}

void HomeMenu::DrawBottomBar(Canvas& c, const std::vector<std::pair<Glyph, std::string>>& hints)
{
    const Theme& t = CurrentTheme();
    c.Line(30, kBottomLine, 1250, kBottomLine, 1.2f, t.line);
    DrawController(c, 64, kHintY + 1, 40, controller ? t.text : Fade(t.dim, 0.6f), Rgb(0, 0));
    float x = 1236;
    for (auto it = hints.rbegin(); it != hints.rend(); ++it)
    {
        const float text_width = c.Text(x, kHintY + 7, it->second, 19, Weight::Regular, t.text, Align::Right);
        x -= text_width + 8 + GlyphWidth(it->first) / 2;
        DrawGlyph(c, it->first, x, kHintY, t.text, t.bg_bottom);
        x -= GlyphWidth(it->first) / 2 + 30;
    }
}

void HomeMenu::DrawSubHeader(Canvas& c, const std::string& text)
{
    const Theme& t = CurrentTheme();
    c.Text(40, kTopBaseline, "MIDWAY", 26, Weight::SemiBold, t.text);
    c.Text(40, 112, text, 34, Weight::SemiBold, t.text);
}

void HomeMenu::DrawDialog(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    Dialog& dialog = dialogs.back();
    const float appear = std::min(1.0f, float(now - dialog.opened) / 0.12f);
    c.Rect(0, 0, 1280, 720, Rgb(0x000000, uint32_t(160 * appear)));

    std::string title = dialog.title, text = dialog.text;
    if (dialog.kind == DialogKind::TitleInfo && dialog.title_index < titles.size())
    {
        const Title& info = titles[dialog.title_index];
        title = info.name;
        text = info.kind + (info.size.empty() ? "" : ", " + info.size) + (info.location.empty() ? "" : ", " + info.location) +
               "\n" + (info.details.empty() ? "" : info.details + "\n") + info.path +
               (info.autostart ? "\nStarts when Midway opens (hold L1 as it opens for this menu)." : "");
    }
    const float w = std::max(720.0f, 196.0f * float(dialog.buttons.size())), text_width = w - 80;
    const std::vector<std::string> title_lines = title.empty() ? std::vector<std::string>{}
                                                               : c.Wrap(title, 24, Weight::SemiBold, text_width);
    const std::vector<std::string> lines = text.empty() ? std::vector<std::string>{}
                                                        : c.Wrap(text, 19, Weight::Regular, text_width);
    const float buttons_h = 76;
    const float h = 58 + title_lines.size() * 34.0f + (title_lines.empty() || lines.empty() ? 0 : 14) +
                    lines.size() * 28.0f + 40 + buttons_h;
    const float x = 640 - w / 2, y = 360 - h / 2 + (1 - appear) * 12;
    c.RoundRect(x, y, w, h, 16, t.dialog);
    c.RoundRectOutline(x, y, w, h, 16, 1.2f, t.dialog_line);
    float ty = y + 58;
    for (const std::string& line : title_lines)
    {
        c.Text(640, ty, line, 24, Weight::SemiBold, t.text, Align::Center);
        ty += 34;
    }
    if (!title_lines.empty() && !lines.empty())
        ty += 14;
    for (const std::string& line : lines)
    {
        c.Text(640, ty, line, 19, Weight::Regular, t.dim, Align::Center);
        ty += 28;
    }
    const float by = y + h - buttons_h;
    c.Line(x, by, x + w, by, 1.2f, t.dialog_line);
    const float bw = w / float(dialog.buttons.size());
    for (size_t i = 0; i < dialog.buttons.size(); i++)
    {
        const float bx = x + i * bw;
        if (i > 0)
            c.Line(bx, by, bx, y + h, 1.2f, t.dialog_line);
        const bool selected = int(i) == dialog.selected;
        if (selected)
            c.RoundRectOutline(bx + 8, by + 8, bw - 16, buttons_h - 16, 8, 3.5f, Cursor(now));
        c.Text(bx + bw / 2, by + 46, dialog.buttons[i], 21, Weight::SemiBold, selected ? t.text : t.dim, Align::Center);
    }
}

// Box Art: the game's tile now, and the pictures found on the USB drives to choose from
void HomeMenu::DrawBoxArt(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    const uint32_t cursor = Cursor(now);
    size_t index = titles.size();
    for (size_t i = 0; i < titles.size(); i++)
        if (titles[i].path == box_path)
            index = i;
    const std::string name = index < titles.size() ? titles[index].name : std::string();
    c.Text(40, kTopBaseline, "MIDWAY", 26, Weight::SemiBold, t.text);
    c.Text(40, 112, "Box Art", 34, Weight::SemiBold, t.text);
    c.Text(40 + c.TextWidth("Box Art", 34, Weight::SemiBold) + 16, 112, name, 24, Weight::Regular, t.dim, Align::Left, 1000);

    // now: its tile, and where it's from
    constexpr float kNowX = 60, kNowY = 190, kNow = 256;
    c.Text(kNowX, kNowY - 18, "Now", 20, Weight::SemiBold, t.text);
    bool imported = false;
    if (index < titles.size())
    {
        imported = titles[index].imported_art;
        c.DrawImage(kNowX, kNowY, kNow, kNow, Tile(index, int(std::lround(kNow * c.Scale()))));
        const char* from = imported                       ? "Imported from a USB drive"
                           : !titles[index].icon.Empty()  ? "The game's own cover"
                                                          : "Drawn by Midway (no cover)";
        c.Text(kNowX, kNowY + kNow + 32, from, 17, Weight::Regular, t.dim, Align::Left, kNow + 40);
    }
    c.Line(360, kContentTop, 360, kBottomLine - 12, 1.2f, t.line);

    // the pictures: 5 a row, each fitted into a case's 3:4, its name under it
    constexpr size_t kColumns = 5;
    constexpr float kLeft = 404, kCellW = 150, kCellH = 200, kGap = 18, kRow = 262, kTop = 166;
    if (box_choices.empty())
    {
        float y = 230;
        for (const std::string& line :
             c.Wrap("No pictures on the USB drives. Put a .jpg or .png of the box art on a USB drive (its top folder, "
                    "or a folder there such as covers or boxart), then choose Box Art again. A picture named after "
                    "the game, or its title ID, comes first.",
                    20, Weight::Regular, 800))
        {
            c.Text(kLeft, y, line, 20, Weight::Regular, t.dim);
            y += 30;
        }
    }
    else
    {
        const float row_y = float(box_index / kColumns) * kRow, view = kBottomLine - kTop - 10;
        if (row_y - box_scroll_target < 0)
            box_scroll_target = row_y;
        if (row_y + kRow - box_scroll_target > view)
            box_scroll_target = row_y + kRow - view;
        box_scroll += (box_scroll_target - box_scroll) * 0.35f;
        if (std::fabs(box_scroll - box_scroll_target) < 0.5f)
            box_scroll = box_scroll_target;
        c.Clip(kLeft - 12, kContentTop, 1280 - kLeft, kBottomLine - kContentTop - 1);
        for (size_t i = 0; i < box_choices.size(); i++)
        {
            const BoxArtChoice& choice = box_choices[i];
            const float x = kLeft + float(i % kColumns) * (kCellW + kGap);
            const float y = kTop + float(i / kColumns) * kRow - box_scroll;
            if (y > kBottomLine || y + kRow < kContentTop)
                continue;
            c.RoundRect(x, y, kCellW, kCellH, 6, t.panel);
            if (!choice.thumbnail.Empty())
            {
                float w = kCellW, h = kCellW * choice.thumbnail.height / std::max(1, choice.thumbnail.width);
                if (h > kCellH)
                    h = kCellH, w = kCellH * choice.thumbnail.width / std::max(1, choice.thumbnail.height);
                c.DrawImage(x + (kCellW - w) / 2, y + (kCellH - h) / 2, w, h, choice.thumbnail);
            }
            if (choice.match)
            {
                c.RoundRect(x + 6, y + 6, 58, 22, 11, Fade(t.bg_bottom, 0.85f));
                c.Text(x + 35, y + 22, "Match", 13, Weight::SemiBold, t.text, Align::Center);
            }
            const bool selected = i == box_index;
            if (selected)
                c.RoundRectOutline(x - 7, y - 7, kCellW + 14, kCellH + 14, 8, 3.5f, cursor);
            c.Text(x + kCellW / 2, y + kCellH + 24, choice.name, 14, selected ? Weight::SemiBold : Weight::Regular,
                   selected ? t.text : t.dim, Align::Center, kCellW + 14);
            c.Text(x + kCellW / 2, y + kCellH + 43, choice.where, 12, Weight::Regular, Fade(t.dim, 0.8f), Align::Center,
                   kCellW + 14);
        }
        c.ResetClip();
    }
    std::vector<std::pair<Glyph, std::string>> hints{{Glyph::Circle, "Back"}};
    if (imported)
        hints.push_back({Glyph::Square, "Remove the Imported One"});
    if (!box_choices.empty())
        hints.push_back({Glyph::Cross, "Use This Picture"});
    DrawBottomBar(c, hints);
}

// The user's icon, round, size units across (made once for each size and colour)
const Image& HomeMenu::Avatar(int character, int color, float size, float scale)
{
    const int pixels = std::max(1, int(std::lround(size * scale)));
    const auto key = std::make_tuple(character, color, pixels);
    auto it = avatars.find(key);
    if (it == avatars.end())
    {
        if (avatars.size() > 160)
            avatars.clear(); // (each is used as soon as it's returned)
        it = avatars.emplace(key, MakeAvatar(character, color, pixels, true)).first;
    }
    return it->second;
}

// The icon's choice: a character from the grid, then its colour
void HomeMenu::DrawIconPicker(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    const uint32_t cursor = Cursor(now);
    DrawSubHeader(c, icon_step == 0 ? "Choose a Character" : "Choose a Colour");
    if (icon_step == 0)
    {
        const float left = (1280 - (kIconColumns * kIconCell + (kIconColumns - 1) * kIconGapX)) / 2;
        for (int i = 0; i < AvatarCount(); i++)
        {
            const float x = left + float(i % kIconColumns) * (kIconCell + kIconGapX);
            const float y = kIconTop + float(i / kIconColumns) * kIconRow;
            c.DrawImage(x, y, kIconCell, kIconCell, Avatar(i, icon_color, kIconCell, c.Scale()));
            if (i == icon_character)
                c.Ring(x + kIconCell / 2, y + kIconCell / 2, kIconCell / 2 + 9, 4, cursor);
        }
        const int rows = (AvatarCount() + kIconColumns - 1) / kIconColumns;
        c.Text(640, kIconTop + rows * kIconRow + 30, AvatarName(icon_character), 24, Weight::SemiBold, t.text,
               Align::Center);
        DrawBottomBar(c, {{Glyph::Circle, "Back"}, {Glyph::Cross, "Next"}});
        return;
    }
    constexpr float kSize = 260, kCy = 290;
    c.DrawImage(640 - kSize / 2, kCy - kSize / 2, kSize, kSize, Avatar(icon_character, icon_color, kSize, c.Scale()));
    c.Text(640, kCy + kSize / 2 + 44, AvatarName(icon_character), 24, Weight::SemiBold, t.text, Align::Center);
    const int count = AvatarColorCount();
    const float total = count * kSwatchRadius * 2 + (count - 1) * kSwatchGap;
    for (int i = 0; i < count; i++)
    {
        const float cx = 640 - total / 2 + kSwatchRadius + float(i) * (kSwatchRadius * 2 + kSwatchGap);
        c.Circle(cx, kSwatchY, kSwatchRadius, AvatarColor(i));
        c.Ring(cx, kSwatchY, kSwatchRadius, 1.5f, Fade(t.text, 0.25f));
        if (i == icon_color)
            c.Ring(cx, kSwatchY, kSwatchRadius + 9, 4, cursor);
    }
    DrawBottomBar(c, {{Glyph::Circle, "Back"}, {Glyph::Cross, "OK"}});
}

// The keyboard, for the nickname: its keys chosen with the D-pad and pressed with Cross
MenuAction HomeMenu::KeyboardPress(uint32_t pressed, uint32_t moves)
{
    const auto commit = [&]() -> MenuAction {
        const std::string name = Trimmed(keyboard_text);
        if (name.empty())
            return {}; // (a nickname needs something in it)
        settings.nickname = name;
        sub = Sub::None;
        return {MenuAction::Kind::SettingsChanged, {}};
    };
    const auto erase = [&] {
        if (!keyboard_text.empty())
            keyboard_text.pop_back();
        if (keyboard_text.empty())
            key_shift = true;
    };
    if (key_row == kKeyRowCount - 1)
    {
        const int key = WideKeyAt(key_column);
        if ((moves & kLeft) && key > 0)
            key_column = kWideKeys[key - 1].first;
        if ((moves & kRight) && key < kWideKeyCount - 1)
            key_column = kWideKeys[key + 1].first;
    }
    else
    {
        if (moves & kLeft)
            key_column = std::max(0, key_column - 1);
        if (moves & kRight)
            key_column = std::min(kKeyColumns - 1, key_column + 1);
    }
    if (moves & kUp)
        key_row = std::max(0, key_row - 1);
    if (moves & kDown)
        key_row = std::min(kKeyRowCount - 1, key_row + 1);
    if (pressed & kTriangle)
    {
        sub = Sub::None; // (unchanged)
        return {};
    }
    if (pressed & kCircle)
    {
        if (keyboard_text.empty())
            sub = Sub::None;
        else
            erase();
        return {};
    }
    if (pressed & kOptions)
        return commit();
    if (!(pressed & kCross))
        return {};
    if (key_row < kKeyRowCount - 1)
    {
        char ch = kKeyRows[key_row][key_column];
        if (key_shift && ch >= 'a' && ch <= 'z')
        {
            ch = char(ch - 'a' + 'A');
            key_shift = false;
        }
        if (keyboard_text.size() < kNicknameLength)
            keyboard_text += ch;
        return {};
    }
    switch (WideKeyAt(key_column))
    {
    case 0:
        key_shift = !key_shift;
        break;
    case 1:
        if (keyboard_text.size() < kNicknameLength && !keyboard_text.empty())
            keyboard_text += ' ';
        break;
    case 2:
        erase();
        break;
    default:
        return commit();
    }
    return {};
}

void HomeMenu::DrawKeyboard(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    const uint32_t cursor = Cursor(now);
    DrawSubHeader(c, "Nickname");
    // the text, its caret blinking, and how many characters it may have
    constexpr float kFieldX = 290, kFieldY = 140, kFieldW = 700, kFieldH = 72;
    c.RoundRect(kFieldX, kFieldY, kFieldW, kFieldH, 10, t.panel);
    c.RoundRectOutline(kFieldX, kFieldY, kFieldW, kFieldH, 10, 2, t.text);
    const float text_width = c.Text(kFieldX + 24, kFieldY + 48, keyboard_text, 32, Weight::Regular, t.text);
    if (std::fmod(now, 1.0) < 0.6)
        c.Rect(kFieldX + 26 + text_width, kFieldY + 16, 2.5f, 40, t.text);
    if (keyboard_text.empty())
        c.Text(kFieldX + 34, kFieldY + 48, "A nickname", 32, Weight::Regular, Fade(t.dim, 0.6f));
    c.Text(kFieldX + kFieldW, kFieldY + kFieldH + 26,
           std::to_string(keyboard_text.size()) + " / " + std::to_string(kNicknameLength), 17, Weight::Regular, t.dim,
           Align::Right);
    const auto key_x = [](int column) { return kKeyLeft + float(column) * (kKeyW + kKeyGap); };
    for (int row = 0; row < kKeyRowCount; row++)
    {
        const float y = kKeyTop + float(row) * (kKeyH + kKeyGap);
        if (row < kKeyRowCount - 1)
        {
            for (int column = 0; column < kKeyColumns; column++)
            {
                char ch = kKeyRows[row][column];
                if (key_shift && ch >= 'a' && ch <= 'z')
                    ch = char(ch - 'a' + 'A');
                const float x = key_x(column);
                c.RoundRect(x, y, kKeyW, kKeyH, 8, t.button);
                c.Text(x + kKeyW / 2, y + kKeyH / 2 + 10, std::string(1, ch), 26, Weight::Regular, t.text, Align::Center);
                if (row == key_row && column == key_column)
                    c.RoundRectOutline(x - 6, y - 6, kKeyW + 12, kKeyH + 12, 10, 3.5f, cursor);
            }
            continue;
        }
        for (int i = 0; i < kWideKeyCount; i++)
        {
            const WideKey& key = kWideKeys[i];
            const float x = key_x(key.first), w = key_x(key.first + key.span - 1) + kKeyW - x;
            const bool ok = i == kWideKeyCount - 1, lit = (i == 0 && key_shift) || ok;
            c.RoundRect(x, y, w, kKeyH, 8, ok ? t.text : lit ? Mix(t.button, t.text, 0.3f) : t.button);
            c.Text(x + w / 2, y + kKeyH / 2 + 8, key.label, 21, Weight::SemiBold, ok ? t.bg_bottom : t.text, Align::Center);
            if (row == key_row && WideKeyAt(key_column) == i)
                c.RoundRectOutline(x - 6, y - 6, w + 12, kKeyH + 12, 10, 3.5f, cursor);
        }
    }
    DrawBottomBar(c, {{Glyph::Triangle, "Cancel"}, {Glyph::Circle, "Delete"}, {Glyph::Options, "OK"},
                      {Glyph::Cross, "Enter"}});
}

} // namespace Ui
