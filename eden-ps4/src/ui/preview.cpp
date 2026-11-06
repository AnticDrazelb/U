// The HOME menu on the desktop: each of its screens, with sample titles, drawn to a PNG (1920x1080,
// as on the PS4). ui-preview.sh builds and runs it.
//   ui-preview <output folder>

#include <cstdio>
#include <string>
#include <vector>

#include "ui/home_menu.h"
#include "ui/loading_screen.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "ui/stb/stb_image_write.h"

using namespace Ui;

namespace {

std::string g_out;
double g_now = 10.0;

void Press(HomeMenu& menu, uint32_t button)
{
    menu.Update(button, g_now);
    g_now += 0.05;
    menu.Update(0, g_now);
    g_now += 0.05;
}

void Shot(HomeMenu& menu, const char* name)
{
    Canvas canvas(1920, 1080, 1.5f);
    // a few frames, so that scrolling settles
    for (int i = 0; i < 40; i++)
    {
        menu.Draw(canvas, g_now);
        g_now += 1.0 / 30;
    }
    const std::string path = g_out + "/" + name + ".png";
    stbi_write_png(path.c_str(), canvas.Width(), canvas.Height(), 4, canvas.Pixels(), canvas.Width() * 4);
    std::printf("%s\n", path.c_str());
}

std::vector<Title> SampleTitles()
{
    std::vector<Title> titles;
    const auto add = [&](std::string name, std::string kind, std::string location, std::string size, bool game,
                         Art art = Art::None) {
        Title t;
        t.name = std::move(name);
        t.kind = std::move(kind);
        t.location = std::move(location);
        t.size = std::move(size);
        t.game = game;
        t.art = art;
        t.path = (t.location.starts_with("USB") ? "/mnt/usb0/" : "/data/eden-ps4/") + t.name + "." +
                 (t.kind == "NRO" ? "nro" : t.kind == "XCI" ? "xci" : "nsp");
        t.on_ps4 = t.location == "data folder";
        titles.push_back(std::move(t));
    };
    add("Sky Garden Adventure", "NSP", "USB 0", "5.2 GB", true);
    add("Racing League Deluxe", "XCI", "data folder", "12.8 GB", true);
    add("Puzzle Quest of the Very Long Title: Remastered Edition", "NSP", "USB 1", "780 MB", true);
    add("Homebrew Menu", "NRO", "data folder", "4 MB", false);
    return titles;
}

HomeMenu MakeMenu(bool white)
{
    HomeMenu menu;
    menu.SetTitles(SampleTitles());
    MenuSettings settings;
    settings.white_theme = white;
    menu.SetSettings(settings);
    menu.SetInfo("03.20", "Vulkan on the PS4's GPU (Mesa's RADV)", true);
    menu.SetClock("9:27");
    menu.SetWebAddress("http://192.168.1.23:8080");
    menu.SetKeysPresent(false);
    menu.SetControllerTest("/app0/padtest.nro");
    menu.SetFolderPlaces({"/data/eden-ps4", "/mnt/usb0", "/mnt/usb1", "/mnt/ext0", "/mnt/ext1", "/mnt"});
    menu.SetNews({{"New Eden 03.20", "A HOME menu laid out as the Switch's: your software in a row, All Software "
                                         "for everything, and the system's round buttons below.\nGames on USB drives "
                                         "appear here too."},
                  {"The PS4's GPU", "Games are drawn with the PS4's GPU through Mesa's RADV. If Vulkan doesn't start, "
                                    "the software renderer takes over, and System Settings says so."}});
    menu.SetGamesStatus({{"Keys", "No prod.keys in /data/eden-ps4/user/keys/", false},
                         {"Firmware", "112 files in /data/eden-ps4/user/nand/system/Contents/registered/", true},
                         {"Games", "3 games: 2 on USB drives, 1 in the data folder", true}},
                        "Games (.nsp, .xci) and homebrew (.nro) go in /data/eden-ps4 or at the top of a USB drive. A "
                        "game needs the keys and firmware of the Switch it was dumped from.");
    return menu;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <output folder>\n", argv[0]);
        return 1;
    }
    g_out = argv[1];

