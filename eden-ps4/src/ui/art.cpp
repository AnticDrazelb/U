// The HOME menu's art (art.h).

#include "ui/art.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Ui {

namespace {

constexpr float kPi = 3.14159265f;

// hue in degrees, saturation and value from 0 to 1
uint32_t Hsv(float h, float s, float v)
{
    h = std::fmod(h, 360.0f);
    if (h < 0)
        h += 360;
    const float c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 60)
        r = c, g = x;
    else if (h < 120)
        r = x, g = c;
    else if (h < 180)
        g = c, b = x;
    else if (h < 240)
        g = x, b = c;
    else if (h < 300)
        r = x, b = c;
    else
        r = c, b = x;
    const auto byte = [&](float f) { return uint32_t(std::lround((f + m) * 255)); };
    return Rgb(byte(r) << 16 | byte(g) << 8 | byte(b));
}

uint64_t Hash(std::string_view text)
{
    uint64_t h = 1469598103934665603ull;
    for (const char c : text)
        h = (h ^ uint8_t(c)) * 1099511628211ull;
    return h;
}

Image Finish(Canvas& canvas)
{
    Image image;
    image.width = canvas.Width();
    image.height = canvas.Height();
    image.pixels.assign(canvas.Pixels(), canvas.Pixels() + size_t(image.width) * image.height);
    image.opaque = true;
    return image;
}

void Brick(Canvas& c, float x, float y, float w, float h, uint32_t color)
{
    c.RoundRect(x, y, w, h, 3, color);
    c.Rect(x + 3, y + 2, w - 6, 3, Fade(Rgb(0xFFFFFF), 0.35f));
}

void Cell(Canvas& c, float x, float y, float s, uint32_t color)
{
    c.RoundRect(x + 1, y + 1, s - 2, s - 2, 3, color);
    c.RoundRect(x + 5, y + 5, s - 10, s - 10, 2, Mix(color, Rgb(0xFFFFFF), 0.25f));
}

} // namespace

