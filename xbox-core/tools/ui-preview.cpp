// Renders the PS4 front end's screen to PNG files on the desktop, to look at it without a console.
//   ui-preview <assets dir> <games dir> <out prefix> [presses...]
// Each press (up down left right x o s t l1 r1, or "wait": a second passes) is applied in turn and a
// picture written after it: <prefix>0.png before any, <prefix>1.png after the first, ... <games>-usb
// stands in for a USB drive.
#include <chrono>
#include <cstdio>
#include <thread>
#include <string>
#include <vector>

#include "../frontend/ui.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../frontend/third_party/stb/stb_image_write.h"

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: ui-preview <assets> <games> <out prefix> [presses...]\n");
        return 1;
    }
    const std::string assets = argv[1], games = argv[2], prefix = argv[3];
    fe::LibraryPaths paths{{games + "/samples"}, games, {games + "-usb"}, games + "/covers", games + "/history.txt"};
    fe::Settings settings;
    settings.Load(games + "/settings.txt");
    fe::UiPaths ui_paths{assets, games + "/settings.txt", games + "/autostart.txt",
                         {"Games: /data/xbox-core/games/<name>/default.xbe, or <name>.iso",
                          "Covers: cover.jpg beside a game, or /data/xbox-core/covers/<title ID>.jpg"},
                         paths,
                         {games + "-usb"},
                         {"xbox-core 0.1 (desktop preview)"},
                         {{games + "/log.txt", 50}},
                         games + "/diagnostics.txt",
                         games};
    fe::LibraryUi ui(fe::ScanLibrary(paths), settings, ui_paths, "0.1");
    if (!ui.Ready())
    {
        std::fprintf(stderr, "fonts missing in %s\n", assets.c_str());
        return 1;
    }
    std::vector<uint32_t> frame(size_t(fe::LibraryUi::kWidth) * fe::LibraryUi::kHeight);
    fe::Canvas canvas(frame.data(), fe::LibraryUi::kWidth, fe::LibraryUi::kHeight, fe::LibraryUi::kWidth);
    auto write = [&](int n) {
        ui.Draw(canvas, 0.5 + n, "21:47", "Player");
        const std::string file = prefix + std::to_string(n) + ".png";
        stbi_write_png(file.c_str(), fe::LibraryUi::kWidth, fe::LibraryUi::kHeight, 4, frame.data(), fe::LibraryUi::kWidth * 4);
    };
    write(0);
    for (int i = 4; i < argc; i++)
    {
        const std::string p = argv[i];
        fe::Input in;
        in.up = p == "up", in.down = p == "down", in.left = p == "left", in.right = p == "right";
        in.cross = p == "x", in.circle = p == "o", in.square = p == "s", in.triangle = p == "t";
        in.l1 = p == "l1", in.r1 = p == "r1";
        if (p == "wait")
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (const fe::Game* g = ui.Update(in))
            std::printf("play: %s\n", g->path.c_str());
        write(i - 3);
    }
    return 0;
}
