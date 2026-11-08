// Files: the Xbox's object paths mapped to host folders, and the Nt*File exports. D: and
// \Device\CdRom0 are the game's folder; the hard disk's partitions (E:, T:, U:, Z: and
// \Device\Harddisk0\PartitionN) are folders under a data folder. Also the XcSHA* exports.
#include <cerrno>
#include <cstdio>
#include <map>
#include <mutex>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core.h"
#include "kernel.h"
#include "sha1.h"

namespace xb {

namespace {

constexpr u32 STATUS_SUCCESS = 0, STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034, STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A,
              STATUS_OBJECT_NAME_COLLISION = 0xC0000035, STATUS_INVALID_HANDLE = 0xC0000008,
              STATUS_END_OF_FILE = 0xC0000011, STATUS_ACCESS_DENIED = 0xC0000022,
              STATUS_INVALID_PARAMETER = 0xC000000D, STATUS_NOT_IMPLEMENTED = 0xC0000002,
              STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA, STATUS_NOT_A_DIRECTORY = 0xC0000103;

std::string g_game_dir = ".";
std::string g_data_dir = "xbox-data";
std::mutex g_links_mutex;
std::map<std::string, std::string> g_links; // lower-case "\??\d:" -> device path

struct HostFile
{
    int fd = -1;
    std::string path;
    bool directory = false;
    std::vector<std::string> listing; // NtQueryDirectoryFile: the names, read at the first query
    size_t next = 0;
    std::string pattern;
};

constexpr u32 kDosDevicesDirectory = 0xfffffffd; // ObDosDevicesDirectory()

// "*" and "?" as the Xbox's file system matches them, case-insensitive.
bool Matches(const char* pattern, const char* name)
{
    if (!*pattern)
        return !*name;
    if (*pattern == '*')
        return Matches(pattern + 1, name) || (*name && Matches(pattern, name + 1));
    if (!*name)
        return false;
    return (*pattern == '?' || std::tolower((unsigned char)*pattern) == std::tolower((unsigned char)*name)) &&
           Matches(pattern + 1, name + 1);
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// An Xbox object path -> a host path, or "" if it isn't a file system path.
std::string HostPath(std::string path, u32 root)
{
    if (root)
        if (auto* dir = static_cast<HostFile*>(HostOf(root, 2)))
            return dir->path + "/" + path;
    for (char& c : path)
        if (c == '\\')
            c = '/';
    std::string lower = Lower(path);
    if (lower.rfind("/?" "?/", 0) == 0)
        path = path.substr(4), lower = lower.substr(4);
    // drive letters: through the symbolic links the program made, else the defaults
    if (lower.size() >= 2 && lower[1] == ':')
    {
        std::string link;
        {
            std::lock_guard lock(g_links_mutex);
            auto it = g_links.find(lower.substr(0, 2));
            if (it != g_links.end())
                link = it->second;
        }
        const std::string rest = path.substr(2);
        if (!link.empty())
            return HostPath(link + rest, 0);
        switch (lower[0])
        {
        case 'd': return g_game_dir + rest;
        case 'c': case 'e': case 't': case 'u': case 'x': case 'y': case 'z':
            return g_data_dir + "/" + char(lower[0]) + rest;
        }
        return "";
    }
    const std::pair<const char*, std::string> devices[] = {
        {"/device/cdrom0", g_game_dir},
        {"/device/harddisk0/partition1", g_data_dir + "/e"},
        {"/device/harddisk0/partition2", g_data_dir + "/c"},
        {"/device/harddisk0/partition3", g_data_dir + "/x"},
        {"/device/harddisk0/partition4", g_data_dir + "/y"},
        {"/device/harddisk0/partition5", g_data_dir + "/z"},
    };
    for (const auto& [device, dir] : devices)
        if (lower.rfind(device, 0) == 0)
            return dir + path.substr(std::strlen(device));
    return "";
}

std::string ObjectPath(u32 object_attributes, u32& root)
{
    root = Read<u32>(object_attributes);
    return GuestString(Read<u32>(object_attributes + 4));
}

void SetIo(u32 io_status, u32 status, u32 information)
{
    if (io_status)
    {
        Write<u32>(io_status, status);
        Write<u32>(io_status + 4, information);
    }
}

s64 FileTime(const struct timespec& t)
{
    return s64(t.tv_sec) * 10000000 + t.tv_nsec / 100 + 116444736000000000LL;
}

// FILE_NETWORK_OPEN_INFORMATION (0x38 bytes)
void NetworkOpenInfo(const struct stat& st, u32 out)
{
    Write<s64>(out + 0x00, FileTime(st.st_ctim));
    Write<s64>(out + 0x08, FileTime(st.st_atim));
    Write<s64>(out + 0x10, FileTime(st.st_mtim));
    Write<s64>(out + 0x18, FileTime(st.st_mtim));
    Write<s64>(out + 0x20, (st.st_size + 4095) & ~4095LL);
    Write<s64>(out + 0x28, st.st_size);
    Write<u32>(out + 0x30, S_ISDIR(st.st_mode) ? 0x10 : 0x80); // DIRECTORY / NORMAL
}

void MakeDirs(const std::string& path)
{
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/')
            mkdir(path.substr(0, i).c_str(), 0755);
}

u32 Open(u32 handle_ptr, u32 object_attributes, u32 io_status, u32 disposition, u32 options, u32 access)
{
    u32 root;
    const std::string xbox = ObjectPath(object_attributes, root);
    const std::string host = HostPath(xbox, root);
    if (host.empty())
    {
        if (g_verbose >= 1)
            Log("files: %s: not a file system path", xbox.c_str());
        SetIo(io_status, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    struct stat st;
    const bool exists = stat(host.c_str(), &st) == 0;
    const bool want_dir = options & 1; // FILE_DIRECTORY_FILE
    // FILE_SUPERSEDE 0, OPEN 1, CREATE 2, OPEN_IF 3, OVERWRITE 4, OVERWRITE_IF 5
    if (!exists && (disposition == 1 || disposition == 4))
    {
        SetIo(io_status, STATUS_OBJECT_NAME_NOT_FOUND, 0);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (exists && disposition == 2)
    {
        SetIo(io_status, STATUS_OBJECT_NAME_COLLISION, 0);
        return STATUS_OBJECT_NAME_COLLISION;
    }
    auto* f = new HostFile;
    f->path = host;
    u32 information = 1; // FILE_OPENED
    if (want_dir || (exists && S_ISDIR(st.st_mode)))
    {
        if (!exists)
        {
            MakeDirs(host + "/");
            information = 2; // FILE_CREATED
        }
        else if (!S_ISDIR(st.st_mode))
        {
            delete f;
            return STATUS_NOT_A_DIRECTORY;
        }
        f->directory = true;
    }
    else
    {
        const bool write = access & (0x40000000 | 0x2 | 0x4 | 0x10000000); // GENERIC_WRITE, FILE_WRITE_DATA...
        int flags = write ? O_RDWR : O_RDONLY;
        if (!exists)
            flags |= O_CREAT, information = 2, MakeDirs(host);
        if (exists && (disposition == 0 || disposition == 4 || disposition == 5))
            flags |= O_TRUNC, information = 3; // FILE_OVERWRITTEN
        f->fd = open(host.c_str(), flags | O_CLOEXEC, 0644);
        if (f->fd < 0 && write && errno == EACCES)
            f->fd = open(host.c_str(), O_RDONLY | O_CLOEXEC);
        if (f->fd < 0)
        {
            delete f;
            SetIo(io_status, STATUS_ACCESS_DENIED, 0);
            return STATUS_ACCESS_DENIED;
        }
    }
    Write<u32>(handle_ptr, NewHostHandle(f->directory ? 2 : 1, f));
    SetIo(io_status, STATUS_SUCCESS, information);
    if (g_verbose >= 1)
        Log("files: %s -> %s", xbox.c_str(), host.c_str());
    return STATUS_SUCCESS;
}

HostFile* FileOf(u32 handle)
{
    if (auto* f = static_cast<HostFile*>(HostOf(handle, 1)))
        return f;
    return static_cast<HostFile*>(HostOf(handle, 2));
}


} // namespace

void SetGameDirectory(const std::string& dir)
{
    g_game_dir = dir;
}

void SetDataDirectory(const std::string& dir)
{
    g_data_dir = dir;
    // the hard disk's partitions, so that listing them works before a program writes there
    for (const char* p : {"", "/c", "/e", "/t", "/u", "/x", "/y", "/z"})
        mkdir((dir + p).c_str(), 0755);
}

void CloseHostHandle(int kind, void* host)
{
    if (kind == 1 || kind == 2)
    {
        auto* f = static_cast<HostFile*>(host);
        if (f->fd >= 0)
            close(f->fd);
        delete f;
    }
}

void RegisterFileExports(RegisterFn reg)
{
    reg("IoCreateSymbolicLink", [](Call& c) -> u64 {
        const std::string link = Lower(GuestString(c.Arg(0))), device = GuestString(c.Arg(1));
        std::string key = link;
        for (char& ch : key)
            if (ch == '\\')
                ch = '/';
        if (key.rfind("/?" "?/", 0) == 0)
            key = key.substr(4);
        {
            std::lock_guard lock(g_links_mutex);
            g_links[key] = device;
        }
        Log("files: %s -> %s", link.c_str(), device.c_str());
        return STATUS_SUCCESS;
    });
    reg("IoDeleteSymbolicLink", [](Call& c) -> u64 {
        std::string key = Lower(GuestString(c.Arg(0)));
        for (char& ch : key)
            if (ch == '\\')
                ch = '/';
        if (key.rfind("/?" "?/", 0) == 0)
            key = key.substr(4);
        std::lock_guard lock(g_links_mutex);
        g_links.erase(key);
        return STATUS_SUCCESS;
    });
    reg("NtCreateFile", [](Call& c) -> u64 {
        // FileHandle*, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes,
        // ShareAccess, CreateDisposition, CreateOptions
        return Open(c.Arg(0), c.Arg(2), c.Arg(3), c.Arg(7), c.Arg(8), c.Arg(1));
    });
    reg("NtOpenFile", [](Call& c) -> u64 {
        // FileHandle*, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions
        return Open(c.Arg(0), c.Arg(2), c.Arg(3), 1, c.Arg(5), c.Arg(1));
    });
    reg("NtReadFile", [](Call& c) -> u64 {
        // FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset*
        HostFile* f = FileOf(c.Arg(0));
        if (!f || f->fd < 0)
            return STATUS_INVALID_HANDLE;
        ssize_t n;
        if (c.Arg(7))
            n = pread(f->fd, Ptr(c.Arg(5)), c.Arg(6), off_t(Read<s64>(c.Arg(7))));
        else
            n = read(f->fd, Ptr(c.Arg(5)), c.Arg(6));
        if (n < 0)
            n = 0;
        const u32 status = n == 0 && c.Arg(6) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
        SetIo(c.Arg(4), status, u32(n));
        if (c.Arg(1))
            WakeAll();
        return status;
    });
    reg("NtWriteFile", [](Call& c) -> u64 {
        HostFile* f = FileOf(c.Arg(0));
        if (!f || f->fd < 0)
            return STATUS_INVALID_HANDLE;
        ssize_t n;
        if (c.Arg(7) && Read<s64>(c.Arg(7)) >= 0)
            n = pwrite(f->fd, Ptr(c.Arg(5)), c.Arg(6), off_t(Read<s64>(c.Arg(7))));
        else
            n = write(f->fd, Ptr(c.Arg(5)), c.Arg(6));
        SetIo(c.Arg(4), STATUS_SUCCESS, n < 0 ? 0 : u32(n));
        return STATUS_SUCCESS;
    });
    reg("NtQueryInformationFile", [](Call& c) -> u64 {
        // FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass
        HostFile* f = FileOf(c.Arg(0));
        if (!f)
            return STATUS_INVALID_HANDLE;
        struct stat st;
        if (stat(f->path.c_str(), &st) != 0)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        const u32 out = c.Arg(2);
        switch (c.Arg(4))
        {
        case 5: // FileStandardInformation
            Write<s64>(out, (st.st_size + 4095) & ~4095LL);
            Write<s64>(out + 8, st.st_size);
            Write<u32>(out + 16, 1);
            Write<u8>(out + 20, 0);
            Write<u8>(out + 21, S_ISDIR(st.st_mode));
            SetIo(c.Arg(1), 0, 24);
            return STATUS_SUCCESS;
        case 14: // FilePositionInformation
            Write<s64>(out, f->fd >= 0 ? lseek(f->fd, 0, SEEK_CUR) : 0);
            SetIo(c.Arg(1), 0, 8);
            return STATUS_SUCCESS;
        case 34: // FileNetworkOpenInformation
            NetworkOpenInfo(st, out);
            SetIo(c.Arg(1), 0, 0x38);
            return STATUS_SUCCESS;
        }
        Log("files: NtQueryInformationFile class %u not implemented", c.Arg(4));
        return STATUS_INVALID_PARAMETER;
    });
    reg("NtSetInformationFile", [](Call& c) -> u64 {
        HostFile* f = FileOf(c.Arg(0));
        if (!f)
            return STATUS_INVALID_HANDLE;
        const u32 in = c.Arg(2);
        switch (c.Arg(4))
        {
        case 14: // FilePositionInformation
            if (f->fd >= 0)
                lseek(f->fd, off_t(Read<s64>(in)), SEEK_SET);
            SetIo(c.Arg(1), 0, 0);
            return STATUS_SUCCESS;
        case 19: // FileAllocationInformation
        case 20: // FileEndOfFileInformation
            if (f->fd >= 0 && ftruncate(f->fd, off_t(Read<s64>(in))) != 0)
                return STATUS_ACCESS_DENIED;
            SetIo(c.Arg(1), 0, 0);
            return STATUS_SUCCESS;
        case 13: // FileDispositionInformation
            if (Read<u8>(in))
                (f->directory ? rmdir : unlink)(f->path.c_str());
            SetIo(c.Arg(1), 0, 0);
            return STATUS_SUCCESS;
        }
        Log("files: NtSetInformationFile class %u not implemented", c.Arg(4));
        return STATUS_INVALID_PARAMETER;
    });
    reg("NtQueryFullAttributesFile", [](Call& c) -> u64 {
        u32 root;
        const std::string host = HostPath(ObjectPath(c.Arg(0), root), root);
        struct stat st;
        if (host.empty() || stat(host.c_str(), &st) != 0)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        NetworkOpenInfo(st, c.Arg(1));
        return STATUS_SUCCESS;
    });
    reg("NtQueryVolumeInformationFile", [](Call& c) -> u64 {
        // FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass
        const u32 out = c.Arg(2);
        if (c.Arg(4) == 3) // FileFsSizeInformation: 8 GiB, 4 GiB free, 16 KiB clusters
        {
            Write<s64>(out, 8ll * 1024 * 1024 * 1024 / 16384);
            Write<s64>(out + 8, 4ll * 1024 * 1024 * 1024 / 16384);
            Write<u32>(out + 16, 32);
            Write<u32>(out + 20, 512);
            SetIo(c.Arg(1), 0, 24);
            return STATUS_SUCCESS;
        }
        Log("files: NtQueryVolumeInformationFile class %u not implemented", c.Arg(4));
        return STATUS_INVALID_PARAMETER;
    });
    reg("NtFlushBuffersFile", [](Call& c) -> u64 {
        SetIo(c.Arg(1), 0, 0);
        return STATUS_SUCCESS;
    });
    // object directories: \?? (the drive letters: D: and the program's links); others are empty
    reg("NtOpenDirectoryObject", [](Call& c) -> u64 {
        u32 root;
        const std::string name = Lower(ObjectPath(c.Arg(1), root));
        if (root == kDosDevicesDirectory || name == "\\??" || name == "\\?" "?\\")
        {
            const u32 h = NewHostHandle(3, nullptr);
            Write<u32>(c.Arg(0), h);
            return STATUS_SUCCESS;
        }
        return STATUS_OBJECT_NAME_NOT_FOUND;
    });
    reg("NtOpenSymbolicLinkObject", [](Call&) -> u64 { return STATUS_OBJECT_NAME_NOT_FOUND; });
    reg("NtQueryDirectoryObject", [](Call& c) -> u64 {
        // DirectoryHandle, Buffer, Length, RestartScan, Context*, ReturnLength*
        if (!HostOf(c.Arg(0), 3) && !IsHandleKind(c.Arg(0), 3))
            return 0x8000001A; // STATUS_NO_MORE_ENTRIES
        std::vector<std::string> names = {"D:"};
        {
            std::lock_guard lock(g_links_mutex);
            for (auto& [key, device] : g_links)
                if (key.size() == 2 && key[1] == ':' && key != "d:")
                    names.push_back(std::string(1, char(std::toupper(key[0]))) + ":");
        }
        const u32 index = (c.Arg(3) & 0xff) ? 0 : Read<u32>(c.Arg(4));
        if (index >= names.size())
            return 0x8000001A;
        Write<u32>(c.Arg(4), index + 1);
        const std::string& n = names[index];
        if (c.Arg(2) < 12 + n.size())
            return 0xC0000023; // STATUS_BUFFER_TOO_SMALL
        // OBJECT_DIRECTORY_INFORMATION: Name (OBJECT_STRING), Type; the name's characters after it
        const u32 out = c.Arg(1);
        Write<u16>(out, u16(n.size()));
        Write<u16>(out + 2, u16(n.size()));
        Write<u32>(out + 4, out + 12);
        Write<u32>(out + 8, 0);
        std::memcpy(Ptr(out + 12), n.data(), n.size());
        if (c.Arg(5))
            Write<u32>(c.Arg(5), u32(12 + n.size()));
        return STATUS_SUCCESS;
    });
    reg("NtQueryDirectoryFile", [](Call& c) -> u64 {
        // FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length,
        // FileInformationClass, FileName (a pattern), RestartScan
        auto* dir = static_cast<HostFile*>(HostOf(c.Arg(0), 2));
        if (!dir)
            return STATUS_INVALID_HANDLE;
        if (c.Arg(7) != 1) // FileDirectoryInformation
        {
            Log("files: NtQueryDirectoryFile class %u not implemented", c.Arg(7));
            return STATUS_INVALID_PARAMETER;
        }
        if ((c.Arg(9) & 0xff) || dir->listing.empty())
        {
            dir->listing.clear();
            dir->next = 0;
            dir->pattern = c.Arg(8) ? GuestString(c.Arg(8)) : "*";
            if (dir->pattern.empty())
                dir->pattern = "*";
            if (DIR* d = opendir(dir->path.c_str()))
            {
                while (dirent* e = readdir(d))
                    if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..") &&
                        Matches(dir->pattern.c_str(), e->d_name))
                        dir->listing.push_back(e->d_name);
                closedir(d);
            }
            if (dir->listing.empty())
            {
                SetIo(c.Arg(4), 0xC000000F, 0);
                return 0xC000000F; // STATUS_NO_SUCH_FILE
            }
        }
        if (dir->next >= dir->listing.size())
        {
            SetIo(c.Arg(4), 0x80000006, 0);
            return 0x80000006; // STATUS_NO_MORE_FILES
        }
        const std::string& name = dir->listing[dir->next];
        struct stat st{};
        stat((dir->path + "/" + name).c_str(), &st);
        const u32 out = c.Arg(5);
        if (c.Arg(6) < 64 + name.size())
            return 0x80000005; // STATUS_BUFFER_OVERFLOW
        dir->next++;
        // FILE_DIRECTORY_INFORMATION (the name in 8-bit characters, as on the Xbox)
        Write<u32>(out + 0, 0);
        Write<u32>(out + 4, u32(dir->next));
        Write<s64>(out + 8, FileTime(st.st_ctim));
        Write<s64>(out + 16, FileTime(st.st_atim));
        Write<s64>(out + 24, FileTime(st.st_mtim));
        Write<s64>(out + 32, FileTime(st.st_mtim));
        Write<s64>(out + 40, st.st_size);
        Write<s64>(out + 48, (st.st_size + 4095) & ~4095LL);
        Write<u32>(out + 56, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        Write<u32>(out + 60, u32(name.size()));
        std::memcpy(Ptr(out + 64), name.data(), name.size());
        SetIo(c.Arg(4), STATUS_SUCCESS, u32(64 + name.size()));
        return STATUS_SUCCESS;
    });

    reg("XcSHAInit", [](Call& c) -> u64 {
        Sha1 s;
        s.Init();
        std::memcpy(Ptr(c.Arg(0)), &s, sizeof(s));
        return 0;
    });
    reg("XcSHAUpdate", [](Call& c) -> u64 {
        Sha1 s;
        std::memcpy(&s, Ptr(c.Arg(0)), sizeof(s));
        s.Update(Ptr<u8>(c.Arg(1)), c.Arg(2));
        std::memcpy(Ptr(c.Arg(0)), &s, sizeof(s));
        return 0;
    });
    reg("XcSHAFinal", [](Call& c) -> u64 {
        Sha1 s;
        std::memcpy(&s, Ptr(c.Arg(0)), sizeof(s));
        s.Final(Ptr<u8>(c.Arg(1)));
        return 0;
    });
    (void)STATUS_NOT_IMPLEMENTED;
    (void)STATUS_FILE_IS_A_DIRECTORY;
}

} // namespace xb
