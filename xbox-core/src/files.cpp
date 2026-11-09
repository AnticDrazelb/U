// Files: the Xbox's object paths mapped to host folders, and the Nt*File exports. D: and
// \Device\CdRom0 are the game's folder, or its disc image (read-only, through xiso.cpp); the hard
// disk's partitions (E:, T:, U:, Z: and \Device\Harddisk0\PartitionN) are folders under a data
// folder. Also the XcSHA* exports.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <strings.h>
#include <unistd.h>

#include "core.h"
#include "diagnostics.h"
#include "kernel.h"
#include "sha1.h"
#include "xiso.h"

namespace xb {

namespace {

constexpr u32 STATUS_SUCCESS = 0, STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034, STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A,
              STATUS_OBJECT_NAME_COLLISION = 0xC0000035, STATUS_INVALID_HANDLE = 0xC0000008,
              STATUS_END_OF_FILE = 0xC0000011, STATUS_ACCESS_DENIED = 0xC0000022,
              STATUS_INVALID_PARAMETER = 0xC000000D, STATUS_NOT_IMPLEMENTED = 0xC0000002,
              STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA, STATUS_NOT_A_DIRECTORY = 0xC0000103;

std::string g_game_dir = "."; // or kIso, for a disc image
std::string g_data_dir = "xbox-data";
std::mutex g_links_mutex;
std::map<std::string, std::string> g_links; // lower-case "\??\d:" -> device path

// what a path in the disc image maps to: this prefix, then its path in the image
const std::string kIso = "\x01xiso:";

bool InIso(const std::string& path, std::string& in_image)
{
    if (path.rfind(kIso, 0) != 0)
        return false;
    in_image = path.substr(kIso.size());
    return true;
}

struct HostFile
{
    const IsoEntry* iso = nullptr; // a file or folder in the disc image
    s64 position = 0;              // the image's files' current position
    int fd = -1;
    std::string path;
    bool directory = false;
    std::vector<std::string> listing; // NtQueryDirectoryFile: the names, read at the first query
    size_t next = 0;
    std::string pattern;
    u32 port = 0, key = 0; // the I/O completion object it is bound to (FileCompletionInformation)
    int raw = -1;      // the raw disk (0) or one of its partitions opened as a volume (1-5)
    u64 raw_size = 0;  // its size: reads past what was ever written there give zeros, as a disk does
};

// The retail hard disk's layout (partition 1 E:, 2 C:, 3-5 the X:, Y:, Z: cache partitions; 0 the
// whole disk, its first 512 KiB the config sectors: the cache table XAPI reads, the refurb info).
struct RawPartition
{
    u64 start, size;
};
constexpr RawPartition kPartitions[6] = {{0, 8ull << 30},          {0xABE80000, 0x131F00000}, {0x8CA80000, 0x1F400000},
                                         {0x00080000, 0x2EE00000}, {0x2EE80000, 0x2EE00000},  {0x5DC80000, 0x2EE00000}};

// "\Device\Harddisk0\PartitionN" itself (no trailing backslash): the volume, not its root folder
int RawPartitionOf(const std::string& xbox_path)
{
    std::string lower;
    for (char ch : xbox_path)
        lower += ch == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(ch)));
    const std::string prefix = "/device/harddisk0/partition";
    if (lower.size() != prefix.size() + 1 || lower.compare(0, prefix.size(), prefix) != 0)
        return -1;
    const char n = lower.back();
    return n >= '0' && n <= '5' ? n - '0' : -1;
}

// The raw disk's reads and writes, logged (they're few: the config sectors, a cache partition's format)
void LogRaw(const HostFile* f, const char* what, u64 at, u32 length, u32 status)
{
    static std::atomic<u32> logged{0};
    if (logged++ < 300)
        Log("disk: %s partition %d at %#llx, %#x bytes: %#x", what, f->raw, (unsigned long long)at, length, status);
}

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

std::string HostPathAsGiven(std::string path, u32 root);

// The host path with each part spelled as it is on the host, case aside (the Xbox's file systems
// ignore case; the host's may not). Parts that don't exist yet stay as given.
std::mutex g_case_mutex;
std::map<std::string, std::string> g_case_cache; // a path as asked -> as found

