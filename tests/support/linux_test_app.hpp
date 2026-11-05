#pragma once

// The kernel calls the application and UI fixtures make besides the file calls of win32_test_compat.hpp, on Linux, over
// the neutral layers (virtual memory, time, threads): the page allocation of a fixture's runtime blocks, the exit of
// a failed check, the tick, ids and the sleep. Test code only (win32_test_compat.hpp includes this at its end).
#if !defined(_WIN32)

#include "maicord/core/intrinsics.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/file.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"

#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define INVALID_FILE_ATTRIBUTES ((DWORD)0xFFFFFFFF)
#define MEM_RELEASE 0x00008000

// The only form the fixtures use: a fresh read-write block, committed at once.
inline LPVOID VirtualAlloc(LPVOID address, SIZE_T bytes, DWORD type, DWORD protect) noexcept {
    if (address || type != (MEM_RESERVE | MEM_COMMIT) || protect != PAGE_READWRITE)
        return nullptr;
    return maicord::commit_pages(bytes);
}

inline BOOL VirtualFree(LPVOID address, SIZE_T bytes, DWORD type) noexcept {
    return bytes == 0 && type == MEM_RELEASE && maicord::pages_release(address);
}

inline BOOL VirtualProtect(LPVOID address, SIZE_T bytes, DWORD protect, PDWORD previous) noexcept {
    const maicord::PageAccess access = protect == PAGE_NOACCESS    ? maicord::PageAccess::none
                                       : protect == PAGE_READONLY ? maicord::PageAccess::read
                                                                   : maicord::PageAccess::read_write;
    if (previous)
        *previous = PAGE_READWRITE;
    return maicord::pages_protect(address, bytes, access);
}

[[noreturn]] inline void ExitProcess(UINT code) noexcept {
    maicord::sys_exit_group(static_cast<int>(code));
}

inline ULONGLONG GetTickCount64() noexcept {
    return maicord::clock_monotonic_milliseconds();
}

inline DWORD GetCurrentThreadId() noexcept {
    return maicord::current_thread_id();
}

inline DWORD GetCurrentProcessId() noexcept {
    return maicord::process_id();
}

inline void Sleep(DWORD milliseconds) noexcept {
    maicord::thread_sleep(milliseconds);
}

// Events, as the fixtures use them: CreateEventW and the waits over the neutral waitable (os/wait.hpp).
inline HANDLE CreateEventW(LPSECURITY_ATTRIBUTES, BOOL manual_reset, BOOL initial, LPCWSTR) noexcept {
    return maicord::event_create(manual_reset != FALSE, initial != FALSE);
}

inline BOOL SetEvent(HANDLE event) noexcept {
    return maicord::event_set(event);
}

inline BOOL ResetEvent(HANDLE event) noexcept {
    return maicord::event_reset(event);
}

inline DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) noexcept {
    return maicord::wait_one(handle, milliseconds);
}

inline BOOL FlushFileBuffers(HANDLE file) noexcept {
    return !maicord::syscall_failed(maicord::sys_fsync(maicord::test_compat::descriptor(file)));
}

inline BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD method) noexcept {
    const long result = maicord::sys_seek(maicord::test_compat::descriptor(file), distance.QuadPart,
                                          method == FILE_BEGIN ? maicord::kSeekSet
                                          : method == FILE_CURRENT ? maicord::kSeekCurrent
                                                                   : maicord::kSeekEnd);
    if (maicord::syscall_failed(result))
        return FALSE;
    if (position)
        position->QuadPart = result;
    return TRUE;
}

// FILE_ATTRIBUTE_DIRECTORY for a directory, FILE_ATTRIBUTE_NORMAL for anything else that exists.
inline DWORD GetFileAttributesW(LPCWSTR path) noexcept {
    maicord::FileStat stat{};
    if (!maicord::file_path_stat(path, &stat))
        return INVALID_FILE_ATTRIBUTES;
    return stat.kind == maicord::FileKind::directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

#endif
