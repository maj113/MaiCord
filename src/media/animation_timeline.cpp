#include "maicord/media/animation_timeline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/deadline.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
namespace {

constexpr bool occupied(const AnimationTimelineEntry& entry) noexcept {
    return (entry.flags & animation_timeline_entry_occupied) != 0;
}

constexpr bool visible(const AnimationTimelineEntry& entry) noexcept {
    return (entry.flags & animation_timeline_entry_visible) != 0;
}

constexpr bool pending(const AnimationTimelineEntry& entry) noexcept {
    return (entry.flags & animation_timeline_entry_pending) != 0;
}

constexpr bool finished(const AnimationTimelineEntry& entry) noexcept {
    return (entry.flags & animation_timeline_entry_finished) != 0;
}

u64 presentation_time(const AnimationTimeline& timeline, u64 now_us) noexcept {
    return timeline.suspended ? timeline.suspended_at_us : now_us;
}

u32 key_bucket(const MediaAssetKey& key, u32 capacity) noexcept {
    return static_cast<u32>(media_asset_hash(key) % capacity);
}

bool descriptor_valid(const AnimationTimelineDescriptor& descriptor) noexcept {
    return descriptor.generation != 0 && descriptor.frame_count != 0 &&
           descriptor.frame_count <= kAnimationTimelineFrameMaximum &&
           descriptor.first_frame_duration_us != 0 &&
           descriptor.first_frame_duration_us <= kAnimationFrameDurationMaximumUs;
}

inline constexpr u32 kEmptyBucket = 0;
inline constexpr u32 kTombstoneBucket = 0xFFFFFFFFu;

u32 find_bucket(const AnimationTimeline* timeline, const MediaAssetKey& key) noexcept {
    if (!timeline || !timeline->entries || !timeline->buckets || timeline->capacity == 0 ||
        timeline->bucket_capacity == 0 || !media_asset_key_valid(key)) {
        return 0xFFFFFFFFu;
    }
    const u32 start = key_bucket(key, timeline->bucket_capacity);
    for (u32 probe = 0; probe < timeline->bucket_capacity; ++probe) {
        const u32 bucket = (start + probe) % timeline->bucket_capacity;
        const u32 value = timeline->buckets[bucket];
        if (value == kEmptyBucket) {
            break;
        }
        if (value == kTombstoneBucket) {
            continue;
        }
        const u32 slot = value - 1;
        if (slot < timeline->capacity && occupied(timeline->entries[slot]) &&
            media_asset_key_equal(timeline->entries[slot].key, key)) {
            return bucket;
        }
    }
    return 0xFFFFFFFFu;
}

u32 find_slot(const AnimationTimeline* timeline, const MediaAssetKey& key) noexcept {
    const u32 bucket = find_bucket(timeline, key);
    return bucket == 0xFFFFFFFFu ? 0xFFFFFFFFu : timeline->buckets[bucket] - 1;
}

u32 find_free_slot(AnimationTimeline* timeline) noexcept {
    if (timeline->entry_count >= timeline->capacity) {
        return 0xFFFFFFFFu;
    }
    u32 slot = timeline->free_cursor;
    for (u32 offset = 0; offset < timeline->capacity; ++offset) {
        if (!occupied(timeline->entries[slot])) {
            timeline->free_cursor = slot + 1 == timeline->capacity ? 0 : slot + 1;
            return slot;
        }
        if (++slot == timeline->capacity)
            slot = 0;
    }
    return 0xFFFFFFFFu;
}

bool insert_bucket(AnimationTimeline* timeline, MediaAssetKey key, u32 slot) noexcept {
    const u32 start = key_bucket(key, timeline->bucket_capacity);
    u32 tombstone_bucket = 0xFFFFFFFFu;
    for (u32 probe = 0; probe < timeline->bucket_capacity; ++probe) {
        const u32 bucket = (start + probe) % timeline->bucket_capacity;
        const u32 value = timeline->buckets[bucket];
        if (value == kTombstoneBucket) {
            if (tombstone_bucket == 0xFFFFFFFFu) {
                tombstone_bucket = bucket;
            }
            continue;
        }
        if (value == kEmptyBucket) {
            timeline->buckets[tombstone_bucket == 0xFFFFFFFFu ? bucket : tombstone_bucket] = slot + 1;
            return true;
        }
    }
    if (tombstone_bucket != 0xFFFFFFFFu) {
        timeline->buckets[tombstone_bucket] = slot + 1;
        return true;
    }
    return false;
}

AnimationTimelineResult find_pending(AnimationTimeline* timeline, const AnimationFrameAdvance& advance,
                                     AnimationTimelineEntry** output) noexcept {
    *output = nullptr;
    if (!timeline || !timeline->entries) {
        return {AnimationTimelineError::invalid_argument, 0};
    }
    const u32 slot = find_slot(timeline, advance.key);
    if (slot == 0xFFFFFFFFu) {
        return {AnimationTimelineError::not_found, 0};
    }
    AnimationTimelineEntry& entry = timeline->entries[slot];
    if (entry.generation != advance.generation) {
        return {AnimationTimelineError::stale_generation, slot};
    }
    if (!pending(entry) || entry.due_at_us != advance.scheduled_at_us ||
        entry.pending_frame_index != advance.frame_index ||
        entry.pending_play_index != advance.play_index) {
        return {AnimationTimelineError::not_pending, slot};
    }
    *output = &entry;
    return {AnimationTimelineError::none, slot};
}

bool next_position(const AnimationTimelineEntry& entry, u32* frame_index, u32* play_index) noexcept {
    if (entry.flags & animation_timeline_entry_restart) {
        *frame_index = 0;
        *play_index = 0;
        return true;
    }
    if (entry.frame_index + 1 < entry.frame_count) {
        *frame_index = entry.frame_index + 1;
        *play_index = entry.play_index;
        return true;
    }
    if (entry.frame_count == 1 || (entry.play_count != 0 && entry.play_index + 1 >= entry.play_count)) {
        return false;
    }
    *frame_index = 0;
    *play_index = entry.play_index + 1;
    return true;
}

} // namespace

