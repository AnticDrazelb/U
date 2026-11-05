// The user icons (avatars.h), their art included whole (EDEN_PS4_UI_ART: the ui-art folder, set by
// the build).

#include "ui/avatars.h"

#include <algorithm>
#include <cmath>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "ui/stb/stb_image_write.h"

#ifndef EDEN_PS4_UI_ART
#error "EDEN_PS4_UI_ART (the ui-art folder) must be set"
#endif

__asm__(".pushsection .rodata\n"
        ".balign 16\n"
        ".global eden_ps4_avatars\neden_ps4_avatars:\n"
        ".incbin \"" EDEN_PS4_UI_ART "/avatars.png\"\n"
        ".global eden_ps4_avatars_end\neden_ps4_avatars_end:\n"
        ".popsection\n");
extern "C" const unsigned char eden_ps4_avatars[], eden_ps4_avatars_end[];

namespace Ui {

namespace {

// avatars.png: 6 columns of 256x256 cells, in this order (tools/avatars/characters.js)
constexpr int kColumns = 6, kCell = 256;
const char* const kNames[] = {"Ginger Cat", "Black Cat", "Calico",  "Shiba",   "Puppy",      "Bear",
                              "Panda",      "Polar Bear", "Koala",  "Rabbit",  "Fox",        "Raccoon",
                              "Mouse",      "Hamster",   "Piglet",  "Cow",     "Sheep",      "Tiger",
                              "Lion",       "Monkey",    "Frog",    "Penguin", "Owl",        "Chick",
                              "Seal",       "Hedgehog",  "Octopus", "Ghost",   "Robot",      "Alien"};
constexpr int kCount = int(sizeof(kNames) / sizeof(kNames[0]));
// the background colours (tools/avatars/render.js previews them)
constexpr uint32_t kColors[] = {0x5AB4F0, 0x3D6FE0, 0x2CC0B0, 0x5CC85C, 0xB5DB4C, 0xFFD34E,
                                0xFF9A3C, 0xF0544F, 0xFF8FB8, 0x9B6CE0, 0x8E8E93, 0x3A3A3C};
constexpr int kColorCount = int(sizeof(kColors) / sizeof(kColors[0]));

// decoded when first needed (ReleaseAvatarArt frees it)
Image g_atlas;
const Image& Atlas()
{
    if (g_atlas.Empty())
        g_atlas = DecodeImage(eden_ps4_avatars, size_t(eden_ps4_avatars_end - eden_ps4_avatars));
    return g_atlas;
}

// Larger than the art: each pixel from the four nearest (bilinear), so the edges stay smooth
Image Enlarge(const Image& image, int size)
{
    Image out;
    out.width = out.height = size;
    out.opaque = image.opaque;
    out.pixels.resize(size_t(size) * size);
    const float step = float(image.width) / float(size);
    for (int y = 0; y < size; y++)
    {
        const float fy = std::clamp((y + 0.5f) * step - 0.5f, 0.0f, float(image.height - 1));
        const int y0 = int(fy), y1 = std::min(y0 + 1, image.height - 1);
        const float ty = fy - float(y0);
        for (int x = 0; x < size; x++)
        {
            const float fx = std::clamp((x + 0.5f) * step - 0.5f, 0.0f, float(image.width - 1));
            const int x0 = int(fx), x1 = std::min(x0 + 1, image.width - 1);
            const float tx = fx - float(x0);
            const uint32_t a = image.pixels[size_t(y0) * image.width + x0], b = image.pixels[size_t(y0) * image.width + x1];
            const uint32_t c = image.pixels[size_t(y1) * image.width + x0], d = image.pixels[size_t(y1) * image.width + x1];
            uint32_t p = 0;
            for (int shift = 0; shift < 32; shift += 8)
            {
                const float top = float(a >> shift & 0xFF) * (1 - tx) + float(b >> shift & 0xFF) * tx;
                const float bottom = float(c >> shift & 0xFF) * (1 - tx) + float(d >> shift & 0xFF) * tx;
                p |= uint32_t(std::lround(top * (1 - ty) + bottom * ty)) << shift;
            }
            out.pixels[size_t(y) * size + x] = p;
        }
    }
    return out;
}

void JpegOut(void* context, void* data, int size)
{
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

} // namespace

int AvatarCount()
{
    return kCount;
}

const char* AvatarName(int character)
{
    return kNames[character >= 0 && character < kCount ? character : 0];
}

int AvatarColorCount()
{
    return kColorCount;
}

uint32_t AvatarColor(int color)
{
    return Rgb(kColors[color >= 0 && color < kColorCount ? color : 0]);
}

Image MakeAvatar(int character, int color, int size, bool round)
{
    if (character < 0 || character >= kCount)
        character = 0;
    const uint32_t bg = AvatarColor(color);
    const Image& atlas = Atlas();
    Image square;
    square.width = square.height = kCell;
    square.pixels.assign(size_t(kCell) * kCell, bg);
    const int x0 = (character % kColumns) * kCell, y0 = (character / kColumns) * kCell;
    if (!atlas.Empty() && x0 + kCell <= atlas.width && y0 + kCell <= atlas.height)
    {
        // the character over its colour (straight alpha)
        for (int y = 0; y < kCell; y++)
        {
            for (int x = 0; x < kCell; x++)
            {
                const uint32_t src = atlas.pixels[size_t(y0 + y) * atlas.width + x0 + x];
                const uint32_t a = src >> 24;
                if (a == 0)
                    continue;
                uint32_t& dst = square.pixels[size_t(y) * kCell + x];
                uint32_t out = 0xFF000000u;
                for (int shift = 0; shift < 24; shift += 8)
                {
                    const uint32_t s = src >> shift & 0xFF, d = dst >> shift & 0xFF;
                    out |= ((s * a + d * (255 - a) + 127) / 255) << shift;
                }
                dst = out;
            }
        }
    }
    Image image = size == kCell ? std::move(square) : size > kCell ? Enlarge(square, size) : ResizeImage(square, size, size);
    if (round)
    {
        // clear outside the circle, its edge anti-aliased
        const float r = size / 2.0f;
        for (int y = 0; y < size; y++)
        {
            for (int x = 0; x < size; x++)
            {
                const float dx = x + 0.5f - r, dy = y + 0.5f - r;
                const float coverage = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
                uint32_t& p = image.pixels[size_t(y) * size + x];
                p = (p & 0x00FFFFFFu) | uint32_t(std::lround(coverage * float(p >> 24))) << 24;
            }
        }
        image.opaque = false;
    }
    return image;
}

void ReleaseAvatarArt()
{
    g_atlas = Image{};
}

std::vector<uint8_t> EncodeJpeg(const Image& image, int quality)
{
    std::vector<uint8_t> out;
    if (!image.Empty())
        stbi_write_jpg_to_func(JpegOut, &out, image.width, image.height, 4, image.pixels.data(), quality);
    return out;
}

} // namespace Ui
