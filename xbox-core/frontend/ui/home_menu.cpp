// The HOME menu (home_menu.h), New Eden's for Midway. Positions are the Switch's, in its 1280x720
// units.

#include "ui/home_menu.h"

#include "ui/avatars.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace Ui {

namespace {

constexpr float kPi = 3.14159265f;

// The HOME screen
constexpr float kRowLeft = 98, kTileTop = 124, kTileSize = 256, kTileGap = 12;
constexpr float kRowRight = 1182; // tiles are scrolled into [kRowLeft, kRowRight]
constexpr float kAllSoftwareSlot = 150, kAllSoftwareRadius = 54;
constexpr float kTitleBaseline = 108;
constexpr float kButtonY = 534, kButtonRadius = 37, kButtonGap = 26, kButtonLabelBaseline = 478;
constexpr float kBottomLine = 648, kHintY = 684;
constexpr size_t kHomeTitles = 12;
// All Software
constexpr int kGridColumns = 6;
constexpr float kGridTile = 176, kGridGapX = 24, kGridRow = 232, kGridTop = 128, kHeaderLine = 104;
// System Settings
constexpr float kCategoryWidth = 380, kItemLeft = 450, kItemRight = 1230, kItemHeight = 64;

struct SystemButton
{
    Icon icon;
    const char* label;
};
constexpr SystemButton kButtons[] = {
    {Icon::News, "News"},
    {Icon::Games, "Games and USB"},
    {Icon::Controllers, "Controllers"},
    {Icon::Settings, "System Settings"},
    {Icon::Power, "Power"},
};
constexpr int kButtonCount = int(sizeof(kButtons) / sizeof(kButtons[0]));

const char* const kCategories[] = {"Themes", "Graphics", "Sound", "System", "About"};

}  // namespace

namespace {

// The user's icon at the top left of the HOME screen
constexpr float kUserX = 66, kUserY = 52, kUserRadius = 25;
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
constexpr int kCategoryCount = 5;

float Smooth(float current, float target, float dt)
{
    const float k = 1 - std::exp(-dt * 14);
    const float next = current + (target - current) * k;
    return std::fabs(next - target) < 0.25f ? target : next;
}

} // namespace

struct HomeMenu::Theme
{
    uint32_t bg, text, dim, line, accent, cursor, cursor_light, button, shadow, dialog, dialog_line, red, orange, icon,
        good, bad, panel;
};

// Basic Black, Basic White and Retro
const HomeMenu::Theme& HomeMenu::CurrentTheme() const
{
    // Retro: navy, cream text, gold for the selection; lavender buttons
    static const Theme retro{Rgb(0x2B2F66), Rgb(0xF6F2E4), Rgb(0xAEB0DA), Rgb(0x565DA8), Rgb(0xFFD447),
                             Rgb(0xF7C531), Rgb(0xFFF1A6), Rgb(0x474E99), Rgb(0x14163A, 210), Rgb(0x3B4187),
                             Rgb(0x6068B5), Rgb(0xEE4B42), Rgb(0xF7A72E), Rgb(0xF6F2E4), Rgb(0x6BDB6B),
                             Rgb(0xEE4B42), Rgb(0x353A7A)};
    if (settings.retro_theme)
        return retro;
    static const Theme black{Rgb(0x2D2D2D), Rgb(0xFFFFFF), Rgb(0xB0B0B0), Rgb(0x585858), Rgb(0x00C8F0),
                             Rgb(0x00B8E8), Rgb(0x6CF2FF), Rgb(0x4E4E4E), Rgb(0x000000, 0), Rgb(0x464646),
                             Rgb(0x5E5E5E), Rgb(0xF0483E), Rgb(0xF5A41E), Rgb(0xE8E8E8), Rgb(0x34C759),
                             Rgb(0xF0483E), Rgb(0x363636)};
    static const Theme white{Rgb(0xEBEBEB), Rgb(0x2D2D2D), Rgb(0x707070), Rgb(0xC6C6C6), Rgb(0x3254E6),
                             Rgb(0x00B4E6), Rgb(0x70E6FF), Rgb(0xFFFFFF), Rgb(0x000000, 28), Rgb(0xF4F4F4),
                             Rgb(0xD6D6D6), Rgb(0xE60012), Rgb(0xF39800), Rgb(0x505050), Rgb(0x1FA84A),
                             Rgb(0xE60012), Rgb(0xE2E2E2)};
    return settings.white_theme ? white : black;
}

HomeMenu::HomeMenu() = default;

void HomeMenu::SetTitles(std::vector<Title> new_titles)
{
    titles = std::move(new_titles);
    tiles.clear();
    home_tile = std::min(home_tile, HomeCount());
    grid_index = titles.empty() ? 0 : std::min(grid_index, titles.size() - 1);
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
        if (i < HomeCount())
            home_tile = i;
        return;
    }
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

size_t HomeMenu::HomeCount() const
{
    return std::min(titles.size(), kHomeTitles);
}