std::string ResolveCase(const std::string& path)
{
    struct stat st;
    if (path.empty() || stat(path.c_str(), &st) == 0)
        return path;
    std::lock_guard lock(g_case_mutex);
    if (auto it = g_case_cache.find(path); it != g_case_cache.end() && stat(it->second.c_str(), &st) == 0)
        return it->second;
    std::string out;
    size_t i = 0;
    if (path[0] == '/')
        out = "/", i = 1;
    bool resolving = true;
    while (i <= path.size())
    {
        size_t end = path.find('/', i);
        if (end == std::string::npos)
            end = path.size();
        std::string part = path.substr(i, end - i);
        i = end + 1;
        if (part.empty())
            continue;
        const std::string dir = out.empty() ? "." : out;
        const std::string sep = out.empty() || out.back() == '/' ? "" : "/";
        if (resolving && stat((out + sep + part).c_str(), &st) != 0)
        {
            bool found = false;
            if (DIR* d = opendir(dir.c_str()))
            {
                while (dirent* e = readdir(d))
                    if (strcasecmp(e->d_name, part.c_str()) == 0)
                    {
                        part = e->d_name, found = true;
                        break;
                    }
                closedir(d);
            }
            resolving = found;
        }
        out += sep + part;
    }
    g_case_cache[path] = out;
    return out;
}

// An Xbox object path -> a host path, or "" if it isn't a file system path.
std::string HostPath(std::string path, u32 root)
{
    const std::string host = HostPathAsGiven(path, root);
    return host.rfind(kIso, 0) == 0 ? host : ResolveCase(host);
}

