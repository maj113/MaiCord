#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/media/asset_key.hpp"

namespace maicord {

// The timeline accepts microsecond durations so rational frame rates do not
// accumulate millisecond-scale drift; decoder policy may impose a larger floor
// for zero or unusually short source delays.
inline constexpr u64 kAnimationFrameDurationMaximumUs = 24ull * 60ull * 60ull * 1'000'000ull;
// Frame positions are stored in 16 bits; oversized descriptors are rejected
// before narrowing.
inline constexpr u32 kAnimationTimelineFrameMaximum = 0xFFFFu;
// Collection lateness beyond this bound resynchronizes the entry's deadline to
// the collection time. Ordinary decode/pump jitter stays below it and keeps the
// absolute, drift-free cadence; a stalled pump never fast-forwards frame by frame.
inline constexpr u64 kAnimationLateResyncUs = 100'000;

enum AnimationTimelineEntryFlags : u32 {
    animation_timeline_entry_occupied = 1u << 0,
    animation_timeline_entry_visible = 1u << 1,
    animation_timeline_entry_pending = 1u << 2,
    animation_timeline_entry_finished = 1u << 3,
    animation_timeline_entry_restart = 1u << 4,
};

struct AnimationTimelineEntry {
    u64 playback_epoch;
    MediaAssetKey key;
    u64 due_at_us;
    u64 paused_remaining_us;
    u32 generation;
    u32 play_count;
    u32 play_index;
    u32 seen_epoch;
    u32 pending_frame_index;
    u32 pending_play_index;
    u32 flags;
    u16 frame_count;
    u16 frame_index;
};

struct AnimationTimelineCounters {
    u64 publishes;
    u64 releases;
    u64 due_advances;
    u64 completed_animations;
    u64 pauses;
    u64 resumes;
    u64 late_advances;
    u64 capacity_rejections;
};

struct AnimationTimeline {
    AnimationTimelineEntry* entries;
    // Stable entry slots are required because renderer composition state is
    // indexed by the published slot. Keep hashing in a separate table so
    // deletion/rehashing never moves that state.
    u32* buckets;
    u32 capacity;
    u32 bucket_capacity;
    u32 entry_count;
    u32 visibility_epoch;
    u32 scan_cursor;
    u32 free_cursor;
    AnimationTimelineCounters counters;
    u64 suspended_at_us;
    bool suspended;
};

enum class AnimationTimelineError : u8 {
    none,
    invalid_argument,
    invalid_key,
    invalid_descriptor,
    no_capacity,
    not_found,
    stale_generation,
    not_pending,
};

struct AnimationTimelineDescriptor {
    MediaAssetKey key;
    u64 first_frame_duration_us;
    u32 generation;
    u32 frame_count;
    // Normalized total plays. Zero means infinite. Format-specific decoders
    // own conversion from source loop fields into this representation.
    u32 play_count;
    bool visible;
};

struct AnimationTimelineResult {
    AnimationTimelineError error;
    u32 slot;

    constexpr bool ok() const noexcept {
        return error == AnimationTimelineError::none;
    }
};

struct AnimationFrameAdvance {
    MediaAssetKey key;
    u64 scheduled_at_us;
    u32 generation;
    u32 frame_index;
    u32 play_index;
};

struct AnimationTimelineCollectResult {
    AnimationTimelineError error;
    u32 advances;
    u32 completed;

    constexpr bool ok() const noexcept {
        return error == AnimationTimelineError::none;
    }
};

bool animation_timeline_initialize(AnimationTimeline* timeline, Slice<AnimationTimelineEntry> entries,
                                   Slice<u32> buckets = {}) noexcept;
// Expands caller-owned stable storage without changing entry slots. Existing
// entries remain in place and only the sparse bucket index is rebuilt.
bool animation_timeline_grow_storage(AnimationTimeline* timeline, Slice<AnimationTimelineEntry> entries,
                                     Slice<u32> buckets) noexcept;
void animation_timeline_reset(AnimationTimeline* timeline) noexcept;
// Modal UI loops stop presentation, not elapsed account/network time. Rebase
// visual deadlines on resume; callers must shift their outstanding advance
// tokens by the returned duration before committing them.
void animation_timeline_suspend(AnimationTimeline* timeline, u64 now_us) noexcept;
u64 animation_timeline_resume(AnimationTimeline* timeline, u64 now_us) noexcept;
AnimationTimelineResult animation_timeline_publish(AnimationTimeline* timeline,
                                                   const AnimationTimelineDescriptor& descriptor,
                                                   u64 monotonic_now_us) noexcept;
AnimationTimelineResult animation_timeline_find(const AnimationTimeline* timeline,
                                                MediaAssetKey key) noexcept;
AnimationTimelineResult animation_timeline_release(AnimationTimeline* timeline, MediaAssetKey key,
                                                   u32 generation) noexcept;
// Rewinds the entry to its first frame once per distinct nonzero playback epoch. False for an unknown
// key or while an advance is pending.
bool animation_timeline_restart(AnimationTimeline* timeline, MediaAssetKey key, u64 epoch,
                                u64 now_us) noexcept;

// A visibility epoch is committed only by end_visibility. Callers that abort a
// partial visible-range scan can omit end_visibility, leaving prior state
// untouched for entries not yet observed.
void animation_timeline_begin_visibility(AnimationTimeline* timeline) noexcept;
bool animation_timeline_observe_visible(AnimationTimeline* timeline, MediaAssetKey key,
                                        u64 monotonic_now_us) noexcept;
void animation_timeline_end_visibility(AnimationTimeline* timeline, u64 monotonic_now_us) noexcept;

AnimationTimelineCollectResult animation_timeline_collect_due(AnimationTimeline* timeline,
                                                              u64 monotonic_now_us,
                                                              Slice<AnimationFrameAdvance> output) noexcept;
AnimationTimelineResult animation_timeline_commit_advance(AnimationTimeline* timeline,
                                                          const AnimationFrameAdvance& advance,
                                                          u64 frame_duration_us) noexcept;
AnimationTimelineResult animation_timeline_defer_advance(AnimationTimeline* timeline,
                                                         const AnimationFrameAdvance& advance,
                                                         u64 monotonic_now_us, u64 retry_delay_us) noexcept;

u64 animation_timeline_next_wake_at_us(const AnimationTimeline* timeline) noexcept;
u32 animation_timeline_wait_timeout_ms(const AnimationTimeline* timeline, u64 monotonic_now_us) noexcept;

static_assert(sizeof(AnimationTimelineEntry) == 72);
static_assert(sizeof(AnimationTimelineCounters) == 64);
static_assert(sizeof(AnimationTimeline) == 120);
static_assert(sizeof(AnimationFrameAdvance) == 40);

} // namespace maicord
