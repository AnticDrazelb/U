// Keys and firmware from a USB drive (usb_import.h). Files are copied with stdio, as the rest of
// the launcher reads and writes them on the PS4.

#include "usb_import.h"
#include "firmware_zip.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

namespace Launcher {

namespace {

namespace fs = std::filesystem;

constexpr size_t kMinFirmwareFiles = 10; // fewer .nca files than this in a folder: not a firmware

bool HasExtension(const fs::path& path, const char* extension)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == extension;
}

bool IsNca(const fs::path& path)
{
    return HasExtension(path, ".nca");
}

std::vector<fs::path> NcaFiles(const fs::path& folder)
{
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        if (IsNca(it->path()) && it->is_regular_file(ec))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<fs::path> Folders(const fs::path& folder)
{
    std::vector<fs::path> folders;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->is_directory(ec))
            folders.push_back(it->path());
    }
    std::sort(folders.begin(), folders.end());
    return folders;
}

// The file in `folder` with this name in any case (Prod.keys, PROD.KEYS), or none
fs::path FindFile(const fs::path& folder, const std::string& name)
{
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    };
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        if (lower(it->path().filename().string()) == name && it->is_regular_file(ec))
            return it->path();
    }
    return {};
}

// Copies through a temporary name, so that a copy cut short leaves no truncated file behind
bool CopyFile(const fs::path& from, const fs::path& to, std::string& error)
{
    const fs::path partial = to.string() + ".part";
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> in{std::fopen(from.c_str(), "rb"), std::fclose};
    if (!in)
    {
        error = "could not read " + from.string();
        return false;
    }
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> out{std::fopen(partial.c_str(), "wb"), std::fclose};
    std::error_code ec;
    const auto fail = [&](std::string what) {
        out.reset();
        fs::remove(partial, ec);
        error = std::move(what);
        return false;
    };
    if (!out)
        return fail("could not write " + partial.string());
    std::vector<char> buffer(1 << 20);
    for (;;)
    {
        const size_t n = std::fread(buffer.data(), 1, buffer.size(), in.get());
        if (n > 0 && std::fwrite(buffer.data(), 1, n, out.get()) != n)
            return fail("could not write " + partial.string() + " (is the console's storage full?)");
        if (n < buffer.size())
        {
            if (std::ferror(in.get()))
                return fail("could not read " + from.string());
            break;
        }
    }
    if (std::fclose(out.release()) != 0)
        return fail("could not write " + partial.string());
    fs::rename(partial, to, ec);
    if (ec)
        return fail("could not rename " + partial.string() + ": " + ec.message());
    return true;
}

// Where prod.keys is on a drive (its name in any case): its top, a folder at its top, or that
// folder's keys folder
fs::path FindKeys(const fs::path& drive)
{
    if (!FindFile(drive, "prod.keys").empty())
        return drive;
    for (const fs::path& folder : Folders(drive))
    {
        if (!FindFile(folder, "prod.keys").empty())
            return folder;
        if (!FindFile(folder / "keys", "prod.keys").empty())
            return folder / "keys";
    }
    return {};
}

// The folder with the most .nca files: the drive's top, a folder at its top, or one below that
fs::path FindFirmware(const fs::path& drive, size_t& count)
{
    fs::path best;
    count = 0;
    const auto consider = [&](const fs::path& folder) {
        const size_t n = NcaFiles(folder).size();
        if (n > count)
        {
            count = n;
            best = folder;
        }
    };
    consider(drive);
    for (const fs::path& folder : Folders(drive))
    {
        consider(folder);
        for (const fs::path& inner : Folders(folder))
            consider(inner);
    }
    if (count < kMinFirmwareFiles)
    {
        count = 0;
        return {};
    }
    return best;
}

// A firmware's .zip at the drive's top or in a folder there, the one with the most .nca files
fs::path FindFirmwareZip(const fs::path& drive, size_t& count)
{
    fs::path best;
    count = 0;
    const auto consider = [&](const fs::path& folder) {
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            if (!HasExtension(it->path(), ".zip") || !it->is_regular_file(ec))
                continue;
            const size_t n = CountZipNcas(it->path());
            if (n > count)
            {
                count = n;
                best = it->path();
            }
        }
    };
    consider(drive);
    for (const fs::path& folder : Folders(drive))
        consider(folder);
    if (count < kMinFirmwareFiles)
    {
        count = 0;
        return {};
    }
    return best;
}

} // namespace

