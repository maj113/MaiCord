#pragma once

#include "maicord/media/asset.hpp"
#include "maicord/media/animated_webp.hpp"
#include "maicord/media/apng.hpp"
#include "maicord/media/gif.hpp"
#include "maicord/render/glyph_atlas.hpp"
#include <stddef.h>

namespace maicord {

struct AnimationFrameCacheKey {
    MediaAssetKey asset;
    u32 frame_index;
    u16 source_generation;
    u16 reserved;
    // Nonzero only after the encoded cache establishes exact byte equality;
    // zero keys the frame by asset and generation.
    u64 content_identity;
};

enum class AnimationFrameCacheKind : u8 {
    none,
    apng,
    gif,
    animated_webp,
    lottie,
};

union AnimationFrameCacheControl {
    ApngFrame apng;
    GifFrame gif;
    AnimatedWebpFrame animated_webp;
};

struct AnimationFrameCacheDescriptor {
    AnimationFrameCacheKey key;
    AnimationFrameCacheControl control;
    u64 duration_us;
    u32 background_rgba;
    u16 width;
    u16 height;
    AnimationFrameCacheKind kind;
    u8 reserved[3];
};

struct AnimationFrameCacheEntry {
    AnimationFrameCacheKey key;
    u64 home_bucket;
    u64 duration_us;
    AnimationFrameCacheControl control;
    GlyphAtlasHandle allocation;
    u32 background_rgba;
    u16 width;
    u16 height;
    AnimationFrameCacheKind kind;
    bool occupied;
    u8 reserved[2];
};

struct AnimationFrameCacheCounters {
    u64 captures;
    u64 hits;
    u64 misses;
    u64 evictions;
    u64 rejected;
};

struct AnimationFrameCache {
    GlyphAtlasAllocator allocations;
    AnimationFrameCacheEntry* entries;
    u32* buckets;
    u32 entry_capacity;
    u32 bucket_capacity;
    u32 entry_count;
    u32 free_cursor;
    u64 next_allocation_key;
    AnimationFrameCacheCounters counters;
};

enum class AnimationFrameCacheError : u8 {
    none,
    invalid_input,
    not_found,
    too_large,
    no_capacity,
    corrupt_state,
};

struct AnimationFrameCacheResult {
    GlyphAtlasAllocation allocation;
    AnimationFrameCacheControl control;
    u64 duration_us;
    u32 background_rgba;
    u32 frame_index;
    u16 source_generation;
    AnimationFrameCacheKind kind;
    AnimationFrameCacheError error;
    bool cached;

    constexpr bool ok() const noexcept {
        return error == AnimationFrameCacheError::none;
    }
};

bool animation_frame_cache_initialize(
    AnimationFrameCache* cache, u16 width, u16 height, u16 padding,
    Slice<AnimationFrameCacheEntry> entries, Slice<u32> buckets,
    Slice<GlyphAtlasEntry> allocations,
    Slice<GlyphAtlasRow> rows) noexcept;
// Grow stable-address backing without invalidating cache or atlas handles.
// Existing entries/allocations must stay at the same addresses; only the
// newly committed tails and rebuilt hash buckets are touched.
bool animation_frame_cache_grow_storage(
    AnimationFrameCache* cache, Slice<AnimationFrameCacheEntry> entries,
    Slice<u32> buckets, Slice<GlyphAtlasEntry> allocations) noexcept;
void animation_frame_cache_reset(AnimationFrameCache* cache) noexcept;
void animation_frame_cache_begin_visibility(
    AnimationFrameCache* cache) noexcept;
AnimationFrameCacheResult animation_frame_cache_find(
    AnimationFrameCache* cache, AnimationFrameCacheKey key) noexcept;
AnimationFrameCacheResult animation_frame_cache_allocate(
    AnimationFrameCache* cache,
    const AnimationFrameCacheDescriptor& descriptor) noexcept;

static_assert(sizeof(AnimationFrameCacheKey) == 32);
static_assert(sizeof(AnimationFrameCacheControl) == 40);
static_assert(sizeof(AnimationFrameCacheDescriptor) == 96);
static_assert(sizeof(AnimationFrameCacheEntry) == 104);
static_assert(offsetof(AnimationFrameCacheEntry, key) == 0);
static_assert(offsetof(AnimationFrameCacheEntry, home_bucket) == 32);
static_assert(offsetof(AnimationFrameCacheEntry, duration_us) == 40);
static_assert(offsetof(AnimationFrameCacheEntry, control) == 48);
static_assert(offsetof(AnimationFrameCacheEntry, allocation) == 88);
static_assert(offsetof(AnimationFrameCacheEntry, background_rgba) == 92);
static_assert(offsetof(AnimationFrameCacheEntry, width) == 96);
static_assert(offsetof(AnimationFrameCacheEntry, height) == 98);
static_assert(offsetof(AnimationFrameCacheEntry, kind) == 100);
static_assert(offsetof(AnimationFrameCacheEntry, occupied) == 101);
static_assert(offsetof(AnimationFrameCacheEntry, reserved) == 102);
static_assert(sizeof(AnimationFrameCacheCounters) == 40);
static_assert(sizeof(AnimationFrameCache) == 160);

} // namespace maicord