const Image& HomeMenu::Tile(size_t index, int pixels)
{
    // by size and font: the Retro theme's covers are written in its pixel font
    std::vector<Image>& list = tiles[pixels * 2 + (settings.retro_theme ? 1 : 0)];
    if (list.size() != titles.size())
        list.assign(titles.size(), Image{});
    Image& tile = list[index];
    if (tile.Empty())
    {
        const Title& title = titles[index];
        if (!title.icon.Empty())
            tile = ResizeImage(title.icon, pixels, pixels);
        else if (title.art != Art::None)
            tile = MakeTileArt(title.art, pixels);
        else
            tile = MakeCover(title.name, title.kind, pixels,
                             settings.retro_theme ? FontFamily::Pixel : FontFamily::Inter);
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
    case DialogKind::TitleInfo:
        if (dialog.title_index >= titles.size())
            break;
        if (button == 1)
        {
            // the middle button: a copy to the PS4, or the title started at launch (or not any more)
            const Title& title = titles[dialog.title_index];
            MenuAction action;
            action.kind = dialog.offers_copy ? MenuAction::Kind::CopyToPs4 : MenuAction::Kind::Autostart;
            action.path = dialog.offers_copy || !title.autostart ? title.path : std::string();
            return action;
        }
        if (button == 2)
            return Start(dialog.title_index);
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
    dialog.buttons = {"Close", dialog.offers_copy ? "Copy to the PS4" : autostart ? "Menu at Launch" : "Start at Launch",
                      "Start"};
    dialog.selected = int(dialog.buttons.size()) - 1;
    OpenDialog(std::move(dialog));
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

    switch (screen)
    {
    case Screen::Home:
        if (home_row < 0)
        {
            // the user's icon: its User Page
            if (moves & kDown)
                home_row = 0;
            if (pressed & kCross)
            {
                screen = Screen::User;
                user_item = 0;
            }
        }
        else if (home_row == 0)
        {
            if ((moves & kLeft) && home_tile > 0)
                home_tile--;
            if ((moves & kRight) && home_tile < HomeCount())
                home_tile++;
            if (moves & kDown)
                home_row = 1;
            if (moves & kUp)
                home_row = -1;
            if (pressed & kCross)
            {
                if (home_tile == HomeCount())
                {
                    screen = Screen::AllSoftware;
                    grid_index = 0;
                    grid_scroll = grid_scroll_target = 0;
                    return {};
                }
                return Start(home_tile);
            }
            if ((pressed & kOptions) && home_tile < HomeCount())
                OpenTitleInfo(home_tile);
        }
        else
        {
            if (moves & kLeft)
                home_button = std::max(0, home_button - 1);
            if (moves & kRight)
                home_button = std::min(kButtonCount - 1, home_button + 1);
            if (moves & kUp)
                home_row = 0;
            if (pressed & kCross)
            {
                page_scroll = 0;
                switch (home_button)
                {
                case 0:
                    screen = Screen::News;
                    break;
                case 1:
                    screen = Screen::Games;
                    break;
                case 2:
                    screen = Screen::Controllers;
                    break;
                case 3:
                    screen = Screen::Settings;
                    settings_items = false;
                    settings_item = 0;
                    settings_first = 0;
                    settings_scroll = settings_scroll_target = 0;
                    break;
                default: {
                    Dialog dialog;
                    dialog.kind = DialogKind::Quit;
                    dialog.title = "Close Midway?";
                    dialog.text = "The PS4's home screen comes back. (A game runs until the app is closed: "
                                  "the PS button, then Close Application, to choose another.)";
                    dialog.buttons = {"Cancel", "Close"};
                    OpenDialog(std::move(dialog));
                    break;
                }
                }
            }
        }
        return {};

    case Screen::User:
        if (pressed & kCircle)
            screen = Screen::Home;
        if (moves & kUp)
            user_item = 0;
        if (moves & kDown)
            user_item = 1;
        if (pressed & kCross)
        {
            if (user_item == 0)
            {
                screen = Screen::Keyboard;
                keyboard_text = settings.nickname;
                key_shift = keyboard_text.empty();
                key_row = 1;
                key_column = 0;
            }
            else
            {
                screen = Screen::Icon;
                icon_step = 0;
                icon_character = std::clamp(settings.avatar, 0, AvatarCount() - 1);
                icon_color = std::clamp(settings.avatar_color, 0, AvatarColorCount() - 1);
            }
        }
        return {};

    case Screen::Icon:
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
                screen = Screen::User;
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
            screen = Screen::User;
            return {MenuAction::Kind::SettingsChanged, {}};
        }
        return {};

    case Screen::Keyboard:
        return KeyboardPress(pressed, moves);

    case Screen::AllSoftware: {
        if (pressed & kCircle)
        {
            screen = Screen::Home;
            return {};
        }
        if (titles.empty())
            return {};
        const size_t count = titles.size();
        if ((moves & kLeft) && grid_index % kGridColumns > 0)
            grid_index--;
        if ((moves & kRight) && grid_index % kGridColumns < kGridColumns - 1 && grid_index + 1 < count)
            grid_index++;
        if ((moves & kUp) && grid_index >= size_t(kGridColumns))
            grid_index -= kGridColumns;
        if ((moves & kDown) && grid_index / kGridColumns < (count - 1) / kGridColumns)
            grid_index = std::min(count - 1, grid_index + kGridColumns);
        if (pressed & kCross)
            return Start(grid_index);
        if (pressed & kOptions)
            OpenTitleInfo(grid_index);
        return {};
    }

    case Screen::Settings: {
        if (pressed & kCircle)
        {
            if (settings_items)
                settings_items = false;
            else
                screen = Screen::Home;
            return {};
        }
        const std::vector<SettingItem> list = SettingItems(settings_category);
        const int items = int(list.size());
        if (!settings_items)
        {
            const int category = settings_category;
            if (moves & kUp)
                settings_category = std::max(0, settings_category - 1);
            if (moves & kDown)
                settings_category = std::min(kCategoryCount - 1, settings_category + 1);
            if (settings_category != category)
            {
                settings_first = 0;
                settings_scroll = settings_scroll_target = 0;
            }
            if (((moves & kRight) || (pressed & kCross)) && items > 0)
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
        if (moves & kUp)
            settings_item = std::max(0, settings_item - 1);
        if (moves & kDown)
            settings_item = std::min(items - 1, settings_item + 1);
        if ((pressed & kCross) && settings_item < items)
        {
            list[settings_item].change(settings);
            return {MenuAction::Kind::SettingsChanged, {}};
        }
        return {};
    }

    case Screen::News:
    case Screen::Games:
        if (pressed & kCircle)
            screen = Screen::Home;
        if (screen == Screen::Games && (pressed & kSquare))
            return {MenuAction::Kind::ImportUsb, {}};
        if (screen == Screen::Games && (pressed & kTriangle))
            return {MenuAction::Kind::Diagnostics, {}};
        if (moves & kDown)
            page_scroll += 60;
        if (moves & kUp)
            page_scroll = std::max(0.0f, page_scroll - 60);
        return {};

    case Screen::Controllers:
        if (pressed & kCircle)
            screen = Screen::Home;
        return {};
    }
    return {};
}

uint32_t HomeMenu::Cursor(double now) const
{
    const Theme& t = CurrentTheme();
    const float wave = 0.5f + 0.5f * std::sin(float(now) * 2 * kPi * 0.85f);
    return Mix(t.cursor, t.cursor_light, wave * 0.75f);
}

void HomeMenu::Draw(Canvas& c, double now)
{
    const float dt = last_draw > 0 ? float(std::min(now - last_draw, 0.1)) : 0.1f;
    last_draw = now;
    scroll = Smooth(scroll, scroll_target, dt);
    grid_scroll = Smooth(grid_scroll, grid_scroll_target, dt);
    settings_scroll = Smooth(settings_scroll, settings_scroll_target, dt);
    const Theme& t = CurrentTheme();
    c.SetFontFamily(settings.retro_theme ? FontFamily::Pixel : FontFamily::Inter);
    if (settings.retro_theme)
        DrawRetroBackground(c, screen == Screen::Home);
    else
        c.Clear(t.bg);
    switch (screen)
    {
    case Screen::Home:
        if (settings.retro_theme)
            DrawRetroHome(c, now);
        else
            DrawHome(c, now);
        break;
    case Screen::AllSoftware:
        DrawAllSoftware(c, now);
        break;
    case Screen::Settings:
        DrawSettings(c, now);
        break;
    case Screen::News:
        DrawNews(c);
        break;
    case Screen::Games:
        DrawGames(c);
        break;
    case Screen::Controllers:
        DrawControllers(c);
        break;
    case Screen::User:
        DrawUser(c, now);
        break;
    case Screen::Icon:
        DrawIconPicker(c, now);
        break;
    case Screen::Keyboard:
        DrawKeyboard(c, now);
        break;
    }
    if (!dialogs.empty())
        DrawDialog(c, now);
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
        DrawGlyph(c, it->first, x, kHintY, t.text, t.bg);
        x -= GlyphWidth(it->first) / 2 + 30;
    }
}

