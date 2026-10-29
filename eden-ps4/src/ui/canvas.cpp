// The HOME menu's canvas (canvas.h).

#include "ui/canvas.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "ui/stb/stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "ui/stb/stb_image.h"

#include "ui/fonts.h"

namespace Ui {

namespace {

inline float Clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

inline uint32_t Channel(uint32_t color, int shift)
{
    return color >> shift & 0xFF;
}

// The next code point of UTF-8 text (a malformed byte stands for itself)
uint32_t NextCodePoint(std::string_view text, size_t& i)
{
    const auto byte = [&](size_t k) { return uint32_t(uint8_t(text[k])); };
    const uint32_t c = byte(i++);
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra == 0 || i + extra > text.size())
        return c;
    uint32_t cp = c & (0x3F >> extra);
    for (; extra > 0; extra--)
        cp = cp << 6 | (byte(i++) & 0x3F);
    return cp;
}

} // namespace

uint32_t Fade(uint32_t color, float opacity)
{
    const uint32_t alpha = uint32_t(Channel(color, 24) * Clamp01(opacity) + 0.5f);
    return (color & 0xFFFFFF) | alpha << 24;
}

uint32_t Mix(uint32_t a, uint32_t b, float t)
{
    t = Clamp01(t);
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        const float v = float(Channel(a, shift)) * (1 - t) + float(Channel(b, shift)) * t;
        out |= uint32_t(v + 0.5f) << shift;
    }
    return out;
}

Image DecodeImage(const void* data, size_t size)
{
    Image image;
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(data), int(size), &w, &h, &channels, 4);
    if (!pixels)
        return image;
    image.width = w;
    image.height = h;
    image.pixels.resize(size_t(w) * h);
    std::memcpy(image.pixels.data(), pixels, image.pixels.size() * 4); // R, G, B, A: the screen's order
    image.opaque = channels != 4 && channels != 2;
    stbi_image_free(pixels);
    return image;
}

Image ResizeImage(const Image& image, int width, int height)
{
    Image out;
    if (image.Empty() || width <= 0 || height <= 0)
        return out;
    out.width = width;
    out.height = height;
    out.opaque = image.opaque;
    out.pixels.resize(size_t(width) * height);
    // each output pixel: the average of the source rectangle it covers (at least one pixel)
    for (int y = 0; y < height; y++)
    {
        const int sy0 = y * image.height / height;
        const int sy1 = std::max(sy0 + 1, (y + 1) * image.height / height);
        for (int x = 0; x < width; x++)
        {
            const int sx0 = x * image.width / width;
            const int sx1 = std::max(sx0 + 1, (x + 1) * image.width / width);
            uint32_t sum[4] = {};
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++)
                {
                    const uint32_t p = image.pixels[size_t(sy) * image.width + sx];
                    for (int c = 0; c < 4; c++)
                        sum[c] += Channel(p, c * 8);
                }
            const uint32_t n = uint32_t((sy1 - sy0) * (sx1 - sx0));
            uint32_t p = 0;
            for (int c = 0; c < 4; c++)
                p |= (sum[c] + n / 2) / n << (c * 8);
            out.pixels[size_t(y) * width + x] = p;
        }
    }
    return out;
}

// The two weights of Inter, and every glyph drawn so far (by weight, pixel size and code point)
struct Canvas::Fonts
{
    struct Glyph
    {
        int width = 0, height = 0, left = 0, top = 0;
        float advance = 0;
        std::vector<uint8_t> coverage;
    };
    stbtt_fontinfo info[2];
    bool ok[2] = {};
    std::unordered_map<uint64_t, Glyph> glyphs;

