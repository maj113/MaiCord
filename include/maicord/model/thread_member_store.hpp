#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
inline constexpr u32 kThreadMemberMaximumIds = 2048;
using ThreadMemberReserve = bool(*)(void* context, u32 required_count) noexcept;
// Identity retention is independent of the bounded guild profile cache. The
// inactive bank stages a replacement so an interrupted snapshot stays invisible.
struct ThreadMemberStore {
    Slice<u64> banks[2];
    u64 guild_id, thread_id;
    u32 count, revision, generation, rejected;
    u32 staging_count;
    ThreadMemberReserve reserve;
    void* reserve_context;
    u32 backed_count;
    u8 active_bank;
    bool known, incomplete, staging, staging_incomplete, staging_sorted;
};
bool thread_member_store_initialize(ThreadMemberStore* store,
    Slice<u64> first, Slice<u64> second) noexcept;
// Both Slice counts describe reserved capacity. A callback must back BOTH banks
// before returning true; initialize and selection never touch their contents.
// Null callback retains the fixed, fully writable caller-bank contract.
bool thread_member_store_set_reserve(ThreadMemberStore* store,
    ThreadMemberReserve callback, void* context) noexcept;
bool thread_member_store_select(ThreadMemberStore* store,
    u64 guild_id, u64 thread_id) noexcept;
void thread_member_store_reset_snapshot(ThreadMemberStore* store) noexcept;
bool thread_member_store_owns(const ThreadMemberStore& store,
    u64 guild_id, u64 thread_id) noexcept;
bool thread_member_store_contains(const ThreadMemberStore& store, u64 user_id) noexcept;
bool thread_member_store_snapshot_begin(ThreadMemberStore* store,
    u64 guild_id, u64 thread_id) noexcept;
bool thread_member_store_snapshot_append(ThreadMemberStore* store, u64 user_id) noexcept;
bool thread_member_store_snapshot_commit(ThreadMemberStore* store, bool incomplete = false) noexcept;
void thread_member_store_snapshot_abort(ThreadMemberStore* store) noexcept;
bool thread_member_store_add(ThreadMemberStore* store, u64 user_id) noexcept;
bool thread_member_store_remove(ThreadMemberStore* store, u64 user_id) noexcept;
} // namespace maicord
