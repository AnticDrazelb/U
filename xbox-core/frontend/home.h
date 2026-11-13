// Midway's launcher behind the HOME menu (ui/home_menu.h, New Eden's): the library made into the
// menu's titles (covers made square, samples given art), the menu's settings kept in settings.txt,
// and what its choices ask for done: a game copied from a USB drive (one at a time, its progress in
// a message), every USB game copied, the logs written to the drives, a title set to start at launch.
// Drawn into a Ui::Canvas: the PS4's screen (ps4/main_ps4.cpp) or a PNG (tools/ui-preview.cpp).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "library.h"
#include "tools.h"
#include "ui/home_menu.h"
#include "ui/loading_screen.h"

namespace fe {

struct HomePaths
{
    LibraryPaths library;          // what to scan (again after a copy)
    std::vector<std::string> usb;  // where USB drives may be mounted
    std::string settings;          // settings.txt
    std::string autostart;         // autostart.txt: a title to start without the menu
    std::string version;           // "00.01"
    std::string system;            // About: a line on where it runs
    // every log onto the USB drives (Games and USB, Triangle); returns where it was written
    std::function<std::vector<std::string>(const std::vector<Game>&)> write_logs;
};

class Home
{
public:
    Home(Settings& settings, HomePaths paths, std::string user);
    ~Home();

    enum class Result { None, Boot, Quit };
    // The buttons held now (Ui::Button bits), `now` in seconds; Boot: Chosen() is the game
    Result Update(uint32_t held, double now);
    const std::string& Chosen() const { return chosen; }
    void Draw(Ui::Canvas& c, double now, const std::string& clock, bool controller);
    const std::vector<Game>& Games() const { return games; }
    Ui::HomeMenu& Menu() { return menu; }
    // The loading screen for the chosen game: its name and tile, the menu's theme
    Ui::LoadingScreen Loading() const;

private:
    void Rescan();
    void Sort();
    void SetStatus();
    void CopyAll();
    void Queue(const UsbGame& game);

    Settings& settings;
    HomePaths paths;
    Ui::HomeMenu menu;
    std::vector<Game> games;
    std::vector<Ui::Image> covers; // by game, made square (empty: none)
    std::string autostart, chosen;
    Copier copier;
    size_t copies_queued = 0;
    bool copy_message = false;
    // the logs being written (on a thread of their own: tens of MiB read and written)
    std::thread logs_thread;
    std::atomic<bool> logs_done{false};
    std::vector<std::string> logs_written;
};

// The menu's settings from the app's, and back
Ui::MenuSettings ToMenu(const Settings& settings);
void FromMenu(const Ui::MenuSettings& menu, Settings& settings);
// A cover (portrait, as the Xbox's cases) made a square tile: the whole cover on a blurred, darkened
// copy of itself
Ui::Image SquareCover(const Ui::Image& cover, int size);

} // namespace fe