bool animation_timeline_restart(AnimationTimeline* timeline, const MediaAssetKey& key_view, u64 epoch,
                                u64 now) noexcept {
    const MediaAssetKey key = key_view; // the timeline is rewritten below
    if (!timeline || !epoch)
        return false;
    now = presentation_time(*timeline, now);
    const u32 slot = find_slot(timeline, key);
    if (slot == ~0u)
        return false;
    auto& entry = timeline->entries[slot];
    if (entry.playback_epoch == epoch)
        return true;
    if (pending(entry))
        return false;
    entry.playback_epoch = epoch;
    entry.flags = (entry.flags & ~animation_timeline_entry_finished) | animation_timeline_entry_restart;
    entry.due_at_us = now;
    entry.paused_remaining_us = 1;
    return true;
}

bool animation_timeline_initialize(AnimationTimeline* timeline, Slice<AnimationTimelineEntry> entries,
                                   Slice<u32> buckets) noexcept {
    if (!timeline || !entries.data || entries.count == 0 || entries.count > (~0u / 2u) || !buckets.data ||
        buckets.count < entries.count * 2u) {
        return false;
    }
    timeline->entries = entries.data;
    timeline->buckets = buckets.data;
    timeline->capacity = entries.count;
    timeline->bucket_capacity = buckets.count;
    animation_timeline_reset(timeline);
    return true;
}

bool animation_timeline_grow_storage(AnimationTimeline* timeline, Slice<AnimationTimelineEntry> entries,
                                     Slice<u32> buckets) noexcept {
    if (!timeline || !timeline->entries || !timeline->buckets || entries.data != timeline->entries ||
        buckets.data != timeline->buckets || entries.count < timeline->capacity ||
        entries.count > (~0u / 2u) || buckets.count < entries.count * 2u ||
        buckets.count < timeline->bucket_capacity) {
        return false;
    }
    if (entries.count == timeline->capacity && buckets.count == timeline->bucket_capacity) {
        return true;
    }

    const u32 previous_capacity = timeline->capacity;
    for (u32 index = previous_capacity; index < entries.count; ++index) {
        reset_value<AnimationTimelineEntry>(&entries[index]);
    }
    for (u32 index = 0; index < buckets.count; ++index) {
        buckets[index] = kEmptyBucket;
    }
    timeline->capacity = entries.count;
    timeline->bucket_capacity = buckets.count;
    for (u32 slot = 0; slot < previous_capacity; ++slot) {
        if (occupied(entries[slot]) && !insert_bucket(timeline, entries[slot].key, slot)) {
            return false;
        }
    }
    return true;
}

