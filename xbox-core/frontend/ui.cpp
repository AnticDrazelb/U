#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fe {
namespace {

// the palette: dark panels, a green accent
const Color kText = Rgb(0xF2F4F5), kDim = Rgb(0x9AA3AB), kFaint = Rgb(0x5E666E);
const Color kAccent = Rgb(0x84D94A), kAccentDark = Rgb(0x2F5A1C);
const Color kPanel = Rgb(0x1A1E22), kPanelEdge = Rgb(0x2C3238), kRaised = Rgb(0x262B30);

constexpr int kCols = 4, kVisibleRows = 2;
constexpr int kCardW = 228, kCardH = 318, kGridX = 62, kGridY = 238, kStepX = 258, kStepY = 356;
constexpr int kPanelX = 1100, kPanelY = 228, kPanelW = 780, kPanelH = 762;
constexpr int kLargeW = 270, kLargeH = 378;
constexpr int kHeaderH = 108, kTabsY = 116, kTabsH = 92, kHintsY = 1004;

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
    selected = 0, first_row = 0;
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
    first_row = std::max(0, int(selected) / kCols - (kVisibleRows - 1));
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
        if (in.left && s % kCols > 0) s--;
        if (in.right && s % kCols < kCols - 1 && s + 1 < n) s++;
        if (in.up && s >= kCols) s -= kCols;
        if (in.down && s + kCols < n) s += kCols;
        else if (in.down && s / kCols < (n - 1) / kCols) s = n - 1; // the last row's end
        selected = size_t(s);
        const int row = s / kCols;
        if (row < first_row) first_row = row;
        if (row >= first_row + kVisibleRows) first_row = row - kVisibleRows + 1;
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
        if (in.up) setting = (setting + 2) % 3;
        if (in.down) setting = (setting + 1) % 3;
        const int step = in.right || in.cross ? 1 : in.left ? -1 : 0;
        if (step)
        {
            if (setting == 0) settings.show_log = !settings.show_log;
            if (setting == 1) settings.picture = (settings.picture + 3 + step) % 3;
            if (setting == 2) settings.sort ^= 1, Sort();
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
    const float hue = float(Hash(g.title) % 360) / 360.f;
    c.VerticalGradient(0, 0, w, h, Hsv(hue, 0.55f, 0.42f), Hsv(hue, 0.65f, 0.16f));
    // light rays from the top corner
    for (int i = 0; i < 3; i++)
        c.Line(float(w) * (0.15f + 0.3f * float(i)), 0, float(w) * (0.6f + 0.35f * float(i)), float(h), 18.f * k,
               Rgb(0xFFFFFF, 10));
    // the top band, as a disc case's
    const int band = int(40 * k);
    c.Fill(0, 0, w, band, Rgb(0x111417, 235));
    c.Fill(0, band, w, std::max(1, int(3 * k)), kAccent);
    const std::string tag = g.sample ? "SAMPLE" : g.kind == "Disc image" ? "DISC" : "GAME";
    semibold.Draw(c, int(12 * k), int(11 * k), tag, 17 * k, kAccent);
    // the title, centred in the middle
    const float size = 30 * k;
    const auto lines = bold.Wrap(g.title, size, w - int(32 * k), 4);
    int y = (h - int(lines.size()) * int(size * 1.2f)) / 2;
    for (const std::string& l : lines)
    {
        bold.Draw(c, (w - bold.Measure(l, size)) / 2, y, l, size, kText);
        y += int(size * 1.2f);
    }
    if (g.title_id)
    {
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", g.title_id);
        regular.Draw(c, (w - regular.Measure(id, 16 * k)) / 2, h - int(34 * k), id, 16 * k, Rgb(0xFFFFFF, 140));
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

void LibraryUi::BuildBackground()
{
    background.assign(size_t(kWidth) * kHeight, 0);
    Canvas c(background.data(), kWidth, kHeight, kWidth);
    c.VerticalGradient(0, 0, kWidth, kHeight, Rgb(0x1E2226), Rgb(0x0C0E10));
    // a soft diagonal band of light across the top right
    for (int y = 0; y < kHeight; y++)
        for (int x = 900; x < kWidth; x++)
        {
            const float d = float(x) - 0.55f * float(y) - 1450.f;
            if (std::fabs(d) < 220)
                c.Blend(x, y, Rgb(0xFFFFFF), 0.035f * (1 - std::fabs(d) / 220.f));
        }
    c.Fill(0, 0, kWidth, kHeaderH, Rgb(0x111316, 225));
    c.Fill(0, kHeaderH, kWidth, 2, kPanelEdge);
    c.Fill(0, kTabsY + kTabsH, kWidth, 2, Rgb(0x23282D));
    c.Fill(0, kHintsY, kWidth, kHeight - kHintsY, Rgb(0x111316, 235));
    c.Fill(0, kHintsY, kWidth, 2, kPanelEdge);
}

void LibraryUi::DrawHeader(Canvas& c, const std::string& clock)
{
    // the emblem: a rounded green tile with the initials
    c.RoundRect(44, 22, 64, 64, 16, kAccentDark);
    c.RoundRect(44, 22, 64, 64, 16, kAccent, 3);
    bold.Draw(c, 44 + (64 - bold.Measure("xc", 30)) / 2, 36, "xc", 30, kAccent);
    semibold.Draw(c, 132, 22, "xbox-core", 44, kText);
    regular.Draw(c, 134 + semibold.Measure("xbox-core", 44) + 18, 40, "Original Xbox on PS4", 24, kDim);
    const int cw = semibold.Measure(clock, 40);
    semibold.Draw(c, kWidth - 60 - cw, 28, clock, 40, kText);
    const std::string v = "v" + version;
    regular.Draw(c, kWidth - 60 - cw - 40 - regular.Measure(v, 22), 42, v, 22, kFaint);

    static const char* kNames[kTabs] = {"LIBRARY", "SETTINGS", "TOOLS", "ABOUT"};
    for (int i = 0; i < kTabs; i++)
    {
        const int x = 60 + i * 200, w = 190, cx = x + w / 2;
        const bool on = tab == i;
        if (on)
        {
            c.RoundRect(x, kTabsY, w, kTabsH, 10, kRaised);
            c.Fill(x, kTabsY + kTabsH - 6, w, 6, kAccent);
        }
        const Color ic = on ? kText : kDim;
        const int iy = kTabsY + 14;
        if (i == kLibrary) // books on a shelf
        {
            c.RoundRect(cx - 20, iy, 9, 30, 2, ic);
            c.RoundRect(cx - 7, iy, 9, 30, 2, ic);
            c.Line(float(cx + 8), float(iy + 2), float(cx + 18), float(iy + 29), 8, ic);
        }
        else if (i == kSettings) // a gear
        {
            for (int t = 0; t < 8; t++)
            {
                const float a = float(t) * 3.14159f / 4;
                c.Line(cx + std::cos(a) * 10, iy + 15 + std::sin(a) * 10, cx + std::cos(a) * 17, iy + 15 + std::sin(a) * 17, 6, ic);
            }
            c.Circle(float(cx), float(iy + 15), 12, ic, 5);
        }
        else if (i == kTools) // a wrench
        {
            c.Line(float(cx - 13), float(iy + 28), float(cx + 6), float(iy + 9), 7, ic);
            c.Circle(float(cx + 9), float(iy + 6), 9, ic, 4);
            c.Fill(cx + 9, iy - 4, 9, 9, on ? kRaised : Rgb(0x15181B));
        }
        else // a circled i
        {
            c.Circle(float(cx), float(iy + 15), 16, ic);
            c.Fill(cx - 2, iy + 13, 5, 11, on ? kRaised : Rgb(0x15181B));
            c.Fill(cx - 2, iy + 6, 5, 5, on ? kRaised : Rgb(0x15181B));
        }
        const int lw = semibold.Measure(kNames[i], 22);
        semibold.Draw(c, cx - lw / 2, kTabsY + 54, kNames[i], 22, on ? kText : kDim);
    }
}

void LibraryUi::Glyph(Canvas& c, int cx, int cy, char button, int r)
{
    c.Circle(float(cx), float(cy), float(r), Rgb(0x2A2F34));
    c.Circle(float(cx), float(cy), float(r), Rgb(0x3A4148), 2);
    const float s = float(r) * 0.45f;
    switch (button)
    {
    case 'x': // cross
        c.Line(cx - s, cy - s, cx + s, cy + s, 3, Rgb(0x7DB4F0));
        c.Line(cx - s, cy + s, cx + s, cy - s, 3, Rgb(0x7DB4F0));
        break;
    case 'o': // circle
        c.Circle(float(cx), float(cy), s, Rgb(0xF07070), 3);
        break;
    case 's': // square
        c.RoundRect(int(cx - s), int(cy - s), int(2 * s), int(2 * s), 1, Rgb(0xE79AD8), 3);
        break;
    case 't': // triangle
        c.Line(cx, cy - s, cx - s, cy + s * 0.8f, 3, Rgb(0x4FD9A6));
        c.Line(cx - s, cy + s * 0.8f, cx + s, cy + s * 0.8f, 3, Rgb(0x4FD9A6));
        c.Line(cx + s, cy + s * 0.8f, cx, cy - s, 3, Rgb(0x4FD9A6));
        break;
    case 'L': case 'R': // shoulder buttons
        c.RoundRect(cx - r, cy - r * 2 / 3, 2 * r, r * 4 / 3, 6, Rgb(0x2A2F34));
        bold.Draw(c, cx - bold.Measure(button == 'L' ? "L1" : "R1", 16) / 2, cy - 10, button == 'L' ? "L1" : "R1", 16, kText);
        break;
    }
}

void LibraryUi::DrawHints(Canvas& c, const std::string& user)
{
    std::vector<std::pair<char, std::string>> hints;
    if (tab == kLibrary && options_open)
        hints = {{'x', "SELECT"}, {'o', "BACK"}};
    else if (tab == kLibrary)
        hints = {{'x', "PLAY"}, {'t', "OPTIONS"}, {'s', "SORT"}, {'L', ""}, {'R', "TABS"}};
    else if (tab == kSettings)
        hints = {{'x', "CHANGE"}, {'L', ""}, {'R', "TABS"}};
    else if (tab == kTools && usb_view)
        hints = {{'x', "COPY"}, {'s', "CANCEL COPY"}, {'t', "LOOK AGAIN"}, {'o', "BACK"}};
    else if (tab == kTools)
        hints = {{'x', "SELECT"}, {'L', ""}, {'R', "TABS"}};
    else
        hints = {{'L', ""}, {'R', "TABS"}};
    int x = 62;
    const int cy = kHintsY + 38;
    for (const auto& [button, label] : hints)
    {
        Glyph(c, x + 20, cy, button);
        x += 50;
        if (!label.empty())
            x += semibold.Draw(c, x, cy - 13, label, 26, kText) + 46;
        else
            x -= 4;
    }
    // the player
    const int uw = semibold.Measure(user, 28);
    const int ux = kWidth - 60 - uw;
    semibold.Draw(c, ux, cy - 15, user, 28, kText);
    c.Circle(float(ux - 34), float(cy - 8), 9, kText);
    c.RoundRect(ux - 52, cy + 3, 36, 18, 9, kText);
    if (now < toast_until && !toast.empty())
    {
        const int tw = regular.Measure(toast, 24) + 48;
        c.RoundRect((kWidth - tw) / 2, kHintsY - 70, tw, 52, 26, Rgb(0x2B3036, 245));
        c.RoundRect((kWidth - tw) / 2, kHintsY - 70, tw, 52, 26, kAccent, 2);
        regular.Draw(c, (kWidth - tw) / 2 + 24, kHintsY - 57, toast, 24, kText);
    }
}

void LibraryUi::DrawDetails(Canvas& c, const Game& g)
{
    c.RoundRect(kPanelX, kPanelY, kPanelW, kPanelH, 18, kPanel);
    c.RoundRect(kPanelX, kPanelY, kPanelW, kPanelH, 18, kPanelEdge, 2);
    const Covers& cv = CoversFor(g);
    const int px = kPanelX + 28, py = kPanelY + 28;
    c.RoundRect(px + 6, py + 8, kLargeW, kLargeH, 10, Rgb(0x000000, 120));
    c.Draw(cv.large, px, py, 10);
    // the title and what kind of file it is
    const int tx = px + kLargeW + 30, tw = kPanelX + kPanelW - 28 - tx;
    int y = py - 4;
    for (const std::string& l : bold.Wrap(g.title, 40, tw, 3))
    {
        bold.Draw(c, tx, y, l, 40, kText);
        y += 48;
    }
    y += 6;
    regular.Draw(c, tx, y, regular.Fit(g.kind + "  \xC2\xB7  " + g.file_name, 22, tw), 22, kDim);
    y += 46;
    c.Fill(tx, y, tw, 2, kPanelEdge);
    y += 20;
    char id[24];
    std::snprintf(id, sizeof(id), g.title_id ? "%08X" : "Unknown", g.title_id);
    const std::pair<const char*, std::string> facts[] = {
        {"TITLE ID", id}, {"REGION", RegionName(g.region)}, {"SIZE", FormatSize(g.size)}};
    for (const auto& [label, value] : facts)
    {
        semibold.Draw(c, tx, y, label, 19, kAccent);
        regular.Draw(c, tx, y + 24, regular.Fit(value, 24, tw), 24, kText);
        y += 64;
    }
    // history, under the cover
    y = py + kLargeH + 26;
    const std::string played = g.plays ? FormatDate(g.last_played) + "  (" + std::to_string(g.plays) + (g.plays == 1 ? " run)" : " runs)") : "Never";
    const std::pair<const char*, std::string> history[] = {
        {"LAST PLAYED", played}, {"LAST RUN", g.last_result.empty() ? "-" : g.last_result}};
    for (const auto& [label, value] : history)
    {
        semibold.Draw(c, px, y, label, 19, kAccent);
        regular.Draw(c, px + 190, y - 3, regular.Fit(value, 24, kPanelW - 56 - 190), 24, kText);
        y += 38;
    }
    // the actions
    const int bx = px, bw = kPanelW - 56;
    int by = kPanelY + kPanelH - 26 - 3 * 62 - 2 * 12;
    auto button = [&](const std::string& label, char glyph, bool primary) {
        c.RoundRect(bx, by, bw, 62, 12, primary ? Rgb(0x23301C) : kRaised);
        c.RoundRect(bx, by, bw, 62, 12, primary ? kAccent : Rgb(0x353B42), primary ? 4 : 2);
        semibold.Draw(c, bx + 32, by + 15, label, 29, kText);
        Glyph(c, bx + bw - 42, by + 31, glyph, 21);
        by += 74;
    };
    button("PLAY NOW", 'x', true);
    button(IsAutostart(g) ? "STARTS WITH THE APP" : "OPTIONS", 't', false);
    button(settings.sort ? "SORT: RECENTLY PLAYED" : "SORT: TITLE", 's', false);
}

void LibraryUi::DrawLibrary(Canvas& c, double seconds)
{
    if (games.empty())
    {
        bold.Draw(c, 62, 280, "No games yet", 48, kText);
        int y = 360;
        for (const std::string& l : paths.help)
        {
            regular.Draw(c, 62, y, l, 26, kDim);
            y += 40;
        }
        return;
    }
    const int rows = (int(games.size()) + kCols - 1) / kCols;
    for (int r = first_row; r < std::min(rows, first_row + kVisibleRows); r++)
        for (int col = 0; col < kCols; col++)
        {
            const size_t i = size_t(r * kCols + col);
            if (i >= games.size())
                break;
            const int x = kGridX + col * kStepX, y = kGridY + (r - first_row) * kStepY;
            c.RoundRect(x + 5, y + 8, kCardW, kCardH, 12, Rgb(0x000000, 110));
            c.Draw(CoversFor(games[i]).card, x, y, 10);
            if (i == selected)
            {
                // a glowing frame, breathing slowly
                const float pulse = 0.5f + 0.5f * float(std::sin(seconds * 3.0));
                c.RoundRect(x - 14, y - 14, kCardW + 28, kCardH + 28, 22, Rgb(0x84D94A, uint8_t(40 + 50 * pulse)), 6);
                c.RoundRect(x - 9, y - 9, kCardW + 18, kCardH + 18, 18, kAccent, 6);
            }
            if (IsAutostart(games[i]))
            {
                c.RoundRect(x + kCardW - 40, y + kCardH - 40, 30, 30, 15, kAccent);
                c.Line(float(x + kCardW - 30), float(y + kCardH - 33), float(x + kCardW - 30), float(y + kCardH - 17), 3, Rgb(0x111316));
                c.Line(float(x + kCardW - 30), float(y + kCardH - 33), float(x + kCardW - 18), float(y + kCardH - 25), 3, Rgb(0x111316));
                c.Line(float(x + kCardW - 30), float(y + kCardH - 17), float(x + kCardW - 18), float(y + kCardH - 25), 3, Rgb(0x111316));
            }
        }
    // where in the list: a scroll bar, and the count
    if (rows > kVisibleRows)
    {
        const int track = kVisibleRows * kStepY - 30, top = kGridY;
        const int knob = std::max(60, track * kVisibleRows / rows);
        const int at = top + (track - knob) * first_row / std::max(1, rows - kVisibleRows);
        c.RoundRect(1068, top, 8, track, 4, Rgb(0x2A2F34));
        c.RoundRect(1068, at, 8, knob, 4, kAccent);
    }
    const std::string count = std::to_string(selected + 1) + " / " + std::to_string(games.size());
    regular.Draw(c, 1060 - regular.Measure(count, 22), kTabsY + kTabsH + 2 - 34, count, 22, kFaint);
    DrawDetails(c, games[selected]);
    if (options_open)
        DrawOptions(c);
}

void LibraryUi::DrawOptions(Canvas& c)
{
    const Game& g = games[selected];
    const int w = 700, h = 300, x = kPanelX + (kPanelW - w) / 2, y = kPanelY + 240;
    c.Fill(0, kHeaderH, kWidth, kHintsY - kHeaderH, Rgb(0x000000, 120));
    c.RoundRect(x, y, w, h, 18, Rgb(0x20252A));
    c.RoundRect(x, y, w, h, 18, kAccent, 2);
    bold.Draw(c, x + 32, y + 26, bold.Fit(g.title, 32, w - 64), 32, kText);
    const std::string items[2] = {IsAutostart(g) ? "Don't start this game with the app" : "Start this game when the app opens",
                                  "Close"};
    for (int i = 0; i < 2; i++)
    {
        const int iy = y + 92 + i * 76;
        if (option == i)
        {
            c.RoundRect(x + 20, iy, w - 40, 62, 10, kRaised);
            c.RoundRect(x + 20, iy, w - 40, 62, 10, kAccent, 3);
        }
        semibold.Draw(c, x + 44, iy + 17, items[i], 26, option == i ? kText : kDim);
    }
}

void LibraryUi::DrawSettings(Canvas& c)
{
    static const char* kPicture[3] = {"Sharp (whole-number scale)", "Fit 4:3", "Fill the screen"};
    const std::pair<std::string, std::string> rows[3] = {
        {"Show the log while a game starts", settings.show_log ? "On" : "Off"},
        {"Picture", kPicture[settings.picture]},
        {"Library order", settings.sort ? "Recently played" : "Title"}};
    static const char* kHelp[3] = {"The first seconds of the log over the game's picture (L3 + R3 toggles it in a game).",
                                   "How the game's picture is scaled to the TV.",
                                   "Your games come first, then the samples."};
    int y = 250;
    for (int i = 0; i < 3; i++)
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
    c.RoundRect(62, 240, 1796, 744, 18, kPanel);
    c.RoundRect(62, 240, 1796, 744, 18, kPanelEdge, 2);
    bold.Draw(c, 100, 272, "xbox-core " + version, 44, kText);
    regular.Draw(c, 100, 332, "Runs original Xbox programs natively on the PS4: their code on its CPU, a kernel of its own, the GPU, sound and controllers emulated.", 24, kDim);
    int y = 400;
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
    c.RoundRect(x + 28, y + 92, w - 56, 12, 6, Rgb(0x2A2F34));
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
        bold.Draw(c, 62, 236, "Copy from USB to the hard drive", 36, kText);
        regular.Draw(c, 62, 286, "USB: " + (drives.empty() ? std::string("no drive found") : drives) + "     Hard drive free: " +
                                     (free ? FormatSize(free) : "unknown"), 22, kDim);
        const int list_y = 330, row_h = 64, visible = copy.busy ? 8 : 9;
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
                semibold.Draw(c, 1830 - tw + 12, y + 21, tag, 18, now_copying ? Rgb(0x111316) : kAccent);
            }
        }
        if (copy.busy)
            DrawCopyProgress(c, 62, kHintsY - 214, 1796);
        return;
    }
    const std::pair<const char*, const char*> rows[2] = {
        {"Copy games from USB to the hard drive", "Disc images and game folders from a USB drive into /data/xbox-core/games."},
        {"Write a diagnostics report", "A text file about the app, your library and the last run's log, to show when asking for help."}};
    int y = 250;
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
    DrawHeader(c, clock);
    if (tab == kLibrary)
        DrawLibrary(c, seconds);
    else if (tab == kSettings)
        DrawSettings(c);
    else if (tab == kTools)
        DrawTools(c);
    else
        DrawAbout(c);
    DrawHints(c, user);
}

} // namespace fe
