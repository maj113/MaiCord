#pragma once

// Whether a reservation the code under test released is gone. Another allocation of the test process (a new
// thread's stack, a heap segment) can take a released address at once, so a check that the address is MEM_FREE
// fails at random under load. A mark, taken while the reservation is live and as close to its release as the test
// allows, records the reservation's base, extent and region layout. The reservation is gone when the address is
// free or holds an allocation with another base, extent or layout. On Linux, include this after the header that
// declares VirtualQuery (tests/support/win32_test_compat.hpp).
#include "maicord/core/base.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_kernel.hpp"
#endif

namespace maicord {

struct MemoryMark {
    const void* address;
    const void* allocation; // Null when the address held no allocation at the mark.
    u64 extent;
    u64 layout; // A hash of the offset, size, state and protection of each region.
};

// The allocation that holds `address`: its base, extent and layout. False when the query fails; a free address
// gives a null allocation.
inline bool memory_allocation(const void* address, const void** allocation, u64* extent, u64* layout) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    *allocation = nullptr;
    *extent = *layout = 0;
    if (!address || VirtualQuery(address, &info, sizeof(info)) != sizeof(info))
        return false;
    if (info.State == MEM_FREE)
        return true;
    const u8* const base = static_cast<const u8*>(info.AllocationBase);
    const u8* at = base;
    u64 hash = 14695981039346656037ull, total = 0;
    while (VirtualQuery(at, &info, sizeof(info)) == sizeof(info) && info.State != MEM_FREE &&
           info.AllocationBase == base && info.RegionSize) {
        const u64 values[4]{static_cast<u64>(at - base), static_cast<u64>(info.RegionSize), info.State, info.Protect};
        for (const u64 value : values)
            hash = (hash ^ value) * 1099511628211ull;
        total += info.RegionSize;
        at += info.RegionSize;
    }
    *allocation = base;
    *extent = total;
    *layout = hash;
    return true;
}

inline MemoryMark memory_mark(const void* address) noexcept {
    MemoryMark mark{address, nullptr, 0, 0};
    memory_allocation(address, &mark.allocation, &mark.extent, &mark.layout);
    return mark;
}

// The marked reservation no longer exists. A mark of no allocation never counts as released.
inline bool memory_released(const MemoryMark& mark) noexcept {
    const void* allocation = nullptr;
    u64 extent = 0, layout = 0;
    if (!mark.allocation || !memory_allocation(mark.address, &allocation, &extent, &layout))
        return false;
    return allocation != mark.allocation || extent != mark.extent || layout != mark.layout;
}

} // namespace maicord