    Fonts()
    {
        for (int w = 0; w < 2; w++)
        {
            const FontData data = UiFont(Weight(w));
            ok[w] = data.data && stbtt_InitFont(&info[w], data.data, stbtt_GetFontOffsetForIndex(data.data, 0));
        }
    }
    float ScaleFor(Weight weight, float pixels)
    {
        return stbtt_ScaleForMappingEmToPixels(&info[int(weight)], pixels);
    }
    const Glyph& Get(Weight weight, float pixels, uint32_t cp)
    {
        const uint32_t size_key = uint32_t(pixels * 4 + 0.5f);
        const uint64_t key = uint64_t(weight) << 56 | uint64_t(size_key) << 32 | cp;
        auto it = glyphs.find(key);
        if (it != glyphs.end())
            return it->second;
        Glyph glyph;
        const int w = int(weight);
        if (ok[w])
        {
            const float scale = ScaleFor(weight, size_key / 4.0f);
            int advance = 0, bearing = 0;
            stbtt_GetCodepointHMetrics(&info[w], int(cp), &advance, &bearing);
            glyph.advance = advance * scale;
            unsigned char* bitmap =
                stbtt_GetCodepointBitmap(&info[w], 0, scale, int(cp), &glyph.width, &glyph.height, &glyph.left, &glyph.top);
            if (bitmap)
            {
                glyph.coverage.assign(bitmap, bitmap + size_t(glyph.width) * glyph.height);
                stbtt_FreeBitmap(bitmap, nullptr);
            }
        }
        return glyphs.emplace(key, std::move(glyph)).first->second;
    }
};

Canvas::Canvas(int width_, int height_, float scale_)
    : width{width_}, height{height_}, scale{scale_}, pixels(size_t(width_) * height_), clip{0, 0, width_, height_}
{
    static Fonts shared;
    fonts = &shared;
}

Canvas::~Canvas() = default;

void Canvas::Clip(float x, float y, float w, float h)
{
    clip = {std::max(0, int(std::floor(x * scale))), std::max(0, int(std::floor(y * scale))),
            std::min(width, int(std::ceil((x + w) * scale))), std::min(height, int(std::ceil((y + h) * scale)))};
}

void Canvas::ResetClip()
{
    clip = {0, 0, width, height};
}

Canvas::Box Canvas::Bounds(float x0, float y0, float x1, float y1) const
{
    return {std::max(clip.x0, int(std::floor(x0 * scale)) - 1), std::max(clip.y0, int(std::floor(y0 * scale)) - 1),
            std::min(clip.x1, int(std::ceil(x1 * scale)) + 1), std::min(clip.y1, int(std::ceil(y1 * scale)) + 1)};
}

inline void Canvas::Blend(int x, int y, uint32_t color, float coverage)
{
    const uint32_t a = uint32_t(Channel(color, 24) * coverage + 0.5f);
    if (a == 0)
        return;
    uint32_t& d = pixels[size_t(y) * width + x];
    if (a >= 255)
    {
        d = color | 0xFF000000u;
        return;
    }
    const uint32_t inv = 255 - a;
    const uint32_t rb = ((color & 0xFF00FF) * a + (d & 0xFF00FF) * inv + 0x800080) >> 8 & 0xFF00FF;
    const uint32_t g = ((color & 0xFF00) * a + (d & 0xFF00) * inv + 0x8000) >> 8 & 0xFF00;
    d = 0xFF000000u | rb | g;
}

// Every pixel near the shape: coverage from its signed distance to the edge (in pixels, negative
// inside) at the pixel's centre
template <typename Distance>
void Canvas::Shape(float x0, float y0, float x1, float y1, uint32_t color, Distance distance)
{
    const Box box = Bounds(x0, y0, x1, y1);
    for (int y = box.y0; y < box.y1; y++)
    {
        const float py = (y + 0.5f) / scale;
        for (int x = box.x0; x < box.x1; x++)
        {
            const float px = (x + 0.5f) / scale;
            const float coverage = Clamp01(0.5f - distance(px, py) * scale);
            if (coverage > 0)
                Blend(x, y, color, coverage);
        }
    }
}

void Canvas::Clear(uint32_t color)
{
    std::fill(pixels.begin(), pixels.end(), color | 0xFF000000u);
}

