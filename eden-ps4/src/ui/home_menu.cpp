// The HOME menu (home_menu.h). Positions are the Switch's, in its 1280x720 units.

#include "ui/home_menu.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

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
    {Icon::Games, "Games and Keys"},
    {Icon::Controllers, "Controllers"},
    {Icon::Settings, "System Settings"},
    {Icon::Power, "Power"},
};
constexpr int kButtonCount = int(sizeof(kButtons) / sizeof(kButtons[0]));

const char* const kCategories[] = {"Themes", "Graphics", "CPU", "Console", "About"};
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

// Basic Black and Basic White
const HomeMenu::Theme& HomeMenu::CurrentTheme() const
{
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

void HomeMenu::SetInfo(std::string version_, std::string renderer_, bool radv_linked_)
{
    version = std::move(version_);
    renderer = std::move(renderer_);
    radv_linked = radv_linked_;
}

void HomeMenu::SetGamesStatus(std::vector<StatusRow> rows, std::string help)
{
    games_status = std::move(rows);
    games_help = std::move(help);
}

void HomeMenu::ShowReport(Report new_report)
{
    report = std::move(new_report);
    screen = Screen::Report;
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
    std::vector<Image>& list = tiles[pixels];
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
            tile = MakeCover(title.name, title.kind, pixels);
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
    if (titles[index].game && !keys_present)
    {
        Dialog dialog;
        dialog.kind = DialogKind::NoKeys;
        dialog.title = "The keys of your Switch are needed.";
        dialog.text = web_address.empty()
            ? "A game (.nsp, .xci) needs prod.keys (and title.keys) from the Switch it was dumped from, and "
              "that Switch's firmware. Put them on a USB drive, where they're used from (the keys at its top "
              "or in a folder there such as switch, the firmware's .nca files in a folder), or copy them into "
              "/data/eden-ps4/import and Import (Square) in Games and Keys."
            : "A game (.nsp, .xci) needs prod.keys (and title.keys) from the Switch it was dumped from, and "
              "that Switch's firmware. Open " + web_address + " in a browser on a PC or phone on the same "
              "network to send them, or put them on a USB drive, where they're used from.";
        dialog.buttons = {"OK"};
        OpenDialog(std::move(dialog));
        return {};
    }
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
        if (dialog.offers_copy && button == 1 && dialog.title_index < titles.size())
        {
            MenuAction action;
            action.kind = MenuAction::Kind::CopyToPs4;
            action.path = titles[dialog.title_index].path;
            return action;
        }
        if (button == (dialog.offers_copy ? 2 : 1))
            return Start(dialog.title_index);
        break;
    case DialogKind::Message:
    case DialogKind::NoKeys:
        break;
    }
    return {};
}

