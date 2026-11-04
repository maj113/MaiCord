#pragma once

// The handful of Win32 file, console and command-line calls the shared tests make, on Linux: raw system calls under
// the Win32 names, so a test source builds unchanged on both systems. Test code only (the Linux build force-includes
// this header into every test executable; cmake/test_support.cmake): shipping code takes the neutral os layer
// instead, and the files seam replaces what is here when it lands.
//
// A HANDLE is the file descriptor plus one (so 0 is never a handle); INVALID_HANDLE_VALUE is -1.
#if !defined(_WIN32)

#include "maicord/core/base.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/linux/error.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"

// <winnt.h>, <fileapi.h>, <winbase.h>, <memoryapi.h>, <processenv.h>
#define GENERIC_READ (0x80000000L)
#define GENERIC_WRITE (0x40000000L)
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define PAGE_READONLY 0x02
#define FILE_MAP_READ 0x0004

union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
};
typedef _LARGE_INTEGER LARGE_INTEGER;
typedef LARGE_INTEGER* PLARGE_INTEGER;
struct _OVERLAPPED;
typedef _OVERLAPPED* LPOVERLAPPED;
struct _SECURITY_ATTRIBUTES;
typedef _SECURITY_ATTRIBUTES* LPSECURITY_ATTRIBUTES;
typedef HANDLE HMODULE;
struct _SYSTEM_INFO {
    DWORD dwOemId;
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress;
    LPVOID lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
};
typedef _SYSTEM_INFO SYSTEM_INFO;
typedef _SYSTEM_INFO* LPSYSTEM_INFO;

namespace maicord {

// LARGE_INTEGER.
struct Win32LargeInteger {
    LONGLONG QuadPart;

    LARGE_INTEGER* native() noexcept {
        return reinterpret_cast<LARGE_INTEGER*>(this);
    }
};

// SYSTEM_INFO.
struct Win32SystemInfo {
    DWORD dwOemId;
    DWORD dwPageSize;
    void* lpMinimumApplicationAddress;
    void* lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;

