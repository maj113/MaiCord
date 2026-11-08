// The Linux bodies of pages_reserve, pages_allocate and pages_release (include/maicord/core/virtual_memory.hpp).
//
// VirtualFree(base, 0, MEM_RELEASE) takes no length and munmap needs one, so every range handed out here is
// recorded as (base, length) and the release finds the length by its base. The records are a dense array sorted by
// descending base, in zero-initialized static storage, under one lock: ranges are mapped and released rarely (owners,
// buffers, arenas), not per object. Only the pages that hold live records become resident, the kernel maps top-down
// so a new range is usually appended, and a lookup is a binary search. kMaximumLive ranges may be live at once; a
// range that does not fit is refused like an out-of-memory.
#include "maicord/core/memory.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/os/linux/srwlock.hpp"
#include "maicord/sys/linux/calls.hpp"

namespace maicord {
namespace {

struct Range {
    u64 base;
    u64 bytes;
};

constexpr u32 kMaximumLive = 32768;

// g_ranges[0, g_live), by descending base.
Range g_ranges[kMaximumLive];
u32 g_live;
SRWLOCK g_lock;

// Caller holds the lock. The index of the first record whose base is at most `address` (g_live when none is): the
// record of a range that starts at or holds `address`, if any does, since ranges never overlap.
u32 position(u64 address) noexcept {
    u32 low = 0, high = g_live;
    while (low < high) {
        const u32 middle = (low + high) / 2;
        if (g_ranges[middle].base > address)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

// Caller holds the lock. False when the table is full.
bool insert(u64 base, u64 bytes) noexcept {
    if (g_live >= kMaximumLive) {
        return false;
    }
    const u32 at = position(base);
    copy_bytes(&g_ranges[at + 1], &g_ranges[at], (g_live - at) * static_cast<u32>(sizeof(Range)));
    g_ranges[at] = Range{base, bytes};
    ++g_live;
    return true;
}

// Caller holds the lock. Removes the range recorded for `base`; its length, or 0 when none is.
u64 take(u64 base) noexcept {
    const u32 at = position(base);
    if (at == g_live || g_ranges[at].base != base) {
        return 0;
    }
    const u64 bytes = g_ranges[at].bytes;
    --g_live;
    copy_bytes(&g_ranges[at], &g_ranges[at + 1], (g_live - at) * static_cast<u32>(sizeof(Range)));
    return bytes;
}

} // namespace

void* pages_map(usize bytes, bool committed) noexcept {
    if (bytes == 0) {
        return nullptr;
    }
    const long result = committed
                            ? sys_mmap(nullptr, bytes, kProtectRead | kProtectWrite, kMapPrivate | kMapAnonymous)
                            : sys_mmap(nullptr, bytes, kProtectNone, kMapPrivate | kMapAnonymous | kMapNoReserve);
    if (syscall_failed(result)) {
        return nullptr;
    }
    AcquireSRWLockExclusive(&g_lock);
    const bool recorded = insert(static_cast<u64>(result), bytes);
    ReleaseSRWLockExclusive(&g_lock);
    if (!recorded) {
        sys_munmap(reinterpret_cast<void*>(result), bytes);
        return nullptr;
    }
    return reinterpret_cast<void*>(result);
}

bool pages_unmap(void* base, usize hint_bytes) noexcept {
    if (base == nullptr) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    const u64 recorded = take(reinterpret_cast<u64>(base));
    ReleaseSRWLockExclusive(&g_lock);
    const usize bytes = recorded != 0 ? static_cast<usize>(recorded) : hint_bytes;
    return bytes != 0 && !syscall_failed(sys_munmap(base, bytes));
}

void* pages_range_base(const void* address) noexcept {
    const u64 target = reinterpret_cast<u64>(address);
    void* found = nullptr;
    AcquireSRWLockExclusive(&g_lock);
    const u32 at = position(target);
    if (at != g_live && target - g_ranges[at].base < g_ranges[at].bytes) {
        found = reinterpret_cast<void*>(g_ranges[at].base);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return found;
}

namespace {
// The listed /proc/meminfo fields, in kibibytes (0 for one that is missing or unreadable).
void meminfo_fields(const char* const* names, u64* values, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index)
        values[index] = 0;
    const long file = sys_open("/proc/meminfo", kOpenReadOnly);
    if (syscall_failed(file))
        return;
    char text[1024];
    const long got = sys_read(static_cast<int>(file), text, sizeof(text) - 1);
    sys_close(static_cast<int>(file));
    if (syscall_failed(got) || got <= 0)
        return;
    text[got] = 0;
    for (u32 index = 0; index < count; ++index) {
        const char* name = names[index];
        for (long at = 0; at < got; ++at) {
            long match = 0;
            while (name[match] && text[at + match] == name[match])
                ++match;
            if (name[match] || (at != 0 && text[at - 1] != '\n'))
                continue;
            u64 value = 0;
            for (long digit = at + match; digit < got && text[digit] != '\n'; ++digit)
                if (text[digit] >= '0' && text[digit] <= '9')
                    value = value * 10 + static_cast<u64>(text[digit] - '0');
            values[index] = value;
            break;
        }
    }
}
} // namespace

bool system_memory_low() noexcept {
    static constexpr const char* names[] = {"MemTotal:", "MemAvailable:"};
    u64 values[2];
    meminfo_fields(names, values, 2);
    const u64 total = values[0], available = values[1];
    return total != 0 && (available < total / 33 || available < 256 * 1024);
}

u64 system_memory_total_kib() noexcept {
    static constexpr const char* names[] = {"MemTotal:"};
    u64 value;
    meminfo_fields(names, &value, 1);
    return value;
}

} // namespace maicord
