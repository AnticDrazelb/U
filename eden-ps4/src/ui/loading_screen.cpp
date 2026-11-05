#include "ui/loading_screen.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace Ui {

namespace {

// 1234567 -> "1,234,567"
std::string Thousands(unsigned value)
{
    std::string digits = std::to_string(value), out;
    for (size_t i = 0; i < digits.size(); i++)
    {
        if (i && (digits.size() - i) % 3 == 0)
            out += ',';
        out += digits[i];
    }
    return out;
}

} // namespace

void DrawLoadingScreen(Canvas& c, const LoadingScreen& screen, int stage, unsigned done, unsigned total, double now,
                       bool failed, bool retrying)
{
    // the HOME menu's Basic Black and Basic White colours
    const bool white = screen.white_theme;
    const uint32_t bg = white ? Rgb(0xEBEBEB) : Rgb(0x2D2D2D);
    const uint32_t text = white ? Rgb(0x2D2D2D) : Rgb(0xFFFFFF);
    const uint32_t dim = white ? Rgb(0x707070) : Rgb(0xB0B0B0);
    const uint32_t track = white ? Rgb(0xC6C6C6) : Rgb(0x585858);
    const uint32_t accent = white ? Rgb(0x3254E6) : Rgb(0x00C8F0);
    const uint32_t tile = white ? Rgb(0xFFFFFF) : Rgb(0x464646);

    c.Clear(bg);
    // the game's icon, as its HOME menu tile
    constexpr float kIcon = 256, kIconX = (1280 - kIcon) / 2, kIconY = 132;
    if (!screen.icon.Empty())
    {
        c.DrawImage(kIconX, kIconY, kIcon, kIcon, screen.icon);
    }
    else
    {
        c.RoundRect(kIconX, kIconY, kIcon, kIcon, 12, tile);
        c.Text(640, kIconY + kIcon / 2 + 22, screen.name.empty() ? "?" : screen.name.substr(0, 1), 64,
               Weight::SemiBold, dim, Align::Center);
    }
    c.Text(640, 448, screen.name, 30, Weight::SemiBold, text, Align::Center, 1000);
    if (!screen.publisher.empty())
        c.Text(640, 482, screen.publisher, 19, Weight::Regular, dim, Align::Center, 1000);

    constexpr float kBarW = 420, kBarH = 6, kBarX = (1280 - kBarW) / 2, kBarY = 598;
    if (failed && retrying)
    {
        // (04.71) the boot watcher takes it from here
        c.Text(640, 574, "The game stopped with an error", 21, Weight::SemiBold, text, Align::Center);
        c.RoundRect(kBarX, kBarY, kBarW, kBarH, kBarH / 2, track);
        c.Text(640, 640, "Starting it again with safer settings...", 18, Weight::Regular, dim, Align::Center);
        return;
    }
    if (failed)
    {
        // (04.69) it won't get further: say so, and how to leave
        const uint32_t red = white ? Rgb(0xE60012) : Rgb(0xF0483E);
        c.Text(640, 574, "The game stopped with an error", 21, Weight::SemiBold, red, Align::Center);
        c.RoundRect(kBarX, kBarY, kBarW, kBarH, kBarH / 2, red);
        c.Text(640, 640, "Hold L3 + R3 to return to the HOME menu", 18, Weight::Regular, text, Align::Center);
        return;
    }
    // what it's doing, and a bar: filling while the shaders build, else a segment going across
    std::string what = stage == 2 ? "Building shaders" : stage == 3 ? "Starting the game" : "Loading the game";
    if (stage == 2 && total)
        what += "  " + Thousands(std::min(done, total)) + " / " + Thousands(total);
    c.Text(640, 574, what, 20, Weight::Regular, dim, Align::Center);
    // (04.69) some games take this long; most that do aren't working
    if (now > 180 && stage != 2)
        c.Text(640, 640, "This is taking longer than usual. If nothing changes, hold L3 + R3 to stop it.", 17,
               Weight::Regular, dim, Align::Center);
    c.RoundRect(kBarX, kBarY, kBarW, kBarH, kBarH / 2, track);
    if (stage == 2 && total)
    {
        const float part = std::clamp(float(done) / float(total), 0.0f, 1.0f);
        if (part > 0)
            c.RoundRect(kBarX, kBarY, std::max(kBarH, kBarW * part), kBarH, kBarH / 2, accent);
    }
    else
    {
        constexpr float kSegment = 110;
        const float phase = float(std::fmod(now / 1.6, 1.0));
        const float x0 = kBarX - kSegment + (kBarW + kSegment) * phase;
        const float left = std::max(x0, kBarX), right = std::min(x0 + kSegment, kBarX + kBarW);
        if (right - left > kBarH)
            c.RoundRect(left, kBarY, right - left, kBarH, kBarH / 2, accent);
    }
}

} // namespace Ui
