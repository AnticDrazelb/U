// The PS4 front end's HOME menu (frontend/home.h) on the desktop: each screen drawn to a PNG
// (1920x1080, as on the PS4), from a library folder made by tools/ui-preview.sh.
//   ui-preview <library folder> <output folder>
// The library folder: games/ (the player's), samples/, covers/, usb0/ (a USB drive), history.txt,
// picture-1.png (the last picture shown).

#include <cstdio>
#include <ctime>
#include <memory>
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
    for (int i = 0; i < 40; i++) // a few frames, so that scrolling and growing settle
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
    static fe::Settings keep[4]; // (each Home keeps a reference)
    const auto home_with = [&](int index, int theme) {
        keep[index].theme = theme;
        fe::HomePaths paths;
        paths.library = {{lib + "/samples"}, lib + "/games", {}, lib + "/covers", lib + "/history.txt"};
        paths.usb = {lib + "/usb0"};
        paths.settings = lib + "/settings.txt";
        paths.autostart = lib + "/autostart.txt";
        paths.picture = lib + "/picture-1.png";
        paths.hidden = lib + "/hidden.txt";
        paths.version = "00.01";
        paths.system = "PS4 firmware 13.52, its code clear of the Xbox's addresses";
        paths.write_logs = [lib](const std::vector<fe::Game>&) { return std::vector<std::string>{lib + "/usb0/xbox-core-logs.txt"}; };
        return std::make_unique<fe::Home>(keep[index], paths, "Jordan");
    };

    auto home = home_with(0, 0);
    Shot(*home, "games");
    Press(*home, kRight);
    Shot(*home, "games-played"); // Crystal Caverns: played, its last picture
    for (int i = 0; i < 3; i++)
        Press(*home, kRight);
    Shot(*home, "games-scrolled");
    for (int i = 0; i < 12; i++)
        Press(*home, kRight);
    Shot(*home, "games-all");
    Press(*home, kUp);
    Shot(*home, "tabs");
    Press(*home, kRight);
    Press(*home, kDown);
    Press(*home, kRight);
    Press(*home, kDown);
    Shot(*home, "library");
    Press(*home, kR1);
    Shot(*home, "usb");
    Press(*home, kR1);
    Shot(*home, "settings");
    Press(*home, kDown);
    Press(*home, kRight);
    Shot(*home, "settings-graphics");
    Press(*home, kLeft);
    for (int i = 0; i < 3; i++)
        Press(*home, kDown);
    Shot(*home, "settings-user");
    Press(*home, kDown);
    Shot(*home, "settings-controls");
    Press(*home, kDown);
    Shot(*home, "settings-news");
    Press(*home, kDown);
    Shot(*home, "settings-about");
    for (int i = 0; i < 4; i++)
        Press(*home, kUp);
    Press(*home, kRight);
    Press(*home, kDown);
    Press(*home, kCross);
    Shot(*home, "dialog-close");
    Press(*home, kCircle);
    Press(*home, kCircle);
    Press(*home, kCircle); // back to Games
    for (int i = 0; i < 16; i++)
        Press(*home, kLeft);
    Press(*home, kOptions);
    Shot(*home, "dialog-info");
    Press(*home, kCircle);
    for (int i = 0; i < 5; i++) // Orbital Golf, on the USB drive
        Press(*home, kRight);
    Shot(*home, "games-usb");
    Press(*home, kOptions);
    Shot(*home, "dialog-info-usb");
    // its copy to the PS4: the message's progress, then its result, and the library read again
    Press(*home, kLeft);
    Press(*home, kLeft); // (past Delete)
    Press(*home, kCross);
    bool copied = false;
    for (int i = 0; i < 200 && !copied; i++)
    {
        Press(*home, 0);
        for (const fe::Game& g : home->Games())
            copied |= g.title == "Orbital Golf" && g.usb.empty();
        struct timespec ts{0, 20 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    Shot(*home, "copy-done");
    std::printf("copy to the PS4: %s (%zu titles)\n", copied ? "done" : "NOT DONE", home->Games().size());

    // OPTIONS on a game on the PS4: Delete, asked first; then a sample taken off the list, and back
    const auto has = [&](const char* title) {
        for (const fe::Game& g : home->Games())
            if (g.title == title)
                return true;
        return false;
    };
    Press(*home, kCross); // (the copy's message closed)
    for (int i = 0; i < 16; i++)
        Press(*home, kLeft);
    Press(*home, kRight);
    Press(*home, kRight); // Harbor Kart Rally
    Press(*home, kOptions);
    Shot(*home, "dialog-options-delete");
    Press(*home, kLeft); // Delete
    Press(*home, kCross);
    Shot(*home, "dialog-confirm-delete");
    Press(*home, kRight);
    Press(*home, kCross);
    for (int i = 0; i < 100 && has("Harbor Kart Rally"); i++)
    {
        Press(*home, 0);
        struct timespec ts{0, 20 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    Shot(*home, "deleted");
    std::printf("delete: %s\n", has("Harbor Kart Rally") ? "NOT DONE" : "done");
    Press(*home, kCross);
    for (int i = 0; i < 6; i++) // a sample (mesh)
        Press(*home, kRight);
    Press(*home, kOptions);
    Press(*home, kLeft);
    Press(*home, kCross); // Remove, asked first
    Shot(*home, "dialog-confirm-remove");
    Press(*home, kRight);
    Press(*home, kCross);
    std::printf("hide: %s\n", has("mesh") ? "NOT DONE" : "done");
    Press(*home, kR1);
    Press(*home, kR1);
    Press(*home, kR1); // Settings, its System category
    for (int i = 0; i < 8; i++)
        Press(*home, kUp);
    Press(*home, kDown);
    for (int i = 0; i < 3; i++)
        Press(*home, kDown);
    Press(*home, kRight);
    Press(*home, kDown);
    Shot(*home, "settings-hidden");
    Press(*home, kCross);
    std::printf("show hidden: %s\n", has("mesh") ? "done" : "NOT DONE");

    auto light = home_with(1, 1);
    Press(*light, kRight);
    Shot(*light, "light-games");
    Press(*light, kR1);
    Shot(*light, "light-library");
    Press(*light, kL1);
    Press(*light, kL1);
    Press(*light, kDown);
    Press(*light, kDown);
    Press(*light, kDown);
    Press(*light, kDown);
    Press(*light, kRight);
    Press(*light, kDown);
    Press(*light, kCross);
    Press(*light, kCross);
    Shot(*light, "light-icon-shade");
    Press(*light, kCircle);
    Press(*light, kCircle);
    Press(*light, kUp);
    Press(*light, kCross);
    Shot(*light, "light-keyboard");

    // the loading screen, in each theme
    for (int theme = 0; theme < 2; theme++)
    {
        auto loading = home_with(2 + theme, theme);
        Press(*loading, kRight);
        loading->Update(kCross, g_now);
        LoadingScreen screen = loading->Loading();
        screen.lines = {"[    2.114] launch: kernel", "[    2.361] nv2a: xemu's (PGRAPH on its Vulkan renderer)",
                        "[    2.902] launch: loading /data/xbox-core/games/Crystal Caverns/default.xbe"};
        Canvas canvas(1920, 1080, 1.5f);
        DrawLoadingScreen(canvas, screen, 1, 0, 0, 1.0);
        Save(canvas, std::string("loading-") + (theme == 0 ? "dark" : "light"));
        if (theme == 1)
        {
            loading->Update(0, g_now += 0.05);
            for (int i = 0; i < 2; i++)
                Press(*loading, kRight); // a game without a cover: its tile in grey
            loading->Update(kCross, g_now);
            DrawLoadingScreen(canvas, loading->Loading(), 3, 0, 0, 20.0, true);
            Save(canvas, "loading-failed");
        }
    }
    return 0;
}
