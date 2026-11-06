// The Linux bodies of pages_reserve, pages_allocate and pages_release (include/maicord/core/virtual_memory.hpp).
//
// VirtualFree(base, 0, MEM_RELEASE) takes no length and munmap needs one, so every range handed out here is
// recorded in a table of (base, length) and the release finds the length by its base. The table is open-addressed
// with linear probing and backward-shift deletion (no tombstones), in zero-initialized static storage whose pages
// cost nothing until used, under one lock: ranges are mapped and released rarely (owners, buffers, arenas), not per
// object. kSlots / 2 ranges may be live at once; a range that does not fit is refused like an out-of-memory.
#include "maicord/core/virtual_memory.hpp"
#include "maicord/sys/linux/calls.hpp"

namespace maicord {
namespace {

struct Range {
    u64 base; // 0 = empty slot
    u64 bytes;
};

constexpr u32 kSlotBits = 16;
constexpr u32 kSlots = 1u << kSlotBits;
constexpr u32 kMaximumLive = kSlots / 2;

Range g_ranges[kSlots];
u32 g_live;
// A futex lock: 0 free, 1 held, 2 held with waiters.
volatile u32 g_lock;

void lock() noexcept {
    u32 expected = 0;
    if (__atomic_compare_exchange_n(&g_lock, &expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return;
    }
    while (__atomic_exchange_n(&g_lock, 2u, __ATOMIC_ACQUIRE) != 0) {
        sys_futex_wait(&g_lock, 2, nullptr);
    }
}

void unlock() noexcept {
    if (__atomic_exchange_n(&g_lock, 0u, __ATOMIC_RELEASE) == 2) {
        sys_futex_wake(&g_lock, 1);
    }
}

u32 home(u64 base) noexcept {
    return static_cast<u32>(((base >> 12) * 0x9E3779B97F4A7C15ull) >> (64 - kSlotBits));
}

// Caller holds the lock. False when the table is full.
bool insert(u64 base, u64 bytes) noexcept {
    if (g_live >= kMaximumLive) {
        return false;
    }
    u32 slot = home(base);
    while (g_ranges[slot].base != 0) {
        slot = (slot + 1) & (kSlots - 1);
    }
    g_ranges[slot] = Range{base, bytes};
    ++g_live;
    return true;
}

// Caller holds the lock. Removes the range recorded for `base`; its length, or 0 when none is.
u64 take(u64 base) noexcept {
    u32 slot = home(base);
    while (g_ranges[slot].base != base) {
        if (g_ranges[slot].base == 0) {
            return 0;
        }
        slot = (slot + 1) & (kSlots - 1);
    }
    const u64 bytes = g_ranges[slot].bytes;
    // Backward-shift deletion: close the gap by moving up the entries of the run that the gap would cut off from
    // their home slots.
    u32 gap = slot;
    for (u32 next = (slot + 1) & (kSlots - 1); g_ranges[next].base != 0; next = (next + 1) & (kSlots - 1)) {
        const u32 ideal = home(g_ranges[next].base);
        // `next` may move into `gap` unless its ideal slot lies cyclically in (gap, next].
        const bool between = gap <= next ? (gap < ideal && ideal <= next) : (gap < ideal || ideal <= next);
        if (!between) {
            g_ranges[gap] = g_ranges[next];
            gap = next;
        }
    }
    g_ranges[gap] = Range{};
    --g_live;
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
    lock();
    const bool recorded = insert(static_cast<u64>(result), bytes);
    unlock();
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
    lock();
    const u64 recorded = take(reinterpret_cast<u64>(base));
    unlock();
    const usize bytes = recorded != 0 ? static_cast<usize>(recorded) : hint_bytes;
    return bytes != 0 && !syscall_failed(sys_munmap(base, bytes));
}

void* pages_range_base(const void* address) noexcept {
    const u64 target = reinterpret_cast<u64>(address);
    void* found = nullptr;
    lock();
    for (u32 slot = 0; slot < kSlots; ++slot) {
        const Range& range = g_ranges[slot];
        if (range.base != 0 && target >= range.base && target - range.base < range.bytes) {
            found = reinterpret_cast<void*>(range.base);
            break;
        }
    }
    unlock();
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