std::string HostPathAsGiven(std::string path, u32 root)
{
    for (char& c : path)
        if (c == '\\')
            c = '/';
    if (root)
        if (auto* dir = static_cast<HostFile*>(HostOf(root, 2)))
            return dir->path + "/" + path;
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
    if (lower == "/device/harddisk0/partition0") // the raw disk
        return g_data_dir + "/partition0.img";
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

// A symbolic link's name as g_links keys it ("d:" for \\??\\D:, else the path with '/'), or why it can't be
// one: it must be absolute and in an object directory that exists.
u32 LinkKey(const std::string& name, std::string& key)
{
    if (name.empty() || name[0] != '\\')
        return 0xC0000033; // STATUS_OBJECT_NAME_INVALID
    key = Lower(name);
    for (char& ch : key)
        if (ch == '\\')
            ch = '/';
    const std::string parent = key.substr(0, key.rfind('/'));
    if (!parent.empty() && parent != "/?" "?" && parent != "/device" && parent != "/win32namedobjects")
        return 0xC000003A; // STATUS_OBJECT_PATH_NOT_FOUND
    if (key.rfind("/?" "?/", 0) == 0)
        key = key.substr(4);
    return 0;
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

// what the queries report about a file, from the host or the disc image
struct Info
{
    s64 size = 0, created = 0, accessed = 0, written = 0;
    bool directory = false, read_only = false;
};

bool StatPath(const std::string& path, Info& info)
{
    std::string in_image;
    if (InIso(path, in_image))
    {
        const IsoEntry* e = IsoFind(in_image);
        if (!e)
            return false;
        info.size = e->size;
        info.directory = e->directory;
        info.created = info.accessed = info.written = IsoTime();
        info.read_only = true;
        return true;
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;
    info.size = st.st_size;
    info.directory = S_ISDIR(st.st_mode);
    info.created = FileTime(st.st_ctim);
    info.accessed = FileTime(st.st_atim);
    info.written = FileTime(st.st_mtim);
    return true;
}

u32 Attributes(const Info& info)
{
    return info.directory ? 0x10 : info.read_only ? 0x01 : 0x80; // DIRECTORY, READONLY, NORMAL
}

// FILE_NETWORK_OPEN_INFORMATION (0x38 bytes)
void NetworkOpenInfo(const Info& info, u32 out)
{
    Write<s64>(out + 0x00, info.created);
    Write<s64>(out + 0x08, info.accessed);
    Write<s64>(out + 0x10, info.written);
    Write<s64>(out + 0x18, info.written);
    Write<s64>(out + 0x20, (info.size + 4095) & ~4095LL);
    Write<s64>(out + 0x28, info.size);
    Write<u32>(out + 0x30, Attributes(info));
}

// Everything in a folder (not the folder itself).
void RemoveContents(const std::string& dir)
{
    DIR* d = opendir(dir.c_str());
    if (!d)
        return;
    while (dirent* e = readdir(d))
    {
        const std::string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        const std::string path = dir + "/" + name;
        struct stat st;
        if (lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        {
            RemoveContents(path);
            rmdir(path.c_str());
        }
        else
            unlink(path.c_str());
    }
    closedir(d);
}

void MakeDirs(const std::string& path)
{
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/')
            mkdir(path.substr(0, i).c_str(), 0755);
}

u32 OpenFile(u32 handle_ptr, u32 object_attributes, u32 io_status, u32 disposition, u32 options, u32 access);

// Every open the guest asks for, logged with where it went and how it ended (the first 4000; then
// only failures), failures counted: a game that can't find its files says which.
u32 Open(u32 handle_ptr, u32 object_attributes, u32 io_status, u32 disposition, u32 options, u32 access)
{
    const u32 status = OpenFile(handle_ptr, object_attributes, io_status, disposition, options, access);
    static std::atomic<u32> logged{0};
    u32 root;
    const std::string xbox = ObjectPath(object_attributes, root);
    const bool failed = status >= 0x80000000u;
    if (logged++ < 4000 || failed)
        Log("file: %s %s (access %#x, disposition %u) -> %s: %#x", failed ? "FAILED" : "open", xbox.c_str(), access,
            disposition, HostPath(xbox, root).c_str(), status);
    if (failed && disposition == 1) // opening something that should be there
        diag::Note("file not found or not opened", xbox);
    return status;
}

u32 OpenFile(u32 handle_ptr, u32 object_attributes, u32 io_status, u32 disposition, u32 options, u32 access)
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
    std::string in_image;
    if (InIso(host, in_image))
    {
        const IsoEntry* e = IsoFind(in_image);
        const bool write = access & (0x40000000 | 0x2 | 0x4 | 0x10000000);
        u32 status = STATUS_SUCCESS;
        if (!e)
            status = disposition == 1 || disposition == 4 ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_ACCESS_DENIED;
        else if (write || disposition == 2 || disposition == 0 || disposition == 4 || disposition == 5)
            status = disposition == 2 ? STATUS_OBJECT_NAME_COLLISION : STATUS_ACCESS_DENIED; // read-only media
        else if ((options & 1) && !e->directory)
            status = STATUS_NOT_A_DIRECTORY;
        if (status != STATUS_SUCCESS)
        {
            if (g_verbose >= 1)
                Log("files: %s (disc image): %#x", xbox.c_str(), status);
            SetIo(io_status, status, 0);
            return status;
        }
        auto* f = new HostFile;
        f->path = host;
        f->iso = e;
        f->directory = e->directory;
        Write<u32>(handle_ptr, NewHostHandle(f->directory ? 2 : 1, f));
        SetIo(io_status, STATUS_SUCCESS, 1);
        if (g_verbose >= 1)
            Log("files: %s -> disc image /%s", xbox.c_str(), in_image.c_str());
        return STATUS_SUCCESS;
    }
    const int raw = RawPartitionOf(xbox);
    if (raw >= 0 && !(options & 1))
    {
        // the disk or a partition as a volume: an image file, sparse, that's always "there"
        auto* f = new HostFile;
        f->raw = raw;
        f->raw_size = kPartitions[raw].size;
        f->path = raw == 0 ? host : g_data_dir + "/partition" + char('0' + raw) + ".img";
        f->fd = open(f->path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (f->fd < 0)
        {
            delete f;
            SetIo(io_status, STATUS_ACCESS_DENIED, 0);
            return STATUS_ACCESS_DENIED;
        }
        Write<u32>(handle_ptr, NewHostHandle(1, f));
        SetIo(io_status, STATUS_SUCCESS, 1);
        return STATUS_SUCCESS;
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

bool LoadGame(const std::string& path, std::string& error)
{
    // an XBE starts "XBEH"; anything else is taken for a disc image (.iso, .xiso, whatever its name)
    bool image = true;
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        char magic[4] = {};
        image = std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "XBEH", 4) != 0;
        std::fclose(f);
    }
    if (!image)
    {
        if (!LoadXbe(path, g_xbe, error))
            return false;
        const size_t slash = path.find_last_of('/');
        SetGameDirectory(slash == std::string::npos ? "." : path.substr(0, slash));
        return true;
    }
    if (!MountIso(path, error))
        return false;
    const IsoEntry* e = IsoFind("default.xbe");
    if (!e || e->directory)
    {
        error = path + " has no default.xbe";
        return false;
    }
    std::vector<u8> data(e->size);
    if (IsoRead(*e, 0, data.size(), data.data()) != data.size())
    {
        error = "can't read default.xbe from " + path;
        return false;
    }
    if (!LoadXbeData(data, "xiso:default.xbe", g_xbe, error))
        return false;
    SetGameDirectory(kIso);
    return true;
}

void SetDataDirectory(const std::string& dir)
{
    g_data_dir = dir;
    // the hard disk's partitions, so that listing them works before a program writes there
    for (const char* p : {"", "/c", "/e", "/t", "/u", "/x", "/y", "/z"})
        mkdir((dir + p).c_str(), 0755);
    // the raw disk (partition 0: its config sectors), empty until a program writes them
    if (FILE* f = std::fopen((dir + "/partition0.img").c_str(), "ab"))
    {
        // a new one as a formatted retail disk has it (XboxHDM's, xemu's image): sector 3's refurb
        // info signature, everything else zeros (the cache table in sector 4 included)
        struct stat st;
        if (fstat(fileno(f), &st) == 0 && st.st_size == 0) // (not ftell: where append mode starts varies)
        {
            static const u8 sectors[0x610] = {};
            std::fwrite(sectors, 1, 0x600, f);
            std::fwrite("BRFR", 1, 4, f);
            std::fwrite(sectors, 1, 0x200 - 4, f);
        }
        std::fclose(f);
    }
}

void CloseHostHandle(int kind, void* host)
{
    if (kind == 4) // a symbolic link object: its target
    {
        delete static_cast<std::string*>(host);
        return;
    }
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
    // the I/O calls (handle, event, APC routine, APC context, I/O status block, ...): each ends as NT's
    // I/O manager ends a request (CompleteIo): overlapped I/O, ReadFileEx and completion ports rely on it
    const auto reg_io = [reg](const char* name, std::function<u64(Call&)> handler) {
        reg(name, [handler](Call& c) -> u64 {
            const u64 status = handler(c);
            if (u32(status) != STATUS_INVALID_HANDLE)
            {
                const HostFile* f = FileOf(c.Arg(0));
                CompleteFileIo(c.Arg(1), c.Arg(2), c.Arg(3), c.Arg(4), u32(status), c.Arg(4) ? Read<u32>(c.Arg(4) + 4) : 0,
                           f ? f->port : 0, f ? f->key : 0);
            }
            return status;
        });
    };
    reg("IoCreateSymbolicLink", [](Call& c) -> u64 { // link name, device name (OBJECT_STRING*)
        if (!c.Arg(0))
            return STATUS_SUCCESS; // an unnamed link: nothing anyone can find
        std::string key;
        if (const u32 status = LinkKey(GuestString(c.Arg(0)), key))
            return status;
        // the target: a device, or a file or directory on one
        const std::string device = GuestString(c.Arg(1));
        std::string lower = Lower(device);
        for (char& ch : lower)
            if (ch == '\\')
                ch = '/';
        bool known = false;
        for (const char* d : {"/device/cdrom0", "/device/harddisk0/partition0", "/device/harddisk0/partition1",
                              "/device/harddisk0/partition2", "/device/harddisk0/partition3",
                              "/device/harddisk0/partition4", "/device/harddisk0/partition5"})
        {
            const size_t n = std::strlen(d);
            if (lower.compare(0, n, d) == 0 && (lower.size() == n || lower[n] == '/'))
                known = true;
        }
        if (!known)
            return STATUS_INVALID_PARAMETER;
        const std::string host = HostPath(device, 0);
        struct stat st;
        if (!host.empty() && host.rfind(kIso, 0) != 0 && stat(host.c_str(), &st) != 0)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        {
            std::lock_guard lock(g_links_mutex);
            if (g_links.count(key))
                return 0xC0000035; // STATUS_OBJECT_NAME_COLLISION
            g_links[key] = device;
        }
        Log("files: %s -> %s", GuestString(c.Arg(0)).c_str(), device.c_str());
        return STATUS_SUCCESS;
    });
    reg("IoDeleteSymbolicLink", [](Call& c) -> u64 {
        std::string key;
        if (const u32 status = LinkKey(GuestString(c.Arg(0)), key))
            return status;
        std::lock_guard lock(g_links_mutex);
        return g_links.erase(key) ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
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
    reg_io("NtReadFile", [](Call& c) -> u64 {
        // FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset*
        HostFile* f = FileOf(c.Arg(0));
        if (!f || (f->fd < 0 && !(f->iso && !f->directory)))
            return STATUS_INVALID_HANDLE;
        ssize_t n;
        if (f->raw >= 0)
        {
            // the disk: every sector there is something (zeros where nothing was ever written)
            const u64 at = c.Arg(7) && Read<s64>(c.Arg(7)) >= 0 ? u64(Read<s64>(c.Arg(7))) : u64(lseek(f->fd, 0, SEEK_CUR));
            const u32 length = at >= f->raw_size ? 0 : u32(std::min<u64>(c.Arg(6), f->raw_size - at));
            u8* out = static_cast<u8*>(Ptr(c.Arg(5)));
            ssize_t got = length ? pread(f->fd, out, length, off_t(at)) : 0;
            if (got < 0)
                got = 0;
            std::memset(out + got, 0, length - size_t(got));
            lseek(f->fd, off_t(at + length), SEEK_SET);
            const u32 status = length == 0 && c.Arg(6) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
            LogRaw(f, "read", at, c.Arg(6), status);
            Log("disk: (into %#x)", c.Arg(5));
            SetIo(c.Arg(4), status, length);
            if (c.Arg(1))
                WakeAll();
            return status;
        }
        if (f->iso)
        {
            const s64 at = c.Arg(7) && Read<s64>(c.Arg(7)) >= 0 ? Read<s64>(c.Arg(7)) : f->position;
            n = ssize_t(IsoRead(*f->iso, u64(at), c.Arg(6), Ptr(c.Arg(5))));
            f->position = at + n;
        }
        else if (c.Arg(7))
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
    reg_io("NtWriteFile", [](Call& c) -> u64 {
        HostFile* f = FileOf(c.Arg(0));
        if (f && f->iso)
            return STATUS_ACCESS_DENIED;
        if (!f || f->fd < 0)
            return STATUS_INVALID_HANDLE;
        ssize_t n;
        if (f->raw >= 0)
        {
            const u64 at = c.Arg(7) && Read<s64>(c.Arg(7)) >= 0 ? u64(Read<s64>(c.Arg(7))) : u64(lseek(f->fd, 0, SEEK_CUR));
            const u32 length = at >= f->raw_size ? 0 : u32(std::min<u64>(c.Arg(6), f->raw_size - at));
            const ssize_t put = length ? pwrite(f->fd, Ptr(c.Arg(5)), length, off_t(at)) : 0;
            const u32 status = put == ssize_t(length) && length == c.Arg(6) ? STATUS_SUCCESS : 0xC0000185; // IO_DEVICE_ERROR
            LogRaw(f, "write", at, c.Arg(6), status);
            // a cache partition (X:, Y:, Z:) given a new file system: what was in it is gone
            if (status == STATUS_SUCCESS && at == 0 && f->raw >= 3 && length >= 4 && !std::memcmp(Ptr(c.Arg(5)), "FATX", 4))
            {
                const std::string dir = g_data_dir + "/" + "xyz"[f->raw - 3];
                Log("disk: partition %d (%s) formatted: emptied", f->raw, dir.c_str());
                RemoveContents(dir);
            }
            lseek(f->fd, off_t(at + length), SEEK_SET);
            SetIo(c.Arg(4), status, put < 0 ? 0 : u32(put));
            if (c.Arg(1))
                WakeAll();
            return status;
        }
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
        Info info;
        if (!StatPath(f->path, info))
            return STATUS_OBJECT_NAME_NOT_FOUND;
        const u32 out = c.Arg(2);
        switch (c.Arg(4))
        {
        case 4: // FileBasicInformation
            Write<s64>(out, info.created);
            Write<s64>(out + 8, info.accessed);
            Write<s64>(out + 16, info.written);
            Write<s64>(out + 24, info.written);
            Write<u32>(out + 32, Attributes(info));
            SetIo(c.Arg(1), 0, 40);
            return STATUS_SUCCESS;
        case 5: // FileStandardInformation
            Write<s64>(out, (info.size + 4095) & ~4095LL);
            Write<s64>(out + 8, info.size);
            Write<u32>(out + 16, 1);
            Write<u8>(out + 20, 0);
            Write<u8>(out + 21, info.directory);
            SetIo(c.Arg(1), 0, 24);
            return STATUS_SUCCESS;
        case 14: // FilePositionInformation
            Write<s64>(out, f->iso ? f->position : f->fd >= 0 ? lseek(f->fd, 0, SEEK_CUR) : 0);
            SetIo(c.Arg(1), 0, 8);
            return STATUS_SUCCESS;
        case 34: // FileNetworkOpenInformation
            NetworkOpenInfo(info, out);
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
        if (f->iso)
        {
            if (c.Arg(4) != 14)
                return STATUS_ACCESS_DENIED; // read-only media
            f->position = Read<s64>(in);
            SetIo(c.Arg(1), 0, 0);
            return STATUS_SUCCESS;
        }
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
        case 30: // FileCompletionInformation: port handle, key
            f->port = KernelObjectOf(Read<u32>(in));
            f->key = Read<u32>(in + 4);
            SetIo(c.Arg(1), 0, 0);
            return f->port ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
        case 10: // FileRenameInformation: ReplaceIfExists, RootDirectory, FileName (OBJECT_STRING): MoveFile
        {
            const std::string to = HostPath(GuestString(in + 8), Read<u32>(in + 4));
            if (to.empty() || f->path.empty())
                return STATUS_INVALID_PARAMETER;
            struct stat st;
            if (!Read<u8>(in) && stat(to.c_str(), &st) == 0)
                return STATUS_OBJECT_NAME_COLLISION;
            if (std::rename(f->path.c_str(), to.c_str()) != 0)
                return STATUS_ACCESS_DENIED;
            Log("files: renamed %s to %s", f->path.c_str(), to.c_str());
            f->path = to;
            SetIo(c.Arg(1), 0, 0);
            return STATUS_SUCCESS;
        }
        case 4:  // FileBasicInformation (times, attributes): kept as the host has them
        case 16: // FileModeInformation
            SetIo(c.Arg(1), 0, 0);
            return STATUS_SUCCESS;
        }
        Log("files: NtSetInformationFile class %u not implemented", c.Arg(4));
        return STATUS_INVALID_PARAMETER;
    });
    reg("NtQueryFullAttributesFile", [](Call& c) -> u64 {
        u32 root;
        const std::string host = HostPath(ObjectPath(c.Arg(0), root), root);
        Info info;
        if (host.empty() || !StatPath(host, info))
            return STATUS_OBJECT_NAME_NOT_FOUND;
        NetworkOpenInfo(info, c.Arg(1));
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
        const u32 length = c.Arg(3);
        switch (c.Arg(4))
        {
        case 1: // FileFsVolumeInformation: creation time, serial number, label length, objects, label
            if (length < 18)
                return 0xC0000004; // STATUS_INFO_LENGTH_MISMATCH
            std::memset(Ptr(out), 0, 18);
            Write<u32>(out + 8, 0x58424f58); // the serial number
            SetIo(c.Arg(1), 0, 18);
            return STATUS_SUCCESS;
        case 4: // FileFsDeviceInformation: type, characteristics
            if (length < 8)
                return 0xC0000004;
            Write<u32>(out, 7); // FILE_DEVICE_DISK
            Write<u32>(out + 4, 0);
            SetIo(c.Arg(1), 0, 8);
            return STATUS_SUCCESS;
        case 5: // FileFsAttributeInformation: attributes, longest name, name length, name ("FATX")
            if (length < 16)
                return 0xC0000004;
            Write<u32>(out, 0x2);    // FILE_CASE_PRESERVED_NAMES
            Write<u32>(out + 4, 42); // FATX names
            Write<u32>(out + 8, 4);
            std::memcpy(Ptr(out + 12), "FATX", 4);
            SetIo(c.Arg(1), 0, 16);
            return STATUS_SUCCESS;
        case 7: // FileFsFullSizeInformation: total, caller's free, free; sectors per cluster, sector size
            if (length < 32)
                return 0xC0000004;
            Write<s64>(out, 8ll * 1024 * 1024 * 1024 / 16384);
            Write<s64>(out + 8, 4ll * 1024 * 1024 * 1024 / 16384);
            Write<s64>(out + 16, 4ll * 1024 * 1024 * 1024 / 16384);
            Write<u32>(out + 24, 32);
            Write<u32>(out + 28, 512);
            SetIo(c.Arg(1), 0, 32);
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
    // symbolic links: the drive letters (the program's own, else the defaults)
    reg("NtOpenSymbolicLinkObject", [](Call& c) -> u64 { // handle*, attributes
        u32 root;
        std::string name = Lower(ObjectPath(c.Arg(1), root));
        for (char& ch : name)
            if (ch == '\\')
                ch = '/';
        if (name.rfind("/?" "?/", 0) == 0)
            name = name.substr(4);
        if (name.size() != 2 || name[1] != ':')
            return STATUS_OBJECT_NAME_NOT_FOUND;
        std::string target;
        {
            std::lock_guard lock(g_links_mutex);
            if (auto it = g_links.find(name); it != g_links.end())
                target = it->second;
        }
        if (target.empty())
            switch (name[0])
            {
            case 'd': target = "\\Device\\CdRom0"; break;
            case 'c': target = "\\Device\\Harddisk0\\Partition2"; break;
            case 'e': target = "\\Device\\Harddisk0\\Partition1"; break;
            case 'x': target = "\\Device\\Harddisk0\\Partition3"; break;
            case 'y': target = "\\Device\\Harddisk0\\Partition4"; break;
            case 'z': target = "\\Device\\Harddisk0\\Partition5"; break;
            default: return STATUS_OBJECT_NAME_NOT_FOUND;
            }
        Write<u32>(c.Arg(0), NewHostHandle(4, new std::string(target)));
        return STATUS_SUCCESS;
    });
    reg("NtQuerySymbolicLinkObject", [](Call& c) -> u64 { // handle, target (OBJECT_STRING)*, length*
        auto* target = static_cast<std::string*>(HostOf(c.Arg(0), 4));
        if (!target)
            return STATUS_INVALID_HANDLE;
        const u32 s = c.Arg(1), max = Read<u16>(s + 2), buffer = Read<u32>(s + 4);
        if (c.Arg(2))
            Write<u32>(c.Arg(2), u32(target->size()));
        if (target->size() > max)
            return 0xC0000023; // STATUS_BUFFER_TOO_SMALL
        std::memcpy(Ptr(buffer), target->data(), target->size());
        Write<u16>(s, u16(target->size()));
        return STATUS_SUCCESS;
    });
    reg("NtCreateDirectoryObject", [](Call& c) -> u64 { Write<u32>(c.Arg(0), NewHostHandle(3, nullptr)); return STATUS_SUCCESS; });
    reg("IoCreateFile", [](Call& c) -> u64 {
        // as NtCreateFile, with Options after CreateOptions
        return Open(c.Arg(0), c.Arg(2), c.Arg(3), c.Arg(7), c.Arg(8), c.Arg(1));
    });
    reg("NtDeleteFile", [](Call& c) -> u64 { // attributes
        u32 root;
        const std::string host = HostPath(ObjectPath(c.Arg(0), root), root);
        std::string in_image;
        if (host.empty())
            return STATUS_OBJECT_PATH_NOT_FOUND;
        if (InIso(host, in_image))
            return STATUS_ACCESS_DENIED;
        if (unlink(host.c_str()) == 0 || rmdir(host.c_str()) == 0)
            return STATUS_SUCCESS;
        return errno == ENOENT ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_ACCESS_DENIED;
    });
    // NtReadFileScatter / NtWriteFileGather: handle, event, apc, context, io status, segments (a page
    // pointer each: FILE_SEGMENT_ELEMENT is 4 bytes on the Xbox), length, offset*
    auto scatter = [](Call& c, bool write) -> u64 {
        HostFile* f = FileOf(c.Arg(0));
        if (!f || f->directory || (!f->iso && f->fd < 0))
            return STATUS_INVALID_HANDLE;
        if (write && f->iso)
            return STATUS_ACCESS_DENIED;
        s64 at = c.Arg(7) ? Read<s64>(c.Arg(7)) : (f->iso ? f->position : lseek(f->fd, 0, SEEK_CUR));
        u32 done = 0;
        for (u32 i = 0; done < c.Arg(6); i++)
        {
            const u32 page = Read<u32>(c.Arg(5) + i * 4), n = std::min<u32>(4096, c.Arg(6) - done);
            ssize_t got;
            if (f->iso)
                got = ssize_t(IsoRead(*f->iso, u64(at), n, Ptr(page)));
            else
                got = write ? pwrite(f->fd, Ptr(page), n, off_t(at)) : pread(f->fd, Ptr(page), n, off_t(at));
            if (got <= 0)
                break;
            done += u32(got), at += got;
            if (u32(got) < n)
                break;
        }
        if (f->iso)
            f->position = at;
        else if (!c.Arg(7))
            lseek(f->fd, off_t(at), SEEK_SET);
        const u32 status = !write && done == 0 && c.Arg(6) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
        SetIo(c.Arg(4), status, done);
        if (c.Arg(1))
            WakeAll();
        return status;
    };
    reg_io("NtReadFileScatter", [scatter](Call& c) -> u64 { return scatter(c, false); });
    reg_io("NtWriteFileGather", [scatter](Call& c) -> u64 { return scatter(c, true); });
    // device controls: the disk's geometry and partitions, as XAPI asks when it mounts drives
    // `partition`: the handle's (the disk 0, a partition 1-5; -1 not known: the 750 MiB of a cache one)
    auto ioctl = [](u32 code, u32 out, u32 out_length, int partition) -> u32 { // returns bytes written
        if (out && out_length)
            std::memset(Ptr(out), 0, out_length);
        switch (code)
        {
        case 0x70000:   // IOCTL_DISK_GET_DRIVE_GEOMETRY: 8 GiB of 512-byte sectors
        case 0x2404c:   // IOCTL_CDROM_GET_DRIVE_GEOMETRY
            if (out_length < 24)
                return 0;
            Write<s64>(out, code == 0x70000 ? 8ll * 1024 * 1024 * 1024 / 512 : 3820880); // cylinders
            Write<u32>(out + 8, code == 0x70000 ? 12 : 11);                              // fixed / removable
            Write<u32>(out + 12, 1);
            Write<u32>(out + 16, 1);
            Write<u32>(out + 20, code == 0x70000 ? 512 : 2048);
            return 24;
        case 0x74004: // IOCTL_DISK_GET_PARTITION_INFO: where it is on the disk, recognized
        {
            if (out_length < 32)
                return 0;
            const RawPartition p = partition >= 0 && partition <= 5 ? kPartitions[partition] : RawPartition{0x00080000, 0x2EE00000};
            Write<s64>(out, s64(p.start));
            Write<s64>(out + 8, s64(p.size));
            Write<u32>(out + 16, 0);                             // hidden sectors
            Write<u32>(out + 20, partition > 0 ? u32(partition) : 1); // partition number
            Write<u8>(out + 24, 0x0b);                           // type
            Write<u8>(out + 25, 0);                              // boot indicator
            Write<u8>(out + 26, 1);                              // recognized
            return 32;
        }
        case 0x74800: // IOCTL_DISK_VERIFY: every sector reads
        case 0x4d014: // IOCTL_SCSI_PASS_THROUGH_DIRECT (the DVD drive's commands): nothing back
            return 0;
        }
        char name[16];
        std::snprintf(name, sizeof(name), "%#x", code);
        diag::Note("device control", name);
        return 0;
    };
    auto partition_of = [](u32 handle) {
        const HostFile* f = FileOf(handle);
        return f ? f->raw : -1;
    };
    reg_io("NtDeviceIoControlFile", [ioctl, partition_of](Call& c) -> u64 {
        // handle, event, apc, context, io status, code, in, in length, out, out length
        const u32 n = ioctl(c.Arg(5), c.Arg(8), c.Arg(9), partition_of(c.Arg(0)));
        static std::atomic<u32> logged{0};
        if (logged++ < 200)
            Log("files: device control %#x on handle %#x (partition %d): %u bytes back", c.Arg(5), c.Arg(0),
                partition_of(c.Arg(0)), n);
        SetIo(c.Arg(4), STATUS_SUCCESS, n);
        if (c.Arg(1))
            WakeAll();
        return STATUS_SUCCESS;
    });
    reg("IoSynchronousDeviceIoControlRequest", [ioctl](Call& c) -> u64 {
        // code, device, in, in length, out, out length, returned length*, internal
        const u32 n = ioctl(c.Arg(0), c.Arg(4), c.Arg(5), -1);
        if (c.Arg(6))
            Write<u32>(c.Arg(6), n);
        return STATUS_SUCCESS;
    });
    reg_io("NtFsControlFile", [](Call& c) -> u64 {
        Log("files: file system control %#x on handle %#x", c.Arg(5), c.Arg(0));
        SetIo(c.Arg(4), STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    });
    reg("IoDismountVolume", [](Call&) -> u64 { return STATUS_SUCCESS; });
    reg("IoDismountVolumeByName", [](Call&) -> u64 { return STATUS_SUCCESS; });
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
    reg_io("NtQueryDirectoryFile", [](Call& c) -> u64 {
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
            if (dir->iso)
            {
                for (u32 child : dir->iso->children)
                    if (Matches(dir->pattern.c_str(), IsoEntryAt(child).name.c_str()))
                        dir->listing.push_back(IsoEntryAt(child).name);
            }
            else if (DIR* d = opendir(dir->path.c_str()))
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
        Info info;
        StatPath(dir->path + "/" + name, info);
        const u32 out = c.Arg(5);
        if (c.Arg(6) < 64 + name.size())
            return 0x80000005; // STATUS_BUFFER_OVERFLOW
        dir->next++;
        // FILE_DIRECTORY_INFORMATION (the name in 8-bit characters, as on the Xbox)
        Write<u32>(out + 0, 0);
        Write<u32>(out + 4, u32(dir->next));
        Write<s64>(out + 8, info.created);
        Write<s64>(out + 16, info.accessed);
        Write<s64>(out + 24, info.written);
        Write<s64>(out + 32, info.written);
        Write<s64>(out + 40, info.size);
        Write<s64>(out + 48, (info.size + 4095) & ~4095LL);
        Write<u32>(out + 56, Attributes(info));
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
