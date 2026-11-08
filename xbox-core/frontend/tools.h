// The front end's tools: copying games from USB drives to the hard drive (in the background, one
// after another, with progress), and a diagnostics report: a text file to show when asking for help.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "library.h"

namespace fe {

// A game found on a USB drive: a disc image, or a folder with a default.xbe (and its files).
struct UsbGame
{
    std::string source; // the image or folder
    std::string name;   // its file or folder name, as it will be in the games folder
    bool folder = false;
    uint64_t size = 0;
    bool on_hdd = false; // the games folder has one by that name already
};

// The USB drives that are there (of `candidates`, e.g. /mnt/usb0..7).
std::vector<std::string> UsbRoots(const std::vector<std::string>& candidates);
// Games on them: in a drive's top folder, or its games/, xbox/ and xbox-core/games/ folders.
std::vector<UsbGame> FindUsbGames(const std::vector<std::string>& roots, const std::string& games_dir);
// Bytes free where `path` is (0 when it can't be told).
uint64_t FreeSpace(const std::string& path);

class Copier
{
public:
    ~Copier();
    void Queue(const UsbGame& game, const std::string& games_dir);
    void Cancel(); // the current copy (its partial files removed) and the rest of the queue

    struct Status
    {
        bool busy = false;
        std::string current;
        uint64_t done = 0, total = 0;
        double rate = 0; // bytes a second
        size_t queued = 0;
        std::vector<std::string> finished; // since the last look: names copied, or "failed: ..."
    };
    Status Poll();

private:
    struct Job
    {
        UsbGame game;
        std::string games_dir;
    };
    void Run();
    bool CopyFile(const std::string& from, const std::string& to, std::string& error);
    bool CopyTree(const std::string& from, const std::string& to, std::string& error);

    std::mutex mutex;
    std::deque<Job> jobs;
    std::thread worker;
    bool running = false;
    std::atomic<bool> cancel{false};
    std::string current;
    std::atomic<uint64_t> done{0}, total{0};
    double started = 0;
    std::vector<std::string> finished;
};

// The diagnostics report: `system` lines from the app (version, where it runs, ...), then the
// settings, the library with each game's facts and last result, the USB drives, and the ends of
// the given log files. Written to each of `outputs` it can; returns the ones written.
struct DiagnosticsInput
{
    std::vector<std::string> system;
    const Settings* settings = nullptr;
    std::vector<Game> games;
    std::vector<std::string> usb_roots;
    std::string data_dir;
    std::vector<std::pair<std::string, size_t>> logs; // a file, how many of its last lines
    // New Eden's way instead: every .log and .txt in data_dir (the last 8 MiB of each), code
    // addresses named from `symbols` ("<address> <size> <name>" lines, hex, relative to .text)
    bool all_logs = false;
    std::string symbols;
};
std::vector<std::string> WriteDiagnostics(const DiagnosticsInput& in, const std::vector<std::string>& outputs);

} // namespace fe
