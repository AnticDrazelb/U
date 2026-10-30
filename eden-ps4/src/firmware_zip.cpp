// Firmware zips (firmware_zip.h): the central directory read for the entries, each .nca streamed
// out through zlib's inflate, or copied when stored.

#include "firmware_zip.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <system_error>
#include <vector>

#include <zlib.h>

namespace Launcher {

namespace {

namespace fs = std::filesystem;
using File = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

constexpr uint32_t kEndOfDirectory = 0x06054b50, kZip64Locator = 0x07064b50, kZip64End = 0x06064b50;
constexpr uint32_t kDirectoryEntry = 0x02014b50, kLocalHeader = 0x04034b50;

uint16_t U16(const uint8_t* p)
{
    return uint16_t(p[0] | p[1] << 8);
}
uint32_t U32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t U64(const uint8_t* p)
{
    return uint64_t(U32(p)) | uint64_t(U32(p + 4)) << 32;
}

bool ReadAt(std::FILE* f, uint64_t offset, void* out, size_t size)
{
    return fseeko(f, off_t(offset), SEEK_SET) == 0 && std::fread(out, 1, size, f) == size;
}

struct Entry
{
    std::string name; // its path in the zip
    uint16_t method = 0;
    uint32_t crc = 0;
    uint64_t compressed = 0, size = 0, local_offset = 0;
};

// The zip's entries, from its central directory; none, and `error` set, if it can't read it
std::vector<Entry> ReadDirectory(std::FILE* f, std::string& error)
{
    std::vector<Entry> entries;
    if (fseeko(f, 0, SEEK_END) != 0)
    {
        error = "it can't be read";
        return entries;
    }
    const uint64_t file_size = uint64_t(ftello(f));
    // the end of central directory record: the last 22 bytes, or before a comment of up to 64 KiB
    const uint64_t tail_size = std::min<uint64_t>(file_size, 22 + 65535);
    std::vector<uint8_t> tail(tail_size);
    if (tail_size < 22 || !ReadAt(f, file_size - tail_size, tail.data(), tail_size))
    {
        error = "it isn't a zip";
        return entries;
    }
    int64_t end = -1;
    for (int64_t i = int64_t(tail_size) - 22; i >= 0 && end < 0; i--)
        if (U32(&tail[size_t(i)]) == kEndOfDirectory)
            end = i;
    if (end < 0)
    {
        error = "it isn't a zip";
        return entries;
    }
    const uint8_t* record = &tail[size_t(end)];
    uint64_t count = U16(record + 10), directory_size = U32(record + 12), directory_offset = U32(record + 16);
    if (count == 0xFFFF || directory_size == 0xFFFFFFFF || directory_offset == 0xFFFFFFFF)
    {
        // ZIP64: a locator right before, pointing at the 64-bit end record
        const uint64_t end_at = file_size - tail_size + uint64_t(end);
        uint8_t locator[20], end64[56];
        if (end_at < 20 || !ReadAt(f, end_at - 20, locator, sizeof(locator)) || U32(locator) != kZip64Locator ||
            !ReadAt(f, U64(locator + 8), end64, sizeof(end64)) || U32(end64) != kZip64End)
        {
            error = "it's a ZIP64 zip it can't read";
            return entries;
        }
        count = U64(end64 + 32);
        directory_size = U64(end64 + 40);
        directory_offset = U64(end64 + 48);
    }
    if (directory_offset + directory_size > file_size || directory_size > (uint64_t(64) << 20))
    {
        error = "it's damaged";
        return entries;
    }
    std::vector<uint8_t> directory(directory_size);
    if (!ReadAt(f, directory_offset, directory.data(), directory.size()))
    {
        error = "it can't be read";
        return entries;
    }
    for (size_t pos = 0; entries.size() < count && pos + 46 <= directory.size();)
    {
        const uint8_t* c = &directory[pos];
        if (U32(c) != kDirectoryEntry)
            break;
        Entry entry;
        entry.method = U16(c + 10);
        entry.crc = U32(c + 16);
        entry.compressed = U32(c + 20);
        entry.size = U32(c + 24);
        const size_t name_length = U16(c + 28), extra_length = U16(c + 30), comment_length = U16(c + 32);
        entry.local_offset = U32(c + 42);
        if (pos + 46 + name_length + extra_length > directory.size())
            break;
        entry.name.assign(reinterpret_cast<const char*>(c + 46), name_length);
        // the ZIP64 field: the 64-bit values of those that read 0xFFFFFFFF, in this order
        for (size_t x = 0; x + 4 <= extra_length;)
        {
            const uint8_t* field = c + 46 + name_length + x;
            const size_t field_size = U16(field + 2);
            if (U16(field) == 0x0001)
            {
                size_t at = 4;
                for (uint64_t* value : {&entry.size, &entry.compressed, &entry.local_offset})
                {
                    if (*value == 0xFFFFFFFF && at + 8 <= 4 + field_size && x + at + 8 <= extra_length)
                    {
                        *value = U64(field + at);
                        at += 8;
                    }
                }
            }
            x += 4 + field_size;
        }
        entries.push_back(std::move(entry));
        pos += 46 + name_length + extra_length + comment_length;
    }
    if (entries.size() != count)
        error = "it's damaged";
    return entries;
}

bool EndsWithNca(const std::string& name)
{
    if (name.size() < 4)
        return false;
    std::string ext = name.substr(name.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".nca";
}

// The .nca files in the zip, by file name, each the entries to join in order: one, or an archive
// bit folder's parts (x.nca/00, x.nca/01...)
std::map<std::string, std::vector<const Entry*>> NcaFiles(const std::vector<Entry>& entries)
{
    std::map<std::string, std::map<int, const Entry*>> parts;
    for (const Entry& entry : entries)
    {
        if (entry.name.empty() || entry.name.back() == '/' || entry.name.back() == '\\')
            continue; // a folder
        const size_t slash = entry.name.find_last_of("/\\");
        const std::string base = slash == std::string::npos ? entry.name : entry.name.substr(slash + 1);
        if (EndsWithNca(base))
        {
            parts[base][-1] = &entry;
            continue;
        }
        // "x.nca/00": a part of x.nca
        if (slash == std::string::npos || base.empty() || base.size() > 2 ||
            !std::all_of(base.begin(), base.end(), [](unsigned char c) { return std::isdigit(c); }))
            continue;
        const std::string parent = entry.name.substr(0, slash);
        const size_t parent_slash = parent.find_last_of("/\\");
        const std::string folder = parent_slash == std::string::npos ? parent : parent.substr(parent_slash + 1);
        if (EndsWithNca(folder))
            parts[folder][std::stoi(base)] = &entry;
    }
    std::map<std::string, std::vector<const Entry*>> files;
    for (auto& [name, numbered] : parts)
    {
        // (only names that stay in the folder: a file name, no "..")
        if (name == ".nca" || name.find("..") != std::string::npos)
            continue;
        std::vector<const Entry*>& list = files[name];
        if (numbered.count(-1))
            list.push_back(numbered[-1]);
        else
            for (const auto& [index, entry] : numbered)
                list.push_back(entry);
    }
    return files;
}

// Appends one entry's data to `out`, checked against its size and CRC
bool CopyEntry(std::FILE* zip, const Entry& entry, std::FILE* out, std::string& error)
{
    uint8_t local[30];
    if (!ReadAt(zip, entry.local_offset, local, sizeof(local)) || U32(local) != kLocalHeader)
    {
        error = "the zip is damaged";
        return false;
    }
    const uint64_t data = entry.local_offset + 30 + U16(local + 26) + U16(local + 28);
    if (fseeko(zip, off_t(data), SEEK_SET) != 0)
    {
        error = "the zip can't be read";
        return false;
    }
    std::vector<uint8_t> in(1 << 20), inflated(1 << 20);
    uLong crc = crc32(0, nullptr, 0);
    uint64_t remaining = entry.compressed, written = 0;
    const auto write = [&](const uint8_t* bytes, size_t size) {
        crc = crc32(crc, bytes, uInt(size));
        written += size;
        return size == 0 || std::fwrite(bytes, 1, size, out) == size;
    };
    if (entry.method == 0)
    {
        while (remaining > 0)
        {
            const size_t n = size_t(std::min<uint64_t>(remaining, in.size()));
            if (std::fread(in.data(), 1, n, zip) != n)
            {
                error = "the zip ends early";
                return false;
            }
            if (!write(in.data(), n))
            {
                error = "it couldn't be written (is the console's storage full?)";
                return false;
            }
            remaining -= n;
        }
    }
    else if (entry.method == 8)
    {
        z_stream z{};
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
        {
            error = "no memory to inflate it";
            return false;
        }
        int status = Z_OK;
        while (status != Z_STREAM_END)
        {
            if (z.avail_in == 0 && remaining > 0)
            {
                const size_t n = size_t(std::min<uint64_t>(remaining, in.size()));
                if (std::fread(in.data(), 1, n, zip) != n)
                {
                    inflateEnd(&z);
                    error = "the zip ends early";
                    return false;
                }
                z.next_in = in.data();
                z.avail_in = uInt(n);
                remaining -= n;
            }
            z.next_out = inflated.data();
            z.avail_out = uInt(inflated.size());
            status = inflate(&z, Z_NO_FLUSH);
            const bool stuck = status == Z_BUF_ERROR && z.avail_in == 0 && remaining == 0;
            if ((status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) || stuck)
            {
                inflateEnd(&z);
                error = "its data is damaged (download the zip again)";
                return false;
            }
            if (!write(inflated.data(), inflated.size() - z.avail_out))
            {
                inflateEnd(&z);
                error = "it couldn't be written (is the console's storage full?)";
                return false;
            }
        }
        inflateEnd(&z);
    }
    else
    {
        error = "it's compressed in a way this can't read (method " + std::to_string(entry.method) +
                "): a plain zip works";
        return false;
    }
    if (written != entry.size || crc != entry.crc)
    {
        error = "it doesn't match its checksum (download the zip again)";
        return false;
    }
    return true;
}

} // namespace

size_t CountZipNcas(const fs::path& zip)
{
    File f{std::fopen(zip.c_str(), "rb"), std::fclose};
    if (!f)
        return 0;
    std::string error;
    const std::vector<Entry> entries = ReadDirectory(f.get(), error);
    return error.empty() ? NcaFiles(entries).size() : 0;
}

ZipExtract ExtractZipNcas(const fs::path& zip, const fs::path& folder,
                          const std::function<void(size_t, size_t)>& progress)
{
    ZipExtract result;
    File f{std::fopen(zip.c_str(), "rb"), std::fclose};
    if (!f)
    {
        result.error = "could not open " + zip.string();
        return result;
    }
    std::string error;
    const std::vector<Entry> entries = ReadDirectory(f.get(), error);
    if (!error.empty())
    {
        result.error = zip.filename().string() + ": " + error;
        return result;
    }
    const auto files = NcaFiles(entries);
    std::error_code ec;
    fs::create_directories(folder, ec);
    for (const auto& [name, parts] : files)
    {
        const fs::path target = folder / name;
        const fs::path partial = target.string() + ".part";
        File out{std::fopen(partial.c_str(), "wb"), std::fclose};
        if (!out)
        {
            result.error = "could not write " + partial.string();
            return result;
        }
        for (const Entry* part : parts)
        {
            if (!CopyEntry(f.get(), *part, out.get(), error))
            {
                out.reset();
                fs::remove(partial, ec);
                result.error = name + ": " + error;
                return result;
            }
        }
        if (std::fclose(out.release()) != 0)
        {
            fs::remove(partial, ec);
            result.error = name + ": it couldn't be written (is the console's storage full?)";
            return result;
        }
        fs::rename(partial, target, ec);
        if (ec)
        {
            fs::remove(partial, ec);
            result.error = "could not rename " + partial.string();
            return result;
        }
        result.files++;
        if (progress)
            progress(result.files, files.size());
    }
    return result;
}

size_t RemoveFolder(const fs::path& folder)
{
    std::error_code ec;
    std::vector<fs::path> children;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        children.push_back(it->path());
    size_t removed = 0;
    for (const fs::path& child : children)
    {
        if (fs::is_directory(fs::symlink_status(child, ec)))
            removed += RemoveFolder(child);
        else if (fs::remove(child, ec))
            removed++;
    }
    return removed + (fs::remove(folder, ec) ? 1 : 0);
}

ZipExtract InstallFirmwareZip(const fs::path& zip, const fs::path& registered,
                              const std::function<void(size_t, size_t)>& progress)
{
    std::error_code ec;
    const fs::path staging = registered.string() + ".new";
    RemoveFolder(staging);
    ZipExtract result = ExtractZipNcas(zip, staging, progress);
    if (!result.error.empty())
    {
        RemoveFolder(staging);
        result.error += " (the firmware that was there is left as it was)";
        return result;
    }
    // (the firmware that was there goes whole: two versions mixed don't work)
    RemoveFolder(registered);
    fs::rename(staging, registered, ec);
    if (ec)
        result.error = "could not move the new firmware into place: " + ec.message();
    return result;
}

} // namespace Launcher
