#pragma once

// The process and memory queries the font tests make, on Linux, under the Win32 names the shared test sources use
// (test code only, like win32_test_compat.hpp, which this extends for the font tests): the open descriptor count for
// GetProcessHandleCount, VirtualQuery over /proc/self/maps and the allocation records of src/os/linux/pages.cpp, and
// MultiByteToWideChar for UTF-8. Empty on Windows.
#if !defined(_WIN32)

#include "maicord/core/utf.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/file.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"

#ifndef MEM_COMMIT
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_FREE 0x00010000
#define MEM_PRIVATE 0x00020000
#define MEM_MAPPED 0x00040000
#endif
#ifndef PAGE_NOACCESS
#define PAGE_NOACCESS 0x01
#endif
#ifndef PAGE_READONLY
#define PAGE_READONLY 0x02
#endif
#ifndef PAGE_READWRITE
#define PAGE_READWRITE 0x04
#endif
#ifndef PAGE_EXECUTE_READ
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#endif
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 0x00000008

struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
};
typedef _MEMORY_BASIC_INFORMATION MEMORY_BASIC_INFORMATION;
typedef _MEMORY_BASIC_INFORMATION* PMEMORY_BASIC_INFORMATION;

namespace maicord::linux_test_process {

inline maicord::u64 parse_hex(const char*& at, const char* end) noexcept {
    maicord::u64 value = 0;
    while (at < end) {
        const char c = *at;
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (digit < 0)
            break;
        value = value << 4 | static_cast<maicord::u64>(digit);
        ++at;
    }
    return value;
}

// One mapping of /proc/self/maps: [start, end), its permissions and whether a file backs it.
struct Mapping {
    maicord::u64 start;
    maicord::u64 end;
    char permissions[5];
    bool file;
    bool found;
};

// The mapping that holds `address`, or {found: false} with the end of the gap in `end`.
inline Mapping find_mapping(maicord::u64 address) noexcept {
    Mapping result{};
    const long fd = maicord::sys_open("/proc/self/maps", maicord::kOpenReadOnly);
    if (maicord::syscall_failed(fd))
        return result;
    char chunk[4096];
    char line[512];
    maicord::u32 length = 0;
    bool overflow = false;
    result.end = ~maicord::u64{0};
    for (;;) {
        const long read = maicord::sys_read(static_cast<int>(fd), chunk, sizeof(chunk));
        if (read <= 0)
            break;
        for (long i = 0; i < read; ++i) {
            if (chunk[i] != '\n') {
                if (length < sizeof(line))
                    line[length++] = chunk[i];
                else
                    overflow = true;
                continue;
            }
            const char* at = line;
            const char* end = line + length;
            const maicord::u64 start = parse_hex(at, end);
            if (at < end && *at == '-')
                ++at;
            const maicord::u64 stop = parse_hex(at, end);
            if (at < end && *at == ' ')
                ++at;
            if (!overflow && start < stop && end - at >= 4) {
                if (address >= start && address < stop) {
                    result.start = start;
                    result.end = stop;
                    for (int k = 0; k < 4; ++k)
                        result.permissions[k] = at[k];
                    // "perms offset dev inode path": the inode is the fourth field.
                    const char* field = at + 4;
                    for (int skipped = 0; skipped < 3 && field < end; ++skipped) {
                        while (field < end && *field == ' ')
                            ++field;
                        while (field < end && *field != ' ')
                            ++field;
                    }
                    while (field < end && *field == ' ')
                        ++field;
                    result.file = field < end && *field != '0';
                    result.found = true;
                    maicord::sys_close(static_cast<int>(fd));
                    return result;
                }
                if (start > address && start < result.end)
                    result.end = start;
            }
            length = 0;
            overflow = false;
        }
    }
    maicord::sys_close(static_cast<int>(fd));
    return result;
}

} // namespace maicord::linux_test_process