void animation_timeline_reset(AnimationTimeline* timeline) noexcept {
    if (!timeline || !timeline->entries || !timeline->buckets) {
        return;
    }
    for (u32 index = 0; index < timeline->capacity; ++index) {
        reset_value<AnimationTimelineEntry>(&timeline->entries[index]);
    }
    for (u32 index = 0; index < timeline->bucket_capacity; ++index) {
        timeline->buckets[index] = kEmptyBucket;
    }
    timeline->entry_count = 0;
    timeline->visibility_epoch = 1;
    timeline->scan_cursor = 0;
    timeline->free_cursor = 0;
    reset_value<AnimationTimelineCounters>(&timeline->counters);
    timeline->suspended_at_us = 0;
    timeline->suspended = false;
}

void animation_timeline_suspend(AnimationTimeline* timeline, u64 now_us) noexcept {
    if (!timeline || !timeline->entries || timeline->suspended) return;
    timeline->suspended_at_us = now_us;
    timeline->suspended = true;
}

u64 animation_timeline_resume(AnimationTimeline* timeline, u64 now_us) noexcept {
    if (!timeline || !timeline->entries || !timeline->suspended) return 0;
    const u64 elapsed = now_us > timeline->suspended_at_us ? now_us - timeline->suspended_at_us : 0;
    timeline->suspended = false;
    timeline->suspended_at_us = 0;
    for (u32 i = 0; elapsed && i < timeline->capacity; ++i) {
        auto& entry = timeline->entries[i];
        if (occupied(entry) && !finished(entry) && (visible(entry) || pending(entry)))
            entry.due_at_us = deadline_after(entry.due_at_us, elapsed);
    }
    return elapsed;
}

AnimationTimelineResult animation_timeline_publish(AnimationTimeline* timeline,
                                                   const AnimationTimelineDescriptor& descriptor,
                                                   u64 monotonic_now_us) noexcept {
    if (!timeline || !timeline->entries || !timeline->buckets) {
        return {AnimationTimelineError::invalid_argument, 0};
    }
    monotonic_now_us = presentation_time(*timeline, monotonic_now_us);
    if (!media_asset_key_valid(descriptor.key)) {
        return {AnimationTimelineError::invalid_key, 0};
    }
    if (!descriptor_valid(descriptor)) {
        return {AnimationTimelineError::invalid_descriptor, 0};
    }
    u32 slot = find_slot(timeline, descriptor.key);
    if (slot == 0xFFFFFFFFu) {
        slot = find_free_slot(timeline);
        if (slot == 0xFFFFFFFFu || !insert_bucket(timeline, descriptor.key, slot)) {
            return {AnimationTimelineError::no_capacity, 0};
        }
        ++timeline->entry_count;
    }
    AnimationTimelineEntry& entry = timeline->entries[slot];
    reset_value<AnimationTimelineEntry>(&entry);
    entry.key = descriptor.key;
    entry.generation = descriptor.generation;
    entry.frame_count = static_cast<u16>(descriptor.frame_count);
    entry.play_count = descriptor.play_count;
    entry.seen_epoch = descriptor.visible ? timeline->visibility_epoch : 0;
    entry.flags = animation_timeline_entry_occupied;
    if (descriptor.visible) {
        entry.flags |= animation_timeline_entry_visible;
        entry.due_at_us = deadline_after(monotonic_now_us, descriptor.first_frame_duration_us);
    } else {
        entry.paused_remaining_us = descriptor.first_frame_duration_us;
    }
    return {AnimationTimelineError::none, slot};
}

AnimationTimelineResult animation_timeline_find(const AnimationTimeline* timeline,
                                                const MediaAssetKey& key) noexcept {
    if (!timeline || !timeline->entries || !timeline->buckets) {
        return {AnimationTimelineError::invalid_argument, 0};
    }
    if (!media_asset_key_valid(key)) {
        return {AnimationTimelineError::invalid_key, 0};
    }
    const u32 slot = find_slot(timeline, key);
    return slot == 0xFFFFFFFFu ? AnimationTimelineResult{AnimationTimelineError::not_found, 0}
                               : AnimationTimelineResult{AnimationTimelineError::none, slot};
}

AnimationTimelineResult animation_timeline_release(AnimationTimeline* timeline, const MediaAssetKey& key_view,
                                                   u32 generation) noexcept {
    const MediaAssetKey key = key_view; // the timeline is rewritten below
    if (!timeline || !timeline->entries || !timeline->buckets) {
        return {AnimationTimelineError::invalid_argument, 0};
    }
    const u32 bucket = find_bucket(timeline, key);
    if (bucket == 0xFFFFFFFFu) {
        return {AnimationTimelineError::not_found, 0};
    }
    const u32 slot = timeline->buckets[bucket] - 1;
    if (timeline->entries[slot].generation != generation) {
        return {AnimationTimelineError::stale_generation, slot};
    }
    reset_value<AnimationTimelineEntry>(&timeline->entries[slot]);
    timeline->buckets[bucket] = kTombstoneBucket;
    timeline->free_cursor = slot;
    --timeline->entry_count;
    return {AnimationTimelineError::none, slot};
}

