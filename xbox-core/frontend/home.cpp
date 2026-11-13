#include "home.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fe {

namespace {

std::string Upper(std::string s)
{
    for (char& c : s)
        c = char(std::toupper(static_cast<unsigned char>(c)));
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

std::vector<unsigned char> ReadFile(const std::string& file, size_t limit)
{
    std::vector<unsigned char> out;
    if (FILE* f = std::fopen(file.c_str(), "rb"))
    {
        unsigned char buffer[1 << 16];
        size_t n;
        while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0 && out.size() < limit)
            out.insert(out.end(), buffer, buffer + n);
        std::fclose(f);
    }
    return out;
}

// the package's samples: art for the ones it has a picture for (by file name), else a cover
Ui::Art SampleArt(const Game& g)
{
    const std::string name = Upper(g.file_name);
    if (name.find("TRIANGLE") != std::string::npos || name.find("MESH") != std::string::npos ||
        name.find("GAMMA") != std::string::npos || name.find("TEST-TEX") != std::string::npos ||
        name.find("TEST-BLIT") != std::string::npos || name.find("TEST-FF") != std::string::npos ||
        name.find("TEST-RT") != std::string::npos || name.find("TEST-LINES") != std::string::npos)
        return Ui::Art::Gpu;
    if (name.find("GAMECONTROLLER") != std::string::npos || name == "USB")
        return Ui::Art::Controller;
    if (name.find("HELLO") != std::string::npos || name.find("WINAPI") != std::string::npos ||
        name.find("TEST-") == 0)
        return Ui::Art::Terminal;
    return Ui::Art::None;
}

uint32_t Channel(uint32_t p, int shift) { return p >> shift & 0xFF; }

// bilinear, for scaling the blurred background up smoothly
Ui::Image Upscale(const Ui::Image& image, int width, int height)
{
    Ui::Image out;
    out.width = width, out.height = height;
    out.pixels.resize(size_t(width) * height);
    for (int y = 0; y < height; y++)
    {
        const float fy = std::clamp((y + 0.5f) * image.height / height - 0.5f, 0.0f, float(image.height - 1));
        const int y0 = int(fy), y1 = std::min(y0 + 1, image.height - 1);
        const float ty = fy - y0;
        for (int x = 0; x < width; x++)
        {
            const float fx = std::clamp((x + 0.5f) * image.width / width - 0.5f, 0.0f, float(image.width - 1));
            const int x0 = int(fx), x1 = std::min(x0 + 1, image.width - 1);
            const float tx = fx - x0;
            const uint32_t a = image.pixels[size_t(y0) * image.width + x0], b = image.pixels[size_t(y0) * image.width + x1];
            const uint32_t c = image.pixels[size_t(y1) * image.width + x0], d = image.pixels[size_t(y1) * image.width + x1];
            uint32_t p = 0xFF000000u;
            for (int s = 0; s < 24; s += 8)
            {
                const float top = Channel(a, s) + (float(Channel(b, s)) - Channel(a, s)) * tx;
                const float bottom = Channel(c, s) + (float(Channel(d, s)) - Channel(c, s)) * tx;
                p |= uint32_t(std::lround(top + (bottom - top) * ty)) << s;
            }
            out.pixels[size_t(y) * width + x] = p;
        }
    }
    return out;
}

} // namespace

