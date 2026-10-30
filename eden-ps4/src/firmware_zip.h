// Firmware as most of it comes: a .zip of the .nca files, extracted on the console (from the web
// page's upload or a USB drive) into the folder Eden's NAND keeps them in.

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

namespace Launcher {

struct ZipExtract
{
    size_t files = 0;   // .nca files written
    std::string error;  // why it stopped, if it did
};

// How many .nca files the zip holds (0 if it isn't a zip it can read)
size_t CountZipNcas(const std::filesystem::path& zip);

// Writes every .nca in the zip, from whatever folder it's in, into `folder` (by its file name; an
// "archive bit" folder x.nca holding parts 00, 01... is joined into x.nca). Stored and deflated
// entries, checked against their CRCs; each file through a ".part" name. `progress` gets the files
// written so far and the total.
ZipExtract ExtractZipNcas(const std::filesystem::path& zip, const std::filesystem::path& folder,
                          const std::function<void(size_t, size_t)>& progress = {});

// Removes a folder and everything in it; the entries removed. (std::filesystem::remove_all can't on
// the PS4: it needs unlinkat, which the PS4's C library has only as a stub that fails.)
size_t RemoveFolder(const std::filesystem::path& folder);

// Replaces the firmware in `registered` (Eden's NAND) with the zip's .nca files: unpacked beside it
// first, so that a damaged zip leaves the firmware that was there as it was
ZipExtract InstallFirmwareZip(const std::filesystem::path& zip, const std::filesystem::path& registered,
                              const std::function<void(size_t, size_t)>& progress = {});

} // namespace Launcher