void Canvas::Rect(float x, float y, float w, float h, uint32_t color)
{
    // whole pixels filled directly, the edges' partial pixels by coverage
    const float fx0 = x * scale, fy0 = y * scale, fx1 = (x + w) * scale, fy1 = (y + h) * scale;
    const Box box = Bounds(x, y, x + w, y + h);
    for (int py = box.y0; py < box.y1; py++)
    {
        const float cy = Clamp01(std::min(py + 1.0f, fy1) - std::max(float(py), fy0));
        if (cy <= 0)
            continue;
        for (int px = box.x0; px < box.x1; px++)
        {
            const float cx = Clamp01(std::min(px + 1.0f, fx1) - std::max(float(px), fx0));
            if (cx > 0)
                Blend(px, py, color, cx * cy);
        }
    }
}

namespace {
inline float RoundBox(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    const float qx = std::fabs(px - cx) - (hw - r), qy = std::fabs(py - cy) - (hh - r);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}
inline float Segment(float px, float py, float x0, float y0, float x1, float y1)
{
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    const float t = len2 > 0 ? Clamp01(((px - x0) * dx + (py - y0) * dy) / len2) : 0;
    const float ex = px - (x0 + t * dx), ey = py - (y0 + t * dy);
    return std::sqrt(ex * ex + ey * ey);
}
} // namespace

void Canvas::RoundRect(float x, float y, float w, float h, float radius, uint32_t color)
{
    radius = std::min({radius, w / 2, h / 2});
    const float cx = x + w / 2, cy = y + h / 2;
    Shape(x, y, x + w, y + h, color,
          [=](float px, float py) { return RoundBox(px, py, cx, cy, w / 2, h / 2, radius); });
}

void Canvas::RoundRectOutline(float x, float y, float w, float h, float radius, float thickness, uint32_t color)
{
    radius = std::min({radius, w / 2, h / 2});
    const float cx = x + w / 2, cy = y + h / 2, half = thickness / 2;
    const float inner_r = std::max(radius - half, 0.0f);
    Shape(x, y, x + w, y + h, color, [=](float px, float py) {
        return std::fabs(RoundBox(px, py, cx, cy, w / 2 - half, h / 2 - half, inner_r)) - half;
    });
}

void Canvas::Circle(float cx, float cy, float radius, uint32_t color)
{
    Shape(cx - radius, cy - radius, cx + radius, cy + radius, color,
          [=](float px, float py) { return std::hypot(px - cx, py - cy) - radius; });
}

void Canvas::Ring(float cx, float cy, float radius, float thickness, uint32_t color)
{
    const float mid = radius - thickness / 2;
    Shape(cx - radius, cy - radius, cx + radius, cy + radius, color,
          [=](float px, float py) { return std::fabs(std::hypot(px - cx, py - cy) - mid) - thickness / 2; });
}

void Canvas::Line(float x0, float y0, float x1, float y1, float w, uint32_t color)
{
    const float r = w / 2;
    Shape(std::min(x0, x1) - r, std::min(y0, y1) - r, std::max(x0, x1) + r, std::max(y0, y1) + r, color,
          [=](float px, float py) { return Segment(px, py, x0, y0, x1, y1) - r; });
}

void Canvas::Arc(float cx, float cy, float radius, float thickness, float from, float to, uint32_t color)
{
    const float mid = radius - thickness / 2, half = thickness / 2;
    const float span = to - from;
    const float ax = cx + mid * std::cos(from), ay = cy + mid * std::sin(from);
    const float bx = cx + mid * std::cos(to), by = cy + mid * std::sin(to);
    Shape(cx - radius, cy - radius, cx + radius, cy + radius, color, [=](float px, float py) {
        float angle = std::atan2(py - cy, px - cx) - from;
        angle -= 6.2831853f * std::floor(angle / 6.2831853f);
        if (angle <= span)
            return std::fabs(std::hypot(px - cx, py - cy) - mid) - half;
        return std::min(std::hypot(px - ax, py - ay), std::hypot(px - bx, py - by)) - half;
    });
}

