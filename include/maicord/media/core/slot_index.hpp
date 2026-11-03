#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Entry tables (media atlas, animation timeline) find entries through an open-addressed index: a bucket
// holds slot + 1, kSlotIndexEmpty ends a probe run and kSlotIndexTombstone marks an erased entry.
inline constexpr u32 kSlotIndexEmpty = 0;
inline constexpr u32 kSlotIndexTombstone = 0xFFFFFFFFu;
inline constexpr u32 kSlotIndexNone = 0xFFFFFFFFu;

// Bucket tables are powers of two in practice: mask instead of dividing on every probe.
constexpr u32 slot_index_wrap(u64 value, u32 capacity) noexcept {
    return (capacity & (capacity - 1)) == 0 ? static_cast<u32>(value) & (capacity - 1)
                                            : static_cast<u32>(value % capacity);
}

// The bucket whose slot satisfies match(slot), or kSlotIndexNone.
template <typename Match>
__forceinline u32 slot_index_find(const u32* buckets, u32 capacity, u64 hash, Match match) noexcept {
    u32 bucket = slot_index_wrap(hash, capacity);
    for (u32 probe = 0; probe < capacity; ++probe, bucket = bucket + 1 == capacity ? 0 : bucket + 1) {
        const u32 value = buckets[bucket];
        if (value == kSlotIndexEmpty)
            break;
        if (value != kSlotIndexTombstone && match(value - 1u))
            return bucket;
    }
    return kSlotIndexNone;
}

// Stores slot at the first tombstone of the probe run, else at its first empty bucket.
__forceinline bool slot_index_insert(u32* buckets, u32 capacity, u64 hash, u32 slot) noexcept {
    u32 bucket = slot_index_wrap(hash, capacity);
    u32 tombstone = kSlotIndexNone;
    for (u32 probe = 0; probe < capacity; ++probe, bucket = bucket + 1 == capacity ? 0 : bucket + 1) {
        const u32 value = buckets[bucket];
        if (value == kSlotIndexTombstone) {
            if (tombstone == kSlotIndexNone)
                tombstone = bucket;
        } else if (value == kSlotIndexEmpty) {
            buckets[tombstone == kSlotIndexNone ? bucket : tombstone] = slot + 1u;
            return true;
        }
    }
    if (tombstone == kSlotIndexNone)
        return false;
    buckets[tombstone] = slot + 1u;
    return true;
}

// The first slot at or after *cursor for which occupied(slot) is false (the cursor then moves past it),
// or kSlotIndexNone when the table is full.
template <typename Occupied>
__forceinline u32 slot_table_take_free(u32 entry_count, u32 capacity, u32* cursor, Occupied occupied) noexcept {
    if (entry_count >= capacity)
        return kSlotIndexNone;
    u32 slot = *cursor;
    for (u32 offset = 0; offset < capacity; ++offset) {
        if (!occupied(slot)) {
            *cursor = slot + 1 == capacity ? 0 : slot + 1;
            return slot;
        }
        if (++slot == capacity)
            slot = 0;
    }
    return kSlotIndexNone;
}

} // namespace maicord