void animation_timeline_begin_visibility(AnimationTimeline* timeline) noexcept {
    if (!timeline || !timeline->entries) {
        return;
    }
    ++timeline->visibility_epoch;
    if (timeline->visibility_epoch != 0) {
        return;
    }
    timeline->visibility_epoch = 1;
    for (u32 index = 0; index < timeline->capacity; ++index) {
        timeline->entries[index].seen_epoch = 0;
    }
}

bool animation_timeline_observe_visible(AnimationTimeline* timeline, const MediaAssetKey& key_view,
                                        u64 monotonic_now_us) noexcept {
    const MediaAssetKey key = key_view; // the timeline is rewritten below
    const u32 slot = find_slot(timeline, key);
    if (slot == 0xFFFFFFFFu) {
        return false;
    }
    monotonic_now_us = presentation_time(*timeline, monotonic_now_us);
    AnimationTimelineEntry& entry = timeline->entries[slot];
    entry.seen_epoch = timeline->visibility_epoch;
    if (visible(entry)) {
        return true;
    }
    entry.flags |= animation_timeline_entry_visible;
    if (!finished(entry) && !pending(entry)) {
        entry.due_at_us = deadline_after(monotonic_now_us, entry.paused_remaining_us);
    }
    entry.paused_remaining_us = 0;
    return true;
}

void animation_timeline_end_visibility(AnimationTimeline* timeline, u64 monotonic_now_us) noexcept {
    if (!timeline || !timeline->entries) {
        return;
    }
    monotonic_now_us = presentation_time(*timeline, monotonic_now_us);
    constexpr u32 kRequiredFlags = animation_timeline_entry_occupied | animation_timeline_entry_visible;
    // entry_count is exact: stop once every occupied entry was visited.
    u32 live = timeline->entry_count;
    for (u32 index = 0; live && index < timeline->capacity; ++index) {
        AnimationTimelineEntry& entry = timeline->entries[index];
        if (!occupied(entry))
            continue;
        --live;
        if ((entry.flags & kRequiredFlags) != kRequiredFlags ||
            entry.seen_epoch == timeline->visibility_epoch) {
            continue;
        }
        entry.flags &= ~animation_timeline_entry_visible;
        if (!finished(entry) && !pending(entry)) {
            entry.paused_remaining_us =
                entry.due_at_us > monotonic_now_us ? entry.due_at_us - monotonic_now_us : 0;
            entry.due_at_us = 0;
        }
    }
}

AnimationTimelineCollectResult animation_timeline_collect_due(
    AnimationTimeline* timeline, u64 monotonic_now_us, Slice<AnimationFrameAdvance> output) noexcept {
    if (!timeline || !timeline->entries || (output.count != 0 && !output.data)) {
        return {AnimationTimelineError::invalid_argument, 0, 0};
    }
    AnimationTimelineCollectResult result{AnimationTimelineError::none, 0, 0};
    if (timeline->capacity == 0 || timeline->suspended) {
        return result;
    }
    const u32 first = timeline->scan_cursor % timeline->capacity;
    constexpr u32 kRequiredFlags = animation_timeline_entry_occupied | animation_timeline_entry_visible;
    constexpr u32 kRejectedFlags = animation_timeline_entry_pending | animation_timeline_entry_finished;
    u32 live = timeline->entry_count;
    for (u32 offset = 0; live && offset < timeline->capacity; ++offset) {
        const u32 index = (first + offset) % timeline->capacity;
        AnimationTimelineEntry& entry = timeline->entries[index];
        if (!occupied(entry))
            continue;
        --live;
        const u32 flags = entry.flags;
        if ((flags & kRequiredFlags) != kRequiredFlags || (flags & kRejectedFlags) != 0 ||
            entry.due_at_us > monotonic_now_us) {
            continue;
        }
        u32 next_frame = 0;
        u32 next_play = 0;
        if (!next_position(entry, &next_frame, &next_play)) {
            entry.flags |= animation_timeline_entry_finished;
            entry.due_at_us = 0;
            ++result.completed;
            continue;
        }
        if (result.advances >= output.count) {
            continue;
        }
        // After a long stall (covered surface, starved scheduler, long decode)
        // resynchronize to wall time instead of replaying every missed frame
        // back to back; small lateness keeps the absolute cadence.
        if (monotonic_now_us - entry.due_at_us > kAnimationLateResyncUs) {
            entry.due_at_us = monotonic_now_us;
        }
        AnimationFrameAdvance& advance = output[result.advances++];
        advance =
            AnimationFrameAdvance{entry.key, entry.due_at_us, entry.generation, next_frame, next_play};
        entry.pending_frame_index = next_frame;
        entry.pending_play_index = next_play;
        entry.flags |= animation_timeline_entry_pending;
        ++timeline->counters.due_advances;
        timeline->scan_cursor = (index + 1) % timeline->capacity;
    }
    return result;
}

