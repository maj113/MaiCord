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
//
// A reservation is released by its base alone, as VirtualFree(base, 0, MEM_RELEASE) does. munmap needs the length,
// so on Linux pages_reserve and pages_allocate record each range they return (src/os/linux/pages.cpp) and the
// release looks the length up; the callers of the Windows API keep their one-argument form.

namespace maicord {

#if !defined(_WIN32)
// The Linux bodies of pages_reserve, pages_allocate and the releases: map a range (readable and writable when
// `committed`, else without access) and record it; unmap a recorded range, or `hint_bytes` bytes of an unrecorded
// one. Null and false when the system refuses.
void* pages_map(usize bytes, bool committed) noexcept;
bool pages_unmap(void* base, usize hint_bytes) noexcept;
// The base of the recorded range that holds `address` (what VirtualQuery reports as AllocationBase), or null when
// the address is in none. Linear in the table: for diagnostics and tests, not for hot paths.
void* pages_range_base(const void* address) noexcept;
#endif

__forceinline void* pages_reserve(usize bytes) noexcept {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
#else
    return pages_map(bytes, false);
#endif
}

// pages_reserve, with the PAGE_NOACCESS protection some Windows call sites name for a reservation (the protection
// of a MEM_RESERVE call is not used, but it is part of the call a caller already compiles to).
__forceinline void* pages_reserve_no_access(usize bytes) noexcept {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
#else
    return pages_reserve(bytes);
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

// pages_commit for a caller that tests or compares the result as VirtualAlloc's: `address` on success, null when
// the system refuses. Like VirtualAlloc it commits whole pages: an address inside a page commits that page and a size
// that ends inside one commits through its end.
__forceinline void* pages_commit_at(void* address, usize bytes) noexcept {
#if defined(_WIN32)
    return VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE);
#else
    const usize page = process_startup().page_size;
    const usize start = reinterpret_cast<usize>(address) & ~(page - 1);
    const usize end = (reinterpret_cast<usize>(address) + bytes + page - 1) & ~(page - 1);
    return pages_commit(reinterpret_cast<void*>(start), end - start) ? address : nullptr;
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

// Releases a whole reservation by its base (the size it was reserved with is not needed: Windows takes none, and
// Linux looks it up). False when the system refuses.
__forceinline bool pages_release(void* base) noexcept {
#if defined(_WIN32)
    return VirtualFree(base, 0, MEM_RELEASE) != 0;
#else
    return pages_unmap(base, 0);
#endif
}

// The same, for a caller that knows the size it reserved (the size only matters for a range that was not mapped by
// pages_reserve or pages_allocate).
__forceinline bool pages_release(void* base, usize bytes) noexcept {
#if defined(_WIN32)
    (void)bytes;
    return VirtualFree(base, 0, MEM_RELEASE) != 0;
#else
    return pages_unmap(base, bytes);
#endif
}

// Whether the system is short of physical memory: Windows' low-memory resource notification (created at first use and
// kept), MemAvailable of /proc/meminfo under 3 percent of MemTotal or a quarter of a gibibyte on Linux.
#if defined(_WIN32)
__forceinline bool system_memory_low() noexcept {
    static HANDLE notification;
    if (!notification)
        notification = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    BOOL low = FALSE;
    return notification && QueryMemoryResourceNotification(notification, &low) && low;
}
#else
bool system_memory_low() noexcept;
#endif

// Zeroed pages, reserved and committed in one call; null when the system refuses.
__forceinline void* pages_allocate(usize bytes) noexcept {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    return pages_map(bytes, true);
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

// Pins committed pages in memory so they are not written to swap (secrets). Not counted: one pages_unlock undoes
// any number of pages_lock calls on a page. False when the system's lock quota is exhausted.
__forceinline bool pages_lock(void* address, usize bytes) noexcept {
#if defined(_WIN32)
    return VirtualLock(address, bytes) != 0;
#else
    return !syscall_failed(sys_mlock(address, bytes));
#endif
}

__forceinline bool pages_unlock(void* address, usize bytes) noexcept {
#if defined(_WIN32)
    return VirtualUnlock(address, bytes) != 0;
#else
    return !syscall_failed(sys_munlock(address, bytes));
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