    SYSTEM_INFO* native() noexcept {
        return reinterpret_cast<SYSTEM_INFO*>(this);
    }
};

namespace test_compat {

inline HANDLE handle_from_descriptor(long fd) noexcept {
    return reinterpret_cast<HANDLE>(static_cast<u64>(fd) + 1);
}

inline int descriptor(HANDLE handle) noexcept {
    return static_cast<int>(reinterpret_cast<u64>(handle) - 1);
}

inline bool usable(HANDLE handle) noexcept {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

// A file mapping is the descriptor it maps; a view is the address and the length, kept in a small table so
// UnmapViewOfFile(address) needs no length.
struct View {
    const void* address;
    u64 bytes;
};
inline View* views() noexcept {
    static View table[16];
    return table;
}

} // namespace test_compat
} // namespace maicord

inline HANDLE GetStdHandle(DWORD which) noexcept {
    const int fd = which == STD_INPUT_HANDLE ? 0 : which == STD_OUTPUT_HANDLE ? 1 : 2;
    return maicord::test_compat::handle_from_descriptor(fd);
}

inline BOOL WriteFile(HANDLE file, const void* buffer, DWORD bytes, DWORD* written, LPOVERLAPPED) noexcept {
    DWORD done = 0;
    while (done < bytes) {
        const long result = maicord::sys_write(maicord::test_compat::descriptor(file),
                                               static_cast<const maicord::u8*>(buffer) + done, bytes - done);
        if (maicord::syscall_failed(result)) {
            if (result == -maicord::kErrorInterrupted) {
                continue;
            }
            maicord::set_thread_last_error(maicord::win32_error_from_result(result));
            break;
        }
        done += static_cast<DWORD>(result);
    }
    if (written) {
        *written = done;
    }
    return done == bytes;
}

inline BOOL ReadFile(HANDLE file, void* buffer, DWORD bytes, DWORD* read, LPOVERLAPPED) noexcept {
    long result;
    do {
        result = maicord::sys_read(maicord::test_compat::descriptor(file), buffer, bytes);
    } while (result == -maicord::kErrorInterrupted);
    if (maicord::syscall_failed(result)) {
        maicord::set_thread_last_error(maicord::win32_error_from_result(result));
        return FALSE;
    }
    if (read) {
        *read = static_cast<DWORD>(result);
    }
    return TRUE;
}

inline HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD, HANDLE) noexcept {
    char utf8[4096];
    if (maicord::path_to_utf8(path, utf8, sizeof(utf8)) == 0) {
        maicord::set_thread_last_error(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    int flags = (access & GENERIC_WRITE) != 0 ? (maicord::kOpenWriteOnly) : maicord::kOpenReadOnly;
    if (disposition == CREATE_ALWAYS) {
        flags |= maicord::kOpenCreate | maicord::kOpenTruncate;
    } else if (disposition == CREATE_NEW) {
        flags |= maicord::kOpenCreate | maicord::kOpenExclusive;
    } else if (disposition == OPEN_ALWAYS) {
        flags |= maicord::kOpenCreate;
    }
    const long fd = maicord::sys_open(utf8, flags, 0600);
    if (maicord::syscall_failed(fd)) {
        maicord::set_thread_last_error(maicord::win32_error_from_result(fd));
        return INVALID_HANDLE_VALUE;
    }
    return maicord::test_compat::handle_from_descriptor(fd);
}

inline BOOL CloseHandle(HANDLE handle) noexcept {
    return maicord::test_compat::usable(handle) && !maicord::syscall_failed(maicord::sys_close(maicord::test_compat::descriptor(handle)));
}

inline BOOL GetFileSizeEx(HANDLE file, PLARGE_INTEGER size) noexcept {
    const int fd = maicord::test_compat::descriptor(file);
    const long end = maicord::sys_seek(fd, 0, maicord::kSeekEnd);
    if (maicord::syscall_failed(end)) {
        return FALSE;
    }
    maicord::sys_seek(fd, 0, maicord::kSeekSet);
    size->QuadPart = end;
    return TRUE;
}

// The mapping is the file itself; MapViewOfFile maps it read-only (the only use).
inline HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR) noexcept {
    return maicord::test_compat::usable(file) ? file : nullptr;
}

inline LPVOID MapViewOfFile(HANDLE mapping, DWORD, DWORD, DWORD, SIZE_T) noexcept {
    const int fd = maicord::test_compat::descriptor(mapping);
    const long end = maicord::sys_seek(fd, 0, maicord::kSeekEnd);
    if (maicord::syscall_failed(end) || end == 0) {
        return nullptr;
    }
    const long address = maicord::syscall6(maicord::kSysMmap, 0, end, maicord::kProtectRead, maicord::kMapPrivate, fd, 0);
    if (maicord::syscall_failed(address)) {
        return nullptr;
    }
    for (maicord::test_compat::View* view = maicord::test_compat::views(); view != maicord::test_compat::views() + 16; ++view) {
        if (view->address == nullptr) {
            *view = {reinterpret_cast<const void*>(address), static_cast<maicord::u64>(end)};
            return reinterpret_cast<LPVOID>(address);
        }
    }
    maicord::sys_munmap(reinterpret_cast<void*>(address), static_cast<maicord::usize>(end));
    return nullptr;
}

inline BOOL UnmapViewOfFile(LPCVOID address) noexcept {
    for (maicord::test_compat::View* view = maicord::test_compat::views(); view != maicord::test_compat::views() + 16; ++view) {
        if (view->address == address) {
            const maicord::u64 bytes = view->bytes;
            *view = {};
            return !maicord::syscall_failed(maicord::sys_munmap(const_cast<void*>(address), static_cast<maicord::usize>(bytes)));
        }
    }
    return FALSE;
}

// The command line after the program name as one UTF-16 string: the arguments joined by blanks, those that hold a
// blank in quotes, as a Windows command line looks.
inline LPWSTR GetCommandLineW() noexcept {
    static wchar_t line[32768];
    static bool built;
    if (!built) {
        built = true;
        const maicord::ProcessStartup& startup = maicord::process_startup();
        maicord::u32 used = 0;
        auto put = [&](wchar_t unit) noexcept {
            if (used + 1 < 32768) {
                line[used++] = unit;
            }
        };
        // The program name stands first, as on Windows; callers skip it.
        put(L'x');
        for (int index = 1; index < startup.argc; ++index) {
            put(L' ');
            const char* argument = startup.argv[index];
            bool blank = argument[0] == 0;
            for (const char* at = argument; *at; ++at) {
                blank = blank || *at == ' ' || *at == '\t';
            }
            wchar_t wide[4096];
            const maicord::u32 count = maicord::path_from_utf8(argument, wide, 4096);
            if (blank) {
                put(L'"');
            }
            for (maicord::u32 i = 0; i < count; ++i) {
                put(wide[i]);
            }
            if (blank) {
                put(L'"');
            }
        }
        line[used] = 0;
    }
    return line;
}

inline void GetSystemInfo(LPSYSTEM_INFO info) noexcept {
    maicord::Win32SystemInfo filled{};
    filled.dwPageSize = static_cast<DWORD>(maicord::process_startup().page_size);
    filled.dwAllocationGranularity = 65536;
    maicord::u64 mask[16]{};
    DWORD processors = 0;
    if (!maicord::syscall_failed(maicord::sys_sched_getaffinity(0, sizeof(mask), mask))) {
        for (const maicord::u64 word : mask) {
            processors += static_cast<DWORD>(__builtin_popcountll(word));
        }
    }
    filled.dwNumberOfProcessors = processors == 0 ? 1 : processors;
    *info = *filled.native();
}

// No library of the system's font stack is ever loaded on Linux, which is what the tests ask.
inline HMODULE GetModuleHandleW(LPCWSTR) noexcept {
    return nullptr;
}

inline DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD capacity) noexcept {
    char utf8[256];
    if (maicord::path_to_utf8(name, utf8, sizeof(utf8)) == 0) {
        return 0;
    }
    const char* value = maicord::environment_get(utf8);
    if (value == nullptr) {
        maicord::set_thread_last_error(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    wchar_t wide[4096];
    const maicord::u32 count = maicord::path_from_utf8(value, wide, 4096);
    if (count + 1 > capacity) {
        return count + 1;
    }
    for (maicord::u32 index = 0; index <= count; ++index) {
        buffer[index] = wide[index];
    }
    return count;
}

// $TMPDIR or /tmp, with a trailing separator.
inline DWORD GetTempPathW(DWORD capacity, LPWSTR buffer) noexcept {
    const char* directory = maicord::environment_get("TMPDIR");
    if (directory == nullptr || directory[0] != '/') {
        directory = "/tmp";
    }
    wchar_t wide[1024];
    maicord::u32 count = maicord::path_from_utf8(directory, wide, 1000);
    if (count == 0 || count + 2 > capacity) {
        return 0;
    }
    if (wide[count - 1] != L'/') {
        wide[count++] = L'/';
    }
    for (maicord::u32 index = 0; index < count; ++index) {
        buffer[index] = wide[index];
    }
    buffer[count] = 0;
    return count;
}

// Creates the file "<path><prefix><process id><counter>.tmp", as GetTempFileName does for unique == 0.
inline UINT GetTempFileNameW(LPCWSTR path, LPCWSTR prefix, UINT, LPWSTR output) noexcept {
    static maicord::u32 counter;
    for (;;) {
        maicord::u32 length = 0;
        auto put = [&](wchar_t unit) noexcept { output[length++] = unit; };
        for (const wchar_t* at = path; *at; ++at) {
            put(*at);
        }
        for (const wchar_t* at = prefix; *at; ++at) {
            put(*at);
        }
        maicord::u32 digits = maicord::process_id() * 1024u + __atomic_add_fetch(&counter, 1u, __ATOMIC_RELAXED);
        wchar_t reversed[16];
        maicord::u32 count = 0;
        do {
            reversed[count++] = static_cast<wchar_t>(L'0' + digits % 10);
            digits /= 10;
        } while (digits);
        while (count) {
            put(reversed[--count]);
        }
        for (const wchar_t* at = L".tmp"; *at; ++at) {
            put(*at);
        }
        output[length] = 0;
        HANDLE created = CreateFileW(output, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (created != INVALID_HANDLE_VALUE) {
            CloseHandle(created);
            return 1;
        }
        if (maicord::thread_last_error() != ERROR_ALREADY_EXISTS) {
            return 0;
        }
    }
}

inline BOOL DeleteFileW(LPCWSTR path) noexcept {
    char utf8[4096];
    if (maicord::path_to_utf8(path, utf8, sizeof(utf8)) == 0) {
        return FALSE;
    }
    return !maicord::syscall_failed(maicord::sys_unlink(utf8));
}

inline BOOL CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES) noexcept {
    char utf8[4096];
    if (maicord::path_to_utf8(path, utf8, sizeof(utf8)) == 0) {
        return FALSE;
    }
    return !maicord::syscall_failed(maicord::sys_mkdir(utf8, 0700));
}

inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency) noexcept {
    frequency->QuadPart = 1'000'000'000;
    return TRUE;
}

inline BOOL QueryPerformanceCounter(LARGE_INTEGER* counter) noexcept {
    maicord::TimeSpec time{};
    clock_gettime(maicord::kClockMonotonic, &time);
    counter->QuadPart = time.seconds * 1'000'000'000 + time.nanoseconds;
    return TRUE;
}

#endif
