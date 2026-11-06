#include "maicord/core/arena.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"

#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_kernel.hpp"
#if !defined(_WIN32)
#include "maicord/sys/linux/process.hpp"
#endif

namespace maicord {
namespace {
// The per-tag totals arena_account reports. Only commits, decommits, initialization and release touch them (one
// interlocked add each), never an allocation that fits the committed pages.
struct AccountCounters {
    volatile LONG64 arenas;
    volatile LONG64 reserved;
    volatile LONG64 committed;
    volatile LONG64 peak_committed;
};
AccountCounters g_accounts[static_cast<u32>(ArenaTag::count)];

AccountCounters& account_of(const Arena* arena) noexcept {
    const u32 tag = static_cast<u32>(arena->tag);
    return g_accounts[tag < static_cast<u32>(ArenaTag::count) ? tag : 0];
}

void account_commit(const Arena* arena, i64 bytes) noexcept {
    AccountCounters& account = account_of(arena);
    const i64 committed = InterlockedAdd64(&account.committed, bytes);
    // A racing commit may raise the peak first; the loop only ever raises it.
    for (i64 peak = account.peak_committed; committed > peak;) {
        const i64 seen = InterlockedCompareExchange64(&account.peak_committed, committed, peak);
        if (seen == peak)
            break;
        peak = seen;
    }
}
#if defined(_WIN32)
volatile LONG g_page_size;

usize system_page_size() noexcept {
    const u32 cached = static_cast<u32>(read_acquire(&g_page_size));
    if (cached) return cached;
    Win32SystemInfo info{};
    GetSystemInfo(info.native());
    const u32 page_size = info.dwPageSize;
    // Page size is fixed per process, so racing first callers publish the same
    // value without a lock. An invalid result is not cached; the caller rejects it.
    if (is_power_of_two(page_size))
        InterlockedCompareExchange(&g_page_size, static_cast<LONG>(page_size), 0);
    return page_size;
}
#else
// The kernel reports the page size in the auxiliary vector.
usize system_page_size() noexcept {
    return process_startup().page_size;
}
#endif

bool align_up(usize value, usize alignment, usize* result) noexcept {
    if (!is_power_of_two(alignment)) {
        return false;
    }
    const usize mask = alignment - 1;
    if (value > (~usize{0}) - mask) {
        return false;
    }
    *result = (value + mask) & ~mask;
    return true;
}

void erase_bytes(u8* memory, usize bytes) noexcept {
    // The arena can rewind to any byte, so word stores must accept unaligned
    // addresses. Volatile preserves the secure-erasure contract.
    u8* word_end = memory + (bytes & ~usize{7});
    while (memory != word_end) {
        *reinterpret_cast<volatile u64 __unaligned*>(memory) = 0;
        memory += sizeof(u64);
    }
    bytes &= 7;
    volatile u8* output = memory;
    while (bytes--) *output++ = 0;
}

// Moves the offset past [aligned_offset, end) and returns its start.
__forceinline void* place(Arena* arena, usize aligned_offset, usize end) noexcept {
    arena->offset = end;
    arena->peak = maximum(arena->peak, end);
    return arena->base + aligned_offset;
}

// Commits whole steps through `end`, then places the allocation. arena_allocate
// tail-calls it, so the uncommitted case costs its fast path no stack frame.
void* commit_and_place(Arena* arena, usize end, usize aligned_offset) noexcept {
    usize target = 0;
    if (!align_up(end, arena->commit_step, &target)) {
        return nullptr;
    }
    target = minimum(target, arena->reserved);
    auto* address = arena->base + arena->committed;
    if (!pages_commit(address, target - arena->committed)) {
        return nullptr;
    }
    account_commit(arena, static_cast<i64>(target - arena->committed));
    arena->committed = target;
    ++arena->commit_count;
    return place(arena, aligned_offset, end);
}

} // namespace

bool arena_initialize(Arena* arena, usize reserve_bytes, usize initial_commit_bytes, usize commit_step_bytes,
                      ArenaTag tag) noexcept {
    if (!arena || arena->base || reserve_bytes == 0) {
        return false;
    }

    const usize page_size = system_page_size();
    const usize page_mask = page_size - 1;
    const usize largest_input = maximum(reserve_bytes, maximum(initial_commit_bytes, commit_step_bytes));
    if (!is_power_of_two(page_size) || largest_input > ~usize{0} - page_mask) {
        return false;
    }
    const usize reserved = (reserve_bytes + page_mask) & ~page_mask;
    const usize initial_commit = (initial_commit_bytes + page_mask) & ~page_mask;
    const usize commit_step = (commit_step_bytes + page_mask) & ~page_mask;
    if (initial_commit > reserved || commit_step == 0) {
        return false;
    }

    u8* base;
    if (initial_commit == reserved) {
        base = commit_pages<u8>(reserved);
    } else {
        base = static_cast<u8*>(pages_reserve(reserved));
        if (base && initial_commit != 0 && !pages_commit(base, initial_commit)) {
            pages_release(base, reserved);
            return false;
        }
    }
    if (!base) {
        return false;
    }

    *arena = Arena{
        base, reserved, initial_commit, 0, 0, commit_step, initial_commit == 0 ? 0u : 1u, tag,
    };
    AccountCounters& account = account_of(arena);
    InterlockedIncrement64(&account.arenas);
    InterlockedAdd64(&account.reserved, static_cast<i64>(reserved));
    account_commit(arena, static_cast<i64>(initial_commit));
    return true;
}

void arena_release(Arena* arena) noexcept {
    if (!arena) {
        return;
    }
    if (arena->base) {
        pages_release(arena->base, arena->reserved);
        AccountCounters& account = account_of(arena);
        InterlockedAdd64(&account.arenas, -1);
        InterlockedAdd64(&account.reserved, -static_cast<i64>(arena->reserved));
        InterlockedAdd64(&account.committed, -static_cast<i64>(arena->committed));
    }
    reset_value<Arena>(arena);
}

void* arena_allocate(Arena* arena, usize bytes, usize alignment) noexcept {
    if (!arena || !arena->base || bytes == 0 || !is_power_of_two(alignment)) {
        return nullptr;
    }
    const usize address = reinterpret_cast<usize>(arena->base) + arena->offset;
    const usize padding = (usize{0} - address) & (alignment - 1);
    const usize remaining = arena->reserved - arena->offset;
    if (padding > remaining || bytes > remaining - padding) {
        return nullptr;
    }
    const usize aligned_offset = arena->offset + padding;
    const usize new_offset = aligned_offset + bytes;

    if (new_offset > arena->committed) {
        return commit_and_place(arena, new_offset, aligned_offset);
    }
    return place(arena, aligned_offset, new_offset);
}

void* arena_allocate_zero(Arena* arena, usize bytes, usize alignment) noexcept {
    // Bytes past the previous peak were never handed out: they are still the
    // zero pages VirtualAlloc committed. Erasing only below it keeps untouched
    // capacity out of the working set.
    const usize pristine = arena ? arena->peak : 0;
    auto* allocation = static_cast<u8*>(arena_allocate(arena, bytes, alignment));
    if (allocation) {
        const usize offset = static_cast<usize>(allocation - arena->base);
        if (offset < pristine) {
            erase_bytes(allocation, minimum(bytes, pristine - offset));
        }
    }
    return allocation;
}

ArenaMark arena_mark(const Arena* arena) noexcept {
    return ArenaMark{arena ? arena->offset : 0};
}

bool arena_rewind(Arena* arena, ArenaMark mark, ArenaRewindMode mode) noexcept {
    if (!arena || !arena->base || mark.offset > arena->offset) {
        return false;
    }
    if (mode == ArenaRewindMode::erase && mark.offset != arena->offset) {
        erase_bytes(arena->base + mark.offset, arena->offset - mark.offset);
    }
    arena->offset = mark.offset;
    return true;
}

void arena_reset(Arena* arena, ArenaRewindMode mode) noexcept {
    if (arena) {
        arena_rewind(arena, ArenaMark{0}, mode);
    }
}

bool arena_trim(Arena* arena, usize offset) noexcept {
    if (!arena || !arena->base || offset > arena->offset) {
        return false;
    }
    const usize page_mask = system_page_size() - 1;
    const usize keep = (offset + page_mask) & ~page_mask;
    if (keep < arena->committed) {
        if (!pages_decommit(arena->base + keep, arena->committed - keep)) {
            return false;
        }
        account_commit(arena, -static_cast<i64>(arena->committed - keep));
        arena->committed = keep;
    }
    arena->offset = offset;
    // Decommitted pages come back as zero pages.
    arena->peak = minimum(arena->peak, keep);
    return true;
}

ArenaAccount arena_account(ArenaTag tag) noexcept {
    const AccountCounters& account = g_accounts[static_cast<u32>(tag) < static_cast<u32>(ArenaTag::count)
                                                    ? static_cast<u32>(tag)
                                                    : 0];
    return ArenaAccount{read_acquire(&account.arenas), read_acquire(&account.reserved), read_acquire(&account.committed),
                        read_acquire(&account.peak_committed)};
}
} // namespace maicord

