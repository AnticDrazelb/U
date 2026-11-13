// Midway's launcher behind the HOME menu (ui/home_menu.h): the library made into the menu's titles
// (covers made square, samples given art, play history, the last picture shown), the menu's settings
// kept in settings.txt, and what its choices ask for done: a game copied from a USB drive (one at a
// time, its progress in a message), every USB game copied, the logs written to the drives, a title
// set to start at launch. Drawn into a Ui::Canvas: the PS4's screen (ps4/main_ps4.cpp) or a PNG
// (tools/ui-preview.cpp).
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
    std::string picture;           // the last picture shown (picture-1.png): the last played game's
    std::string hidden;            // hidden.txt: the samples taken off the list, a path a line
    // every log onto the USB drives (the USB tab, Triangle); returns where it was written
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
    // What deleting a game deletes: its folder, else its image; "" when it isn't in the games folder
    // or on a USB drive (the package's samples, anything else)
    std::string DeleteTarget(const Game& game) const;
    std::vector<std::string> Hidden() const;
    void Delete(const std::string& path);

    Settings& settings;
    HomePaths paths;
    Ui::HomeMenu menu;
    std::vector<Game> games;
    std::vector<Ui::Image> covers; // by game, made square (empty: none)
    Ui::Image picture;             // the last picture shown, and the game it's of
    std::string picture_of;
    std::string autostart, chosen;
    Copier copier;
    size_t copies_queued = 0;
    bool copy_message = false;
    // the logs being written (on a thread of their own: tens of MiB read and written)
    std::thread logs_thread;
    std::atomic<bool> logs_done{false};
    std::vector<std::string> logs_written;
    // a game being deleted (on a thread of its own: a game's folder has thousands of files)
    std::thread delete_thread;
    std::atomic<bool> delete_done{false};
    std::string delete_name, delete_error;
};

// The menu's settings from the app's, and back
Ui::MenuSettings ToMenu(const Settings& settings);
void FromMenu(const Ui::MenuSettings& menu, Settings& settings);
// A cover (portrait, as the Xbox's cases) made a square tile: the whole cover on a blurred, darkened
// copy of itself
Ui::Image SquareCover(const Ui::Image& cover, int size);

} // namespace fe