    // (04.63) the loading screen: building shaders (an icon), then loading (none, white theme)
    {
        LoadingScreen loading;
        loading.name = "A Sample Game";
        loading.publisher = "Sample Publisher";
        loading.icon.width = loading.icon.height = 64;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
                loading.icon.pixels.push_back(Rgb(((x * 4) << 16) | ((y * 4) << 8) | 0xC0));
        Canvas canvas(1920, 1080, 1.5f);
        DrawLoadingScreen(canvas, loading, 2, 1234, 3249, 3.0);
        std::string path = g_out + "/loading-shaders.png";
        stbi_write_png(path.c_str(), canvas.Width(), canvas.Height(), 4, canvas.Pixels(), canvas.Width() * 4);
        std::printf("%s\n", path.c_str());
        loading.icon = {};
        loading.white_theme = true;
        // (04.69) a game that stopped with an error, and one taking a long time
        {
            Canvas failed(1920, 1080, 1.5f);
            DrawLoadingScreen(failed, loading, 3, 0, 0, 20.0, true);
            const std::string fail_path = g_out + "/loading-failed.png";
            stbi_write_png(fail_path.c_str(), failed.Width(), failed.Height(), 4, failed.Pixels(), failed.Width() * 4);
            std::printf("%s\n", fail_path.c_str());
            Canvas slow(1920, 1080, 1.5f);
            DrawLoadingScreen(slow, loading, 3, 0, 0, 200.0, false);
            const std::string slow_path = g_out + "/loading-slow.png";
            stbi_write_png(slow_path.c_str(), slow.Width(), slow.Height(), 4, slow.Pixels(), slow.Width() * 4);
            std::printf("%s\n", slow_path.c_str());
            // (04.71) the boot watcher starting it again
            Canvas retry(1920, 1080, 1.5f);
            DrawLoadingScreen(retry, loading, 3, 0, 0, 20.0, true, true);
            const std::string retry_path = g_out + "/loading-retry.png";
            stbi_write_png(retry_path.c_str(), retry.Width(), retry.Height(), 4, retry.Pixels(), retry.Width() * 4);
            std::printf("%s\n", retry_path.c_str());
        }
        Canvas small(1280, 720, 1.0f);
        DrawLoadingScreen(small, loading, 1, 0, 0, 0.7);
        path = g_out + "/loading-game.png";
        stbi_write_png(path.c_str(), small.Width(), small.Height(), 4, small.Pixels(), small.Width() * 4);
        std::printf("%s\n", path.c_str());
    }

    HomeMenu menu = MakeMenu(false);
    Shot(menu, "home");
    Press(menu, kRight);
    Shot(menu, "home-game");
    for (int i = 0; i < 6; i++)
        Press(menu, kRight);
    Shot(menu, "home-scrolled");
    for (int i = 0; i < 4; i++)
        Press(menu, kRight);
    Shot(menu, "home-all-software");
    Press(menu, kDown);
    Press(menu, kRight);
    Press(menu, kRight);
    Press(menu, kRight);
    Shot(menu, "home-buttons");
    Press(menu, kCross);
    Shot(menu, "settings");
    Press(menu, kDown);
    Press(menu, kRight);
    Press(menu, kDown);
    Press(menu, kCross);
    Shot(menu, "settings-graphics");
    for (int i = 0; i < 12; i++)  // (04.21) to the last row: the list scrolls
        Press(menu, kDown);
    Shot(menu, "settings-graphics-end");
    Press(menu, kLeft);
    Press(menu, kDown);
    Press(menu, kRight);
    Press(menu, kDown);
    Shot(menu, "settings-cpu");
    // (04.79) CPU > Speed-ups' page: open, a speed-up selected and turned off, back to CPU
    Press(menu, kCross);
    Shot(menu, "settings-cpu-speedups");
    for (int i = 0; i < 6; i++)
        Press(menu, kDown);
    Press(menu, kCross);
    Shot(menu, "settings-cpu-speedups-native-off");
    Press(menu, kCircle);
    Shot(menu, "settings-cpu-back");
    Press(menu, kCircle);  // the categories
    Press(menu, kDown);    // Console
    Press(menu, kDown);    // About
    Shot(menu, "settings-about");
    Press(menu, kCircle);
    Press(menu, kLeft);
    Press(menu, kLeft);
    Press(menu, kLeft);
    Press(menu, kCross);
    Shot(menu, "news");
    Press(menu, kCircle);
    Press(menu, kRight);
    Press(menu, kCross);
    Shot(menu, "games");
    Press(menu, kTriangle); // Game Folder: its places (those that exist where the preview runs)
    Shot(menu, "game-folder");
    Press(menu, kCross); // the first place, opened
    Shot(menu, "game-folder-opened");
    Press(menu, kCircle);
    Press(menu, kCircle);
    Press(menu, kCircle);
    Press(menu, kRight);
    Press(menu, kCross);
    Shot(menu, "controllers");
    Press(menu, kCircle);
    Press(menu, kRight);
    Press(menu, kRight);
    Press(menu, kCross);
    Shot(menu, "dialog-power");
    Press(menu, kCircle);
    Press(menu, kUp);
    Press(menu, kCross); // All Software
    Press(menu, kRight);
    Press(menu, kDown);
    Shot(menu, "all-software");
    Press(menu, kUp);
    Press(menu, kCross); // a game, without keys
    Shot(menu, "dialog-no-keys");
    Press(menu, kCross);
    Press(menu, kOptions);
    Shot(menu, "dialog-info");