UsbKeysAndFirmware FindKeysAndFirmware(const std::vector<std::string>& drives)
{
    UsbKeysAndFirmware result;
    for (const std::string& drive_name : drives)
    {
        const fs::path drive{drive_name};
        std::error_code ec;
        if (!fs::is_directory(drive, ec))
            continue;
        if (result.keys_folder.empty())
        {
            // (Eden opens prod.keys and title.keys by those names: Prod.keys only on a drive that
            // ignores case)
            const fs::path keys = FindKeys(drive);
            if (!keys.empty() && fs::is_regular_file(keys / "prod.keys", ec))
            {
                result.keys_folder = keys.string();
                result.title_keys = fs::is_regular_file(keys / "title.keys", ec);
            }
        }
        size_t count = 0;
        const fs::path folder = FindFirmware(drive, count);
        if (count > result.firmware_files)
        {
            result.firmware_folder = folder.string();
            result.firmware_files = count;
        }
    }
    return result;
}

UsbImport ImportFromUsb(const std::vector<std::string>& drives, const std::string& user_dir,
                        const std::function<void(size_t, size_t)>& progress)
{
    UsbImport result;
    fs::path keys, firmware, firmware_zip;
    size_t firmware_count = 0, zip_count = 0;
    for (const std::string& drive_name : drives)
    {
        const fs::path drive{drive_name};
        std::error_code ec;
        if (!fs::is_directory(drive, ec))
            continue;
        if (keys.empty())
            keys = FindKeys(drive);
        size_t count = 0;
        const fs::path folder = FindFirmware(drive, count);
        if (count > firmware_count)
        {
            firmware = folder;
            firmware_count = count;
        }
        // a firmware still in its zip, as most come, if no drive has it unpacked
        size_t in_zip = 0;
        const fs::path zip = FindFirmwareZip(drive, in_zip);
        if (in_zip > zip_count)
        {
            firmware_zip = zip;
            zip_count = in_zip;
        }
    }
    std::error_code ec;
    if (!keys.empty())
    {
        const fs::path keys_dir = fs::path{user_dir} / "keys";
        fs::create_directories(keys_dir, ec);
        if (!CopyFile(FindFile(keys, "prod.keys"), keys_dir / "prod.keys", result.error))
            return result;
        result.keys_folder = keys.string();
        if (const fs::path title_keys = FindFile(keys, "title.keys"); !title_keys.empty())
        {
            if (!CopyFile(title_keys, keys_dir / "title.keys", result.error))
                return result;
            result.title_keys = true;
        }
    }
    if (firmware.empty() && !firmware_zip.empty())
    {
        const fs::path registered = fs::path{user_dir} / "nand" / "system" / "Contents" / "registered";
        const ZipExtract extracted = InstallFirmwareZip(firmware_zip, registered, progress);
        result.firmware_folder = firmware_zip.string();
        result.firmware_files = extracted.files;
        result.error = extracted.error;
        return result;
    }
    if (!firmware.empty())
    {
        // the firmware that was there goes (two versions mixed don't work), as Eden's own firmware
        // install does
        const fs::path registered = fs::path{user_dir} / "nand" / "system" / "Contents" / "registered";
        RemoveFolder(registered); // (not std::filesystem::remove_all: see firmware_zip.h)
        fs::create_directories(registered, ec);
        const std::vector<fs::path> files = NcaFiles(firmware);
        result.firmware_folder = firmware.string();
        for (const fs::path& file : files)
        {
            if (!CopyFile(file, registered / file.filename(), result.error))
                return result;
            result.firmware_files++;
            if (progress)
                progress(result.firmware_files, files.size());
        }
    }
    return result;
}

bool CopyWithProgress(const std::string& from, const std::string& to, std::string& error,
                      const std::function<void(uint64_t, uint64_t)>& progress)
{
    const fs::path partial = to + ".part";
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> in{std::fopen(from.c_str(), "rb"), std::fclose};
    if (!in)
    {
        error = "could not read " + from;
        return false;
    }
    std::error_code ec;
    const uint64_t total = fs::file_size(from, ec);
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> out{std::fopen(partial.c_str(), "wb"), std::fclose};
    const auto fail = [&](std::string what) {
        out.reset();
        fs::remove(partial, ec);
        error = std::move(what);
        return false;
    };
    if (!out)
        return fail("could not write " + partial.string());
    std::vector<char> buffer(size_t(4) << 20);
    uint64_t copied = 0;
    for (;;)
    {
        const size_t n = std::fread(buffer.data(), 1, buffer.size(), in.get());
        if (n > 0 && std::fwrite(buffer.data(), 1, n, out.get()) != n)
            return fail("could not write " + partial.string() + " (is the console's storage full?)");
        copied += n;
        if (progress)
            progress(copied, total);
        if (n < buffer.size())
        {
            if (std::ferror(in.get()))
                return fail("could not read " + from + " (was the drive unplugged?)");
            break;
        }
    }
    if (std::fclose(out.release()) != 0)
        return fail("could not write " + partial.string());
    fs::rename(partial, to, ec);
    if (ec)
        return fail("could not rename " + partial.string() + ": " + ec.message());
    return true;
}

} // namespace Launcher
