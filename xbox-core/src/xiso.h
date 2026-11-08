// Xbox disc images (XISO: the XDVDFS file system, alone or inside a full disc dump), read-only: the
// game's D: when it is run from an .iso.
#pragma once

#include <string>
#include <vector>

#include "core.h"

namespace xb {

struct IsoEntry
{
    std::string name;
    u64 offset = 0; // in the image file
    u32 size = 0;
    bool directory = false;
    std::vector<u32> children; // indices of the entries in a directory
};

bool MountIso(const std::string& path, std::string& error);
bool IsoMounted();
// An entry by its path in the image ("" is the root; '/' separates; case-insensitive), or null.
const IsoEntry* IsoFind(const std::string& path);
const IsoEntry& IsoEntryAt(u32 index);
// Reads n bytes at `at` in the entry's data; returns how many.
size_t IsoRead(const IsoEntry& e, u64 at, size_t n, void* out);
s64 IsoTime(); // the image's time stamp (NT time)

} // namespace xb
