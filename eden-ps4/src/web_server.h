// A small web server inside the app: from any browser on the same network, a page to upload the
// keys, the firmware and games into the data folder (no root access or FTP needed), and to
// download the logs, all of them as one file (instead of photographs of the screen). It serves
// only the data folder, to anyone on the network, while the app runs.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace Launcher {

struct WebServerConfig
{
    std::string data_dir; // /data/eden-ps4: everything it reads and writes is under it
    uint16_t port = 8080;
    std::string version;
    // Extra status for the page, as JSON members ("\"renderer\":\"...\",..."), or empty
    std::function<std::string()> status;
    // Called after a file was added or removed (the menu reloads what it shows)
    std::function<void()> changed;
    // A line for the app's own log (where it listens, what was uploaded)
    std::function<void(const std::string&)> log;
    // The app's function names (log_tools.h), for the code addresses in the logs it sends
    std::string symbols_path;
    // True while a program runs: the logs are sent without names then (they take ~11 MiB to load)
    std::function<bool()> program_running;
};

// Starts serving on its own threads; false if the port can't be opened.
bool StartWebServer(WebServerConfig config);

// Every log in data_dir as one file (what the page's "Download all logs" gives), code addresses
// named: to take away without a network, on a USB drive. While no program runs. False if it couldn't
// be written.
bool WriteLogBundle(const std::string& data_dir, const std::string& version, const std::string& symbols_path,
                    const std::filesystem::path& file);

// The address other machines on the network reach this one at ("" without a network)
std::string LocalAddress();

} // namespace Launcher