inline SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION information, SIZE_T) noexcept {
    const maicord::u64 target = reinterpret_cast<maicord::u64>(address);
    const auto mapping = maicord::linux_test_process::find_mapping(target);
    const maicord::u64 page = maicord::pages_page_size();
    *information = {};
    information->BaseAddress = reinterpret_cast<PVOID>(target & ~(page - 1));
    if (!mapping.found) {
        information->AllocationBase = nullptr;
        information->State = MEM_FREE;
        information->Protect = PAGE_NOACCESS;
        information->RegionSize = static_cast<SIZE_T>(mapping.end - (target & ~(page - 1)));
        return sizeof(*information);
    }
    const bool none = mapping.permissions[0] == '-' && mapping.permissions[1] == '-' && mapping.permissions[2] == '-';
    const bool executable = mapping.permissions[2] == 'x';
    const bool writable = mapping.permissions[1] == 'w';
    information->State = none ? MEM_RESERVE : MEM_COMMIT;
    information->Protect = none ? PAGE_NOACCESS
                                : executable ? (writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                                             : (writable ? PAGE_READWRITE : PAGE_READONLY);
    information->AllocationProtect = information->Protect;
    information->Type = mapping.file ? MEM_MAPPED : MEM_PRIVATE;
    void* recorded = maicord::pages_range_base(address);
    information->AllocationBase = recorded ? recorded : reinterpret_cast<PVOID>(mapping.start);
    information->RegionSize = static_cast<SIZE_T>(mapping.end - (target & ~(page - 1)));
    return sizeof(*information);
}

// A hard link: linkat(AT_FDCWD, existing, AT_FDCWD, link, 0).
inline BOOL CreateHardLinkW(LPCWSTR link, LPCWSTR existing, LPSECURITY_ATTRIBUTES) noexcept {
    char link_path[4096];
    char existing_path[4096];
    if (maicord::path_to_utf8(link, link_path, sizeof(link_path)) == 0 ||
        maicord::path_to_utf8(existing, existing_path, sizeof(existing_path)) == 0)
        return FALSE;
    constexpr long kLinkat = 265;
    constexpr long kCurrent = -100;
    return !maicord::syscall_failed(maicord::syscall5(kLinkat, kCurrent, reinterpret_cast<long>(existing_path), kCurrent,
                                                      reinterpret_cast<long>(link_path), 0));
}

inline BOOL RemoveDirectoryW(LPCWSTR path) noexcept {
    return maicord::dir_remove(path) ? TRUE : FALSE;
}

#ifndef INVALID_FILE_SIZE
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#endif
// The low 32 bits of the size (the high part, when asked for, in *high).
inline DWORD GetFileSize(HANDLE file, LPDWORD high) noexcept {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size))
        return INVALID_FILE_SIZE;
    if (high)
        *high = static_cast<DWORD>(static_cast<maicord::u64>(size.QuadPart) >> 32);
    return static_cast<DWORD>(size.QuadPart);
}

// There is no Windows directory: the tests that read Windows' own fonts through it (their *_OWNED_TEST cases) fail to
// find them and are not registered on Linux.
inline UINT GetWindowsDirectoryW(LPWSTR, UINT) noexcept {
    return 0;
}

inline int lstrcmpW(LPCWSTR a, LPCWSTR b) noexcept {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return static_cast<int>(static_cast<maicord::u16>(*a)) - static_cast<int>(static_cast<maicord::u16>(*b));
}

inline HANDLE GetCurrentProcess() noexcept {
    return reinterpret_cast<HANDLE>(~maicord::u64{0});
}

// The descriptors this process holds open: the entries of /proc/self/fd, less the one the listing itself opened.
inline BOOL GetProcessHandleCount(HANDLE, PDWORD count) noexcept {
    maicord::DirIter iterator{};
    if (!maicord::dir_open(&iterator, L"/proc/self/fd"))
        return FALSE;
    maicord::DirEntry entry;
    DWORD entries = 0;
    while (maicord::dir_next(&iterator, &entry))
        ++entries;
    maicord::dir_close(&iterator);
    *count = entries ? entries - 1 : 0;
    return TRUE;
}

// UTF-8 to UTF-16 only; with a zero capacity it returns the length the conversion needs. Zero when the input is
// not valid UTF-8 (and the flags ask for that: they always do here).
inline int MultiByteToWideChar(UINT, DWORD, LPCCH text, int bytes, LPWSTR output, int capacity) noexcept {
    if (bytes < 0) {
        bytes = 0;
        while (text[bytes])
            ++bytes;
    }
    int units = 0;
    for (int offset = 0; offset < bytes;) {
        const maicord::Utf8DecodeResult decoded = maicord::utf8_decode_one(text + offset, static_cast<maicord::u32>(bytes - offset));
        if (!decoded.valid)
            return 0;
        offset += decoded.width;
        const int needed = decoded.codepoint >= 0x10000 ? 2 : 1;
        if (capacity != 0) {
            if (units + needed > capacity)
                return 0;
            if (needed == 2) {
                const maicord::u32 value = decoded.codepoint - 0x10000;
                output[units] = static_cast<wchar_t>(0xD800 + (value >> 10));
                output[units + 1] = static_cast<wchar_t>(0xDC00 + (value & 0x3FF));
            } else {
                output[units] = static_cast<wchar_t>(decoded.codepoint);
            }
        }
        units += needed;
    }
    return units;
}

#endif