Image MakeTileArt(Art art, int size)
{
    Canvas c(size, size, size / 256.0f);
    switch (art)
    {
    case Art::Breakout: {
        c.Gradient(0, 0, 256, 256, Rgb(0x1C2A6B), Rgb(0x0A1030));
        const uint32_t rows[] = {Rgb(0xF0453A), Rgb(0xF59A23), Rgb(0xF5D623), Rgb(0x3FCB5A), Rgb(0x2FA4F0)};
        for (int r = 0; r < 5; r++)
            for (int i = 0; i < 6; i++)
                Brick(c, 14 + i * 38.5f, 34 + r * 19.0f, 34, 14, rows[r]);
        c.Circle(152, 172, 8, Rgb(0xFFFFFF));
        c.Line(126, 150, 146, 166, 3, Fade(Rgb(0xFFFFFF), 0.25f));
        c.RoundRect(96, 214, 72, 12, 6, Rgb(0xE8ECF5));
        break;
    }
    case Art::Blocks: {
        c.Gradient(0, 0, 256, 256, Rgb(0x3B1670), Rgb(0x160830));
        const float s = 28;
        const uint32_t cyan = Rgb(0x2FD0E8), yellow = Rgb(0xF5CF2A), red = Rgb(0xEC4C4C), green = Rgb(0x48C95A),
                       blue = Rgb(0x3F6CF0), orange = Rgb(0xF59134), purple = Rgb(0xB257E8);
        const struct { int x, y; uint32_t color; } cells[] = {
            {0, 7, blue},  {1, 7, blue},  {2, 7, blue},  {2, 6, blue},  {3, 7, yellow}, {4, 7, yellow}, {3, 6, yellow},
            {4, 6, yellow}, {5, 7, red},  {6, 7, red},   {6, 6, red},   {7, 6, red},    {0, 6, green},  {0, 5, green},
            {1, 6, green}, {1, 5, orange}, {1, 4, orange}, {2, 5, orange}, {7, 7, cyan}, {7, 5, cyan},  {7, 4, cyan},
            {5, 6, purple}, {5, 5, purple}, {4, 5, purple}, {6, 5, purple},
        };
        for (const auto& cell : cells)
            Cell(c, 16 + cell.x * s, 16 + cell.y * s, s, cell.color);
        // the falling piece
        Cell(c, 16 + 3 * s, 16 + 1 * s, s, purple);
        Cell(c, 16 + 2 * s, 16 + 2 * s, s, purple);
        Cell(c, 16 + 3 * s, 16 + 2 * s, s, purple);
        Cell(c, 16 + 4 * s, 16 + 2 * s, s, purple);
        break;
    }
    case Art::Controller:
        c.Gradient(0, 0, 256, 256, Rgb(0x13B5B0), Rgb(0x07656E));
        DrawController(c, 128, 136, 184, Rgb(0xF4F7F8), Rgb(0x0E8C8C));
        break;
    case Art::Gpu: {
        c.Gradient(0, 0, 256, 256, Rgb(0x1A1F33), Rgb(0x0A0C16));
        c.Circle(84, 92, 46, Fade(Rgb(0x2FD0E8), 0.85f));
        c.Circle(170, 92, 46, Fade(Rgb(0xE84FD0), 0.85f));
        c.Circle(127, 164, 46, Fade(Rgb(0xF5CF2A), 0.85f));
        c.Triangle(128, 54, 200, 196, 56, 196, Fade(Rgb(0xFFFFFF), 0.35f));
        c.RoundRectOutline(24, 24, 208, 208, 10, 3, Fade(Rgb(0xFFFFFF), 0.4f));
        break;
    }
    case Art::Terminal: {
        c.Clear(Rgb(0x101214));
        c.Rect(0, 0, 256, 34, Rgb(0x2A2E33));
        c.Circle(22, 17, 6, Rgb(0xF0605A));
        c.Circle(42, 17, 6, Rgb(0xF5BF3A));
        c.Circle(62, 17, 6, Rgb(0x52C84F));
        const uint32_t green = Rgb(0x52E07A), dim = Rgb(0x2F8F4C);
        for (int i = 0; i < 5; i++)
            c.RoundRect(22, 56 + i * 22, 70 + float((i * 53) % 110), 9, 4, i == 4 ? green : dim);
        c.Text(22, 222, ">_", 54, Weight::SemiBold, green);
        break;
    }
    case Art::None:
        c.Clear(Rgb(0x505050));
        break;
    }
    return Finish(c);
}

Image MakeCover(std::string_view name, std::string_view kind, int size)
{
    Canvas c(size, size, size / 256.0f);
    const uint64_t h = Hash(name);
    const float hue = float(h % 360);
    c.Gradient(0, 0, 256, 256, Hsv(hue, 0.55f, 0.62f), Hsv(hue + 35, 0.65f, 0.30f));
    c.Circle(210, 40, 110, Fade(Rgb(0xFFFFFF), 0.07f));
    c.Circle(30, 250, 90, Fade(Rgb(0x000000), 0.10f));
    std::vector<std::string> lines = c.Wrap(name, 27, Weight::SemiBold, 220);
    if (lines.size() > 4)
    {
        lines.resize(4);
        lines.back() += "\xE2\x80\xA6";
    }
    const float line_height = 33;
    float y = 118 - (float(lines.size()) - 1) * line_height / 2 + 10;
    for (const std::string& line : lines)
    {
        c.Text(128, y, line, 27, Weight::SemiBold, Rgb(0xFFFFFF), Align::Center, 228);
        y += line_height;
    }
    if (!kind.empty())
    {
        const float w = c.TextWidth(kind, 14, Weight::SemiBold) + 18;
        c.RoundRect(14, 220, w, 22, 11, Fade(Rgb(0x000000), 0.35f));
        c.Text(14 + w / 2, 236, kind, 14, Weight::SemiBold, Rgb(0xFFFFFF), Align::Center);
    }
    return Finish(c);
}