Ui::Image SquareCover(const Ui::Image& cover, int size)
{
    if (cover.Empty() || size <= 0)
        return {};
    // the background: the cover's middle square, blurred (made 6x6 and scaled back up), darkened
    const int side = std::min(cover.width, cover.height);
    Ui::Image middle;
    middle.width = middle.height = side;
    middle.pixels.resize(size_t(side) * side);
    const int x0 = (cover.width - side) / 2, y0 = (cover.height - side) / 2;
    for (int y = 0; y < side; y++)
        std::memcpy(&middle.pixels[size_t(y) * side], &cover.pixels[size_t(y0 + y) * cover.width + x0],
                    size_t(side) * sizeof(uint32_t));
    Ui::Image out = Upscale(Ui::ResizeImage(middle, 6, 6), size, size);
    for (uint32_t& p : out.pixels)
        p = 0xFF000000u | (Channel(p, 16) * 5 / 10) << 16 | (Channel(p, 8) * 5 / 10) << 8 | Channel(p, 0) * 5 / 10;
    // the whole cover on it, as large as fits
    const int w = cover.width >= cover.height ? size : std::max(1, cover.width * size / cover.height);
    const int h = cover.width >= cover.height ? std::max(1, cover.height * size / cover.width) : size;
    const Ui::Image fitted = Ui::ResizeImage(cover, w, h);
    const int left = (size - w) / 2, top = (size - h) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            const uint32_t p = fitted.pixels[size_t(y) * w + x];
            const uint32_t alpha = p >> 24;
            uint32_t& q = out.pixels[size_t(top + y) * size + left + x];
            if (alpha == 255)
                q = p;
            else if (alpha)
            {
                uint32_t r = 0xFF000000u;
                for (int s = 0; s < 24; s += 8)
                    r |= (Channel(p, s) * alpha + Channel(q, s) * (255 - alpha)) / 255 << s;
                q = r;
            }
        }
    out.opaque = true;
    return out;
}

Ui::MenuSettings ToMenu(const Settings& s)
{
    Ui::MenuSettings m;
    m.white_theme = s.theme == 1;
    m.retro_theme = s.theme == 2;
    m.nv2a = s.nv2a;
    m.picture = s.picture;
    m.show_log = s.show_log;
    m.dsp = s.dsp;
    m.sort = s.sort;
    if (!s.nickname.empty())
        m.nickname = s.nickname;
    m.avatar = s.avatar;
    m.avatar_color = s.avatar_color;
    return m;
}

void FromMenu(const Ui::MenuSettings& m, Settings& s)
{
    s.theme = m.retro_theme ? 2 : m.white_theme ? 1 : 0;
    s.nv2a = m.nv2a;
    s.picture = m.picture;
    s.show_log = m.show_log;
    s.dsp = m.dsp;
    s.sort = m.sort;
    s.nickname = m.nickname;
    s.avatar = m.avatar;
    s.avatar_color = m.avatar_color;
}

Home::Home(Settings& settings_, HomePaths paths_, std::string user) : settings(settings_), paths(std::move(paths_))
{
    // the nickname: the PS4 user's name until one is chosen (the menu's limit is the Switch's)
    if (settings.nickname.empty() && !user.empty())
        settings.nickname = user.substr(0, Ui::kNicknameLength);
    menu.SetSettings(ToMenu(settings));
    menu.SetInfo(paths.version, paths.system);
    menu.SetNews({
        {"Midway " + paths.version,
         "An original Xbox emulator for the PS4. The Xbox's own code runs on the PS4's CPU as it is (both are x86), "
         "with a kernel of Midway's own; the Xbox's GPU, sound chip, USB and network are xemu's, its GPU drawn by "
         "the PS4's through Vulkan."},
        {"This HOME menu",
         "New Eden's, made Midway's: the row of your games (their covers, when there are), All Software, and "
         "the buttons below. OPTIONS on a game: what it is, a copy from a USB drive to the PS4, or starting it "
         "as soon as Midway opens. System Settings has the themes: Basic Black, Basic White and Retro."},
        {"Faster drawing",
         "What the Xbox's GPU draws stays on the PS4's GPU until the game's own code looks at it (most never do), "
         "instead of being copied back after every frame: several times the frames of before in 3D games. "
         "Shaders are kept in one file (hdd/shaders.pack), so a game that has run before starts without "
         "building them again."},
        {"Where games go",
         "/data/xbox-core/games: a folder with the game's default.xbe and its files, or a disc image (.iso). On a "
         "USB drive (its top folder, or games/, xbox/ or xbox-core/games/) they're listed too, and can be copied "
         "to the PS4 from here. A cover: cover.jpg or cover.png in the game's folder, <name>.jpg beside an .iso, "
         "or /data/xbox-core/covers/<title ID>.jpg."},
        {"When something goes wrong",
         "Every log goes to xbox-core-logs.txt at the top of each USB drive: as the app opens, every minute while a "
         "game runs, and when it crashes. Games and USB, Triangle writes it at once. That file is what to send."},
    });
    autostart = ReadFirstLine(paths.autostart);
    Rescan();
}

