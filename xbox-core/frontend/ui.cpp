#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fe {
namespace {

// the palette: a light mint world, deep green text, a fresh green accent and an aqua glow
const Color kText = Rgb(0x1E3A22), kDim = Rgb(0x4E6B52), kFaint = Rgb(0x7E9A80);
const Color kAccent = Rgb(0x2FA84F), kAccentDark = Rgb(0xD5F0C8), kGlow = Rgb(0x3FE0C5);
const Color kPanel = Rgb(0xFFFFFF, 170), kPanelEdge = Rgb(0xFFFFFF, 235), kRaised = Rgb(0xFFFFFF, 240);
const Color kOnAccent = Rgb(0xFFFFFF);

// the carousel: portrait covers (a case's shape), the chosen one larger
constexpr int kCardW = 262, kCardH = 370, kLargeW = 300, kLargeH = 424;
constexpr int kVisible = 5, kStepX = 344, kCarouselX = 118, kCarouselY = 236, kCaptionH = 86;
constexpr int kDockY = 846, kDockH = 138, kHintsY = 1010;

uint32_t Hash(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (char c : s)
        h = (h ^ uint8_t(c)) * 16777619u;
    return h;
}

// a colour from hue (0..1), saturation and value
Color Hsv(float h, float s, float v)
{
    const float c = v * s, x = c * (1 - std::fabs(std::fmod(h * 6, 2.f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    switch (int(h * 6) % 6)
    {
    case 0: r = c, g = x; break;
    case 1: r = x, g = c; break;
    case 2: g = c, b = x; break;
    case 3: g = x, b = c; break;
    case 4: r = x, b = c; break;
    default: r = c, b = x;
    }
    return {uint8_t((r + m) * 255), uint8_t((g + m) * 255), uint8_t((b + m) * 255), 255};
}

Canvas CanvasOf(Image& image)
{
    return Canvas(reinterpret_cast<uint32_t*>(image.rgba.data()), image.width, image.height, image.width);
}

std::string Upper(std::string s)
{
    for (char& c : s)
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    return s;
}

std::string ReadFirstLine(const std::string& file)
{
    std::string out;
    if (FILE* f = std::fopen(file.c_str(), "r"))
    {
        char line[1024] = {};
        if (std::fgets(line, sizeof(line), f))
            out = line;
        std::fclose(f);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

} // namespace

LibraryUi::LibraryUi(std::vector<Game> g, Settings& s, UiPaths p, std::string v)
    : games(std::move(g)), settings(s), paths(std::move(p)), version(std::move(v))
{
    ready = regular.Load(paths.assets + "/Inter-Regular.otf") && semibold.Load(paths.assets + "/Inter-SemiBold.otf") &&
            bold.Load(paths.assets + "/Inter-Bold.otf");
    autostart_game = ReadFirstLine(paths.autostart);
    Sort();
    selected = 0, first = 0, scroll = 0;
    BuildBackground();
}

void LibraryUi::Sort()
{
    const std::string keep = selected < games.size() ? games[selected].path : "";
    std::stable_sort(games.begin(), games.end(), [this](const Game& a, const Game& b) {
        if (a.sample != b.sample) // the player's games before the samples
            return !a.sample;
        if (settings.sort == 1 && a.last_played != b.last_played)
            return a.last_played > b.last_played;
        return Upper(a.title) < Upper(b.title);
    });
    selected = 0;
    for (size_t i = 0; i < games.size(); i++)
        if (games[i].path == keep)
            selected = i;
    first = std::max(0, int(selected) - (kVisible - 1));
    scroll = float(first);
}

void LibraryUi::Toast(const std::string& text)
{
    toast = text;
    toast_until = now + 3.0;
}

bool LibraryUi::IsAutostart(const Game& g) const { return !autostart_game.empty() && autostart_game == g.path; }

void LibraryUi::Rescan()
{
    if (paths.library.games.empty())
        return;
    games = ScanLibrary(paths.library);
    Sort();
    usb_games = FindUsbGames(usb_roots, paths.library.games);
}

void LibraryUi::UpdateTools(const Input& in)
{
    if (!usb_view)
    {
        if (in.up || in.down)
            tool ^= 1;
        if (in.cross && tool == 0)
        {
            usb_roots = UsbRoots(paths.usb);
            usb_games = FindUsbGames(usb_roots, paths.library.games);
            usb_selected = 0;
            usb_view = true;
        }
        else if (in.cross && tool == 1)
        {
            DiagnosticsInput d;
            d.system = paths.system;
            d.settings = &settings;
            d.games = games;
            d.usb_roots = UsbRoots(paths.usb);
            d.data_dir = paths.data_dir;
            d.logs = paths.logs;
            std::vector<std::string> outputs{paths.report};
            for (const std::string& u : d.usb_roots)
                outputs.push_back(u + "/xbox-core-diagnostics.txt");
            report_written = WriteDiagnostics(d, outputs);
            Toast(report_written.empty() ? "The report couldn't be written" : "Report written: " + report_written.back());
        }
        return;
    }
    if (in.circle)
        usb_view = false;
    if (in.triangle)
    {
        usb_roots = UsbRoots(paths.usb);
        usb_games = FindUsbGames(usb_roots, paths.library.games);
        usb_selected = std::min(usb_selected, usb_games.empty() ? 0 : usb_games.size() - 1);
    }
    if (in.square && copy.busy)
    {
        copier.Cancel();
        Toast("Copying cancelled");
    }
    if (usb_games.empty())
        return;
    if (in.up && usb_selected > 0) usb_selected--;
    if (in.down && usb_selected + 1 < usb_games.size()) usb_selected++;
    if (in.cross)
    {
        UsbGame& g = usb_games[usb_selected];
        if (g.on_hdd)
            Toast(g.name + " is on the hard drive already");
        else
        {
            copier.Queue(g, paths.library.games);
            g.on_hdd = true; // queued: not twice
            Toast("Copying " + g.name);
        }
    }
}

const Game* LibraryUi::Update(const Input& in)
{
    // copies finishing: the library learns of them
    copy = copier.Poll();
    if (!copy.finished.empty())
    {
        Rescan();
        const std::string& last = copy.finished.back();
        Toast(last.find(": ") == std::string::npos ? "Copied " + last + " to the hard drive" : "Not copied: " + last);
    }
    if (in.l1 || in.r1)
    {
        tab = Tab((tab + (in.r1 ? 1 : kTabs - 1)) % kTabs);
        options_open = false;
        return nullptr;
    }
    switch (tab)
    {
    case kLibrary:
    {
        if (games.empty())
            return nullptr;
        if (options_open)
        {
            if (in.up || in.down)
                option ^= 1;
            if (in.circle || (in.cross && option == 1))
                options_open = false;
            else if (in.cross && option == 0)
            {
                const Game& g = games[selected];
                if (IsAutostart(g))
                {
                    std::remove(paths.autostart.c_str());
                    autostart_game.clear();
                    Toast("The library will open at start again");
                }
                else if (FILE* f = std::fopen(paths.autostart.c_str(), "w"))
                {
                    std::fprintf(f, "%s\n", g.path.c_str());
                    std::fclose(f);
                    autostart_game = g.path;
                    Toast("Starts with the app now. Hold L1 while it opens for the library");
                }
                options_open = false;
            }
            return nullptr;
        }
        const int n = int(games.size());
        int s = int(selected);
        if (in.left && s > 0) s--;
        if (in.right && s + 1 < n) s++;
        if (in.up) s = std::max(0, s - kVisible);      // a page at a time
        if (in.down) s = std::min(n - 1, s + kVisible);
        selected = size_t(s);
        if (s < first) first = s;
        if (s >= first + kVisible) first = s - kVisible + 1;
        if (in.cross)
            return &games[selected];
        if (in.triangle)
            options_open = true, option = 0;
        if (in.square)
        {
            settings.sort ^= 1;
            settings.Save(paths.settings);
            Sort();
            Toast(settings.sort ? "Sorted by recently played" : "Sorted by title");
        }
        return nullptr;
    }
    case kSettings:
    {
        if (in.up) setting = (setting + 4) % 5;
        if (in.down) setting = (setting + 1) % 5;
        const int step = in.right || in.cross ? 1 : in.left ? -1 : 0;
        if (step)
        {
            if (setting == 0) settings.show_log = !settings.show_log;
            if (setting == 1) settings.picture = (settings.picture + 3 + step) % 3;
            if (setting == 2) settings.sort ^= 1, Sort();
            if (setting == 3) settings.nv2a ^= 1;
            if (setting == 4) settings.dsp ^= 1;
            settings.Save(paths.settings);
        }
        return nullptr;
    }
    case kTools:
        UpdateTools(in);
        return nullptr;
    default:
        return nullptr;
    }
}

// ---- covers

Image LibraryUi::GeneratedCover(const Game& g, int w, int h)
{
    Image img;
    img.width = w, img.height = h;
    img.rgba.assign(size_t(w) * h * 4, 255);
    Canvas c = CanvasOf(img);
    const float k = float(w) / float(kCardW);
    // a bright card in the game's own colour, soft shapes floating in it
    const float hue = float(Hash(g.title) % 360) / 360.f;
    c.VerticalGradient(0, 0, w, h, Hsv(hue, 0.42f, 0.98f), Hsv(std::fmod(hue + 0.08f, 1.f), 0.62f, 0.78f));
    const uint32_t seed = Hash(g.path);
    for (int i = 0; i < 6; i++)
    {
        const float cx = float((seed >> (i * 5)) % 100) / 100.f * float(w), cy = float((seed >> (i * 3 + 2)) % 100) / 100.f * float(h);
        c.Circle(cx, cy, (24.f + float((seed >> i) % 50)) * k, Rgb(0xFFFFFF, 34));
    }
    // a disc in the corner
    const float dr = 54 * k;
    c.Circle(float(w) - dr * 0.55f, float(h) - dr * 0.55f, dr, Rgb(0xFFFFFF, 70));
    c.Circle(float(w) - dr * 0.55f, float(h) - dr * 0.55f, dr * 0.32f, Hsv(hue, 0.5f, 0.85f));
    // the kind, as a chip
    const std::string tag = g.sample ? "SAMPLE" : !g.usb.empty() ? "USB" : g.kind == "Disc image" ? "DISC" : "GAME";
    const int tw = semibold.Measure(tag, 17 * k) + int(24 * k);
    c.RoundRect(int(14 * k), int(14 * k), tw, int(30 * k), int(15 * k), Rgb(0xFFFFFF, 215));
    semibold.Draw(c, int(26 * k), int(19 * k), tag, 17 * k, Hsv(hue, 0.7f, 0.45f));
    // the title, big, in the middle
    const float size = 32 * k;
    const auto lines = bold.Wrap(g.title, size, w - int(36 * k), 4);
    int y = (h - int(lines.size()) * int(size * 1.18f)) / 2 - int(10 * k);
    for (const std::string& l : lines)
    {
        const int x = (w - bold.Measure(l, size)) / 2;
        bold.Draw(c, x + int(2 * k), y + int(3 * k), l, size, Rgb(0x000000, 60));
        bold.Draw(c, x, y, l, size, Rgb(0xFFFFFF));
        y += int(size * 1.18f);
    }
    return img;
}

const LibraryUi::Covers& LibraryUi::CoversFor(const Game& g)
{
    auto it = covers.find(g.path);
    if (it != covers.end())
        return it->second;
    Covers c;
    if (!g.cover.empty())
    {
        const Image source = LoadImage(g.cover);
        c.card = CoverScaled(source, kCardW, kCardH);
        c.large = CoverScaled(source, kLargeW, kLargeH);
    }
    if (c.card.Empty())
    {
        c.card = GeneratedCover(g, kCardW, kCardH);
        c.large = GeneratedCover(g, kLargeW, kLargeH);
    }
    return covers.emplace(g.path, std::move(c)).first->second;
}

// ---- the screen

namespace {
// the background's pattern: small outlines scattered on a grid (a controller, a plus, a power sign,
// a ring), each drawn faintly
void PatternIcon(Canvas& c, int kind, float x, float y, float s, Color col)
{
    switch (kind)
    {
    case 0: // a controller
        c.RoundRect(int(x - s), int(y - s * 0.45f), int(2 * s), int(s * 0.9f), int(s * 0.45f), col, 3);
        c.Line(x - s * 0.55f, y, x - s * 0.25f, y, 3, col);
        c.Line(x - s * 0.4f, y - s * 0.15f, x - s * 0.4f, y + s * 0.15f, 3, col);
        c.Circle(x + s * 0.45f, y - s * 0.08f, s * 0.08f, col);
        c.Circle(x + s * 0.3f, y + s * 0.1f, s * 0.08f, col);
        break;
    case 1: // a plus
        c.Line(x - s * 0.5f, y, x + s * 0.5f, y, 4, col);
        c.Line(x, y - s * 0.5f, x, y + s * 0.5f, 4, col);
        break;
    case 2: // a power sign
        c.Circle(x, y, s * 0.55f, col, 3);
        c.Line(x, y - s * 0.75f, x, y - s * 0.1f, 4, col);
        break;
    default: // a ring
        c.Circle(x, y, s * 0.4f, col, 3);
    }
}
} // namespace

void LibraryUi::BuildBackground()
{
    background.assign(size_t(kWidth) * kHeight, 0);
    Canvas c(background.data(), kWidth, kHeight, kWidth);
    c.VerticalGradient(0, 0, kWidth, kHeight, Rgb(0xE6F7D6), Rgb(0xB5E39B));
    // a glow behind the carousel
    for (int y = 120; y < 860; y++)
        for (int x = 0; x < kWidth; x += 1)
        {
            const float dx = (float(x) - 960.f) / 1000.f, dy = (float(y) - 470.f) / 380.f;
            const float d = dx * dx + dy * dy;
            if (d < 1)
                c.Blend(x, y, Rgb(0xFFFFFF), 0.30f * (1 - d));
        }
    // the pattern
    uint32_t r = 12345;
    for (int gy = 0; gy < 14; gy++)
        for (int gx = 0; gx < 22; gx++)
        {
            r = r * 1103515245u + 12345u;
            const float x = float(gx) * 92.f + 40.f + float((r >> 8) % 30), y = float(gy) * 82.f + 30.f + float((r >> 16) % 30);
            PatternIcon(c, int((r >> 24) % 4), x, y, 22.f + float((r >> 4) % 8), Rgb(0x6FBF57, 46));
        }
}

// the four places' icons: a grid of cases, a gear, a wrench, a circled i
void LibraryUi::DrawIcon(Canvas& c, int t, int cx, int cy, Color col, Color back)
{
    if (t == kLibrary)
    {
        for (int i = 0; i < 3; i++)
        {
            const int x = cx - 24 + i * 17;
            c.RoundRect(x, cy - 18, 14, 36, 3, col);
            c.Fill(x + 3, cy - 12, 8, 3, back);
        }
    }
    else if (t == kSettings)
    {
        for (int k = 0; k < 8; k++)
        {
            const float a = float(k) * 3.14159f / 4;
            c.Line(cx + std::cos(a) * 10, cy + std::sin(a) * 10, cx + std::cos(a) * 18, cy + std::sin(a) * 18, 7, col);
        }
        c.Circle(float(cx), float(cy), 13, col, 6);
    }
    else if (t == kTools)
    {
        c.Line(float(cx - 14), float(cy + 15), float(cx + 6), float(cy - 5), 8, col);
        c.Circle(float(cx + 9), float(cy - 9), 10, col, 5);
        c.Fill(cx + 9, cy - 21, 11, 10, back);
    }
    else
    {
        c.Circle(float(cx), float(cy), 18, col);
        c.Fill(cx - 2, cy - 2, 5, 12, back);
        c.Fill(cx - 2, cy - 10, 5, 5, back);
    }
}

void LibraryUi::DrawHeader(Canvas& c, const std::string& clock, const std::string& user)
{
    bold.Draw(c, 62, 40, "YOUR ORIGINAL XBOX COLLECTION", 50, kText);
    regular.Draw(c, 64, 102, "Player 1: " + user + "  \xC2\xB7  DualShock 4 as the controller", 26, kDim);
    const int cw = semibold.Measure(clock, 40);
    semibold.Draw(c, kWidth - 62 - cw, 42, clock, 40, kText);
    std::string under = "v" + version;
    if (tab == kLibrary && !games.empty())
        under = std::to_string(selected + 1) + " of " + std::to_string(games.size()) + (games.size() == 1 ? " game" : " games") +
                "   \xC2\xB7   " + under;
    regular.Draw(c, kWidth - 62 - regular.Measure(under, 22), 100, under, 22, kDim);
}

void LibraryUi::DrawDock(Canvas& c)
{
    static const char* kNames[kTabs] = {"Collection", "Settings", "Tools", "About"};
    static const char* kSubs[kTabs] = {"Your games", "Picture & order", "Copy & reports", "Help & credits"};
    const int item = 236, w = item * kTabs + 40, x0 = (kWidth - w) / 2;
    c.RoundRect(x0 + 4, kDockY + 8, w, kDockH, kDockH / 2, Rgb(0x2E6B2E, 40));
    c.RoundRect(x0, kDockY, w, kDockH, kDockH / 2, Rgb(0xFFFFFF, 225));
    for (int i = 0; i < kTabs; i++)
    {
        const int cx = x0 + 20 + item * i + item / 2;
        const bool on = tab == i;
        if (on)
            c.RoundRect(cx - item / 2 + 10, kDockY + 10, item - 20, kDockH - 20, 30, Rgb(0xE3F6D8));
        const Color tile = on ? kAccent : Rgb(0xDDF1D1);
        c.RoundRect(cx - 32, kDockY + 18, 64, 56, 16, tile);
        DrawIcon(c, i, cx, kDockY + 46, on ? kOnAccent : kAccent, tile);
        const int lw = semibold.Measure(kNames[i], 24);
        semibold.Draw(c, cx - lw / 2, kDockY + 80, kNames[i], 24, kText);
        const int sw = regular.Measure(kSubs[i], 17);
        regular.Draw(c, cx - sw / 2, kDockY + 108, kSubs[i], 17, kDim);
    }
}

void LibraryUi::Glyph(Canvas& c, int cx, int cy, char button, int r)
{
    c.Circle(float(cx), float(cy), float(r), Rgb(0xFFFFFF));
    c.Circle(float(cx), float(cy), float(r), Rgb(0x9CCB8C), 2);
    const float s = float(r) * 0.45f;
    switch (button)
    {
    case 'x': // cross
        c.Line(cx - s, cy - s, cx + s, cy + s, 3, Rgb(0x3E7BD6));
        c.Line(cx - s, cy + s, cx + s, cy - s, 3, Rgb(0x3E7BD6));
        break;
    case 'o': // circle
        c.Circle(float(cx), float(cy), s, Rgb(0xE0474C), 3);
        break;
    case 's': // square
        c.RoundRect(int(cx - s), int(cy - s), int(2 * s), int(2 * s), 1, Rgb(0xC85BB5), 3);
        break;
    case 't': // triangle
        c.Line(cx, cy - s, cx - s, cy + s * 0.8f, 3, Rgb(0x1FA77E));
        c.Line(cx - s, cy + s * 0.8f, cx + s, cy + s * 0.8f, 3, Rgb(0x1FA77E));
        c.Line(cx + s, cy + s * 0.8f, cx, cy - s, 3, Rgb(0x1FA77E));
        break;
    case 'L': case 'R': // shoulder buttons
        c.RoundRect(cx - r, cy - r * 2 / 3, 2 * r, r * 4 / 3, 8, Rgb(0xFFFFFF));
        c.RoundRect(cx - r, cy - r * 2 / 3, 2 * r, r * 4 / 3, 8, Rgb(0x9CCB8C), 2);
        bold.Draw(c, cx - bold.Measure(button == 'L' ? "L1" : "R1", 16) / 2, cy - 10, button == 'L' ? "L1" : "R1", 16, kText);
        break;
    }
}

void LibraryUi::DrawHints(Canvas& c, const std::string&)
{
    std::vector<std::pair<char, std::string>> hints;
    if (tab == kLibrary && options_open)
        hints = {{'o', "Back"}, {'x', "Select"}};
    else if (tab == kLibrary)
        hints = {{'s', "Sort"}, {'t', "Options"}, {'x', "Start"}};
    else if (tab == kSettings)
        hints = {{'x', "Change"}};
    else if (tab == kTools && usb_view)
        hints = {{'o', "Back"}, {'t', "Look again"}, {'s', "Cancel copy"}, {'x', "Copy"}};
    else if (tab == kTools)
        hints = {{'x', "Select"}};
    const int cy = kHintsY + 34;
    // left: the controller, and the shoulders that move between places
    {
        const float x = 92, y = float(cy);
        c.RoundRect(int(x - 30), int(y - 16), 60, 32, 16, kText);
        c.Circle(x - 12, y, 6, Rgb(0xFFFFFF));
        c.Circle(x + 12, y, 6, Rgb(0xFFFFFF));
        Glyph(c, 166, cy, 'L', 22);
        Glyph(c, 220, cy, 'R', 22);
        semibold.Draw(c, 252, cy - 14, "Move between places", 24, kText);
    }
    // right: this place's buttons, right-aligned
    int x = kWidth - 62;
    for (auto it = hints.rbegin(); it != hints.rend(); ++it)
    {
        const int lw = semibold.Measure(it->second, 26);
        x -= lw;
        semibold.Draw(c, x, cy - 15, it->second, 26, kText);
        x -= 34;
        Glyph(c, x, cy, it->first, 22);
        x -= 52;
    }
    if (now < toast_until && !toast.empty())
    {
        const int tw = regular.Measure(toast, 24) + 56;
        const int ty = kDockY - 74;
        c.RoundRect((kWidth - tw) / 2, ty, tw, 54, 27, Rgb(0x1E3A22, 235));
        regular.Draw(c, (kWidth - tw) / 2 + 28, ty + 14, toast, 24, Rgb(0xFFFFFF));
    }
}

// the chosen game's facts, on one line under the carousel
void LibraryUi::DrawInfo(Canvas& c, const Game& g, int y)
{
    char id[24];
    std::snprintf(id, sizeof(id), g.title_id ? "%08X" : "unknown", g.title_id);
    std::string line = std::string("Title ID ") + id + "   \xC2\xB7   " + RegionName(g.region) + "   \xC2\xB7   " + FormatSize(g.size) +
                       "   \xC2\xB7   " + (g.plays ? "Last played " + FormatDate(g.last_played) : "Never played");
    if (!g.last_result.empty())
        line += "   \xC2\xB7   " + g.last_result;
    if (IsAutostart(g))
        line += "   \xC2\xB7   Starts with the app";
    line = regular.Fit(line, 22, kWidth - 200);
    const int lw = regular.Measure(line, 22) + 48;
    c.RoundRect((kWidth - lw) / 2, y, lw, 44, 22, Rgb(0xFFFFFF, 150));
    regular.Draw(c, (kWidth - lw) / 2 + 24, y + 10, line, 22, kDim);
}

void LibraryUi::DrawLibrary(Canvas& c, double seconds)
{
    if (games.empty())
    {
        const int w = 1100, x = (kWidth - w) / 2, y = 300;
        c.RoundRect(x, y, w, 300, 30, kRaised);
        bold.Draw(c, x + 48, y + 44, "Your collection is empty", 44, kText);
        int ly = y + 120;
        for (const std::string& l : paths.help)
        {
            regular.Draw(c, x + 48, ly, regular.Fit(l, 24, w - 96), 24, kDim);
            ly += 40;
        }
        return;
    }
    // the carousel eases to where it should be
    scroll += (float(first) - scroll) * 0.3f;
    if (std::fabs(scroll - float(first)) < 0.01f)
        scroll = float(first);
    const int lo = std::max(0, int(std::floor(scroll)) - 1), hi = std::min(int(games.size()), int(std::ceil(scroll)) + kVisible + 1);
    auto draw_card = [&](int i) {
        const float slot = float(i) - scroll;
        const bool on = size_t(i) == selected;
        const int w = on ? kLargeW : kCardW, h = on ? kLargeH : kCardH;
        const int cx = kCarouselX + int(slot * float(kStepX)) + kCardW / 2;
        const int x = cx - w / 2, y = kCarouselY + (on ? -22 : 10);
        if (x + w < 0 || x > kWidth)
            return;
        const Game& g = games[size_t(i)];
        const Covers& cv = CoversFor(g);
        if (on)
        {
            const float pulse = 0.5f + 0.5f * float(std::sin(seconds * 3.0));
            c.RoundRect(x - 20, y - 20, w + 40, h + 40, 34, Rgb(0x3FE0C5, uint8_t(40 + 40 * pulse)), 10);
            c.RoundRect(x - 9, y - 9, w + 18, h + 18, 26, Rgb(0xFFFFFF));
            c.RoundRect(x - 9, y - 9, w + 18, h + 18, 26, kGlow, 4);
        }
        else
            c.RoundRect(x + 6, y + 10, w, h, 20, Rgb(0x2E6B2E, 45));
        c.Draw(on ? cv.large : cv.card, x, y, 18);
        if (IsAutostart(g)) // a play mark: starts with the app
        {
            const int px = x + w - 46, py = y + 14;
            c.Circle(float(px + 16), float(py + 16), 18, kAccent);
            c.Line(float(px + 11), float(py + 7), float(px + 11), float(py + 25), 3, Rgb(0xFFFFFF));
            c.Line(float(px + 11), float(py + 7), float(px + 25), float(py + 16), 3, Rgb(0xFFFFFF));
            c.Line(float(px + 11), float(py + 25), float(px + 25), float(py + 16), 3, Rgb(0xFFFFFF));
        }
        // the caption: the title and what it is
        const int ly = kCarouselY + kLargeH + (on ? 6 : 0) - 8, lw = kCardW + (on ? 30 : 0), lx = cx - lw / 2;
        c.RoundRect(lx, ly, lw, kCaptionH, 20, on ? Rgb(0xFFFFFF, 245) : Rgb(0xFFFFFF, 190));
        const std::string title = semibold.Fit(g.title, 23, lw - 28);
        semibold.Draw(c, cx - semibold.Measure(title, 23) / 2, ly + 14, title, 23, kText);
        std::string sub = g.sample ? "Sample program" : !g.usb.empty() ? "On USB" : g.kind;
        if (g.plays)
            sub += "  \xC2\xB7  played " + std::to_string(g.plays) + (g.plays == 1 ? " time" : " times");
        sub = regular.Fit(sub, 18, lw - 28);
        regular.Draw(c, cx - regular.Measure(sub, 18) / 2, ly + 48, sub, 18, kDim);
    };
    for (int i = lo; i < hi; i++)
        if (size_t(i) != selected)
            draw_card(i);
    draw_card(int(selected)); // on top
    // arrows when there's more either side
    const int ay = kCarouselY + kCardH / 2;
    if (first > 0)
    {
        c.Circle(58, float(ay), 26, Rgb(0xFFFFFF, 220));
        c.Line(64, float(ay - 10), 52, float(ay), 4, kAccent);
        c.Line(52, float(ay), 64, float(ay + 10), 4, kAccent);
    }
    if (first + kVisible < int(games.size()))
    {
        c.Circle(kWidth - 58, float(ay), 26, Rgb(0xFFFFFF, 220));
        c.Line(kWidth - 64, float(ay - 10), kWidth - 52, float(ay), 4, kAccent);
        c.Line(kWidth - 52, float(ay), kWidth - 64, float(ay + 10), 4, kAccent);
    }
    DrawInfo(c, games[selected], kDockY - 66);
    if (options_open)
        DrawOptions(c);
}

void LibraryUi::DrawOptions(Canvas& c)
{
    const Game& g = games[selected];
    const int w = 760, h = 300, x = (kWidth - w) / 2, y = 330;
    c.Fill(0, 0, kWidth, kHeight, Rgb(0x1E3A22, 90));
    c.RoundRect(x + 6, y + 10, w, h, 30, Rgb(0x1E3A22, 60));
    c.RoundRect(x, y, w, h, 30, Rgb(0xFFFFFF));
    bold.Draw(c, x + 36, y + 28, bold.Fit(g.title, 32, w - 72), 32, kText);
    const std::string items[2] = {IsAutostart(g) ? "Don't start this game with the app" : "Start this game when the app opens",
                                  "Close"};
    for (int i = 0; i < 2; i++)
    {
        const int iy = y + 96 + i * 80;
        if (option == i)
            c.RoundRect(x + 22, iy, w - 44, 64, 20, Rgb(0xE3F6D8));
        if (option == i)
            c.RoundRect(x + 22, iy, w - 44, 64, 20, kAccent, 3);
        semibold.Draw(c, x + 48, iy + 18, items[i], 26, option == i ? kText : kDim);
    }
}

void LibraryUi::DrawSettings(Canvas& c)
{
    static const char* kPicture[3] = {"Sharp (whole-number scale)", "Fit 4:3", "Fill the screen"};
    const std::pair<std::string, std::string> rows[5] = {
        {"Show the log while a game starts", settings.show_log ? "On" : "Off"},
        {"Picture", kPicture[settings.picture]},
        {"Library order", settings.sort ? "Recently played" : "Title"},
        {"Graphics", settings.nv2a ? "xbox-core's NV2A" : "xemu's NV2A"},
        {"Sound effects (the APU's DSPs)", settings.dsp ? "On (experimental)" : "Off"}};
    static const char* kHelp[5] = {"The first seconds of the log over the game's picture (L3 + R3 toggles it in a game).",
                                   "How the game's picture is scaled to the TV.",
                                   "Your games come first, then the samples.",
                                   "The GPU's emulation: xemu's (its Vulkan renderer) or xbox-core's own. From the next game.",
                                   "Reverb and the console's other DSP effects. Heavy: off again by itself if it can't keep up."};
    bold.Draw(c, 62, 200, "Settings", 40, kText);
    int y = 270;
    for (int i = 0; i < 5; i++)
    {
        const bool on = setting == i;
        c.RoundRect(62, y, 1796, 104, 14, on ? kRaised : kPanel);
        c.RoundRect(62, y, 1796, 104, 14, on ? kAccent : kPanelEdge, on ? 3 : 2);
        semibold.Draw(c, 96, y + 18, rows[i].first, 30, kText);
        regular.Draw(c, 96, y + 60, kHelp[i], 21, kDim);
        const std::string value = rows[i].second;
        const int vw = semibold.Measure(value, 28);
        semibold.Draw(c, 1858 - 70 - vw, y + 36, value, 28, on ? kAccent : kText);
        if (on)
        {
            c.Line(1858 - 92 - float(vw), float(y + 42), 1858 - 102 - float(vw), float(y + 52), 3, kAccent);
            c.Line(1858 - 102 - float(vw), float(y + 52), 1858 - 92 - float(vw), float(y + 62), 3, kAccent);
            c.Line(1858 - 50, float(y + 42), 1858 - 40, float(y + 52), 3, kAccent);
            c.Line(1858 - 40, float(y + 52), 1858 - 50, float(y + 62), 3, kAccent);
        }
        y += 124;
    }
}

void LibraryUi::DrawAbout(Canvas& c)
{
    c.RoundRect(62, 176, 1796, 590, 30, kPanel);
    c.RoundRect(62, 176, 1796, 590, 30, kPanelEdge, 2);
    bold.Draw(c, 100, 204, "Midway " + version, 44, kText);
    regular.Draw(c, 100, 264, "Runs original Xbox programs natively on the PS4: their code on its CPU, a kernel of its own, the GPU, sound and controllers emulated.", 24, kDim);
    int y = 324;
    semibold.Draw(c, 100, y, "ADDING GAMES", 20, kAccent);
    y += 36;
    for (const std::string& l : paths.help)
    {
        regular.Draw(c, 100, y, l, 24, kText);
        y += 36;
    }
    y += 24;
    semibold.Draw(c, 100, y, "CONTROLS IN A GAME", 20, kAccent);
    y += 36;
    for (const char* l : {"Cross, Circle, Square, Triangle: A, B, X, Y.   L1, R1: white, black.   L2, R2: the triggers.",
                          "OPTIONS: Start.   Touch pad: Back.   L3 + R3: the log on or off.   PS button: leave."})
    {
        regular.Draw(c, 100, y, l, 24, kText);
        y += 36;
    }
    y += 24;
    semibold.Draw(c, 100, y, "CREDITS", 20, kAccent);
    y += 36;
    regular.Draw(c, 100, y, "Inter typeface (SIL Open Font License 1.1); stb libraries (public domain). Not affiliated with Microsoft or Sony.", 22, kDim);
}

void LibraryUi::DrawCopyProgress(Canvas& c, int x, int y, int w)
{
    c.RoundRect(x, y, w, 120, 14, kPanel);
    c.RoundRect(x, y, w, 120, 14, kAccent, 2);
    const float f = copy.total ? float(double(copy.done) / double(copy.total)) : 0.f;
    char line[160];
    std::snprintf(line, sizeof(line), "%s / %s   %.0f%%   %.1f MB/s%s", FormatSize(copy.done).c_str(),
                  FormatSize(copy.total).c_str(), f * 100, copy.rate / 1048576.0,
                  copy.queued ? ("   (" + std::to_string(copy.queued) + " more queued)").c_str() : "");
    semibold.Draw(c, x + 28, y + 18, semibold.Fit("Copying " + copy.current, 28, w - 56), 28, kText);
    regular.Draw(c, x + 28, y + 56, line, 22, kDim);
    c.RoundRect(x + 28, y + 92, w - 56, 12, 6, Rgb(0xD5EBC9));
    if (f > 0)
        c.RoundRect(x + 28, y + 92, std::max(12, int(float(w - 56) * f)), 12, 6, kAccent);
}

void LibraryUi::DrawTools(Canvas& c)
{
    if (usb_view)
    {
        const uint64_t free = FreeSpace(paths.library.games);
        std::string drives;
        for (const std::string& u : usb_roots)
            drives += (drives.empty() ? "" : ", ") + u;
        bold.Draw(c, 62, 176, "Copy from USB to the hard drive", 36, kText);
        regular.Draw(c, 62, 226, "USB: " + (drives.empty() ? std::string("no drive found") : drives) + "     Hard drive free: " +
                                     (free ? FormatSize(free) : "unknown"), 22, kDim);
        const int list_y = 270, row_h = 64, visible = copy.busy ? 5 : 7;
        if (usb_games.empty())
        {
            regular.Draw(c, 62, list_y + 20, "No games found. Put .iso files or game folders (with default.xbe) at the top of the", 26, kDim);
            regular.Draw(c, 62, list_y + 60, "drive or in a games, xbox or xbox-core/games folder, then press Triangle.", 26, kDim);
        }
        const size_t first = usb_selected >= size_t(visible) ? usb_selected - size_t(visible) + 1 : 0;
        for (size_t i = first; i < usb_games.size() && i < first + size_t(visible); i++)
        {
            const UsbGame& g = usb_games[i];
            const int y = list_y + int(i - first) * (row_h + 8);
            const bool on = i == usb_selected;
            c.RoundRect(62, y, 1796, row_h, 12, on ? kRaised : kPanel);
            c.RoundRect(62, y, 1796, row_h, 12, on ? kAccent : kPanelEdge, on ? 3 : 2);
            semibold.Draw(c, 92, y + 16, semibold.Fit(g.name, 28, 1100), 28, kText);
            regular.Draw(c, 1260, y + 20, g.folder ? "Game folder" : "Disc image", 22, kDim);
            regular.Draw(c, 1460, y + 20, FormatSize(g.size), 22, kDim);
            const bool now_copying = copy.busy && copy.current == g.name;
            if (g.on_hdd || now_copying)
            {
                const char* tag = now_copying ? "COPYING" : "ON HDD";
                const int tw = semibold.Measure(tag, 18) + 24;
                c.RoundRect(1830 - tw, y + 17, tw, 30, 15, now_copying ? kAccent : kAccentDark);
                semibold.Draw(c, 1830 - tw + 12, y + 21, tag, 18, now_copying ? kOnAccent : kAccent);
            }
        }
        if (copy.busy)
            DrawCopyProgress(c, 62, kDockY - 140, 1796);
        return;
    }
    const std::pair<const char*, const char*> rows[2] = {
        {"Copy games from USB to the hard drive", "Disc images and game folders from a USB drive into /data/xbox-core/games."},
        {"Write a diagnostics report", "A text file about the app, your library and the last run's log, to show when asking for help."}};
    bold.Draw(c, 62, 200, "Tools", 40, kText);
    int y = 270;
    for (int i = 0; i < 2; i++)
    {
        const bool on = tool == i;
        c.RoundRect(62, y, 1796, 104, 14, on ? kRaised : kPanel);
        c.RoundRect(62, y, 1796, 104, 14, on ? kAccent : kPanelEdge, on ? 3 : 2);
        semibold.Draw(c, 96, y + 18, rows[i].first, 30, kText);
        regular.Draw(c, 96, y + 60, rows[i].second, 21, kDim);
        Glyph(c, 1858 - 50, y + 52, 'x', 22);
        y += 124;
    }
    if (copy.busy)
        DrawCopyProgress(c, 62, y + 10, 1796), y += 150;
    if (!report_written.empty())
    {
        semibold.Draw(c, 62, y + 10, "LAST REPORT", 20, kAccent);
        y += 44;
        for (const std::string& f : report_written)
        {
            regular.Draw(c, 62, y, f, 24, kText);
            y += 34;
        }
    }
}

void LibraryUi::Draw(Canvas& c, double seconds, const std::string& clock, const std::string& user)
{
    now = seconds;
    c.CopyFrom(Canvas(background.data(), kWidth, kHeight, kWidth));
    DrawHeader(c, clock, user);
    if (tab == kLibrary)
        DrawLibrary(c, seconds);
    else if (tab == kSettings)
        DrawSettings(c);
    else if (tab == kTools)
        DrawTools(c);
    else
        DrawAbout(c);
    DrawDock(c);
    DrawHints(c, user);
}

} // namespace fe