void DrawIcon(Canvas& c, Icon icon, float cx, float cy, float s, uint32_t color)
{
    switch (icon)
    {
    case Icon::News: {
        const float w = s * 0.82f, h = s * 0.66f, x = cx - w / 2, y = cy - h / 2, t = s * 0.085f;
        c.RoundRectOutline(x, y, w, h, s * 0.08f, t, color);
        c.RoundRect(x + s * 0.12f, y + s * 0.13f, s * 0.24f, s * 0.20f, s * 0.03f, color);
        c.Line(x + s * 0.45f, y + s * 0.17f, x + w - s * 0.13f, y + s * 0.17f, t * 0.8f, color);
        c.Line(x + s * 0.45f, y + s * 0.30f, x + w - s * 0.13f, y + s * 0.30f, t * 0.8f, color);
        c.Line(x + s * 0.12f, y + s * 0.46f, x + w - s * 0.13f, y + s * 0.46f, t * 0.8f, color);
        break;
    }
    case Icon::Games: {
        const float w = s * 0.86f, h = s * 0.62f, x = cx - w / 2, y = cy - h / 2 + s * 0.06f;
        c.RoundRect(x, y - s * 0.12f, w * 0.42f, s * 0.2f, s * 0.06f, color);
        c.RoundRect(x, y, w, h, s * 0.08f, color);
        c.Line(x + s * 0.06f, y + s * 0.12f, x + w - s * 0.06f, y + s * 0.12f, s * 0.04f, Fade(Rgb(0xFFFFFF), 0.5f));
        break;
    }
    case Icon::Controllers:
        DrawController(c, cx, cy + s * 0.04f, s * 0.95f, color, Rgb(0x000000, 0));
        break;
    case Icon::Settings: {
        const float r = s * 0.27f;
        for (int i = 0; i < 8; i++)
        {
            const float a = i * kPi / 4;
            c.Line(cx + std::cos(a) * r * 0.9f, cy + std::sin(a) * r * 0.9f, cx + std::cos(a) * s * 0.42f,
                   cy + std::sin(a) * s * 0.42f, s * 0.15f, color);
        }
        c.Ring(cx, cy, r + s * 0.06f, s * 0.17f, color);
        break;
    }
    case Icon::Power: {
        const float r = s * 0.36f, t = s * 0.1f;
        c.Arc(cx, cy + s * 0.03f, r, t, -kPi / 2 + 0.62f, -kPi / 2 + 2 * kPi - 0.62f, color);
        c.Line(cx, cy - s * 0.42f, cx, cy - s * 0.04f, t, color);
        break;
    }
    case Icon::AllSoftware: {
        const float cell = s * 0.24f, gap = s * 0.1f, start = -(cell * 1.5f + gap);
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 3; x++)
                c.RoundRect(cx + start + x * (cell + gap), cy + start + y * (cell + gap), cell, cell, cell * 0.25f,
                            color);
        break;
    }
    case Icon::User:
        c.Circle(cx, cy - s * 0.12f, s * 0.2f, color);
        c.Clip(cx - s * 0.5f, cy - s * 0.5f, s, s * 0.88f);
        c.Circle(cx, cy + s * 0.42f, s * 0.36f, color);
        c.ResetClip();
        break;
    case Icon::WiFi: {
        const float base = cy + s * 0.32f, t = s * 0.11f;
        for (int i = 0; i < 3; i++)
        {
            const float r = s * (0.24f + 0.2f * i);
            c.Arc(cx, base, r, t, -kPi * 0.75f, -kPi * 0.25f, color);
        }
        c.Circle(cx, base - s * 0.02f, s * 0.08f, color);
        break;
    }
    case Icon::Check:
        c.Line(cx - s * 0.3f, cy, cx - s * 0.08f, cy + s * 0.24f, s * 0.14f, color);
        c.Line(cx - s * 0.08f, cy + s * 0.24f, cx + s * 0.34f, cy - s * 0.24f, s * 0.14f, color);
        break;
    case Icon::Cross:
        c.Line(cx - s * 0.26f, cy - s * 0.26f, cx + s * 0.26f, cy + s * 0.26f, s * 0.14f, color);
        c.Line(cx + s * 0.26f, cy - s * 0.26f, cx - s * 0.26f, cy + s * 0.26f, s * 0.14f, color);
        break;
    }
}

