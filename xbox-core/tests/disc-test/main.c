// Reads the files that tests/disc-test/make-image.sh puts beside it on D: (a folder or a disc image)
// and reports each check through DbgPrint ("PASS"/"FAIL"), for xbox-core's log.
#include <hal/debug.h>
#include <hal/video.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

static int failures = 0;
static void check(const char *what, int ok)
{
    DbgPrint("%s %s\n", ok ? "PASS" : "FAIL", what);
    debugPrint("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

// the pattern make-image.sh writes: byte i is (i * 7 + i / 251) & 255
static int pattern_ok(const unsigned char *data, DWORD at, DWORD n)
{
    for (DWORD i = 0; i < n; i++)
        if (data[i] != (unsigned char)(((at + i) * 7 + (at + i) / 251) & 255))
            return 0;
    return 1;
}

static unsigned char buffer[300000];

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    HANDLE f = CreateFileA("D:\\data\\big.bin", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    check("open D:\\data\\big.bin", f != INVALID_HANDLE_VALUE);
    check("its size", GetFileSize(f, NULL) == 300000);
    DWORD got = 0;
    check("read all of it", ReadFile(f, buffer, sizeof(buffer), &got, NULL) && got == 300000 && pattern_ok(buffer, 0, got));
    check("read at the end: nothing more", ReadFile(f, buffer, 100, &got, NULL) && got == 0);
    SetFilePointer(f, 2040, NULL, FILE_BEGIN);
    check("read across a sector boundary", ReadFile(f, buffer, 20, &got, NULL) && got == 20 && pattern_ok(buffer, 2040, 20));
    check("the position after it", SetFilePointer(f, 0, NULL, FILE_CURRENT) == 2060);
    CloseHandle(f);

    f = CreateFileA("D:\\Data\\SUB\\Hello.TXT", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    check("open a nested file by other case", f != INVALID_HANDLE_VALUE);
    memset(buffer, 0, 64);
    check("its text", ReadFile(f, buffer, 64, &got, NULL) && got == 12 && memcmp(buffer, "hello, disc\n", 12) == 0);
    CloseHandle(f);

    check("a missing file", CreateFileA("D:\\data\\nope.bin", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) ==
                                INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND);

    WIN32_FIND_DATAA fd;
    int count = 0, saw_sub = 0;
    HANDLE find = FindFirstFileA("D:\\data\\*", &fd);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            count++;
            if (strcmp(fd.cFileName, "sub") == 0 && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                saw_sub = 1;
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    DbgPrint("  D:\\data holds %d entries\n", count);
    check("list D:\\data (big.bin, sub, a1..a20.txt)", count == 22 && saw_sub);
    count = 0;
    find = FindFirstFileA("D:\\data\\a1*.txt", &fd);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
            count++;
        while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    check("list with a pattern (a1*.txt: a1, a10..a19)", count == 11);

    WIN32_FILE_ATTRIBUTE_DATA attributes;
    check("GetFileAttributesEx", GetFileAttributesExA("D:\\data\\big.bin", GetFileExInfoStandard, &attributes) &&
                                     attributes.nFileSizeLow == 300000);

    // the D: link
    OBJECT_ATTRIBUTES oa;
    ANSI_STRING name;
    HANDLE link;
    RtlInitAnsiString(&name, "\\??\\D:");
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    char target_text[64];
    ANSI_STRING target = {0, sizeof(target_text), target_text};
    check("NtOpenSymbolicLinkObject D:", NtOpenSymbolicLinkObject(&link, &oa) == 0);
    check("NtQuerySymbolicLinkObject: \\Device\\CdRom0", NtQuerySymbolicLinkObject(link, &target, NULL) == 0 &&
                                                          target.Length == 14 && memcmp(target_text, "\\Device\\CdRom0", 14) == 0);
    NtClose(link);

    // a file made and deleted on E:
    f = CreateFileA("E:\\disc-test.tmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    check("create E:\\disc-test.tmp", f != INVALID_HANDLE_VALUE && WriteFile(f, "x", 1, &got, NULL));
    CloseHandle(f);
    check("DeleteFile", DeleteFileA("E:\\disc-test.tmp") && GetFileAttributesA("E:\\disc-test.tmp") == INVALID_FILE_ATTRIBUTES);

    // a scatter read: two pages of big.bin from 4096 on
    unsigned char *pages = MmAllocateContiguousMemory(8192);
    FILE_SEGMENT_ELEMENT segments[3] = {0};
    segments[0].Buffer = pages;
    segments[1].Buffer = pages + 4096;
    HANDLE raw;
    IO_STATUS_BLOCK io;
    RtlInitAnsiString(&name, "\\??\\D:\\data\\big.bin");
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    NtOpenFile(&raw, GENERIC_READ | SYNCHRONIZE, &oa, &io, FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_NONALERT);
    LARGE_INTEGER offset = {.QuadPart = 4096};
    check("NtReadFileScatter", NtReadFileScatter(raw, NULL, NULL, NULL, &io, segments, 8192, &offset) == 0 &&
                                   io.Information == 8192 && pattern_ok(pages, 4096, 8192));
    DISK_GEOMETRY geometry;
    check("IOCTL_DISK_GET_DRIVE_GEOMETRY", NtDeviceIoControlFile(raw, NULL, NULL, NULL, &io, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
                                                                 &geometry, sizeof(geometry)) == 0 && geometry.BytesPerSector == 512);
    NtClose(raw);

    // the raw disk, as XAPI's cache drive set-up uses it (XMountUtilityDrive): partition 0's config
    // sectors read even on a fresh disk (zeros), written and read back; a cache partition opened as
    // a volume, its geometry and place, given a FATX header (formatted: its files go)
    RtlInitAnsiString(&name, "\\Device\\Harddisk0\\Partition0");
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    check("open the raw disk", NtOpenFile(&raw, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa, &io, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          FILE_SYNCHRONOUS_IO_ALERT) == 0);
    offset.QuadPart = 0x600000; // well past anything written (the image grows only as it's written)
    memset(pages, 0xcc, 512);
    check("a sector never written reads as zeros", NtReadFile(raw, NULL, NULL, NULL, &io, pages, 512, &offset) == 0 &&
                                                   io.Information == 512 && pages[0] == 0 && pages[511] == 0);
    offset.QuadPart = 4 * 512;
    memset(pages, 0x5a, 512);
    check("write a config sector", NtWriteFile(raw, NULL, NULL, NULL, &io, pages, 512, &offset) == 0 && io.Information == 512);
    memset(pages, 0, 512);
    check("and read it back", NtReadFile(raw, NULL, NULL, NULL, &io, pages, 512, &offset) == 0 && pages[0] == 0x5a && pages[511] == 0x5a);
    memset(pages, 0, 512); // left as a fresh disk's
    NtWriteFile(raw, NULL, NULL, NULL, &io, pages, 512, &offset);
    NtClose(raw);

    f = CreateFileA("Z:\\cache-file.tmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CloseHandle(f);
    RtlInitAnsiString(&name, "\\Device\\Harddisk0\\Partition5");
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    check("open partition 5 (Z:) as a volume", NtOpenFile(&raw, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa, &io, 0,
                                                          FILE_SYNCHRONOUS_IO_ALERT) == 0);
    PARTITION_INFORMATION partition;
    check("its place on the disk", NtDeviceIoControlFile(raw, NULL, NULL, NULL, &io, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0, &partition,
                                                        sizeof(partition)) == 0 &&
                                       partition.StartingOffset.QuadPart == 0x5DC80000 && partition.PartitionLength.QuadPart == 0x2EE00000);
    memset(pages, 0, 4096);
    memcpy(pages, "FATX", 4);
    offset.QuadPart = 0;
    check("format it", NtWriteFile(raw, NULL, NULL, NULL, &io, pages, 4096, &offset) == 0);
    NtClose(raw);
    check("what was on it is gone", GetFileAttributesA("Z:\\cache-file.tmp") == INVALID_FILE_ATTRIBUTES);

    DbgPrint("disc test: %d failures\n", failures);
    debugPrint("disc test: %d failures\n", failures);
    for (;;)
        Sleep(1000);
}
