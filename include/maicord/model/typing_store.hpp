#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u32 kTypingRecordCapacity = 64;
inline constexpr u64 kTypingLifetimeMs = 10'000;
inline constexpr u64 kTypingLocalRefreshMs = 8'000;
inline constexpr u64 kTypingLocalDelayMs = 1'500;

struct TypingRecord {
    u64 channel_id;
    u64 guild_id;
    u64 user_id;
    u64 expires_at_ms;
};

enum class TypingStoreError : u8 {
    none,
    invalid_input,
    capacity,
};

struct TypingStoreResult {
    TypingStoreError error;
    bool changed;
    bool accepted;

    constexpr bool ok() const noexcept {
        return error == TypingStoreError::none;
    }
};

struct TypingPost {
    u64 channel_id;
    bool ready;
    bool suppressed;
};

struct TypingStore {
    Slice<TypingRecord> records;
    u64 local_channel_id;
    u64 local_user_id;
    u64 local_previous_send_ms;
    u64 local_post_at_ms;
    u32 count;
    u32 revision;
    u32 dropped;
    bool local_post_pending;
};

bool typing_store_initialize(
    TypingStore* store,
    Slice<TypingRecord> records) noexcept;
bool typing_store_reset(TypingStore* store) noexcept;
TypingStoreResult typing_store_start(
    TypingStore* store, u64 channel_id, u64 guild_id, u64 user_id,
    u64 now_ms) noexcept;
TypingStoreResult typing_store_stop(
    TypingStore* store, u64 channel_id, u64 user_id) noexcept;
TypingStoreResult typing_store_note_local_input(
    TypingStore* store, u64 channel_id, u64 guild_id, u64 user_id,
    u64 now_ms) noexcept;
TypingStoreResult typing_store_stop_local(
    TypingStore* store, u64 channel_id) noexcept;
TypingStoreResult typing_store_note_message(
    TypingStore* store, u64 channel_id, u64 author_id,
    bool optimistic) noexcept;
TypingStoreResult typing_store_tick(
    TypingStore* store, u64 now_ms) noexcept;
TypingPost typing_store_take_local_post(
    TypingStore* store, u64 now_ms) noexcept;
u64 typing_store_next_wake_at(
    const TypingStore& store) noexcept;
u32 typing_store_channel_count(
    const TypingStore& store, u64 channel_id,
    u64 excluded_user_id = 0) noexcept;
u32 typing_store_channel_users(
    const TypingStore& store, u64 channel_id, u64 excluded_user_id,
    Slice<u64> users) noexcept;

} // namespace maicord
