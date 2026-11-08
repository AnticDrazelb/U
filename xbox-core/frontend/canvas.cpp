#include "canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb/stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb/stb_image_resize.h"

namespace fe {

Color Mix(Color a, Color b, float t)
{
    auto m = [t](uint8_t x, uint8_t y) { return uint8_t(float(x) + (float(y) - float(x)) * t + 0.5f); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

Image LoadImage(const std::string& path)
{
    Image out;
    int n = 0;
    if (unsigned char* p = stbi_load(path.c_str(), &out.width, &out.height, &n, 4))
    {
        out.rgba.assign(p, p + size_t(out.width) * out.height * 4);
        stbi_image_free(p);
    }
    return out;
}

Image CoverScaled(const Image& source, int w, int h)
{
    Image out;
    if (source.Empty() || w <= 0 || h <= 0)
        return out;
    // the part of the source with the target's shape, centred
    const float target = float(w) / float(h), have = float(source.width) / float(source.height);
    int cw = source.width, ch = source.height, cx = 0, cy = 0;
    if (have > target)
        cw = std::max(1, int(float(source.height) * target)), cx = (source.width - cw) / 2;
    else
        ch = std::max(1, int(float(source.width) / target)), cy = (source.height - ch) / 2;
    out.width = w, out.height = h;
    out.rgba.resize(size_t(w) * h * 4);
    stbir_resize_uint8(source.rgba.data() + (size_t(cy) * source.width + cx) * 4, cw, ch, source.width * 4,
                       out.rgba.data(), w, h, w * 4, 4);
    return out;
}

// ---- the canvas

void Canvas::Blend(int x, int y, Color c, float coverage)
{
    if (x < 0 || y < 0 || x >= w || y >= h || coverage <= 0)
        return;
    uint32_t& p = px[size_t(y) * stride + x];
    const float a = std::min(1.f, coverage) * float(c.a) / 255.f;
    if (a >= 0.999f)
    {
        p = uint32_t(c.r) | uint32_t(c.g) << 8 | uint32_t(c.b) << 16 | 0xff000000u;
        return;
    }
    const float r = float(p & 0xff), g = float((p >> 8) & 0xff), b = float((p >> 16) & 0xff);
    p = uint32_t(r + (float(c.r) - r) * a + 0.5f) | uint32_t(g + (float(c.g) - g) * a + 0.5f) << 8 |
        uint32_t(b + (float(c.b) - b) * a + 0.5f) << 16 | 0xff000000u;
}

void Canvas::Clear(Color c)
{
    const uint32_t v = uint32_t(c.r) | uint32_t(c.g) << 8 | uint32_t(c.b) << 16 | 0xff000000u;
    for (int y = 0; y < h; y++)
        std::fill(px + size_t(y) * stride, px + size_t(y) * stride + w, v);
}

void Canvas::Fill(int x, int y, int fw, int fh, Color c)
{
    const int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(w, x + fw), y1 = std::min(h, y + fh);
    if (c.a == 255)
    {
        const uint32_t v = uint32_t(c.r) | uint32_t(c.g) << 8 | uint32_t(c.b) << 16 | 0xff000000u;
        for (int yy = y0; yy < y1; yy++)
            std::fill(px + size_t(yy) * stride + x0, px + size_t(yy) * stride + std::max(x0, x1), v);
        return;
    }
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            Blend(xx, yy, c, 1);
}

void Canvas::RoundRect(int x, int y, int rw, int rh, int r, Color c, int thickness)
{
    r = std::min({r, rw / 2, rh / 2});
    const float fr = float(r);
    auto pixel = [&](int xx, int yy) {
        const float pxf = float(xx) + 0.5f, py = float(yy) + 0.5f;
        // the signed distance to the rounded rectangle's edge (negative inside)
        const float qx = std::max(float(x) + fr - pxf, pxf - float(x + rw) + fr);
        const float qy = std::max(float(y) + fr - py, py - float(y + rh) + fr);
        const float d = qx > 0 && qy > 0 ? std::sqrt(qx * qx + qy * qy) - fr : std::max(qx, qy) - fr;
        float coverage = std::clamp(0.5f - d, 0.f, 1.f);
        if (thickness > 0)
            coverage = std::min(coverage, std::clamp(d + float(thickness) + 0.5f, 0.f, 1.f));
        if (coverage > 0)
            Blend(xx, yy, c, coverage);
    };
    // rows clear of the corners: their middle is one span (filled, or empty for an outline), only
    // the ends need the distance
    const int edge = std::max(r, thickness + 1);
    for (int yy = std::max(0, y); yy < std::min(h, y + rh); yy++)
    {
        if (yy >= y + edge && yy < y + rh - edge)
        {
            for (int xx = std::max(0, x); xx < std::min({w, x + rw, x + edge}); xx++)
                pixel(xx, yy);
            if (thickness == 0)
                Fill(x + edge, yy, rw - 2 * edge, 1, c);
            for (int xx = std::max(0, x + rw - edge); xx < std::min(w, x + rw); xx++)
                pixel(xx, yy);
            continue;
        }
        for (int xx = std::max(0, x); xx < std::min(w, x + rw); xx++)
            pixel(xx, yy);
    }
}

void Canvas::VerticalGradient(int x, int y, int gw, int gh, Color top, Color bottom)
{
    for (int yy = 0; yy < gh; yy++)
        Fill(x, y + yy, gw, 1, Mix(top, bottom, gh > 1 ? float(yy) / float(gh - 1) : 0.f));
}

void Canvas::Circle(float cx, float cy, float r, Color c, float thickness)
{
    for (int yy = int(cy - r - 1); yy <= int(cy + r + 1); yy++)
        for (int xx = int(cx - r - 1); xx <= int(cx + r + 1); xx++)
        {
            const float d = std::hypot(float(xx) + 0.5f - cx, float(yy) + 0.5f - cy) - r;
            float coverage = std::clamp(0.5f - d, 0.f, 1.f);
            if (thickness > 0)
                coverage = std::min(coverage, std::clamp(d + thickness + 0.5f, 0.f, 1.f));
            Blend(xx, yy, c, coverage);
        }
}

void Canvas::Line(float x0, float y0, float x1, float y1, float width, Color c)
{
    const float dx = x1 - x0, dy = y1 - y0, len2 = std::max(1e-6f, dx * dx + dy * dy);
    const int bx0 = int(std::min(x0, x1) - width), bx1 = int(std::max(x0, x1) + width);
    const int by0 = int(std::min(y0, y1) - width), by1 = int(std::max(y0, y1) + width);
    for (int yy = by0; yy <= by1; yy++)
        for (int xx = bx0; xx <= bx1; xx++)
        {
            const float pxf = float(xx) + 0.5f, py = float(yy) + 0.5f;
            const float t = std::clamp(((pxf - x0) * dx + (py - y0) * dy) / len2, 0.f, 1.f);
            const float d = std::hypot(pxf - (x0 + t * dx), py - (y0 + t * dy)) - width / 2;
            Blend(xx, yy, c, std::clamp(0.5f - d, 0.f, 1.f));
        }
}

void Canvas::Draw(const Image& image, int x, int y, int r, uint8_t alpha)
{
    if (image.Empty())
        return;
    const float fr = float(r);
    for (int yy = 0; yy < image.height; yy++)
    {
        const int ty = y + yy;
        if (ty < 0 || ty >= h)
            continue;
        for (int xx = 0; xx < image.width; xx++)
        {
            const int tx = x + xx;
            if (tx < 0 || tx >= w)
                continue;
            const uint8_t* s = &image.rgba[(size_t(yy) * image.width + xx) * 4];
            float coverage = float(s[3]) / 255.f * float(alpha) / 255.f;
            if (r > 0)
            {
                // rounded corners: the distance into the corner's circle
                const float pxf = float(xx) + 0.5f, py = float(yy) + 0.5f;
                const float qx = std::max(fr - pxf, pxf - float(image.width) + fr);
                const float qy = std::max(fr - py, py - float(image.height) + fr);
                if (qx > 0 && qy > 0)
                    coverage *= std::clamp(0.5f - (std::hypot(qx, qy) - fr), 0.f, 1.f);
            }
            Blend(tx, ty, {s[0], s[1], s[2], 255}, coverage);
        }
    }
}

void Canvas::CopyFrom(const Canvas& other)
{
    for (int y = 0; y < std::min(h, other.h); y++)
        std::memcpy(px + size_t(y) * stride, other.px + size_t(y) * other.stride, size_t(std::min(w, other.w)) * 4);
}

// ---- text

std::vector<uint32_t> DecodeUtf8(const std::string& s)
{
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();)
    {
        const uint8_t c = uint8_t(s[i]);
        uint32_t cp = c;
        int extra = 0;
        if (c >= 0xf0) cp = c & 7, extra = 3;
        else if (c >= 0xe0) cp = c & 15, extra = 2;
        else if (c >= 0xc0) cp = c & 31, extra = 1;
        i++;
        for (int k = 0; k < extra && i < s.size(); k++, i++)
            cp = cp << 6 | (uint8_t(s[i]) & 63);
        out.push_back(cp);
    }
    return out;
}

bool Font::Load(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    data.resize(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const bool ok = std::fread(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    auto* fi = new stbtt_fontinfo;
    info.reset(fi, [](void* p) { delete static_cast<stbtt_fontinfo*>(p); });
    if (!ok || !stbtt_InitFont(fi, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0)))
    {
        data.clear();
        return false;
    }
    return true;
}

const Font::Glyph& Font::Get(uint32_t codepoint, float size)
{
    const auto key = std::make_pair(codepoint, int(size * 4));
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    Glyph g{};
    auto* fi = static_cast<stbtt_fontinfo*>(info.get());
    const float scale = stbtt_ScaleForPixelHeight(fi, size);
    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(fi, int(codepoint), &advance, &lsb);
    g.advance = int(float(advance) * scale + 0.5f);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(fi, int(codepoint), scale, scale, &x0, &y0, &x1, &y1);
    g.x0 = x0, g.y0 = y0, g.w = std::max(0, x1 - x0), g.h = std::max(0, y1 - y0);
    g.alpha.resize(size_t(g.w) * g.h);
    if (g.w && g.h)
        stbtt_MakeCodepointBitmap(fi, g.alpha.data(), g.w, g.h, g.w, scale, scale, int(codepoint));
    return cache.emplace(key, std::move(g)).first->second;
}

int Font::Draw(Canvas& c, int x, int y, const std::string& text, float size, Color color)
{
    if (!Loaded())
        return 0;
    auto* fi = static_cast<stbtt_fontinfo*>(info.get());
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(fi, &ascent, &descent, &gap);
    const int baseline = y + int(float(ascent) * stbtt_ScaleForPixelHeight(fi, size) + 0.5f);
    int pen = x;
    for (uint32_t cp : DecodeUtf8(text))
    {
        const Glyph& g = Get(cp, size);
        for (int gy = 0; gy < g.h; gy++)
            for (int gx = 0; gx < g.w; gx++)
                if (const uint8_t a = g.alpha[size_t(gy) * g.w + gx])
                    c.Blend(pen + g.x0 + gx, baseline + g.y0 + gy, color, float(a) / 255.f);
        pen += g.advance;
    }
    return pen - x;
}

int Font::Measure(const std::string& text, float size)
{
    if (!Loaded())
        return 0;
    int width = 0;
    for (uint32_t cp : DecodeUtf8(text))
        width += Get(cp, size).advance;
    return width;
}

std::string Font::Fit(const std::string& text, float size, int width)
{
    if (Measure(text, size) <= width)
        return text;
    std::string cut = text;
    while (!cut.empty() && Measure(cut + "...", size) > width)
    {
        cut.pop_back();
        while (!cut.empty() && (uint8_t(cut.back()) & 0xc0) == 0x80) // a whole UTF-8 character
            cut.pop_back();
    }
    return cut + "...";
}

std::vector<std::string> Font::Wrap(const std::string& text, float size, int width, int max_lines)
{
    std::vector<std::string> lines;
    std::string line, word;
    auto flush_word = [&] {
        if (word.empty())
            return;
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (Measure(candidate, size) <= width || line.empty())
            line = candidate;
        else
        {
            lines.push_back(line);
            line = word;
        }
        word.clear();
    };
    for (char ch : text)
    {
        if (ch == ' ')
            flush_word();
        else
            word += ch;
    }
    flush_word();
    if (!line.empty())
        lines.push_back(line);
    if (int(lines.size()) > max_lines)
    {
        lines.resize(size_t(max_lines));
        lines.back() = Fit(lines.back() + " ...", size, width);
    }
    for (std::string& l : lines)
        l = Fit(l, size, width);
    return lines;
}

} // namespace fe
