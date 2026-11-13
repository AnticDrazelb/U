// The PS4 front end's HOME menu (frontend/home.h, New Eden's menu) on the desktop: each screen drawn
// to a PNG (1920x1080, as on the PS4), from a library folder made by tools/ui-preview.sh.
//   ui-preview <library folder> <output folder>
// The library folder: games/ (the player's), samples/, covers/, usb0/ (a USB drive), history.txt.

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "../frontend/home.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "../frontend/ui/stb/stb_image_write.h"

using namespace Ui;

namespace {

std::string g_out;
double g_now = 10.0;

void Press(fe::Home& home, uint32_t button)
{
    home.Update(button, g_now);
    g_now += 0.05;
    home.Update(0, g_now);
    g_now += 0.05;
}

void Save(const Canvas& canvas, const std::string& name)
{
    const std::string path = g_out + "/" + name + ".png";
    stbi_write_png(path.c_str(), canvas.Width(), canvas.Height(), 4, canvas.Pixels(), canvas.Width() * 4);
    std::printf("%s\n", path.c_str());
}

void Shot(fe::Home& home, const char* name)
{
    Canvas canvas(1920, 1080, 1.5f);
    for (int i = 0; i < 40; i++) // a few frames, so that scrolling settles
    {
        home.Update(0, g_now);
        home.Draw(canvas, g_now, "21:47", true);
        g_now += 1.0 / 30;
    }
    Save(canvas, name);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <library folder> <output folder>\n", argv[0]);
        return 1;
    }
    const std::string lib = argv[1];
    g_out = argv[2];
    // (Settings must outlive Home: kept here)
    static fe::Settings keep[4];
    const auto home_with = [&](int theme) {
        keep[theme].theme = theme;
        fe::HomePaths paths;
        paths.library = {{lib + "/samples"}, lib + "/games", {}, lib + "/covers", lib + "/history.txt"};
        paths.usb = {lib + "/usb0"};
        paths.settings = lib + "/settings.txt";
        paths.autostart = lib + "/autostart.txt";
        paths.version = "00.01";
        paths.system = "PS4 firmware 13.52, its code clear of the Xbox's addresses";
        paths.write_logs = [lib](const std::vector<fe::Game>&) { return std::vector<std::string>{lib + "/usb0/xbox-core-logs.txt"}; };
        return std::make_unique<fe::Home>(keep[theme], paths, "Jordan");
    };

    auto home = home_with(0);
    Shot(*home, "home");
    Press(*home, kRight);
    Shot(*home, "home-game");
    for (int i = 0; i < 12; i++)
        Press(*home, kRight);
    Shot(*home, "home-all-software");
    Press(*home, kDown);
    Press(*home, kRight);
    Press(*home, kRight);
    Press(*home, kRight);
    Shot(*home, "home-buttons");
    Press(*home, kCross);
    Shot(*home, "settings");
    Press(*home, kDown);
    Press(*home, kRight);
    Shot(*home, "settings-graphics");
    Press(*home, kLeft);
    Press(*home, kDown);
    Press(*home, kRight);
    Shot(*home, "settings-sound");
    Press(*home, kLeft);
    Press(*home, kDown);
    Press(*home, kDown);
    Shot(*home, "settings-about");
    Press(*home, kCircle);
    for (int i = 0; i < 3; i++)
        Press(*home, kLeft);
    Press(*home, kCross);
    Shot(*home, "news");
    Press(*home, kCircle);
    Press(*home, kRight);
    Press(*home, kCross);
    Shot(*home, "games");
    Press(*home, kCircle);
    Press(*home, kRight);
    Press(*home, kCross);
    Shot(*home, "controllers");
    Press(*home, kCircle);
    Press(*home, kRight);
    Press(*home, kRight);
    Press(*home, kCross);
    Shot(*home, "dialog-power");
    Press(*home, kCircle);
    Press(*home, kUp);
    for (int i = 0; i < 13; i++)
        Press(*home, kLeft);
    Press(*home, kOptions);
    Shot(*home, "dialog-info");
    Press(*home, kCircle);
    for (int i = 0; i < 5; i++) // Orbital Golf, on the USB drive
        Press(*home, kRight);
    Press(*home, kOptions);
    Shot(*home, "dialog-info-usb");
    // its copy to the PS4: the message's progress, then its result, and the library read again
    Press(*home, kLeft);
    Press(*home, kCross);
    for (int i = 0; i < 200 && home->Games().size() == 14; i++)
    {
        Press(*home, 0);
        bool copied = false;
        for (const fe::Game& g : home->Games())
            copied |= g.title == "Orbital Golf" && g.usb.empty();
        if (copied)
            break;
        struct timespec ts{0, 20 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    Shot(*home, "copy-done");
    size_t on_ps4 = 0;
    for (const fe::Game& g : home->Games())
        on_ps4 += g.title == "Orbital Golf" && g.usb.empty();
    std::printf("copy to the PS4: %s (%zu titles)\n", on_ps4 ? "done" : "NOT DONE", home->Games().size());
    Press(*home, kCross);

    // All Software
    auto all = home_with(1);
    for (int i = 0; i < 12; i++)
        Press(*all, kRight);
    Press(*all, kCross);
    Press(*all, kRight);
    Press(*all, kDown);
    Shot(*all, "all-software-white");
    Press(*all, kCircle);
    Press(*all, kLeft);
    Shot(*all, "home-white");

    auto retro = home_with(2);
    Press(*retro, kRight);
    Shot(*retro, "retro-home");
    Press(*retro, kDown);
    Press(*retro, kRight);
    Press(*retro, kRight);
    Press(*retro, kRight);
    Press(*retro, kCross);
    Shot(*retro, "retro-settings");

    // the user page
    auto user = home_with(3);
    Press(*user, kUp);
    Shot(*user, "user-home");
    Press(*user, kCross);
    Shot(*user, "user-page");

    // the loading screen, in each theme
    for (int theme = 0; theme < 3; theme++)
    {
        auto loading = home_with(theme);
        for (int i = 0; i < 2; i++)
            Press(*loading, kRight);
        loading->Update(kCross, g_now);
        LoadingScreen screen = loading->Loading();
        screen.lines = {"[    2.114] launch: kernel", "[    2.361] nv2a: xemu's (PGRAPH on its Vulkan renderer)",
                        "[    2.902] launch: loading /data/xbox-core/games/Moonlit Manor/default.xbe"};
        Canvas canvas(1920, 1080, 1.5f);
        DrawLoadingScreen(canvas, screen, 1, 0, 0, 1.0);
        Save(canvas, std::string("loading-") + (theme == 0 ? "black" : theme == 1 ? "white" : "retro"));
        if (theme == 0)
        {
            DrawLoadingScreen(canvas, screen, 3, 0, 0, 20.0, true);
            Save(canvas, "loading-failed");
        }
    }
    return 0;
}
