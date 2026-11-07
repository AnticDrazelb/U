// Every log in the data folder as one file, code addresses named: to take away without a network,
// on a USB drive (the Game Folder's eden-ps4-logs.txt). (04.87: from the removed web server.)

#pragma once

#include <filesystem>
#include <string>

namespace Launcher {

// Every log in data_dir as one file, code addresses named. While no program runs. False if it
// couldn't be written.
bool WriteLogBundle(const std::string& data_dir, const std::string& version, const std::string& symbols_path,
                    const std::filesystem::path& file);

} // namespace Launcher
