#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

struct Arena {
    u8* base;
    usize reserved;
    usize committed;
    usize offset;
    usize peak;
    usize commit_step;
    u32 commit_count;
};

// Process-wide totals since start: arena initializations, commit calls and bytes
// committed. Updated only on initialization and commit, never on the bump path.
struct ArenaProcessCounters {
    u64 initializations;
    u64 commits;
    u64 committed_bytes;
};

struct ArenaMark {
    usize offset;
};

enum class ArenaRewindMode : u8 {
    retain,
    erase,
};

bool arena_initialize(Arena* arena, usize reserve_bytes, usize initial_commit_bytes,
                      usize commit_step_bytes = 64 * 1024) noexcept;
void arena_release(Arena* arena) noexcept;
void* arena_allocate(Arena* arena, usize bytes, usize alignment) noexcept;
void* arena_allocate_zero(Arena* arena, usize bytes, usize alignment) noexcept;
ArenaMark arena_mark(const Arena* arena) noexcept;
bool arena_rewind(Arena* arena, ArenaMark mark, ArenaRewindMode mode = ArenaRewindMode::retain) noexcept;
void arena_reset(Arena* arena, ArenaRewindMode mode = ArenaRewindMode::retain) noexcept;
ArenaProcessCounters arena_process_counters() noexcept;

// A zero count yields nullptr, like a zero byte count.
template <typename T> T* arena_allocate(Arena* arena, u32 count = 1) noexcept {
    return static_cast<T*>(arena_allocate(arena, sizeof(T) * static_cast<usize>(count), alignof(T)));
}

template <typename T> T* arena_allocate_zero(Arena* arena, u32 count = 1) noexcept {
    return static_cast<T*>(arena_allocate_zero(arena, sizeof(T) * static_cast<usize>(count), alignof(T)));
}

} // namespace maicord