void HomeMenu::DrawHeader(Canvas& c, Icon icon, uint32_t icon_color, const std::string& text)
{
    const Theme& t = CurrentTheme();
    DrawIcon(c, icon, 66, 62, 34, icon_color);
    c.Text(100, 72, text, 26, Weight::SemiBold, t.text);
    c.Line(30, kHeaderLine, 1250, kHeaderLine, 1.2f, t.line);
}

void HomeMenu::DrawHome(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    const uint32_t cursor = Cursor(now);

    // the user (its icon, and its User Page when chosen) and the time
    c.DrawImage(kUserX - kUserRadius, kUserY - kUserRadius, kUserRadius * 2, kUserRadius * 2,
                Avatar(settings.avatar, settings.avatar_color, kUserRadius * 2, c.Scale()));
    c.Ring(kUserX, kUserY, kUserRadius, 1.5f, Fade(Rgb(0xFFFFFF), 0.5f));
    if (home_row < 0)
    {
        c.Ring(kUserX, kUserY, kUserRadius + 9, 5, cursor);
        c.Text(108, 60, settings.nickname + "'s User Page", 21, Weight::SemiBold, t.accent);
    }
    c.Text(1188, 62, clock, 22, Weight::SemiBold, t.text, Align::Right);
    DrawIcon(c, Icon::WiFi, 1219, 52, 26, t.text);

    // the row: titles, then All Software; scrolled to keep the selection in view
    const size_t count = HomeCount();
    const auto slot_x = [&](size_t i) { return float(i) * (kTileSize + kTileGap); };
    const float sel_x = slot_x(home_tile), sel_w = home_tile == count ? kAllSoftwareSlot : kTileSize;
    if (sel_x - scroll_target < 0)
        scroll_target = sel_x;
    if (sel_x + sel_w - scroll_target > kRowRight - kRowLeft)
        scroll_target = sel_x + sel_w - (kRowRight - kRowLeft);
    const int tile_pixels = int(std::lround(kTileSize * c.Scale()));
    for (size_t i = 0; i < count; i++)
    {
        const float x = kRowLeft + slot_x(i) - scroll;
        if (x > 1280 || x + kTileSize < 0)
            continue;
        c.DrawImage(x, kTileTop, kTileSize, kTileSize, Tile(i, tile_pixels));
    }
    const float all_cx = kRowLeft + slot_x(count) - scroll + kAllSoftwareSlot / 2;
    const float all_cy = kTileTop + kTileSize / 2;
    c.Circle(all_cx, all_cy, kAllSoftwareRadius, t.button);
    DrawIcon(c, Icon::AllSoftware, all_cx, all_cy, 40, t.icon);

    if (home_row == 0)
    {
        if (home_tile < count)
        {
            const float x = kRowLeft + slot_x(home_tile) - scroll;
            c.RoundRectOutline(x - 10, kTileTop - 10, kTileSize + 20, kTileSize + 20, 10, 5, cursor);
            c.Text(x, kTitleBaseline, titles[home_tile].name, 21, Weight::SemiBold, t.accent, Align::Left,
                   1250 - x);
        }
        else
        {
            c.Ring(all_cx, all_cy, kAllSoftwareRadius + 10, 5, cursor);
            c.Text(all_cx, kTitleBaseline, "All Software", 21, Weight::SemiBold, t.accent, Align::Center);
        }
    }
    else if (count > 0 && home_tile < count)
    {
        // the selected title keeps its name, dimmed, while the buttons have the cursor
        const float x = kRowLeft + slot_x(home_tile) - scroll;
        c.Text(x, kTitleBaseline, titles[home_tile].name, 21, Weight::SemiBold, t.dim, Align::Left, 1250 - x);
    }
    if (count == 0)
        c.Text(kRowLeft, kTitleBaseline, "No games yet: put them in /data/xbox-core/games (a folder with its "
               "default.xbe, or an .iso) or on a USB drive", 19, Weight::Regular, t.dim);

    // the round buttons
    const float total = kButtonCount * kButtonRadius * 2 + (kButtonCount - 1) * kButtonGap;
    for (int i = 0; i < kButtonCount; i++)
    {
        const float cx = 640 - total / 2 + kButtonRadius + i * (kButtonRadius * 2 + kButtonGap);
        if ((t.shadow >> 24) != 0)
            c.Circle(cx, kButtonY + 2, kButtonRadius + 1, t.shadow);
        c.Circle(cx, kButtonY, kButtonRadius, t.button);
        const uint32_t color = i == 0 ? t.red : i == 1 ? t.orange : t.icon;
        DrawIcon(c, kButtons[i].icon, cx, kButtonY, 36, color);
        if (home_row == 1 && home_button == i)
        {
            c.Ring(cx, kButtonY, kButtonRadius + 10, 5, cursor);
            c.Text(cx, kButtonLabelBaseline, kButtons[i].label, 19, Weight::SemiBold, t.accent, Align::Center);
        }
    }

    if (home_row == 0 && home_tile < count)
        DrawBottomBar(c, {{Glyph::Options, "Options"}, {Glyph::Cross, "Start"}});
    else
        DrawBottomBar(c, {{Glyph::Cross, "OK"}});
}

