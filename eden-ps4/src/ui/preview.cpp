// The HOME menu on the desktop: each of its screens, with sample titles, drawn to a PNG (1920x1080,
// as on the PS4). ui-preview.sh builds and runs it.
//   ui-preview <output folder>

#include <cstdio>
#include <string>
#include <vector>

#include "ui/home_menu.h"
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
        t.path = "/data/eden-ps4/" + t.name;
        titles.push_back(std::move(t));
    };
    add("Breakout", "NRO", "in the package", "1 MB", false, Art::Breakout);
    add("Sky Garden Adventure", "NSP", "USB 0", "5.2 GB", true);
    add("Blocks", "NRO", "in the package", "1 MB", false, Art::Blocks);
    add("Racing League Deluxe", "XCI", "data folder", "12.8 GB", true);
    add("Controller test", "NRO", "in the package", "1 MB", false, Art::Controller);
    add("GPU test", "NRO", "in the package", "1 MB", false, Art::Gpu);
    add("The test program", "NRO", "in the package", "1 MB", false, Art::Terminal);
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
    menu.SetKeysPresent(false);
    menu.SetControllerTest("/app0/padtest.nro");
    menu.SetNews({{"Eden for PS4 03.20", "A HOME menu laid out as the Switch's: your software in a row, All Software "
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
    Press(menu, kLeft);
    Press(menu, kDown);
    Press(menu, kRight);
    Press(menu, kDown);
    Shot(menu, "settings-cpu");
    Press(menu, kCircle);
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
    return 0;
}