Home::~Home()
{
    if (logs_thread.joinable())
        logs_thread.join();
}

void Home::Sort()
{
    std::vector<size_t> order(games.size());
    for (size_t i = 0; i < order.size(); i++)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [this](size_t ia, size_t ib) {
        const Game &a = games[ia], &b = games[ib];
        if (a.sample != b.sample) // the player's games before the samples
            return !a.sample;
        if (settings.sort == 1 && a.last_played != b.last_played)
            return a.last_played > b.last_played;
        return Upper(a.title) < Upper(b.title);
    });
    std::vector<Game> sorted_games;
    std::vector<Ui::Image> sorted_covers;
    for (size_t i : order)
    {
        sorted_games.push_back(std::move(games[i]));
        sorted_covers.push_back(std::move(covers[i]));
    }
    games = std::move(sorted_games);
    covers = std::move(sorted_covers);

    std::vector<Ui::Title> titles;
    for (size_t i = 0; i < games.size(); i++)
    {
        const Game& g = games[i];
        Ui::Title t;
        t.path = g.path;
        t.name = g.title;
        t.kind = g.kind;
        t.size = g.size ? FormatSize(g.size) : "";
        t.location = g.sample ? "in the package" : g.usb.empty() ? "on the PS4" : "on " + g.usb;
        if (g.title_id)
        {
            char id[64];
            std::snprintf(id, sizeof(id), "Title ID %08X", g.title_id);
            t.details = id;
            if (g.region)
                t.details += ", " + RegionName(g.region);
        }
        if (g.plays)
            t.details += (t.details.empty() ? "" : "\n") + std::string("Last played ") + FormatDate(g.last_played) +
                         (g.last_result.empty() ? "" : ": " + g.last_result);
        t.on_usb = !g.usb.empty();
        t.autostart = !autostart.empty() && autostart == g.path;
        t.art = g.sample ? SampleArt(g) : Ui::Art::None;
        t.icon = covers[i];
        titles.push_back(std::move(t));
    }
    menu.SetTitles(std::move(titles));
}

void Home::Rescan()
{
    paths.library.usb_drives = UsbRoots(paths.usb);
    games = ScanLibrary(paths.library);
    // a game on a drive that's on the PS4 already (copied) is listed once, as the PS4's
    const std::vector<UsbGame> usb = FindUsbGames(paths.library.usb_drives, paths.library.games);
    games.erase(std::remove_if(games.begin(), games.end(),
                               [&](const Game& g) {
                                   return !g.usb.empty() && std::any_of(usb.begin(), usb.end(), [&](const UsbGame& u) {
                                       return u.on_hdd && (g.path == u.source ||
                                                           g.path.compare(0, u.source.size() + 1, u.source + "/") == 0);
                                   });
                               }),
                games.end());
    covers.assign(games.size(), Ui::Image{});
    for (size_t i = 0; i < games.size(); i++)
    {
        if (games[i].cover.empty())
            continue;
        const std::vector<unsigned char> data = ReadFile(games[i].cover, 16u << 20);
        if (!data.empty())
            covers[i] = SquareCover(Ui::DecodeImage(data.data(), data.size()), 384); // a tile at 1.5x
    }
    Sort();
    SetStatus();
}

