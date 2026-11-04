#pragma once

#include "maicord/core/base.hpp"

#if defined(_WIN32)
#include "maicord/core/win32_kernel.hpp"
#else
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
#endif

// Page-granular address space. A range is reserved first (no access, no memory charged), committed in
// page-multiple pieces (zeroed, read and write) and decommitted again (contents dropped, access removed); the
// whole reservation is released at once. On Windows each function is the VirtualAlloc or VirtualFree call it
// stands for; elsewhere mmap, mprotect and madvise. Sizes are multiples of the page size and addresses page
// aligned, as the system requires.

namespace maicord {

__forceinline void* pages_reserve(usize bytes) noexcept {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
#else
    const long result = sys_mmap(nullptr, bytes, kProtectNone, kMapPrivate | kMapAnonymous | kMapNoReserve);
    return syscall_failed(result) ? nullptr : reinterpret_cast<void*>(result);
#endif
}

// Commits [address, address + bytes) of a reservation.
__forceinline bool pages_commit(void* address, usize bytes) noexcept {
#if defined(_WIN32)
    return VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) == address;
#else
    return !syscall_failed(sys_mprotect(address, bytes, kProtectRead | kProtectWrite));
#endif
}

// Returns the pages to the system; the range stays reserved and a later commit gives zero pages.
__forceinline bool pages_decommit(void* address, usize bytes) noexcept {
#if defined(_WIN32)
    return VirtualFree(address, bytes, MEM_DECOMMIT) != 0;
#else
    return !syscall_failed(sys_madvise(address, bytes, kAdviseDontNeed)) &&
           !syscall_failed(sys_mprotect(address, bytes, kProtectNone));
#endif
}

// Releases a whole reservation: its base and the size it was reserved with.
__forceinline void pages_release(void* base, usize bytes) noexcept {
#if defined(_WIN32)
    (void)bytes;
    VirtualFree(base, 0, MEM_RELEASE);
#else
    sys_munmap(base, bytes);
#endif
}

// Zeroed pages, reserved and committed in one call; null when the system refuses.
__forceinline void* pages_allocate(usize bytes) noexcept {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    const long result = sys_mmap(nullptr, bytes, kProtectRead | kProtectWrite, kMapPrivate | kMapAnonymous);
    return syscall_failed(result) ? nullptr : reinterpret_cast<void*>(result);
#endif
}

enum class PageAccess : u8 {
    none,
    read,
    read_write,
};

// Changes the access of committed pages (guard pages for tests and bounds probes).
__forceinline bool pages_protect(void* address, usize bytes, PageAccess access) noexcept {
#if defined(_WIN32)
    DWORD previous = 0;
    return VirtualProtect(address, bytes,
                          access == PageAccess::none   ? PAGE_NOACCESS
                          : access == PageAccess::read ? PAGE_READONLY
                                                       : PAGE_READWRITE,
                          &previous) != 0;
#else
    return !syscall_failed(sys_mprotect(address, bytes,
                                        access == PageAccess::none   ? kProtectNone
                                        : access == PageAccess::read ? kProtectRead
                                                                     : kProtectRead | kProtectWrite));
#endif
}

// The system page size: the granularity of the functions above.
__forceinline usize pages_page_size() noexcept {
#if defined(_WIN32)
    Win32SystemInfo info{};
    GetSystemInfo(info.native());
    return info.dwPageSize;
#else
    return process_startup().page_size;
#endif
}

} // namespace maicord
