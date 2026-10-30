// Keys and firmware from a USB drive, into Eden's user folder: on a console where FTP and file
// managers can't reach the app's data folder (no root access), the app itself can read a USB
// drive. prod.keys and title.keys are taken from the top of the drive, a folder at its top
// (Lockpick_RCM writes them to switch/), or such a folder's keys folder; the firmware is the
// .nca files of the folder (at the top, or one below) that has the most of them, e.g.
// "Firmware 19.0.1".

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Launcher {

struct UsbImport
{
    std::string keys_folder; // where prod.keys was (empty: not found)
    bool title_keys = false;
    std::string firmware_folder; // the folder the firmware came from (empty: none found)
    size_t firmware_files = 0;   // .nca files copied
    std::string error;           // what failed (empty: nothing)
};

// Keys and firmware a drive has, to be used where they are (Launcher::SetKeysAndFirmware)
struct UsbKeysAndFirmware
{
    std::string keys_folder;     // the folder with prod.keys (empty: none)
    bool title_keys = false;     // title.keys is there too
    std::string firmware_folder; // the folder with the firmware's .nca files (empty: none)
    size_t firmware_files = 0;
};

// The first of `drives` with prod.keys (named so: Eden reads it by that name), and the folder of
// .nca files with the most of them, as ImportFromUsb looks for them. A firmware still in its .zip
// isn't one: Eden can't read it from there (ImportFromUsb unpacks it).
UsbKeysAndFirmware FindKeysAndFirmware(const std::vector<std::string>& drives);

// From `drives` (e.g. /mnt/usb0../mnt/usb7): the keys of the first that has them go to
// <user_dir>/keys; the firmware (from whichever has the most .nca files) replaces what's in
// <user_dir>/nand/system/Contents/registered. progress(copied, total) after each firmware file.
UsbImport ImportFromUsb(const std::vector<std::string>& drives, const std::string& user_dir,
                        const std::function<void(size_t, size_t)>& progress);

} // namespace Launcher