void HomeMenu::OpenTitleInfo(size_t index)
{
    Dialog dialog;
    dialog.kind = DialogKind::TitleInfo;
    dialog.title_index = index;
    // a game on a USB drive or in the Game Folder can be copied to the PS4's own storage
    dialog.offers_copy = index < titles.size() && titles[index].game && titles[index].location != "data folder" &&
                         titles[index].location != "in the package";
    dialog.buttons = dialog.offers_copy ? std::vector<std::string>{"Close", "Copy to the PS4", "Start"}
                                        : std::vector<std::string>{"Close", "Start"};
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
        if (home_row == 0)
        {
            if ((moves & kLeft) && home_tile > 0)
                home_tile--;
            if ((moves & kRight) && home_tile < HomeCount())
                home_tile++;
            if (moves & kDown)
                home_row = 1;
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
                    break;
                default: {
                    Dialog dialog;
                    dialog.kind = DialogKind::Quit;
                    dialog.title = "Close Eden for PS4?";
                    dialog.text = "The PS4's home screen comes back. (In a game, L3 + R3 held for a second "
                                  "stops it and returns here.)";
                    dialog.buttons = {"Cancel", "Close"};
                    OpenDialog(std::move(dialog));
                    break;
                }
                }
            }
        }
        return {};

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
            if (moves & kUp)
                settings_category = std::max(0, settings_category - 1);
            if (moves & kDown)
                settings_category = std::min(kCategoryCount - 1, settings_category + 1);
            if (((moves & kRight) || (pressed & kCross)) && items > 0)
            {
                settings_items = true;
                settings_item = 0;
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
        {
            OpenFolder("");
            screen = Screen::Folder;
            return {};
        }
        if (moves & kDown)
            page_scroll += 60;
        if (moves & kUp)
            page_scroll = std::max(0.0f, page_scroll - 60);
        return {};

    case Screen::Controllers:
        if (pressed & kCircle)
            screen = Screen::Home;
        if ((pressed & kCross) && !controller_test.empty())
            return {MenuAction::Kind::Boot, controller_test};
        return {};

    case Screen::Report:
        if (pressed & (kCross | kCircle))
            screen = Screen::Home;
        return {};

    case Screen::Folder: {
        const auto selectable = [&](int i) { return folder_rows[size_t(i)].kind != FolderRow::Kind::Heading; };
        const int count = int(folder_rows.size());
        if (moves & kUp)
            for (int i = folder_selected - 1; i >= 0; i--)
                if (selectable(i))
                {
                    folder_selected = i;
                    break;
                }
        if (moves & kDown)
            for (int i = folder_selected + 1; i < count; i++)
                if (selectable(i))
                {
                    folder_selected = i;
                    break;
                }
        if (pressed & kCircle)
        {
            // up a folder, from the places back to Games and Keys
            if (browse_dir.empty())
                screen = Screen::Games;
            else
                OpenFolder(FolderParent(browse_dir));
            return {};
        }
        if ((pressed & kCross) && folder_selected < count)
        {
            const FolderRow row = folder_rows[size_t(folder_selected)];
            switch (row.kind)
            {
            case FolderRow::Kind::Open:
                OpenFolder(row.target);
                return {};
            case FolderRow::Kind::Use:
            case FolderRow::Kind::Clear:
                settings.games_folder = row.kind == FolderRow::Kind::Use ? browse_dir : std::string();
                screen = Screen::Games;
                page_scroll = 0;
                return {MenuAction::Kind::SettingsChanged, {}};
            case FolderRow::Kind::Heading:
                break;
            }
        }
        return {};
    }
    }
    return {};
}

std::string HomeMenu::FolderParent(const std::string& path) const
{
    if (path == "/" || std::find(folder_places.begin(), folder_places.end(), path) != folder_places.end())
        return {};
    return path.substr(0, std::max<size_t>(path.rfind('/'), 1));
}

