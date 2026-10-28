#include "maicord/core/arena.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

inline bool is_power_of_two(usize value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

volatile LONG g_page_size;
volatile LONG64 g_arena_initializations;
volatile LONG64 g_arena_commits;
volatile LONG64 g_arena_committed_bytes;

void note_commit(usize bytes) noexcept {
    if (bytes == 0) return;
    InterlockedIncrement64(&g_arena_commits);
    InterlockedExchangeAdd64(&g_arena_committed_bytes, static_cast<LONG64>(bytes));
}

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

} // namespace

bool arena_initialize(Arena* arena, usize reserve_bytes, usize initial_commit_bytes,
                      usize commit_step_bytes) noexcept {
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
        base = static_cast<u8*>(VirtualAlloc(nullptr, reserved, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    } else {
        base = static_cast<u8*>(VirtualAlloc(nullptr, reserved, MEM_RESERVE, PAGE_READWRITE));
        if (base && initial_commit != 0 &&
            VirtualAlloc(base, initial_commit, MEM_COMMIT, PAGE_READWRITE) != base) {
            VirtualFree(base, 0, MEM_RELEASE);
            return false;
        }
    }
    if (!base) {
        return false;
    }

    *arena = Arena{
        base, reserved, initial_commit, 0, 0, commit_step, initial_commit == 0 ? 0u : 1u,
    };
    InterlockedIncrement64(&g_arena_initializations);
    note_commit(initial_commit);
    return true;
}

void arena_release(Arena* arena) noexcept {
    if (!arena) {
        return;
    }
    if (arena->base) {
        VirtualFree(arena->base, 0, MEM_RELEASE);
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
        usize commit_target = 0;
        if (!align_up(new_offset, arena->commit_step, &commit_target)) {
            return nullptr;
        }
        commit_target = minimum(commit_target, arena->reserved);
        const usize commit_bytes = commit_target - arena->committed;
        auto* commit_address = arena->base + arena->committed;
        if (VirtualAlloc(commit_address, commit_bytes, MEM_COMMIT, PAGE_READWRITE) != commit_address) {
            return nullptr;
        }
        arena->committed = commit_target;
        ++arena->commit_count;
        note_commit(commit_bytes);
    }

    arena->offset = new_offset;
    arena->peak = maximum(arena->peak, new_offset);
    return arena->base + aligned_offset;
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
        if (!VirtualFree(arena->base + keep, arena->committed - keep, MEM_DECOMMIT)) {
            return false;
        }
        arena->committed = keep;
    }
    arena->offset = offset;
    // Decommitted pages come back as zero pages.
    arena->peak = minimum(arena->peak, keep);
    return true;
}

ArenaProcessCounters arena_process_counters() noexcept {
    return ArenaProcessCounters{static_cast<u64>(read_acquire(&g_arena_initializations)),
                                static_cast<u64>(read_acquire(&g_arena_commits)),
                                static_cast<u64>(read_acquire(&g_arena_committed_bytes))};
}

} // namespace maicord