    // OPTIONS on a game on the PS4's storage: Delete, which asks first
    HomeMenu deleting = MakeMenu(false);
    for (int i = 0; i < 3; i++)
        Press(deleting, kRight);
    Press(deleting, kOptions);
    Shot(deleting, "dialog-info-delete");
    Press(deleting, kLeft);
    Press(deleting, kCross);
    Shot(deleting, "dialog-delete");
    Press(deleting, kRight);
    const MenuAction deleted = deleting.Update(kCross, g_now);
    std::printf("delete action: %s\n", deleted.kind == MenuAction::Kind::Delete ? deleted.path.c_str() : "none");

    HomeMenu dark = MakeMenu(true);
    dark.SetSettings(MenuSettings{false, false, false});
    Report report;
    report.log = {"booting game.nsp", "[  12.345] Core <Critical> core.cpp:365: Failed to load", "..."};
    report.crash = {"=== signal 11 (SIGSEGV), fault address 0x0", "rip 0x7ff92109ec0c (not eboot code)"};
    report.trace = {"[  3.796] start page: booting /data/eden-ps4/game.nsp", "[  4.240] booting game.nsp"};
    dark.ShowReport(report);
    Shot(dark, "report");
    Press(dark, kCross);
    Press(dark, kRight);
    Shot(dark, "home-black");

    HomeMenu white = MakeMenu(true);
    Press(white, kRight);
    Shot(white, "home-white");

    // (04.64) the user's icon, its User Page, the icon's choice and the keyboard
    HomeMenu user = MakeMenu(false);
    Press(user, kUp);
    Shot(user, "user-home");
    Press(user, kCross);
    Shot(user, "user-page");
    Press(user, kDown);
    Press(user, kCross);
    for (int i = 0; i < 3; i++)
        Press(user, kRight);
    Press(user, kDown);
    Shot(user, "user-icon-character");
    Press(user, kCross);
    for (int i = 0; i < 4; i++)
        Press(user, kRight);
    Shot(user, "user-icon-colour");
    const MenuAction chosen = user.Update(kCross, g_now);
    std::printf("icon chosen: %s, %d on %d\n", chosen.kind == MenuAction::Kind::SettingsChanged ? "saved" : "not saved",
                user.Settings().avatar, user.Settings().avatar_color);
    user.Update(0, g_now += 0.05);
    Press(user, kUp);
    Press(user, kCross);
    for (int i = 0; i < 6; i++)
        Press(user, kCircle); // "Player" deleted
    // "Alex", the cursor left on Shift
    for (const uint32_t key : {kDown, kCross, kRight, kRight, kRight, kRight, kRight, kRight, kRight, kRight, kCross,
                               kUp, kLeft, kLeft, kLeft, kLeft, kLeft, kLeft, kCross, kDown, kDown, kLeft, kCross, kDown})
        Press(user, key);
    Shot(user, "user-keyboard");
    const MenuAction named = user.Update(kOptions, g_now);
    std::printf("nickname: %s (%s)\n", user.Settings().nickname.c_str(),
                named.kind == MenuAction::Kind::SettingsChanged ? "saved" : "not saved");
    user.Update(0, g_now += 0.05);
    Shot(user, "user-page-changed");
    HomeMenu user_white = MakeMenu(true);
    Press(user_white, kUp);
    Press(user_white, kCross);
    Press(user_white, kCross);
    Shot(user_white, "user-keyboard-white");
    return 0;
}
