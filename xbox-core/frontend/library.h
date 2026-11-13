// The game library: what's in the games folder (folders with a default.xbe, disc images) and the
// bundled samples, each with what its XBE says about itself (title, title ID, region), its cover
// art if the player put one beside it, and when it was last played and how that run ended.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fe {

struct Game
{
    std::string path;      // what runs: a default.xbe or an .iso
    std::string file_name; // the folder's or image's name
    std::string title;     // the XBE's certificate title, else the file name
    std::string kind;      // "Disc image", "Game folder", "Sample"
    uint32_t title_id = 0, region = 0;
    uint64_t size = 0;     // bytes (the image, or the folder's XBE)
    std::string cover;     // a picture's path, or ""
    bool sample = false;
    std::string usb;       // the USB drive it's on ("" on the hard drive)
    std::string folder;    // its own folder (a folder game, or a folder holding its image), or ""
    // history
    int plays = 0;
    int64_t last_played = 0; // seconds since 1970
    std::string last_result;
};

struct LibraryPaths
{
    std::vector<std::string> sample_dirs; // folders of bundled .xbe/.iso samples
    std::string games;                    // the player's games
    std::vector<std::string> usb_drives;  // USB drives' tops, their games listed too
    std::string covers;                   // pictures by title ID or name
    std::string history;                  // a file
};

std::vector<Game> ScanLibrary(const LibraryPaths& paths);
// The name a picture imported for the game has in <covers>/imported/ (and an extension): its title ID
// (8 hex digits), or its file's name for homebrew (nxdk's title ID is every homebrew's)
std::string CoverKey(const Game& game);
// a disc image's name: .iso or .xiso
bool IsDiscImageName(const std::string& name);
// USB drive i (0-7) where the app sees it: /mnt/usbN, or /usbN where a console shows an app its
// drives instead (as New Eden found); "" when neither is there
std::string UsbDrive(int i);
void SaveHistory(const std::string& file, const std::vector<Game>& games);
// a run starting (counted) or its end ("" leaves the result as it was)
void RecordRun(const std::string& file, const std::string& path, const std::string& result);

std::string RegionName(uint32_t region);
std::string FormatSize(uint64_t bytes);
std::string FormatDate(int64_t when);

struct Settings
{
    bool show_log = true; // the log over the game's picture, its first seconds
    int picture = 0;      // 0 sharp (whole-number scale), 1 fit 4:3, 2 fill the screen
    int sort = 0;         // 0 by title, 1 recently played first
    int nv2a = 0;         // the GPU's emulation: 0 xemu's NV2A, 1 xbox-core's own
    int dsp = 0;          // the APU's DSPs (effects): 0 off (the voices mixed directly), 1 on
    // the HOME menu's: its theme (0 Basic Black, 1 Basic White, 2 Retro), the user's nickname and icon
    int theme = 0;
    bool logs = true;      // logging while a game runs (the log files, the profiler, the pictures)
    std::string nickname;  // "" the PS4 user's name
    int avatar = 0, avatar_color = 0;
    bool Load(const std::string& file);
    void Save(const std::string& file) const;
};

} // namespace fe
