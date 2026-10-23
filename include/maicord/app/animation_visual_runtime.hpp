#pragma once

#include "maicord/core/stable_storage.hpp"
#include "maicord/media/animated_webp.hpp"
#include "maicord/media/animation_timeline.hpp"
#include "maicord/media/apng.hpp"
#include "maicord/media/gif.hpp"

namespace maicord {

enum class AccountMediaFrameKind : u8;

inline constexpr u32 kAnimationVisualInitialCapacity = 128;
inline constexpr u32 kAnimationVisualMaximumCapacity = 4'096;
inline constexpr u32 kAnimationPendingAdvanceCapacity = 4;

union AnimationVisualFrame {
    ApngFrame apng;
    GifFrame gif;
    struct {
        AnimatedWebpFrame frame;
        u32 background_rgba;
    } animated_webp;
};

struct AnimationVisualState {
    u64 content_identity;
    MediaAssetKey key;
    AnimationVisualFrame frame;
    u32 generation;
    u16 cache_generation;
    AccountMediaFrameKind kind;
    bool occupied;
    // Exact initial owner: message ID/revision or catalog scope/generation,
    // selected by the typed asset kind. Cold advances carry only the token.
    u64 source_scope;
    u32 source_revision;
};

// Stable-address, demand-committed ownership for the synchronized animation
// timeline and renderer composition state. Slot indices stay valid as the
// active working set grows.
struct AnimationVisualRuntime {
    AnimationTimeline timeline;
    StableStorage entries;
    StableStorage buckets;
    StableStorage visual_storage;
    AnimationVisualState* visuals;
    AnimationFrameAdvance pending_advances[kAnimationPendingAdvanceCapacity];
    u32 pending_advance_count;
    u32 next_generation;
};

bool animation_visual_runtime_initialize(AnimationVisualRuntime* runtime) noexcept;
bool animation_visual_runtime_reserve(AnimationVisualRuntime* runtime, u32 required_capacity) noexcept;
AnimationTimelineResult animation_visual_runtime_publish(AnimationVisualRuntime* runtime,
                                                         const AnimationTimelineDescriptor& descriptor,
                                                         u64 monotonic_now_us) noexcept;
void animation_visual_runtime_reset(AnimationVisualRuntime* runtime) noexcept;
void animation_visual_runtime_suspend(AnimationVisualRuntime* runtime, u64 now_us) noexcept;
void animation_visual_runtime_resume(AnimationVisualRuntime* runtime, u64 now_us) noexcept;
void animation_visual_runtime_release(AnimationVisualRuntime* runtime) noexcept;

static_assert(sizeof(AnimationVisualState) == 80);
static_assert(sizeof(AnimationVisualRuntime) < 1'024);

} // namespace maicord