namespace {

// The Retro theme's colours besides its Theme
constexpr uint32_t kRetroInk = Rgb(0x15173B);           // outlines and hard shadows
constexpr uint32_t kRetroCream = Rgb(0xEDEAF8);         // the tiles' inner frame, the name tag's border
constexpr uint32_t kRetroLavender = Rgb(0xC9C7EE);      // the buttons
constexpr uint32_t kRetroLavenderLight = Rgb(0xE6E4F9); // their top edge
constexpr uint32_t kRetroIconInk = Rgb(0x3A3F7E);       // icons on lavender
constexpr uint32_t kRetroTag = Rgb(0x1C1F4D);           // the name tag

// A pixel star ('#' its outline, 'o' its face, '+' the light on it) and a twinkle
const char* const kRetroStar[] = {
    "......#......", ".....#+#.....", ".....#+#.....", "....#+oo#....", "#####+ooo####",
    "#+++oooooooo#", ".#+ooooooooo#", "..#ooooooo#..", "..#ooooooo#..", ".#ooo#.#ooo#.",
    ".#oo#...#oo#.", "#o##.....##o#", "##.........##",
};
const char* const kRetroTwinkle[] = {"..#..", "..#..", "##.##", "..#..", "..#.."};

void DrawSprite(Canvas& c, const char* const* rows, int count, float x, float y, float pixel, uint32_t outline,
                uint32_t face, uint32_t light)
{
    for (int r = 0; r < count; r++)
        for (int k = 0; rows[r][k]; k++)
        {
            const char p = rows[r][k];
            if (p == '.')
                continue;
            c.Rect(x + k * pixel, y + r * pixel, pixel, pixel, p == '#' ? outline : p == 'o' ? face : light);
        }
}

// A chunky button or tile frame: a hard shadow, a dark outline, a lavender face lit along its top
void RetroButton(Canvas& c, float cx, float cy, float half, float radius)
{
    c.RoundRect(cx - half + 4, cy - half + 5, half * 2, half * 2, radius, Fade(kRetroInk, 0.8f));
    c.RoundRect(cx - half, cy - half, half * 2, half * 2, radius, kRetroInk);
    c.RoundRect(cx - half + 4, cy - half + 4, half * 2 - 8, half * 2 - 8, radius - 4, kRetroLavender);
    c.RoundRect(cx - half + 10, cy - half + 8, half * 2 - 20, 5, 2.5f, kRetroLavenderLight);
}

} // namespace

void HomeMenu::DrawRetroBackground(Canvas& c, bool stars)
{
    Image& background = retro_background[stars ? 1 : 0];
    if (background.width != c.Width() || background.height != c.Height())
    {
        Canvas b(c.Width(), c.Height(), c.Scale());
        // a night sky in navy, a faint quilt of squares over it
        b.Gradient(0, 0, 1280, 720, Rgb(0x31377C), Rgb(0x222659));
        for (int row = 0; row < 9; row++)
            for (int col = 0; col < 16; col++)
                if ((row + col) % 2 == 0)
                    b.Rect(col * 80.0f, row * 80.0f, 80, 80, Rgb(0xFFFFFF, 7));
        if (stars)
        {
            // stars where the tiles and buttons leave room, and twinkles (in fixed places: the
            // background is drawn once)
            struct Star { float x, y, pixel, opacity; };
            static constexpr Star kStars[] = {
                {470, 36, 4, 0.9f}, {760, 40, 3, 0.8f}, {1010, 76, 4, 0.85f}, {40, 412, 4, 0.85f},
                {320, 452, 3, 0.75f}, {980, 446, 4, 0.85f}, {1186, 420, 3, 0.8f}, {150, 590, 3, 0.7f},
                {1120, 586, 4, 0.75f},
            };
            for (const Star& star : kStars)
                DrawSprite(b, kRetroStar, 13, star.x, star.y, star.pixel, Fade(Rgb(0xC08A1C), star.opacity),
                           Fade(Rgb(0xF7CB45), star.opacity), Fade(Rgb(0xFFF0B4), star.opacity));
            uint32_t seed = 0x2B2F66;
            const auto next = [&seed] {
                seed = seed * 1664525u + 1013904223u;
                return float(seed >> 8) / float(1u << 24);
            };
            for (int i = 0; i < 28; i++)
            {
                const float x = 20 + next() * 1240, y = 20 + next() * 680, opacity = 0.18f + next() * 0.3f;
                const uint32_t color = Fade(Rgb(0xDCD9FF), opacity);
                DrawSprite(b, kRetroTwinkle, 5, x, y, 2, color, color, color);
            }
        }
        // a CRT's scan lines: every third row of pixels a little darker
        background.width = c.Width();
        background.height = c.Height();
        background.pixels.assign(b.Pixels(), b.Pixels() + size_t(c.Width()) * c.Height());
        for (int y = 2; y < background.height; y += 3)
        {
            uint32_t* line = background.pixels.data() + size_t(y) * background.width;
            for (int x = 0; x < background.width; x++)
            {
                const uint32_t p = line[x];
                const uint32_t r = (p & 0xFF) * 7 / 8, g = (p >> 8 & 0xFF) * 7 / 8, bl = (p >> 16 & 0xFF) * 7 / 8;
                line[x] = (p & 0xFF000000u) | bl << 16 | g << 8 | r;
            }
        }
    }
    std::memcpy(c.Pixels(), background.pixels.data(), background.pixels.size() * sizeof(uint32_t));
}

