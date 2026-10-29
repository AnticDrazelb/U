// A CPU canvas for the launcher's HOME menu: anti-aliased shapes (each drawn from its signed
// distance at every pixel it may cover), images and text (stb_truetype, the Inter font). Positions
// and sizes are in layout units, the Switch's 1280x720 screen; the canvas is any size in pixels
// (the PS4's 1920x1080: 1.5 pixels a unit). Colours are in the screen's own byte order: R, G, B, A
// in memory, 0xAABBGGRR as a little-endian word (Rgb() makes one from 0xRRGGBB).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Ui {

constexpr uint32_t Rgb(uint32_t rgb, uint32_t alpha = 255)
{
    return alpha << 24 | (rgb & 0xFF) << 16 | (rgb & 0xFF00) | (rgb >> 16 & 0xFF);
}
// `color` with its alpha scaled by `opacity` (0 to 1)
uint32_t Fade(uint32_t color, float opacity);
// a blend of two colours, t from 0 (a) to 1 (b)
uint32_t Mix(uint32_t a, uint32_t b, float t);

struct Image
{
    int width = 0, height = 0;
    std::vector<uint32_t> pixels; // the screen's byte order, straight (not premultiplied) alpha
    bool opaque = true;
    bool Empty() const { return pixels.empty(); }
};
// A JPEG or PNG (a Switch title's icon), or an empty image if it can't be read
Image DecodeImage(const void* data, size_t size);
// Scaled to width x height (an average of the pixels each covers)
Image ResizeImage(const Image& image, int width, int height);

enum class Weight { Regular, SemiBold };
enum class Align { Left, Center, Right };

class Canvas
{
public:
    Canvas(int width, int height, float scale);
    ~Canvas();

    int Width() const { return width; }
    int Height() const { return height; }
    float Scale() const { return scale; }
    const uint32_t* Pixels() const { return pixels.data(); }
    uint32_t* Pixels() { return pixels.data(); }

    // Drawing is limited to this rectangle until ResetClip
    void Clip(float x, float y, float w, float h);
    void ResetClip();

    void Clear(uint32_t color);
    void Rect(float x, float y, float w, float h, uint32_t color);
    void RoundRect(float x, float y, float w, float h, float radius, uint32_t color);
    // A border `thickness` wide just inside the rounded rectangle
    void RoundRectOutline(float x, float y, float w, float h, float radius, float thickness, uint32_t color);
    void Circle(float cx, float cy, float radius, uint32_t color);
    // A ring just inside the circle of this radius
    void Ring(float cx, float cy, float radius, float thickness, uint32_t color);
    // A line `width` wide with round ends
    void Line(float x0, float y0, float x1, float y1, float width, uint32_t color);
    // Part of a ring (angles in radians, clockwise from 3 o'clock on screen), with round ends
    void Arc(float cx, float cy, float radius, float thickness, float from, float to, uint32_t color);
    void Triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color);
    // A vertical gradient from `top` to `bottom`
    void Gradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
    // The image stretched over the rectangle, with this opacity
    void DrawImage(float x, float y, float w, float h, const Image& image, float opacity = 1.0f);

    // Text from its baseline at y; returns its width. With max_width, it is cut short with an
    // ellipsis to fit.
    float Text(float x, float y, std::string_view text, float size, Weight weight, uint32_t color,
               Align align = Align::Left, float max_width = 0);
    float TextWidth(std::string_view text, float size, Weight weight);
    // The text broken into lines no wider than max_width (at spaces, or anywhere in a long word)
    std::vector<std::string> Wrap(std::string_view text, float size, Weight weight, float max_width);
    // How far above and below the baseline the font reaches, at this size
    float Ascent(float size, Weight weight);
    float Descent(float size, Weight weight);

private:
    struct Fonts;
    struct Box
    {
        int x0, y0, x1, y1;
    };
    Box Bounds(float x0, float y0, float x1, float y1) const;
    void Blend(int x, int y, uint32_t color, float coverage);
    template <typename Distance>
    void Shape(float x0, float y0, float x1, float y1, uint32_t color, Distance distance);
    std::string Fit(std::string_view text, float size, Weight weight, float max_width);

    int width, height;
    float scale;
    std::vector<uint32_t> pixels;
    Box clip;
    Fonts* fonts; // shared by every canvas (with their glyphs)
};

} // namespace Ui
