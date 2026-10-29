// A homebrew program's (.nro) name, author and icon, from the assets after its code: no keys
// needed, unlike a game's.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Ui {

struct NroInfo
{
    std::string name, publisher;
    std::vector<uint8_t> icon; // JPEG
};
// False if it has no assets (or isn't an NRO)
bool ReadNroInfo(const std::string& path, NroInfo& info);

} // namespace Ui