void HomeMenu::DrawRetroHome(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    const uint32_t cursor = Cursor(now);
    // text with a hard shadow, a pixel down and right
    const auto shadowed = [&c](float x, float y, std::string_view text, float size, Weight weight, uint32_t color,
                               Align align = Align::Left, float max_width = 0) {
        c.Text(x + 2, y + 2, text, size, weight, kRetroInk, align, max_width);
        return c.Text(x, y, text, size, weight, color, align, max_width);
    };

    // the user (its icon in a dark ring; its User Page when chosen) and the time
    c.Circle(kUserX + 3, kUserY + 4, kUserRadius + 4, Fade(kRetroInk, 0.8f));
    c.Circle(kUserX, kUserY, kUserRadius + 4, kRetroInk);
    c.DrawImage(kUserX - kUserRadius, kUserY - kUserRadius, kUserRadius * 2, kUserRadius * 2,
                Avatar(settings.avatar, settings.avatar_color, kUserRadius * 2, c.Scale()));
    c.Ring(kUserX, kUserY, kUserRadius, 2, kRetroCream);
    if (home_row < 0)
    {
        c.Ring(kUserX, kUserY, kUserRadius + 12, 5, cursor);
        shadowed(112, 61, settings.nickname + "'s User Page", 22, Weight::SemiBold, t.accent);
    }
    shadowed(1186, 64, clock, 28, Weight::SemiBold, t.text, Align::Right);
    DrawIcon(c, Icon::WiFi, 1221, 54, 26, kRetroInk);
    DrawIcon(c, Icon::WiFi, 1219, 52, 26, t.text);

    // the row: titles in chunky frames, then All Software; scrolled to keep the selection in view
    const size_t count = HomeCount();
    const auto slot_x = [&](size_t i) { return float(i) * (kTileSize + kTileGap); };
    const float sel_x = slot_x(home_tile), sel_w = home_tile == count ? kAllSoftwareSlot : kTileSize;
    if (sel_x - scroll_target < 0)
        scroll_target = sel_x;
    if (sel_x + sel_w - scroll_target > kRowRight - kRowLeft)
        scroll_target = sel_x + sel_w - (kRowRight - kRowLeft);
    constexpr float kInset = 9; // the frame: 4 of outline, then the cream
    const int tile_pixels = int(std::lround((kTileSize - 2 * kInset) * c.Scale()));
    for (size_t i = 0; i < count; i++)
    {
        const float x = kRowLeft + slot_x(i) - scroll;
        if (x > 1280 || x + kTileSize + 8 < 0)
            continue;
        c.RoundRect(x + 6, kTileTop + 7, kTileSize, kTileSize, 18, Fade(kRetroInk, 0.8f));
        c.RoundRect(x, kTileTop, kTileSize, kTileSize, 18, kRetroInk);
        c.DrawImage(x + kInset, kTileTop + kInset, kTileSize - 2 * kInset, kTileSize - 2 * kInset,
                    Tile(i, tile_pixels));
        // the cream frame also rounds the art's corners
        c.RoundRectOutline(x + 4, kTileTop + 4, kTileSize - 8, kTileSize - 8, 14, 6, kRetroCream);
    }
    const float all_cx = kRowLeft + slot_x(count) - scroll + kAllSoftwareSlot / 2;
    const float all_cy = kTileTop + kTileSize / 2;
    RetroButton(c, all_cx, all_cy, kAllSoftwareRadius, 18);
    DrawIcon(c, Icon::AllSoftware, all_cx, all_cy, 40, kRetroIconInk);

    // the selection: a gold frame, and the name in a tag below, pointing at it
    const auto name_tag = [&](float cx, float top, const std::string& name, uint32_t color, bool pointer) {
        const float text_width = std::min(c.TextWidth(name, 21, Weight::SemiBold), 560.0f);
        const float w = text_width + 40, h = 42;
        const float x = std::clamp(cx - w / 2, 16.0f, 1264.0f - w);
        c.RoundRect(x + 3, top + 4, w, h, 10, Fade(kRetroInk, 0.8f));
        c.RoundRect(x, top, w, h, 10, kRetroCream);
        if (pointer)
        {
            c.Triangle(cx, top - 12, cx - 12, top + 1, cx + 12, top + 1, kRetroCream);
            c.Triangle(cx, top - 5, cx - 7, top + 4, cx + 7, top + 4, kRetroTag);
        }
        c.RoundRect(x + 3, top + 3, w - 6, h - 6, 8, kRetroTag);
        c.Text(x + w / 2, top + 29, name, 21, Weight::SemiBold, color, Align::Center, 560);
    };
    if (home_row == 0)
    {
        if (home_tile < count)
        {
            const float x = kRowLeft + slot_x(home_tile) - scroll;
            c.RoundRectOutline(x - 13, kTileTop - 13, kTileSize + 26, kTileSize + 26, 29, 3, kRetroInk);
            c.RoundRectOutline(x - 10, kTileTop - 10, kTileSize + 20, kTileSize + 20, 26, 7, cursor);
            name_tag(x + kTileSize / 2, kTileTop + kTileSize + 22, titles[home_tile].name, t.text, true);
        }
        else
        {
            const float half = kAllSoftwareRadius + 10;
            c.RoundRectOutline(all_cx - half - 3, all_cy - half - 3, half * 2 + 6, half * 2 + 6, 29, 3, kRetroInk);
            c.RoundRectOutline(all_cx - half, all_cy - half, half * 2, half * 2, 26, 7, cursor);
            name_tag(all_cx, all_cy + half + 20, "All Software", t.text, true);
        }
    }
    else if (count > 0 && home_tile < count)
    {
        // the selected title keeps its tag, dimmed, while the buttons have the cursor
        name_tag(kRowLeft + slot_x(home_tile) - scroll + kTileSize / 2, kTileTop + kTileSize + 22,
                 titles[home_tile].name, t.dim, false);
    }
    if (count == 0)
        shadowed(kRowLeft, kTitleBaseline, "No games yet: put them in /data/xbox-core/games (a folder with "
                 "its default.xbe, or an .iso) or on a USB drive", 19, Weight::Regular, t.dim);

    // the buttons: chunky lavender squares, each with its name below
    constexpr float kHalf = 37, kGap = 50;
    const float total = kButtonCount * kHalf * 2 + (kButtonCount - 1) * kGap;
    for (int i = 0; i < kButtonCount; i++)
    {
        const float cx = 640 - total / 2 + kHalf + i * (kHalf * 2 + kGap);
        const bool selected = home_row == 1 && home_button == i;
        RetroButton(c, cx, kButtonY, kHalf, 16);
        const uint32_t color = i == 0 ? t.red : i == 1 ? t.orange : kRetroIconInk;
        DrawIcon(c, kButtons[i].icon, cx, kButtonY + 1, 36, color);
        if (selected)
        {
            c.RoundRectOutline(cx - kHalf - 12, kButtonY - kHalf - 12, kHalf * 2 + 24, kHalf * 2 + 24, 26, 3,
                               kRetroInk);
            c.RoundRectOutline(cx - kHalf - 9, kButtonY - kHalf - 9, kHalf * 2 + 18, kHalf * 2 + 18, 23, 6, cursor);
        }
        float baseline = kButtonY + kHalf + 31;
        for (const std::string& line : c.Wrap(kButtons[i].label, 17, Weight::SemiBold, kHalf * 2 + kGap - 6))
        {
            shadowed(cx, baseline, line, 17, Weight::SemiBold, selected ? t.accent : t.text, Align::Center);
            baseline += 19;
        }
    }

    if (home_row == 0 && home_tile < count)
        DrawBottomBar(c, {{Glyph::Options, "Options"}, {Glyph::Cross, "Start"}});
    else
        DrawBottomBar(c, {{Glyph::Cross, "OK"}});
}

