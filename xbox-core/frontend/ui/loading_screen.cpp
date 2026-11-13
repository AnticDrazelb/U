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
                       bool failed)
{
    // the HOME menu's Basic Black and Basic White colours, or its Retro ones and pixel font
    const bool white = screen.white_theme && !screen.retro_theme, retro = screen.retro_theme;
    const uint32_t bg = retro ? Rgb(0x2B2F66) : white ? Rgb(0xEBEBEB) : Rgb(0x2D2D2D);
    const uint32_t text = retro ? Rgb(0xF6F2E4) : white ? Rgb(0x2D2D2D) : Rgb(0xFFFFFF);
    const uint32_t dim = retro ? Rgb(0xAEB0DA) : white ? Rgb(0x707070) : Rgb(0xB0B0B0);
    const uint32_t track = retro ? Rgb(0x565DA8) : white ? Rgb(0xC6C6C6) : Rgb(0x585858);
    const uint32_t accent = retro ? Rgb(0xFFD447) : white ? Rgb(0x3254E6) : Rgb(0x00C8F0);
    const uint32_t tile = retro ? Rgb(0x474E99) : white ? Rgb(0xFFFFFF) : Rgb(0x464646);

    c.SetFontFamily(retro ? FontFamily::Pixel : FontFamily::Inter);
    if (retro)
        c.Gradient(0, 0, 1280, 720, Rgb(0x31377C), Rgb(0x222659));
    else
        c.Clear(bg);
    // the game's icon, as its HOME menu tile
    constexpr float kIcon = 256, kIconX = (1280 - kIcon) / 2, kIconY = 132;
    if (retro)
    {
        // the Retro theme's chunky tile: a hard shadow, a dark outline, a cream frame
        const uint32_t ink = Rgb(0x15173B);
        c.RoundRect(kIconX + 6, kIconY + 7, kIcon, kIcon, 18, Fade(ink, 0.8f));
        c.RoundRect(kIconX, kIconY, kIcon, kIcon, 18, ink);
        if (!screen.icon.Empty())
            c.DrawImage(kIconX + 9, kIconY + 9, kIcon - 18, kIcon - 18, screen.icon);
        else
        {
            c.Rect(kIconX + 9, kIconY + 9, kIcon - 18, kIcon - 18, tile);
            c.Text(640, kIconY + kIcon / 2 + 22, screen.name.empty() ? "?" : screen.name.substr(0, 1), 64,
                   Weight::SemiBold, dim, Align::Center);
        }
        c.RoundRectOutline(kIconX + 4, kIconY + 4, kIcon - 8, kIcon - 8, 14, 6, Rgb(0xEDEAF8));
    }
    else if (!screen.icon.Empty())
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
    if (failed)
    {
        // it won't get further: say so, and how to leave
        const uint32_t red = white ? Rgb(0xE60012) : Rgb(0xF0483E);
        c.Text(640, 574, "The game stopped with an error", 21, Weight::SemiBold, red, Align::Center);
        c.RoundRect(kBarX, kBarY, kBarW, kBarH, kBarH / 2, red);
        c.Text(640, 640, "The log says why. To leave: the PS button, then Close Application", 18, Weight::Regular, text,
               Align::Center);
        return;
    }
    // what it's doing, and a bar: filling while the shaders build, else a segment going across
    std::string what = stage == 2 ? "Building shaders" : stage == 3 ? "Starting the game" : "Loading the game";
    if (stage == 2 && total)
        what += "  " + Thousands(std::min(done, total)) + " / " + Thousands(total);
    c.Text(640, 574, what, 20, Weight::Regular, dim, Align::Center);
    // some games take this long; most that do aren't working
    if (now > 180 && stage != 2)
        c.Text(640, 640, "This is taking longer than usual. If nothing changes, the log says where it is.", 17,
               Weight::Regular, dim, Align::Center);
    // what the emulator is doing, in a few small lines under it all
    float line_y = 676;
    for (const std::string& line : screen.lines)
    {
        c.Text(640, line_y, line, 11.5f, Weight::Regular, dim, Align::Center, 1240);
        line_y += 14;
    }
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