void Home::SetStatus()
{
    std::vector<Ui::StatusRow> rows;
    size_t on_ps4 = 0, with_cover = 0, players = 0;
    for (const Game& g : games)
    {
        on_ps4 += !g.sample && g.usb.empty();
        players += !g.sample;
        with_cover += !g.sample && !g.cover.empty();
    }
    rows.push_back({"Games on the PS4",
                    std::to_string(on_ps4) + (on_ps4 == 1 ? " game in " : " games in ") + paths.library.games, on_ps4 > 0});
    const std::vector<std::string> roots = UsbRoots(paths.usb);
    const std::vector<UsbGame> usb = FindUsbGames(roots, paths.library.games);
    if (roots.empty())
        rows.push_back({"No USB drive", "A drive's games are listed with the others, to start from it or copy to the PS4.", false});
    for (const std::string& root : roots)
    {
        size_t found = 0, new_ones = 0;
        for (const UsbGame& u : usb)
            if (u.source.compare(0, root.size() + 1, root + "/") == 0)
                found++, new_ones += !u.on_hdd;
        rows.push_back({"USB drive: " + root,
                        std::to_string(found) + (found == 1 ? " game" : " games") +
                            (found ? ", " + std::to_string(new_ones) + " not on the PS4 yet" : std::string()) +
                            ". Free on the PS4: " + FormatSize(FreeSpace(paths.library.games)),
                        true});
    }
    rows.push_back({"Covers", std::to_string(with_cover) + " of " + std::to_string(players) + " games have one", with_cover > 0});
    menu.SetGamesStatus(std::move(rows),
                        "Games go in " + paths.library.games + ": a folder with the game's default.xbe and its files, or a "
                        "disc image (.iso). On a USB drive: its top folder, or games/, xbox/ or xbox-core/games/. "
                        "Square copies every game on the drives that isn't on the PS4 yet (OPTIONS on a game copies "
                        "that one). Triangle writes every log to xbox-core-logs.txt on the drives, to send when "
                        "something goes wrong.");
}

void Home::Queue(const UsbGame& game)
{
    copier.Queue(game, paths.library.games);
    copies_queued++;
}

void Home::CopyAll()
{
    const std::vector<UsbGame> usb = FindUsbGames(UsbRoots(paths.usb), paths.library.games);
    size_t queued = 0;
    for (const UsbGame& u : usb)
        if (!u.on_hdd)
            Queue(u), queued++;
    if (queued == 0)
        menu.ShowMessage("Copy from USB", usb.empty() ? "No games on a USB drive." : "Every game on the USB drives is on the PS4 already.");
    else
    {
        menu.ShowMessage("Copy from USB", "Copying " + std::to_string(queued) + (queued == 1 ? " game..." : " games..."));
        copy_message = true;
    }
}