void Canvas::Triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color)
{
    // inside: on the same side of all three edges; distance: to the nearest edge
    const float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    const float sign = area < 0 ? -1.0f : 1.0f;
    Shape(std::min({x0, x1, x2}), std::min({y0, y1, y2}), std::max({x0, x1, x2}), std::max({y0, y1, y2}), color,
          [=](float px, float py) {
              const float d = std::min({Segment(px, py, x0, y0, x1, y1), Segment(px, py, x1, y1, x2, y2),
                                        Segment(px, py, x2, y2, x0, y0)});
              const bool inside = sign * ((x1 - x0) * (py - y0) - (y1 - y0) * (px - x0)) >= 0 &&
                                  sign * ((x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)) >= 0 &&
                                  sign * ((x0 - x2) * (py - y2) - (y0 - y2) * (px - x2)) >= 0;
              return inside ? -d : d;
          });
}

void Canvas::Gradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom)
{
    const Box box = Bounds(x, y, x + w, y + h);
    const float fy0 = y * scale, fy1 = (y + h) * scale, fx0 = x * scale, fx1 = (x + w) * scale;
    for (int py = box.y0; py < box.y1; py++)
    {
        const float cy = Clamp01(std::min(py + 1.0f, fy1) - std::max(float(py), fy0));
        if (cy <= 0)
            continue;
        const uint32_t color = Mix(top, bottom, (py + 0.5f - fy0) / std::max(fy1 - fy0, 1.0f));
        for (int px = box.x0; px < box.x1; px++)
        {
            const float cx = Clamp01(std::min(px + 1.0f, fx1) - std::max(float(px), fx0));
            if (cx > 0)
                Blend(px, py, color, cx * cy);
        }
    }
}

void Canvas::DrawImage(float x, float y, float w, float h, const Image& image, float opacity)
{
    if (image.Empty())
        return;
    const int px0 = int(std::lround(x * scale)), py0 = int(std::lround(y * scale));
    const int pw = int(std::lround(w * scale)), ph = int(std::lround(h * scale));
    if (pw <= 0 || ph <= 0)
        return;
    const int x0 = std::max(px0, clip.x0), x1 = std::min(px0 + pw, clip.x1);
    const int y0 = std::max(py0, clip.y0), y1 = std::min(py0 + ph, clip.y1);
    const bool exact = image.width == pw && image.height == ph;
    const uint32_t alpha_scale = uint32_t(Clamp01(opacity) * 256);
    for (int py = y0; py < y1; py++)
    {
        uint32_t* out = &pixels[size_t(py) * width];
        if (exact)
        {
            const uint32_t* src = &image.pixels[size_t(py - py0) * image.width - px0];
            if (image.opaque && alpha_scale >= 256)
            {
                std::memcpy(out + x0, src + x0, size_t(x1 - x0) * 4);
                continue;
            }
            for (int px = x0; px < x1; px++)
                Blend(px, py, src[px] | 0xFF000000u, (Channel(src[px], 24) * alpha_scale >> 8) / 255.0f);
            continue;
        }
        // bilinear
        const float sy = ((py - py0) + 0.5f) * image.height / ph - 0.5f;
        const int iy = std::clamp(int(std::floor(sy)), 0, image.height - 1);
        const int iy1 = std::min(iy + 1, image.height - 1);
        const float fy = Clamp01(sy - iy);
        for (int px = x0; px < x1; px++)
        {
            const float sx = ((px - px0) + 0.5f) * image.width / pw - 0.5f;
            const int ix = std::clamp(int(std::floor(sx)), 0, image.width - 1);
            const int ix1 = std::min(ix + 1, image.width - 1);
            const float fx = Clamp01(sx - ix);
            const uint32_t a = image.pixels[size_t(iy) * image.width + ix];
            const uint32_t b = image.pixels[size_t(iy) * image.width + ix1];
            const uint32_t c = image.pixels[size_t(iy1) * image.width + ix];
            const uint32_t d = image.pixels[size_t(iy1) * image.width + ix1];
            const uint32_t p = Mix(Mix(a, b, fx), Mix(c, d, fx), fy);
            if (image.opaque && alpha_scale >= 256)
                out[px] = p | 0xFF000000u;
            else
                Blend(px, py, p | 0xFF000000u, (Channel(p, 24) * alpha_scale >> 8) / 255.0f);
        }
    }
}

float Canvas::Ascent(float size, Weight weight)
{
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&fonts->info[int(weight)], &ascent, &descent, &gap);
    return ascent * fonts->ScaleFor(weight, size * scale) / scale;
}

