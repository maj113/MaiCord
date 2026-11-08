#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// What an arena holds, for the memory accounting below: every arena counts under its tag (other unless its owner
// names one), the short names in kArenaTagNames.
enum class ArenaTag : u8 {
    other,
    account,
    messages,
    model,
    protocol,
    media,
    text,
    render,
    ui,
    platform,
    storage,
    count,
};
inline constexpr char kArenaTagNames[] = "other\0account\0messages\0model\0protocol\0media\0text\0render\0ui\0platform\0"
                                         "storage\0";

struct Arena {
    u8* base;
    usize reserved;
    usize committed;
    usize offset;
    usize peak;
    usize commit_step;
    u32 commit_count;
    ArenaTag tag;
};

struct ArenaMark {
    usize offset;
};

enum class ArenaRewindMode : u8 {
    retain,
    erase,
};

bool arena_initialize(Arena* arena, usize reserve_bytes, usize initial_commit_bytes,
                      usize commit_step_bytes = 64 * 1024, ArenaTag tag = ArenaTag::other) noexcept;
void arena_release(Arena* arena) noexcept;
void* arena_allocate(Arena* arena, usize bytes, usize alignment) noexcept;
void* arena_allocate_zero(Arena* arena, usize bytes, usize alignment) noexcept;
ArenaMark arena_mark(const Arena* arena) noexcept;
bool arena_rewind(Arena* arena, ArenaMark mark, ArenaRewindMode mode = ArenaRewindMode::retain) noexcept;
void arena_reset(Arena* arena, ArenaRewindMode mode = ArenaRewindMode::retain) noexcept;
// Rewinds to `offset` (at most the current offset) and decommits the whole
// pages past it; the reservation stays and later allocations commit again.
bool arena_trim(Arena* arena, usize offset) noexcept;

// The live arenas of one tag, summed: bytes reserved and committed now, and the most committed at once. Kept by the
// commits and decommits themselves (an interlocked add each, never per allocation), so the totals are the arenas'
// own counts at every moment.
struct ArenaAccount {
    i64 arenas;
    i64 reserved;
    i64 committed;
    i64 peak_committed;
};
ArenaAccount arena_account(ArenaTag tag) noexcept;
// Counts pages an owner reserves and commits itself, outside an Arena, under `tag`: +1 reservation with its reserved
// bytes when it maps them, committed bytes as it commits (negative as it decommits), all negated at the release.
void arena_account_add(ArenaTag tag, i64 reservations, i64 reserved, i64 committed) noexcept;

// A zero count yields nullptr, like a zero byte count.
template <typename T> T* arena_allocate(Arena* arena, u32 count = 1) noexcept {
    return static_cast<T*>(arena_allocate(arena, sizeof(T) * static_cast<usize>(count), alignof(T)));
}

template <typename T> T* arena_allocate_zero(Arena* arena, u32 count = 1) noexcept {
    return static_cast<T*>(arena_allocate_zero(arena, sizeof(T) * static_cast<usize>(count), alignof(T)));
}

} // namespace maicord