Home::Result Home::Update(uint32_t held, double now)
{
    // a copy's progress in its message, and the library read again when one finishes
    const Copier::Status copy = copier.Poll();
    if (!copy.finished.empty())
    {
        std::string report;
        for (const std::string& f : copy.finished)
            report += (report.empty() ? "" : "\n") +
                      (f.find(": ") == std::string::npos ? f + " is on the PS4 now." : "Not copied: " + f);
        Rescan();
        if (!copy.busy)
        {
            copies_queued = 0;
            if (!copy_message || !menu.UpdateMessage("Copy from USB", report))
                menu.ShowMessage("Copy from USB", report);
            copy_message = false;
        }
    }
    if (copy.busy && copy_message)
    {
        char progress[256];
        std::snprintf(progress, sizeof(progress), "Copying %s: %s of %s (%.0f MB/s)%s", copy.current.c_str(),
                      FormatSize(copy.done).c_str(), FormatSize(copy.total).c_str(), copy.rate / 1e6,
                      copy.queued ? (", then " + std::to_string(copy.queued) + " more").c_str() : "");
        menu.UpdateMessage("Copy from USB", progress);
    }
    if (logs_done.exchange(false))
    {
        logs_thread.join();
        std::string where;
        for (const std::string& w : logs_written)
            where += "\n" + w;
        menu.UpdateMessage("Write Diagnostics", logs_written.empty()
                                                    ? "No USB drive took them. They're in /data/xbox-core."
                                                    : "Every log, in one file:" + where);
    }

    const Ui::MenuAction action = menu.Update(held, now);
    using Kind = Ui::MenuAction::Kind;
    switch (action.kind)
    {
    case Kind::Boot:
        if (copy.busy)
        {
            menu.ShowMessage("Copy from USB", "Still copying: start it once the copy is done.");
            copy_message = true;
            break;
        }
        chosen = action.path;
        return Result::Boot;
    case Kind::Quit:
        return Result::Quit;
    case Kind::SettingsChanged: {
        const int sort = settings.sort;
        FromMenu(menu.Settings(), settings);
        settings.Save(paths.settings);
        if (settings.sort != sort)
            Sort();
        break;
    }
    case Kind::CopyToPs4: {
        const std::vector<UsbGame> usb = FindUsbGames(UsbRoots(paths.usb), paths.library.games);
        const auto it = std::find_if(usb.begin(), usb.end(), [&](const UsbGame& u) {
            return action.path == u.source || action.path.compare(0, u.source.size() + 1, u.source + "/") == 0;
        });
        if (it == usb.end())
            menu.ShowMessage("Copy from USB", "That game isn't on a USB drive any more.");
        else if (it->on_hdd)
            menu.ShowMessage("Copy from USB", it->name + " is on the PS4 already.");
        else
        {
            Queue(*it);
            menu.ShowMessage("Copy from USB", "Copying " + it->name + "...");
            copy_message = true;
        }
        break;
    }
    case Kind::ImportUsb:
        CopyAll();
        break;
    case Kind::Autostart:
        if (action.path.empty())
            std::remove(paths.autostart.c_str());
        else if (FILE* f = std::fopen(paths.autostart.c_str(), "w"))
        {
            std::fprintf(f, "%s\n", action.path.c_str());
            std::fclose(f);
        }
        autostart = ReadFirstLine(paths.autostart);
        Sort();
        menu.ShowMessage(action.path.empty() ? "Menu at Launch" : "Start at Launch",
                         action.path.empty() ? "Midway opens with this menu again."
                                             : "This game starts as soon as Midway opens. Hold L1 while it opens for "
                                               "this menu (OPTIONS on the game, Menu at Launch, undoes it).");
        break;
    case Kind::Diagnostics:
        if (!paths.write_logs)
            menu.ShowMessage("Write Diagnostics", "Not on this system.");
        else if (!logs_thread.joinable())
        {
            menu.ShowMessage("Write Diagnostics", "Writing every log to the USB drives...");
            logs_thread = std::thread([this, list = games] {
                logs_written = paths.write_logs(list);
                logs_done = true;
            });
        }
        break;
    case Kind::None:
        break;
    }
    return Result::None;
}

void Home::Draw(Ui::Canvas& c, double now, const std::string& clock, bool controller)
{
    menu.SetClock(clock);
    menu.SetController(controller);
    menu.Draw(c, now);
}

Ui::LoadingScreen Home::Loading() const
{
    Ui::LoadingScreen screen;
    screen.white_theme = settings.theme == 1;
    screen.retro_theme = settings.theme == 2;
    for (size_t i = 0; i < games.size(); i++)
    {
        if (games[i].path != chosen)
            continue;
        screen.name = games[i].title;
        screen.publisher = games[i].kind;
        screen.icon = covers[i];
        if (screen.icon.Empty())
        {
            const Ui::Art art = games[i].sample ? SampleArt(games[i]) : Ui::Art::None;
            screen.icon = art != Ui::Art::None
                              ? Ui::MakeTileArt(art, 384)
                              : Ui::MakeCover(games[i].title, games[i].kind, 384,
                                              screen.retro_theme ? Ui::FontFamily::Pixel : Ui::FontFamily::Inter);
        }
    }
    if (screen.name.empty())
        screen.name = chosen.substr(chosen.find_last_of('/') + 1);
    return screen;
}

} // namespace fe