float Canvas::Descent(float size, Weight weight)
{
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&fonts->info[int(weight)], &ascent, &descent, &gap);
    return -descent * fonts->ScaleFor(weight, size * scale) / scale;
}

float Canvas::TextWidth(std::string_view text, float size, Weight weight)
{
    const float pixels_size = size * scale;
    float pen = 0;
    for (size_t i = 0; i < text.size();)
        pen += fonts->Get(weight, pixels_size, NextCodePoint(text, i)).advance;
    return pen / scale;
}

std::string Canvas::Fit(std::string_view text, float size, Weight weight, float max_width)
{
    if (max_width <= 0 || TextWidth(text, size, weight) <= max_width)
        return std::string(text);
    const std::string_view ellipsis = "\xE2\x80\xA6";
    const float room = max_width - TextWidth(ellipsis, size, weight);
    size_t cut = 0;
    float pen = 0;
    for (size_t i = 0; i < text.size();)
    {
        const uint32_t cp = NextCodePoint(text, i);
        pen += fonts->Get(weight, size * scale, cp).advance / scale;
        if (pen > room)
            break;
        cut = i;
    }
    std::string out(text.substr(0, cut));
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out + std::string(ellipsis);
}

float Canvas::Text(float x, float y, std::string_view text_in, float size, Weight weight, uint32_t color, Align align,
                   float max_width)
{
    const std::string text = Fit(text_in, size, weight, max_width);
    const float text_width = TextWidth(text, size, weight);
    if (align == Align::Center)
        x -= text_width / 2;
    else if (align == Align::Right)
        x -= text_width;
    const float pixels_size = size * scale;
    float pen = x * scale;
    const int baseline = int(std::lround(y * scale));
    for (size_t i = 0; i < text.size();)
    {
        const Fonts::Glyph& glyph = fonts->Get(weight, pixels_size, NextCodePoint(text, i));
        const int gx = int(std::lround(pen)) + glyph.left, gy = baseline + glyph.top;
        for (int row = 0; row < glyph.height; row++)
        {
            const int py = gy + row;
            if (py < clip.y0 || py >= clip.y1)
                continue;
            const uint8_t* coverage = &glyph.coverage[size_t(row) * glyph.width];
            for (int col = 0; col < glyph.width; col++)
            {
                const int px = gx + col;
                if (coverage[col] && px >= clip.x0 && px < clip.x1)
                    Blend(px, py, color, coverage[col] / 255.0f);
            }
        }
        pen += glyph.advance;
    }
    return text_width;
}

std::vector<std::string> Canvas::Wrap(std::string_view text, float size, Weight weight, float max_width)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size())
    {
        const size_t newline = text.find('\n', start);
        const std::string_view paragraph = text.substr(start, newline == std::string_view::npos ? std::string_view::npos
                                                                                              : newline - start);
        std::string line;
        size_t i = 0;
        while (i <= paragraph.size())
        {
            size_t end = paragraph.find(' ', i);
            if (end == std::string_view::npos)
                end = paragraph.size();
            const std::string_view word = paragraph.substr(i, end - i);
            const std::string candidate = line.empty() ? std::string(word) : line + " " + std::string(word);
            if (TextWidth(candidate, size, weight) <= max_width || line.empty())
            {
                if (TextWidth(candidate, size, weight) > max_width)
                {
                    // a word longer than the line: as much of it as fits, the rest on the next
                    std::string part;
                    for (size_t k = 0; k < word.size();)
                    {
                        const size_t s = k;
                        NextCodePoint(word, k);
                        if (!part.empty() && TextWidth(part + std::string(word.substr(s, k - s)), size, weight) > max_width)
                        {
                            lines.push_back(part);
                            part.clear();
                        }
                        part += word.substr(s, k - s);
                    }
                    line = part;
                }
                else
                {
                    line = candidate;
                }
            }
            else
            {
                lines.push_back(line);
                line = std::string(word);
            }
            i = end + 1;
        }
        lines.push_back(line);
        if (newline == std::string_view::npos)
            break;
        start = newline + 1;
    }
    return lines;
}

} // namespace Ui