void HomeMenu::DrawAllSoftware(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    DrawHeader(c, Icon::AllSoftware, t.text, "All Software");
    c.Text(1236, 72, std::to_string(titles.size()) + (titles.size() == 1 ? " title" : " titles"), 18,
           Weight::Regular, t.dim, Align::Right);
    if (titles.empty())
    {
        c.Text(640, 360, "No software found.", 22, Weight::Regular, t.dim, Align::Center);
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
    c.Clip(0, kHeaderLine + 1, 1280, kBottomLine - kHeaderLine - 2);
    for (size_t i = 0; i < titles.size(); i++)
    {
        const float x = left + float(i % kGridColumns) * (kGridTile + kGridGapX);
        const float y = kGridTop + float(i / kGridColumns) * kGridRow - grid_scroll;
        if (y > kBottomLine || y + kGridRow < kHeaderLine)
            continue;
        c.DrawImage(x, y, kGridTile, kGridTile, Tile(i, pixels));
        const bool selected = i == grid_index;
        if (selected)
            c.RoundRectOutline(x - 8, y - 8, kGridTile + 16, kGridTile + 16, 8, 4.5f, cursor);
        c.Text(x + kGridTile / 2, y + kGridTile + 30, titles[i].name, 16, selected ? Weight::SemiBold : Weight::Regular,
               selected ? t.accent : t.text, Align::Center, kGridTile + 12);
    }
    c.ResetClip();
    DrawBottomBar(c, {{Glyph::Circle, "Back"}, {Glyph::Options, "Options"}, {Glyph::Cross, "Start"}});
}

std::vector<HomeMenu::SettingItem> HomeMenu::SettingItems(int category) const
{
    const auto on_off = [](bool on) { return std::string(on ? "On" : "Off"); };
    switch (category)
    {
    case 0:
        return {{"Theme", settings.retro_theme ? "Retro" : settings.white_theme ? "Basic White" : "Basic Black",
                 "The HOME menu's look: Basic Black, Basic White, or Retro (a pixel font, a starry night, "
                 "chunky tiles and buttons). The loading screen follows it.",
                 [](MenuSettings& s) {
                     // Basic Black, Basic White, Retro, and round again
                     if (s.retro_theme)
                         s.retro_theme = false, s.white_theme = false;
                     else if (s.white_theme)
                         s.retro_theme = true, s.white_theme = false;
                     else
                         s.white_theme = true;
                 }}};
    case 1:
        return {
            {"GPU Emulation", settings.nv2a == 1 ? "Midway's Own" : "xemu's NV2A",
             "xemu's NV2A: the Xbox's GPU as xemu emulates it, drawn by the PS4's GPU through Vulkan (RADV): "
             "the most complete, and the fast one. Midway's own: a simpler NV2A of Midway's, which covers far "
             "less (for comparing when a game draws wrongly). If xemu's didn't get started on this console the "
             "last time, Midway's own takes over by itself. From the next game.",
             [](MenuSettings& s) { s.nv2a ^= 1; }},
            {"Picture", settings.picture == 2 ? "Fill the Screen" : settings.picture == 1 ? "Fit 4:3" : "Sharp",
             "Sharp: the game's picture scaled by a whole number (2x for 640x480), centred: every pixel the "
             "same size. Fit 4:3: the full height of the screen, at the Xbox's 4:3. Fill the Screen: stretched "
             "to 16:9 (for games set to widescreen).",
             [](MenuSettings& s) { s.picture = (s.picture + 1) % 3; }},
            {"Loading Log", on_off(settings.show_log),
             "What Midway is doing, in a few lines under the loading screen and over the game's first 15 "
             "seconds. L3 + R3 in a game shows or hides it. Every line is in the log files either way.",
             [](MenuSettings& s) { s.show_log = !s.show_log; }},
        };
    case 2:
        return {
            {"Sound Effects", settings.dsp == 1 ? "On (the DSPs)" : "Off",
             "The Xbox's sound chip has two DSPs for effects (reverb, echo, a voice's filters): On runs them as "
             "xemu does, which costs a large share of a CPU core, and turns itself off again if it can't keep "
             "up. Off: the voices are mixed without their effects: every sound plays, drier. From the next game.",
             [](MenuSettings& s) { s.dsp ^= 1; }},
        };
    case 3:
        return {
            {"Title Order", settings.sort == 1 ? "Recently Played" : "By Name",
             "The order of the HOME screen's row and All Software: by name, or the games played last first. "
             "The player's games come before the samples in the package.",
             [](MenuSettings& s) { s.sort ^= 1; }},
        };
    default:
        return {};
    }
}

void HomeMenu::DrawSettings(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    DrawHeader(c, Icon::Settings, t.text, "System Settings");
    const uint32_t cursor = Cursor(now);
    // the categories
    for (int i = 0; i < kCategoryCount; i++)
    {
        const float y = 124 + i * 60.0f;
        const bool current = i == settings_category;
        if (current && !settings_items)
            c.RoundRectOutline(36, y - 4, kCategoryWidth - 20, 56, 6, 4.5f, cursor);
        if (current)
            c.Rect(48, y + 12, 4, 24, t.accent);
        c.Text(66, y + 32, kCategories[i], 20, current ? Weight::SemiBold : Weight::Regular, current ? t.accent : t.text);
    }
    c.Line(kCategoryWidth + 30, kHeaderLine + 12, kCategoryWidth + 30, kBottomLine - 12, 1.2f, t.line);

    // the category's items, and what the selected one means
    const std::vector<SettingItem> items = SettingItems(settings_category);
    c.Text(kItemLeft, 150, kCategories[settings_category], 24, Weight::SemiBold, t.text);
    // The rows that fit above the longest description of the category (so the layout
    // doesn't move as the selection does), scrolled to keep the selected one in view; before,
    // the rows just went on down, past the bottom bar (Graphics: GPU Accuracy, Game Speed), and
    // the description with them
    constexpr float kListTop = 176;
    const float wrap_width = kItemRight - kItemLeft - 16;
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
                c.RoundRectOutline(kItemLeft - 8, y + 4, kItemRight - kItemLeft + 16, kItemHeight - 8, 6, 4.5f, cursor);
            c.Text(kItemLeft + 16, y + 40, items[i].label, 20, Weight::Regular, t.text);
            c.Text(kItemRight - 16, y + 40, items[i].value, 20, Weight::Regular, t.accent, Align::Right);
            c.Line(kItemLeft, y + kItemHeight, kItemRight, y + kItemHeight, 1.0f, t.line);
        }
        y += kItemHeight;
    }
    c.ResetClip();
    if (count > visible)
    {
        // where the shown rows are in the list
        const float track = rows_bottom - kListTop - 8;
        const float thumb = std::max(24.0f, track * float(visible) / float(count));
        const float at = kListTop + 4 + (track - thumb) * (settings_scroll / ((count - visible) * kItemHeight));
        c.RoundRect(kItemRight + 22, kListTop + 4, 4, track, 2, t.line);
        c.RoundRect(kItemRight + 21, at, 6, thumb, 3, t.dim);
    }
    y = items.empty() ? kListTop : rows_bottom;
    std::string description;
    if (items.empty())
        description = "Midway " + version + "\nAn original Xbox emulator for the PS4: the Xbox's code runs on the "
                      "PS4's CPU as it is, with a kernel of Midway's own and xemu's GPU, sound, USB and network "
                      "chips (xemu.app).\n\n" + system_line + "\nData folder: /data/xbox-core\n\nGPL-2.0-or-later. "
                      "The menu is New Eden's. Its fonts are Inter (rsms.me/inter) and, for the Retro theme, Pixelify "
                      "Sans (github.com/eifetx/Pixelify-Sans), both under the SIL Open Font License.";
    else
        description = items[std::min(size_t(settings_items ? settings_item : 0), items.size() - 1)].description;
    y += items.empty() ? 0 : 36;
    for (const std::string& line : c.Wrap(description, 17, Weight::Regular, wrap_width))
    {
        c.Text(kItemLeft + 8, y, line, 17, Weight::Regular, t.dim);
        y += 26;
    }
    if (settings_items)
        DrawBottomBar(c, {{Glyph::Circle, "Back"}, {Glyph::Cross, "Change"}});
    else
        DrawBottomBar(c, {{Glyph::Circle, "Back"}, {Glyph::Cross, "OK"}});
}

