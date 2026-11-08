// Software drawing for the front end: an RGBA pixel buffer (bytes R, G, B, A, as the PS4's
// A8B8G8R8 scan-out and PNG files hold them), filled rectangles and rounded panels with alpha,
// gradients, anti-aliased text (stb_truetype, glyphs cached) and pictures (stb_image).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fe {

struct Color
{
    uint8_t r = 0, g = 0, b = 0, a = 255;
};
// 0xRRGGBB with an alpha
inline Color Rgb(uint32_t rgb, uint8_t a = 255) { return {uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb), a}; }
Color Mix(Color a, Color b, float t);

// A picture in memory, RGBA bytes.
struct Image
{
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    bool Empty() const { return rgba.empty(); }
};
Image LoadImage(const std::string& path);
// Scaled to cover w x h (cropping what overflows), as a cover card shows it.
Image CoverScaled(const Image& source, int w, int h);

class Canvas
{
public:
    Canvas(uint32_t* pixels, int width, int height, int stride) : px(pixels), w(width), h(height), stride(stride) {}
    int Width() const { return w; }
    int Height() const { return h; }

    void Clear(Color c);
    void Fill(int x, int y, int fw, int fh, Color c);
    // corner radius r; with an outline `thickness` > 0 only the border
    void RoundRect(int x, int y, int rw, int rh, int r, Color c, int thickness = 0);
    void VerticalGradient(int x, int y, int gw, int gh, Color top, Color bottom);
    void Circle(float cx, float cy, float r, Color c, float thickness = 0);
    void Line(float x0, float y0, float x1, float y1, float width, Color c);
    // a picture at (x, y), its corners rounded by r, faded by alpha
    void Draw(const Image& image, int x, int y, int r = 0, uint8_t alpha = 255);
    void CopyFrom(const Canvas& other);

    // one pixel, blended by coverage (0..1)
    void Blend(int x, int y, Color c, float coverage);

private:
    uint32_t* px;
    int w, h, stride;
};

class Font
{
public:
    bool Load(const std::string& path);
    bool Loaded() const { return !data.empty(); }
    // text in UTF-8 at (x, y) (its top), `size` pixels high; returns its width
    int Draw(Canvas& c, int x, int y, const std::string& text, float size, Color color);
    int Measure(const std::string& text, float size);
    // lines no wider than `width`, at most `max_lines` (the last ending in "..." when cut)
    std::vector<std::string> Wrap(const std::string& text, float size, int width, int max_lines);
    // the text cut to `width`, with "..." when it doesn't fit
    std::string Fit(const std::string& text, float size, int width);

private:
    struct Glyph
    {
        int x0, y0, w, h, advance;
        std::vector<uint8_t> alpha;
    };
    const Glyph& Get(uint32_t codepoint, float size);
    std::vector<uint8_t> data;
    std::shared_ptr<void> info; // stbtt_fontinfo
    std::map<std::pair<uint32_t, int>, Glyph> cache;
};

std::vector<uint32_t> DecodeUtf8(const std::string& s);

} // namespace fe