float GlyphWidth(Glyph glyph)
{
    return glyph == Glyph::Options || glyph == Glyph::L1 || glyph == Glyph::R1 ? 30 : 22;
}

void DrawGlyph(Canvas& c, Glyph glyph, float cx, float cy, uint32_t color, uint32_t bg)
{
    const float r = 11;
    switch (glyph)
    {
    case Glyph::Cross:
        c.Circle(cx, cy, r, color);
        c.Line(cx - 4.2f, cy - 4.2f, cx + 4.2f, cy + 4.2f, 2.2f, bg);
        c.Line(cx + 4.2f, cy - 4.2f, cx - 4.2f, cy + 4.2f, 2.2f, bg);
        break;
    case Glyph::Circle:
        c.Circle(cx, cy, r, color);
        c.Ring(cx, cy, 5.6f, 2.1f, bg);
        break;
    case Glyph::Triangle:
        c.Circle(cx, cy, r, color);
        c.Line(cx, cy - 5.2f, cx + 4.8f, cy + 3.4f, 1.9f, bg);
        c.Line(cx + 4.8f, cy + 3.4f, cx - 4.8f, cy + 3.4f, 1.9f, bg);
        c.Line(cx - 4.8f, cy + 3.4f, cx, cy - 5.2f, 1.9f, bg);
        break;
    case Glyph::Square:
        c.Circle(cx, cy, r, color);
        c.RoundRectOutline(cx - 4.6f, cy - 4.6f, 9.2f, 9.2f, 1.2f, 1.9f, bg);
        break;
    case Glyph::Options:
        c.RoundRect(cx - 15, cy - 9, 30, 18, 9, color);
        for (int i = -1; i <= 1; i++)
            c.Line(cx - 6, cy + i * 3.6f, cx + 6, cy + i * 3.6f, 1.6f, bg);
        break;
    case Glyph::L1:
    case Glyph::R1:
        c.RoundRect(cx - 15, cy - 9, 30, 18, 6, color);
        c.Text(cx, cy + 4.5f, glyph == Glyph::L1 ? "L1" : "R1", 12.5f, Weight::SemiBold, bg, Align::Center);
        break;
    }
}

void DrawController(Canvas& c, float cx, float cy, float w, uint32_t color, uint32_t bg)
{
    // body and grips
    c.RoundRect(cx - w * 0.44f, cy - w * 0.24f, w * 0.88f, w * 0.36f, w * 0.14f, color);
    c.Circle(cx - w * 0.31f, cy + w * 0.1f, w * 0.17f, color);
    c.Circle(cx + w * 0.31f, cy + w * 0.1f, w * 0.17f, color);
    c.Line(cx - w * 0.31f, cy + w * 0.1f, cx - w * 0.36f, cy + w * 0.2f, w * 0.3f, color);
    c.Line(cx + w * 0.31f, cy + w * 0.1f, cx + w * 0.36f, cy + w * 0.2f, w * 0.3f, color);
    if ((bg >> 24) == 0)
        return; // small: the silhouette only
    // touchpad, D-pad, face buttons and sticks, in the background colour
    c.RoundRect(cx - w * 0.15f, cy - w * 0.21f, w * 0.3f, w * 0.15f, w * 0.03f, bg);
    const float dx = cx - w * 0.28f, dy = cy - w * 0.08f, arm = w * 0.055f;
    c.Line(dx - arm, dy, dx + arm, dy, w * 0.04f, bg);
    c.Line(dx, dy - arm, dx, dy + arm, w * 0.04f, bg);
    const float fx = cx + w * 0.28f, fy = cy - w * 0.08f, step = w * 0.06f, fr = w * 0.026f;
    c.Circle(fx, fy - step, fr, bg);
    c.Circle(fx, fy + step, fr, bg);
    c.Circle(fx - step, fy, fr, bg);
    c.Circle(fx + step, fy, fr, bg);
    c.Circle(cx - w * 0.12f, cy + w * 0.06f, w * 0.055f, bg);
    c.Circle(cx + w * 0.12f, cy + w * 0.06f, w * 0.055f, bg);
}

} // namespace Ui