void HomeMenu::DrawNews(Canvas& c)
{
    const Theme& t = CurrentTheme();
    DrawHeader(c, Icon::News, t.red, "News");
    c.Clip(0, kHeaderLine + 1, 1280, kBottomLine - kHeaderLine - 2);
    float y = 150 - page_scroll;
    for (const auto& [heading, body] : news)
    {
        c.Text(80, y, heading, 23, Weight::SemiBold, t.text);
        y += 36;
        for (const std::string& line : c.Wrap(body, 18, Weight::Regular, 1110))
        {
            c.Text(80, y, line, 18, Weight::Regular, line.empty() ? t.dim : t.text);
            y += 27;
        }
        y += 26;
    }
    // no scrolling past the end
    const float end = y + page_scroll;
    if (end - page_scroll < kBottomLine - 40 && page_scroll > 0)
        page_scroll = std::max(0.0f, end - (kBottomLine - 40));
    c.ResetClip();
    DrawBottomBar(c, {{Glyph::Circle, "Back"}});
}

void HomeMenu::DrawGames(Canvas& c)
{
    const Theme& t = CurrentTheme();
    DrawHeader(c, Icon::Games, t.orange, "Games and USB");
    c.Clip(0, kHeaderLine + 1, 1280, kBottomLine - kHeaderLine - 2);
    float y = 140 - page_scroll;
    for (const StatusRow& row : games_status)
    {
        c.Circle(88, y + 4, 15, row.ok ? t.good : t.bad);
        DrawIcon(c, row.ok ? Icon::Check : Icon::Cross, 88, y + 4, 20, Rgb(0xFFFFFF));
        c.Text(118, y + 11, row.label, 20, Weight::SemiBold, t.text);
        float dy = 36;
        for (const std::string& line : c.Wrap(row.detail, 17, Weight::Regular, 1080))
        {
            c.Text(118, y + dy, line, 17, Weight::Regular, t.dim);
            dy += 25;
        }
        y += dy + 22;
    }
    y += 10;
    for (const std::string& line : c.Wrap(games_help, 18, Weight::Regular, 1110))
    {
        c.Text(80, y, line, 18, Weight::Regular, t.text);
        y += 27;
    }
    const float end = y + page_scroll;
    if (end - page_scroll < kBottomLine - 40 && page_scroll > 0)
        page_scroll = std::max(0.0f, end - (kBottomLine - 40));
    c.ResetClip();
    DrawBottomBar(c, {{Glyph::Triangle, "Write Diagnostics"}, {Glyph::Square, "Copy All from USB"},
                      {Glyph::Circle, "Back"}});
}

void HomeMenu::DrawControllers(Canvas& c)
{
    const Theme& t = CurrentTheme();
    DrawHeader(c, Icon::Controllers, t.text, "Controllers");
    DrawController(c, 330, 300, 360, t.text, t.bg);
    c.Text(330, 520, controller ? "Player 1: the PS4 controller, as an Xbox controller" : "No controller connected", 18,
           Weight::Regular, controller ? t.text : t.bad, Align::Center);
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
    const float step = 37;
    float y = 150;
    for (const Row& row : rows)
    {
        if (row.has_glyph)
            DrawGlyph(c, row.glyph, 690, y - 7, t.text, t.bg);
        else
            c.Text(676, y, row.ps4, 18, Weight::Regular, t.text);
        c.Text(900, y, row.xbox, 19, Weight::SemiBold, t.accent);
        y += step;
    }
    c.Text(676, y + 6, "PS4 button", 15, Weight::Regular, t.dim);
    c.Text(900, y + 6, "Xbox button", 15, Weight::Regular, t.dim);
    c.Text(676, y + 40, "The controller's rumble is the Xbox controller's.", 16, Weight::Regular, t.dim);
    DrawBottomBar(c, {{Glyph::Circle, "Back"}});
}