// Game Folder's browser at `path` ("" the places): "Use This Folder", "..", and its folders
void HomeMenu::OpenFolder(const std::string& path)
{
    browse_dir = path;
    folder_rows.clear();
    folder_selected = 0;
    folder_scroll = 0;
    struct stat status{};
    if (path.empty())
    {
        folder_rows.push_back({FolderRow::Kind::Heading, "Places", "", ""});
        for (const std::string& place : folder_places)
        {
            if (stat(place.c_str(), &status) == 0 && S_ISDIR(status.st_mode))
                folder_rows.push_back({FolderRow::Kind::Open, place, "", place});
        }
        folder_rows.push_back({FolderRow::Kind::Open, "/", "The console's root, to find a drive elsewhere", "/"});
        folder_rows.push_back({FolderRow::Kind::Clear, "No Game Folder",
                               "Games only in /data/eden-ps4 and on the USB drives' tops and folders", ""});
    }
    else
    {
        folder_rows.push_back({FolderRow::Kind::Heading, path, "", ""});
        folder_rows.push_back({FolderRow::Kind::Use, "Use This Folder",
                               "Games, prod.keys, title.keys and the firmware are looked for in it and its folders", ""});
        const std::string parent = FolderParent(path);
        folder_rows.push_back({FolderRow::Kind::Open, "..", parent.empty() ? "Places" : parent, parent});
        std::vector<std::string> folders;
        std::string error;
        if (DIR* directory = opendir(path.c_str()))
        {
            while (const dirent* entry = readdir(directory))
            {
                const std::string name = entry->d_name;
                if (name.empty() || name[0] == '.')
                    continue;
                const std::string full = (path == "/" ? "" : path) + "/" + name;
                if (stat(full.c_str(), &status) == 0 && S_ISDIR(status.st_mode))
                    folders.push_back(name);
            }
            closedir(directory);
        }
        else
        {
            error = std::string("This folder can't be opened: ") + std::strerror(errno);
        }
        std::sort(folders.begin(), folders.end());
        if (!error.empty())
            folder_rows.push_back({FolderRow::Kind::Heading, error, "", ""});
        for (const std::string& name : folders)
            folder_rows.push_back({FolderRow::Kind::Open, name + "/", "", (path == "/" ? "" : path) + "/" + name});
    }
    for (size_t i = 0; i < folder_rows.size(); i++)
    {
        if (folder_rows[i].kind != FolderRow::Kind::Heading)
        {
            folder_selected = int(i);
            break;
        }
    }
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
    const Theme& t = CurrentTheme();
    c.Clear(t.bg);
    switch (screen)
    {
    case Screen::Home:
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
        DrawControllers(c, now);
        break;
    case Screen::Report:
        DrawReport(c);
        break;
    case Screen::Folder:
        DrawFolder(c, now);
        break;
    }
    if (countdown >= 0)
    {
        const std::string text = "No controller found: the test program starts in " + std::to_string(countdown) + " s";
        const float w = c.TextWidth(text, 18, Weight::Regular) + 48;
        c.RoundRect(640 - w / 2, 580, w, 44, 22, t.dialog);
        c.Text(640, 608, text, 18, Weight::Regular, t.text, Align::Center);
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

    // the user and the time
    c.Circle(66, 52, 25, Rgb(0x4FA3E8));
    DrawIcon(c, Icon::User, 66, 54, 36, Rgb(0xFFFFFF));
    c.Ring(66, 52, 25, 1.5f, Fade(Rgb(0xFFFFFF), 0.5f));
    // where a browser on the network reaches the app's web page (keys, firmware, games, logs)
    if (!web_address.empty())
        c.Text(108, 60, "From a PC or phone: " + web_address, 19, Weight::Regular, t.dim);
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
        c.Text(kRowLeft, kTitleBaseline, "No software: put games (.nsp, .xci) or homebrew (.nro) in /data/eden-ps4 "
               "or on a USB drive", 19, Weight::Regular, t.dim);

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
        return {{"Theme", settings.white_theme ? "Basic White" : "Basic Black",
                 "The HOME menu's colours, as the Switch's two themes.",
                 [](MenuSettings& s) { s.white_theme = !s.white_theme; }}};
    case 1: {
        const int width = (settings.docked ? 1920 : 1280) * settings.resolution / 100;
        const int height = (settings.docked ? 1080 : 720) * settings.resolution / 100;
        return {
            {"Renderer", settings.software_renderer ? "Software (the PS4's CPU)" : "Vulkan (the PS4's GPU)",
             "Vulkan draws the Switch's GPU work with the PS4's GPU, through Mesa's RADV driver" +
                 std::string(radv_linked ? "." : " (not in this build).") +
                 " The software renderer draws it with the CPU: every title works the same, but 3D games are far "
                 "too slow to play. If Vulkan doesn't start, the software renderer takes over.\n\nNow: " +
                 renderer + ".",
             [](MenuSettings& s) { s.software_renderer = !s.software_renderer; }},
            {"Resolution",
             std::to_string(settings.resolution) + "%  (" + std::to_string(width) + "x" + std::to_string(height) + ")",
             "Games draw at this share of the console mode's resolution, then scaled up: less of the PS4's GPU "
             "memory (a quarter at 50%) and less GPU work, at the cost of sharpness. It doesn't speed up a game held "
             "back by the CPU, as most are. Some games show glitches below 100%.",
             [](MenuSettings& s) { s.resolution = s.resolution == 100 ? 75 : s.resolution == 75 ? 50 : 100; }},
            {"Shader Cache", on_off(settings.shader_cache),
             "A game's shaders are kept in /data/eden-ps4/user/cache/shader, and built while it loads the next time "
             "instead of when they first appear: a longer start, less stutter once it runs.",
             [](MenuSettings& s) { s.shader_cache = !s.shader_cache; }},
            {"Asynchronous Shaders", on_off(settings.async_shaders),
             "New shaders are built in the background instead of stopping the game while they compile (slow on "
             "the PS4's CPU): what needs one is missing for a moment instead of the game freezing.",
             [](MenuSettings& s) { s.async_shaders = !s.async_shaders; }},
            {"FPS Counter", on_off(settings.fps_overlay),
             "The frames shown each second, top left of the picture while a game runs (a game's own target is "
             "usually 30 or 60).",
             [](MenuSettings& s) { s.fps_overlay = !s.fps_overlay; }},
            {"GPU Accuracy", settings.gpu_high ? "High" : "Normal",
             "How closely the CPU waits for the GPU. High reads back what the GPU drew whenever the game's CPU code "
             "might look at it, and makes the CPU's every access to such memory take a slow path: needed by a few "
             "games (effects, screenshots), a large cost on the PS4's CPU for the rest. Normal is Eden's choice on "
             "phones.",
             [](MenuSettings& s) { s.gpu_high = !s.gpu_high; }},
        };
    }
    case 2:
        return {
            {"Accuracy", settings.cpu_unsafe ? "Unsafe" : "Auto",
             "How exactly the Switch's CPU is followed. Unsafe skips some exactness in floating-point maths and in "
             "how cores share memory: about 10% faster on vector maths on the PS4 (measured), no faster on the "
             "rest, and some games misbehave with it.",
             [](MenuSettings& s) { s.cpu_unsafe = !s.cpu_unsafe; }},
            {"CPU Cores", settings.seventh_core ? "7 (with the shared one)" : "6",
             "A PS4 app has 6 cores to itself and may use a 7th that the system shares. With 7, Eden's threads "
             "(shader compilers, services, the GPU's work) have more room; with 6, none of them is slowed when the "
             "system needs its core.",
             [](MenuSettings& s) { s.seventh_core = !s.seventh_core; }},
            {"Profiler", on_off(settings.profiler),
             "While a game runs, where the emulated CPU cores, the GPU thread and the Vulkan thread spend their "
             "time: each is looked at 1000 times a second, and every 10 seconds the counts go to samples.log. "
             "The web page's Profile summary sums them up by function, and Download all logs includes them: "
             "what to send when a game is slow. It costs a little speed itself.",
             [](MenuSettings& s) { s.profiler = !s.profiler; }},
        };
    case 3:
        return {{"Console Mode", settings.docked ? "Docked (TV, 1920x1080)" : "Handheld (1280x720)",
                 "Games run as on a Switch in handheld mode, at 1280x720, or docked, at 1920x1080. Handheld asks "
                 "less of the PS4's GPU.",
                 [](MenuSettings& s) { s.docked = !s.docked; }},
                {"Switch Memory", settings.ram_on_demand ? "As the game uses it" : "All 4 GB up front",
                 "The Switch's 4 GB of RAM come out of the PS4's memory as the game first uses them, so what it "
                 "doesn't use is left to the PS4's GPU for textures and buffers: often a few GB instead of about "
                 "half of one. All 4 GB up front is how it was before 03.70: try it if a game fails with this on.",
                 [](MenuSettings& s) { s.ram_on_demand = !s.ram_on_demand; }}};
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
    float y = 176;
    for (size_t i = 0; i < items.size(); i++)
    {
        const bool selected = settings_items && int(i) == settings_item;
        c.Line(kItemLeft, y, kItemRight, y, 1.0f, t.line);
        if (selected)
            c.RoundRectOutline(kItemLeft - 8, y + 4, kItemRight - kItemLeft + 16, kItemHeight - 8, 6, 4.5f, cursor);
        c.Text(kItemLeft + 16, y + 40, items[i].label, 20, Weight::Regular, t.text);
        c.Text(kItemRight - 16, y + 40, items[i].value, 20, Weight::Regular, t.accent, Align::Right);
        y += kItemHeight;
        c.Line(kItemLeft, y, kItemRight, y, 1.0f, t.line);
    }
    std::string description;
    if (items.empty())
        description = "Eden for PS4 " + version + "\nThe Nintendo Switch emulator Eden (eden-emu.dev), on the PS4.\n\n"
                      "Renderer: " + renderer + "\nData folder: /data/eden-ps4\n\nGPL-3.0-or-later. The font is "
                      "Inter (rsms.me/inter), under the SIL Open Font License.";
    else
        description = items[std::min(size_t(settings_items ? settings_item : 0), items.size() - 1)].description;
    y += items.empty() ? 0 : 36;
    for (const std::string& line : c.Wrap(description, 17, Weight::Regular, kItemRight - kItemLeft - 16))
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
    DrawHeader(c, Icon::Games, t.orange, "Games and Keys");
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
    DrawBottomBar(c, {{Glyph::Triangle, "Game Folder"}, {Glyph::Square, "Import"}, {Glyph::Circle, "Back"}});
}

void HomeMenu::DrawFolder(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    DrawHeader(c, Icon::Games, t.orange, "Game Folder");
    constexpr float kTop = 124, kRow = 56;
    // the selected row kept on the screen
    const float selected_y = kTop + float(folder_selected) * kRow;
    if (selected_y - folder_scroll > kBottomLine - kRow - 10)
        folder_scroll = selected_y - (kBottomLine - kRow - 10);
    if (selected_y - folder_scroll < kTop)
        folder_scroll = std::max(0.0f, selected_y - kTop);
    c.Clip(0, kHeaderLine + 1, 1280, kBottomLine - kHeaderLine - 2);
    for (size_t i = 0; i < folder_rows.size(); i++)
    {
        const FolderRow& row = folder_rows[i];
        const float y = kTop + float(i) * kRow - folder_scroll;
        if (y < kHeaderLine - kRow || y > kBottomLine)
            continue;
        if (row.kind == FolderRow::Kind::Heading)
        {
            c.Text(80, y + 34, row.label, 20, Weight::SemiBold, t.dim);
            continue;
        }
        const bool current = int(i) == folder_selected;
        if (current)
            c.RoundRect(64, y + 4, 1152, kRow - 8, 6, Fade(Cursor(now), 0.25f));
        c.Text(88, y + 35, row.label, 20, current ? Weight::SemiBold : Weight::Regular,
               row.kind == FolderRow::Kind::Use ? t.accent : t.text);
        if (!row.detail.empty())
            c.Text(1192, y + 35, row.detail, 16, Weight::Regular, t.dim, Align::Right);
    }
    c.ResetClip();
    DrawBottomBar(c, {{Glyph::Circle, "Back"}, {Glyph::Cross, "OK"}});
}

void HomeMenu::DrawControllers(Canvas& c, double now)
{
    const Theme& t = CurrentTheme();
    DrawHeader(c, Icon::Controllers, t.text, "Controllers");
    DrawController(c, 330, 300, 360, t.text, t.bg);
    c.Text(330, 520, controller ? "Player 1: the PS4 controller, as a Pro Controller" : "No controller connected", 18,
           Weight::Regular, controller ? t.text : t.bad, Align::Center);
    struct Row
    {
        Glyph glyph;
        bool has_glyph;
        const char* ps4;
        const char* sw;
    };
    const Row rows[] = {
        {Glyph::Cross, true, "", "A"},          {Glyph::Circle, true, "", "B"},
        {Glyph::Triangle, true, "", "X"},       {Glyph::Square, true, "", "Y"},
        {Glyph::L1, false, "L1 / R1", "L / R"}, {Glyph::L1, false, "L2 / R2", "ZL / ZR"},
        {Glyph::Options, true, "", "+"},        {Glyph::L1, false, "Touchpad click", "\xE2\x88\x92"},
        {Glyph::L1, false, "L3 + R3, held 1 s", "Stops the software"},
    };
    float y = 150;
    for (const Row& row : rows)
    {
        if (row.has_glyph)
            DrawGlyph(c, row.glyph, 690, y - 7, t.text, t.bg);
        else
            c.Text(676, y, row.ps4, 18, Weight::Regular, t.text);
        c.Text(900, y, row.sw, 19, Weight::SemiBold, t.accent);
        y += 40;
    }
    c.Text(676, y + 6, "PS4 button", 15, Weight::Regular, t.dim);
    c.Text(900, y + 6, "Switch button", 15, Weight::Regular, t.dim);
    if (!controller_test.empty())
    {
        const float bx = 676, by = 560, bw = 330, bh = 56;
        c.RoundRect(bx, by, bw, bh, 10, t.button);
        c.RoundRectOutline(bx - 8, by - 8, bw + 16, bh + 16, 12, 4.5f, Cursor(now));
        c.Text(bx + bw / 2, by + 36, "Test Input Devices", 20, Weight::SemiBold, t.text, Align::Center);
        DrawBottomBar(c, {{Glyph::Circle, "Back"}, {Glyph::Cross, "Test"}});
    }
    else
    {
        DrawBottomBar(c, {{Glyph::Circle, "Back"}});
    }
}

void HomeMenu::DrawReport(Canvas& c)
{
    const Theme& t = CurrentTheme();
    c.Circle(68, 62, 18, t.bad);
    c.Text(68, 70, "!", 24, Weight::SemiBold, Rgb(0xFFFFFF), Align::Center);
    c.Text(100, 72, "The software was closed because an error occurred.", 25, Weight::SemiBold, t.text);
    c.Line(30, kHeaderLine, 1250, kHeaderLine, 1.2f, t.line);
    c.Text(60, 136,
           web_address.empty()
               ? std::string("Take a photo of this screen to report it: the end of its output, its crash report and "
                             "its last steps.")
               : "Every log, to send: open " + web_address + " in a browser on the same network and choose "
                 "Download all logs. Or take a photo of this screen.",
           17, Weight::Regular, t.dim, Align::Left, 1160);
    const auto column = [&](float x, float y, float bottom, const std::string& heading,
                            const std::vector<std::string>& lines, uint32_t color) {
        c.Text(x, y, heading, 17, Weight::SemiBold, t.accent);
        y += 24;
        if (lines.empty())
        {
            c.Text(x, y, "(none)", 14, Weight::Regular, t.dim);
            return y + 22;
        }
        const float line_height = 18.5f;
        const size_t room = size_t(std::max(1.0f, (bottom - y) / line_height));
        const size_t first = lines.size() > room ? lines.size() - room : 0;
        for (size_t i = first; i < lines.size(); i++, y += line_height)
            c.Text(x, y, lines[i], 13.5f, Weight::Regular, color, Align::Left, 560);
        return y + 12;
    };
    column(60, 176, 630, "Its output (log.txt)", report.log, t.text);
    const float y = column(660, 176, 400, "Its crash report (crash.log)", report.crash, t.bad);
    column(660, std::max(y, 300.0f), 630, "Its last steps (boot-trace.log)", report.trace, t.text);
    DrawBottomBar(c, {{Glyph::Cross, "Continue"}});
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
               (info.location.empty() ? "" : ", " + info.location) + "\n" + info.path;
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

} // namespace Ui
