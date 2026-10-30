#include "maicord/app/animation_visual_runtime.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {

void animation_visual_runtime_suspend(AnimationVisualRuntime* runtime, u64 now_us) noexcept {
    if (runtime) animation_timeline_suspend(&runtime->timeline, now_us);
}

void animation_visual_runtime_resume(AnimationVisualRuntime* runtime, u64 now_us) noexcept {
    if (!runtime) return;
    const u64 elapsed = animation_timeline_resume(&runtime->timeline, now_us);
    // Codec completions still refer to the frame/generation requested before
    // the pause. Keep their timeline tokens synchronized without restarting
    // decoding or discarding compositing state.
    for (u32 i = 0; elapsed && i < runtime->pending_advance_count; ++i)
        runtime->pending_advances[i].scheduled_at_us =
            deadline_after(runtime->pending_advances[i].scheduled_at_us, elapsed);
}

bool animation_visual_runtime_initialize(
    AnimationVisualRuntime* runtime) noexcept {
    if (!runtime || runtime->entries.arena.base ||
        runtime->buckets.arena.base || runtime->visual_storage.arena.base) {
        return false;
    }
    reset_value<AnimationVisualRuntime>(runtime);
    if (!stable_storage_initialize(
            &runtime->entries, kAnimationVisualMaximumCapacity,
            kAnimationVisualInitialCapacity, sizeof(AnimationTimelineEntry),
            alignof(AnimationTimelineEntry), true) ||
        !stable_storage_initialize(
            &runtime->buckets, kAnimationVisualMaximumCapacity * 2u,
            kAnimationVisualInitialCapacity * 2u, sizeof(u32), alignof(u32),
            true) ||
        !stable_storage_initialize(
            &runtime->visual_storage, kAnimationVisualMaximumCapacity,
            kAnimationVisualInitialCapacity, sizeof(AnimationVisualState),
            alignof(AnimationVisualState), true)) {
        animation_visual_runtime_release(runtime);
        return false;
    }
    runtime->visuals =
        stable_storage_data<AnimationVisualState>(runtime->visual_storage);
    if (!animation_timeline_initialize(
            &runtime->timeline,
            Slice<AnimationTimelineEntry>{
                stable_storage_data<AnimationTimelineEntry>(runtime->entries),
                runtime->entries.capacity},
            Slice<u32>{stable_storage_data<u32>(runtime->buckets),
                       runtime->buckets.capacity})) {
        animation_visual_runtime_release(runtime);
        return false;
    }
    return true;
}

bool animation_visual_runtime_reserve(
    AnimationVisualRuntime* runtime, u32 required_capacity) noexcept {
    if (!runtime || !runtime->visuals || required_capacity == 0 ||
        required_capacity > kAnimationVisualMaximumCapacity) {
        return false;
    }
    if (required_capacity <= runtime->timeline.capacity) {
        return true;
    }

    u32 capacity = runtime->timeline.capacity;
    while (capacity < required_capacity) {
        capacity = capacity > kAnimationVisualMaximumCapacity / 2u
                       ? kAnimationVisualMaximumCapacity
                       : capacity * 2u;
    }
    if (!stable_storage_reserve(&runtime->entries, capacity) ||
        !stable_storage_reserve(&runtime->buckets, capacity * 2u) ||
        !stable_storage_reserve(&runtime->visual_storage, capacity)) {
        return false;
    }
    return animation_timeline_grow_storage(
        &runtime->timeline,
        Slice<AnimationTimelineEntry>{
            stable_storage_data<AnimationTimelineEntry>(runtime->entries),
            capacity},
        Slice<u32>{stable_storage_data<u32>(runtime->buckets), capacity * 2u});
}

AnimationTimelineResult animation_visual_runtime_publish(
    AnimationVisualRuntime* runtime,
    const AnimationTimelineDescriptor& descriptor,
    u64 monotonic_now_us) noexcept {
    if (!runtime || !runtime->visuals) {
        return {AnimationTimelineError::invalid_argument, 0};
    }
    AnimationTimelineResult published = animation_timeline_publish(
        &runtime->timeline, descriptor, monotonic_now_us);
    if (published.error != AnimationTimelineError::no_capacity ||
        runtime->timeline.capacity >= kAnimationVisualMaximumCapacity) {
        return published;
    }
    if (!animation_visual_runtime_reserve(
            runtime, runtime->timeline.capacity + 1u)) {
        return published;
    }
    return animation_timeline_publish(
        &runtime->timeline, descriptor, monotonic_now_us);
}

void animation_visual_runtime_reset(AnimationVisualRuntime* runtime) noexcept {
    if (!runtime || !runtime->visuals) {
        return;
    }
    animation_timeline_reset(&runtime->timeline);
    for (u32 index = 0; index < runtime->visual_storage.capacity; ++index) {
        reset_value<AnimationVisualState>(&runtime->visuals[index]);
    }
    for (u32 index = 0; index < kAnimationPendingAdvanceCapacity; ++index) {
        reset_value<AnimationFrameAdvance>(&runtime->pending_advances[index]);
    }
    runtime->pending_advance_count = 0;
}

void animation_visual_runtime_release(AnimationVisualRuntime* runtime) noexcept {
    if (!runtime) {
        return;
    }
    stable_storage_release(&runtime->visual_storage);
    stable_storage_release(&runtime->buckets);
    stable_storage_release(&runtime->entries);
    reset_value<AnimationVisualRuntime>(runtime);
}

} // namespace maicord