void HomeMenu::DrawDialog(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    Dialog& dialog = dialogs.back();
    const float appear = std::min(1.0f, float(now - dialog.opened) / 0.12f);
    c.Rect(0, 0, 1280, 720, Rgb(0x000000, uint32_t(150 * appear)));

    std::string title = dialog.title, text = dialog.text;
    if (dialog.kind == DialogKind::TitleInfo && dialog.title_index < titles.size())
    {
        const Title& info = titles[dialog.title_index];
        title = info.name;
        text = (info.publisher.empty() ? "" : info.publisher + "\n") + info.kind + (info.size.empty() ? "" : ", " + info.size) +
               (info.location.empty() ? "" : ", " + info.location) + "\n" + (info.details.empty() ? "" : info.details + "\n") +
               info.path + (info.autostart ? "\nStarts when Midway opens (hold L1 as it opens for this menu)." : "");
    }
    const float w = 700, text_width = 620;
    const std::vector<std::string> title_lines = title.empty() ? std::vector<std::string>{}
                                                               : c.Wrap(title, 22, Weight::SemiBold, text_width);
    const std::vector<std::string> lines = text.empty() ? std::vector<std::string>{}
                                                        : c.Wrap(text, 19, Weight::Regular, text_width);
    const float buttons_h = 76;
    const float h = 56 + title_lines.size() * 32.0f + (title_lines.empty() || lines.empty() ? 0 : 14) +
                    lines.size() * 28.0f + 40 + buttons_h;
    const float x = 640 - w / 2, y = 360 - h / 2 + (1 - appear) * 12;
    if (settings.retro_theme)
    {
        // the Retro theme's chunky frame: a hard shadow, a dark outline, a cream border
        c.RoundRect(x - 2, y + 4, w + 12, h + 10, 20, Fade(kRetroInk, 0.85f));
        c.RoundRect(x - 8, y - 8, w + 16, h + 16, 22, kRetroInk);
        c.RoundRect(x - 4, y - 4, w + 8, h + 8, 18, kRetroCream);
    }
    c.RoundRect(x, y, w, h, 14, t.dialog);
    float ty = y + 56;
    for (const std::string& line : title_lines)
    {
        c.Text(640, ty, line, 22, Weight::SemiBold, t.text, Align::Center);
        ty += 32;
    }
    if (!title_lines.empty() && !lines.empty())
        ty += 14;
    for (const std::string& line : lines)
    {
        c.Text(640, ty, line, 19, Weight::Regular, t.text, Align::Center);
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
            c.RoundRectOutline(bx + 8, by + 8, bw - 16, buttons_h - 16, 8, 4.5f, Cursor(now));
        c.Text(bx + bw / 2, by + 46, dialog.buttons[i], 21, Weight::SemiBold, selected ? t.accent : t.text,
               Align::Center);
    }
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

// The User Page: the user, as games see it (its icon and nickname), and the two to change
void HomeMenu::DrawUser(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    const uint32_t cursor = Cursor(now);
    DrawHeader(c, Icon::User, t.text, "User Page");
    constexpr float kCx = 290, kCy = 318, kSize = 232;
    c.DrawImage(kCx - kSize / 2, kCy - kSize / 2, kSize, kSize,
                Avatar(settings.avatar, settings.avatar_color, kSize, c.Scale()));
    c.Text(kCx, kCy + kSize / 2 + 58, settings.nickname, 32, Weight::SemiBold, t.text, Align::Center, 440);
    c.Text(kCx, kCy + kSize / 2 + 90, "Midway user", 17, Weight::Regular, t.dim, Align::Center);
    c.Line(560, kHeaderLine + 12, 560, kBottomLine - 12, 1.2f, t.line);

    constexpr float kLeft = 620, kRight = 1230, kTop = 150;
    c.Text(kLeft, kTop - 4, "User Settings", 24, Weight::SemiBold, t.text);
    const std::string values[2] = {settings.nickname, AvatarName(settings.avatar)};
    const char* const labels[2] = {"Nickname", "Icon"};
    float y = kTop + 22;
    for (int i = 0; i < 2; i++)
    {
        c.Line(kLeft, y, kRight, y, 1.0f, t.line);
        if (user_item == i)
            c.RoundRectOutline(kLeft - 8, y + 4, kRight - kLeft + 16, kItemHeight - 8, 6, 4.5f, cursor);
        c.Text(kLeft + 16, y + 40, labels[i], 20, Weight::Regular, t.text);
        const float value_width = c.Text(kRight - 16, y + 40, values[i], 20, Weight::Regular, t.accent, Align::Right);
        if (i == 1)
            c.DrawImage(kRight - 16 - value_width - 48, y + 14, 36, 36,
                        Avatar(settings.avatar, settings.avatar_color, 36, c.Scale()));
        c.Line(kLeft, y + kItemHeight, kRight, y + kItemHeight, 1.0f, t.line);
        y += kItemHeight;
    }
    y += 40;
    const char* const help =
        "Your nickname and icon at the top of the HOME screen. (Xbox games keep their own profiles and saves: "
        "these don't change them.)";
    for (const std::string& line : c.Wrap(help, 17, Weight::Regular, kRight - kLeft - 16))
    {
        c.Text(kLeft + 8, y, line, 17, Weight::Regular, t.dim);
        y += 26;
    }
    DrawBottomBar(c, {{Glyph::Circle, "Back"}, {Glyph::Cross, "Change"}});
}

// The icon's choice: a character from the grid, then its background colour
void HomeMenu::DrawIconPicker(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    const uint32_t cursor = Cursor(now);
    DrawHeader(c, Icon::User, t.text, icon_step == 0 ? "Choose a Character" : "Choose a Background Colour");
    if (icon_step == 0)
    {
        const float left = (1280 - (kIconColumns * kIconCell + (kIconColumns - 1) * kIconGapX)) / 2;
        for (int i = 0; i < AvatarCount(); i++)
        {
            const float x = left + float(i % kIconColumns) * (kIconCell + kIconGapX);
            const float y = kIconTop + float(i / kIconColumns) * kIconRow;
            c.DrawImage(x, y, kIconCell, kIconCell, Avatar(i, icon_color, kIconCell, c.Scale()));
            if (i == icon_character)
                c.Ring(x + kIconCell / 2, y + kIconCell / 2, kIconCell / 2 + 9, 5, cursor);
        }
        const int rows = (AvatarCount() + kIconColumns - 1) / kIconColumns;
        c.Text(640, kIconTop + rows * kIconRow + 30, AvatarName(icon_character), 24, Weight::SemiBold, t.accent,
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
        c.Ring(cx, kSwatchY, kSwatchRadius, 1.5f, Fade(t.text, 0.2f));
        if (i == icon_color)
            c.Ring(cx, kSwatchY, kSwatchRadius + 9, 5, cursor);
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
        screen = Screen::User;
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
        screen = Screen::User; // (unchanged)
        return {};
    }
    if (pressed & kCircle)
    {
        if (keyboard_text.empty())
            screen = Screen::User;
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
    DrawHeader(c, Icon::User, t.text, "Nickname");
    // the text, its caret blinking, and how many characters it may have
    constexpr float kFieldX = 290, kFieldY = 136, kFieldW = 700, kFieldH = 72;
    c.RoundRect(kFieldX, kFieldY, kFieldW, kFieldH, 10, t.panel);
    c.RoundRectOutline(kFieldX, kFieldY, kFieldW, kFieldH, 10, 2, t.accent);
    const float text_width = c.Text(kFieldX + 24, kFieldY + 48, keyboard_text, 32, Weight::Regular, t.text);
    if (std::fmod(now, 1.0) < 0.6)
        c.Rect(kFieldX + 26 + text_width, kFieldY + 16, 2.5f, 40, t.accent);
    if (keyboard_text.empty())
        c.Text(kFieldX + 34, kFieldY + 48, "A nickname", 32, Weight::Regular, Fade(t.dim, 0.6f));
    c.Text(kFieldX + kFieldW, kFieldY + kFieldH + 26,
           std::to_string(keyboard_text.size()) + " / " + std::to_string(kNicknameLength), 17, Weight::Regular, t.dim,
           Align::Right);
    // the keys
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
                    c.RoundRectOutline(x - 6, y - 6, kKeyW + 12, kKeyH + 12, 10, 4.5f, cursor);
            }
            continue;
        }
        for (int i = 0; i < kWideKeyCount; i++)
        {
            const WideKey& key = kWideKeys[i];
            const float x = key_x(key.first), w = key_x(key.first + key.span - 1) + kKeyW - x;
            const bool ok = i == kWideKeyCount - 1, lit = (i == 0 && key_shift) || ok;
            c.RoundRect(x, y, w, kKeyH, 8, ok ? t.accent : lit ? Mix(t.button, t.accent, 0.35f) : t.button);
            c.Text(x + w / 2, y + kKeyH / 2 + 8, key.label, 21, Weight::SemiBold, ok ? Rgb(0xFFFFFF) : t.text,
                   Align::Center);
            if (row == key_row && WideKeyAt(key_column) == i)
                c.RoundRectOutline(x - 6, y - 6, w + 12, kKeyH + 12, 10, 4.5f, cursor);
        }
    }
    DrawBottomBar(c, {{Glyph::Triangle, "Cancel"}, {Glyph::Circle, "Delete"}, {Glyph::Options, "OK"},
                      {Glyph::Cross, "Enter"}});
}

} // namespace Ui