AnimationTimelineResult animation_timeline_commit_advance(AnimationTimeline* timeline,
                                                          const AnimationFrameAdvance& advance,
                                                          u64 frame_duration_us) noexcept {
    if (frame_duration_us == 0 || frame_duration_us > kAnimationFrameDurationMaximumUs) {
        return {AnimationTimelineError::invalid_descriptor, 0};
    }
    AnimationTimelineEntry* entry = nullptr;
    const AnimationTimelineResult found = find_pending(timeline, advance, &entry);
    if (!found.ok()) {
        return found;
    }
    entry->frame_index = static_cast<u16>(entry->pending_frame_index);
    entry->flags &= ~animation_timeline_entry_restart;
    entry->play_index = entry->pending_play_index;
    entry->flags &= ~animation_timeline_entry_pending;
    if (visible(*entry)) {
        // Anchor to the presentation deadline, not completion time. Late
        // decode therefore catches up without permanently slowing animation.
        entry->due_at_us = deadline_after(advance.scheduled_at_us, frame_duration_us);
    } else {
        entry->paused_remaining_us = frame_duration_us;
        entry->due_at_us = 0;
    }
    entry->pending_frame_index = 0;
    entry->pending_play_index = 0;
    return found;
}

AnimationTimelineResult animation_timeline_defer_advance(AnimationTimeline* timeline,
                                                         const AnimationFrameAdvance& advance,
                                                         u64 monotonic_now_us,
                                                         u64 retry_delay_us) noexcept {
    if (retry_delay_us == 0 || retry_delay_us > kAnimationFrameDurationMaximumUs) {
        return {AnimationTimelineError::invalid_descriptor, 0};
    }
    AnimationTimelineEntry* entry = nullptr;
    const AnimationTimelineResult found = find_pending(timeline, advance, &entry);
    if (!found.ok()) {
        return found;
    }
    entry->flags &= ~animation_timeline_entry_pending;
    if (visible(*entry)) {
        monotonic_now_us = presentation_time(*timeline, monotonic_now_us);
        entry->due_at_us = deadline_after(monotonic_now_us, retry_delay_us);
    } else {
        entry->paused_remaining_us = retry_delay_us;
        entry->due_at_us = 0;
    }
    entry->pending_frame_index = 0;
    entry->pending_play_index = 0;
    return found;
}

u64 animation_timeline_next_wake_at_us(const AnimationTimeline* timeline) noexcept {
    if (!timeline || !timeline->entries || timeline->suspended) {
        return 0;
    }
    u64 next = 0;
    constexpr u32 kRequiredFlags = animation_timeline_entry_occupied | animation_timeline_entry_visible;
    constexpr u32 kRejectedFlags = animation_timeline_entry_pending | animation_timeline_entry_finished;
    u32 live = timeline->entry_count;
    for (u32 index = 0; live && index < timeline->capacity; ++index) {
        const AnimationTimelineEntry& entry = timeline->entries[index];
        if (!occupied(entry))
            continue;
        --live;
        const u32 flags = entry.flags;
        if ((flags & kRequiredFlags) != kRequiredFlags || (flags & kRejectedFlags) != 0 ||
            entry.due_at_us == 0) {
            continue;
        }
        if (next == 0 || entry.due_at_us < next) {
            next = entry.due_at_us;
        }
    }
    return next;
}

u32 animation_timeline_wait_timeout_ms(const AnimationTimeline* timeline, u64 monotonic_now_us) noexcept {
    const u64 wake = animation_timeline_next_wake_at_us(timeline);
    if (wake == 0) {
        return INFINITE;
    }
    if (wake <= monotonic_now_us) {
        return 0;
    }
    const u64 remaining = wake - monotonic_now_us;
    const u64 milliseconds = remaining / 1'000 + (remaining % 1'000 != 0 ? 1 : 0);
    return milliseconds >= INFINITE ? INFINITE - 1 : static_cast<u32>(milliseconds);
}

} // namespace maicord
