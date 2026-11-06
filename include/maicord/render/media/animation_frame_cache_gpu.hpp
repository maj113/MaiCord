#pragma once

#include "maicord/core/stable_storage.hpp"
#include "maicord/render/media/animation_frame_cache.hpp"
#include "maicord/gpu/gpu.hpp"

namespace maicord {

struct MediaAtlas;
struct MediaAtlasHandle;
struct MediaAtlasResult;

inline constexpr u32 kAnimationFrameCacheGpuEntryCapacity = 4'096;
inline constexpr u32 kAnimationFrameCacheGpuBucketCapacity =
    kAnimationFrameCacheGpuEntryCapacity * 2;
inline constexpr u32 kAnimationFrameCacheGpuRowCapacity = 128;
inline constexpr u32 kAnimationFrameCacheGpuInitialEntryCapacity = 128;
// Frame tiles live in a paged surface 2048 wide, in pages of 1024 rows (8 MiB): two pages for identity frames
// (up to 128 pixels), four for previews (up to 512). One page of rows is kept beyond the frames shown.
inline constexpr u16 kAnimationFrameCacheGpuExtent = 2'048;
inline constexpr u16 kAnimationFrameCacheGpuPageRows = 1'024;
inline constexpr u16 kAnimationFrameCacheGpuMaximumFrameExtent = 128;
inline constexpr u16 kAnimationFrameCacheGpuPreviewFrameExtent = 512;

struct AnimationFramePresentation {
    GlyphAtlasHandle frame;
    u16 media_generation;
    bool materialized;
    u8 reserved;
};

struct AnimationFrameCacheGpuStorage {
    StableStorage entries;
    StableStorage buckets;
    StableStorage allocations;
    StableStorage presentations;
};

struct AnimationFrameCacheGpu {
    AnimationFrameCache cache;
    GpuPagedTexture* surface;
    GpuSrv* texture_view; // the surface's view, owned by it
    AnimationFrameCacheGpuStorage storage;
    GlyphAtlasRow rows[kAnimationFrameCacheGpuRowCapacity];
    u64 captures;
    u64 restores;
    u64 copied_pixels;
    u32 failures;
    u16 maximum_frame_extent;
};

// Frames neither shown nor replayed within this window go at idle trim, once
// the cache has made no new frame for kAnimationFrameCacheGpuIdleMs.
inline constexpr u32 kAnimationFrameCacheGpuProtectMs = 8'000;
inline constexpr u32 kAnimationFrameCacheGpuIdleMs = 10'000;

// The identity cache, or (`preview`) the large-preview one.
bool animation_frame_cache_gpu_initialize(AnimationFrameCacheGpu* gpu, bool preview = false) noexcept;
void animation_frame_cache_gpu_observe_time(AnimationFrameCacheGpu* gpu, u64 now_ms) noexcept;
// Zero when nothing is pending; otherwise when the idle trim may run.
u64 animation_frame_cache_gpu_trim_deadline(const AnimationFrameCacheGpu& gpu) noexcept;
// Idle trim: drops what is neither shown nor recently replayed and returns
// the memory of rows that empty.
bool animation_frame_cache_gpu_trim(AnimationFrameCacheGpu* gpu, GpuContext1* context, u64 now_ms) noexcept;

bool animation_frame_cache_gpu_reserve(AnimationFrameCacheGpu* gpu,
                                       u32 required_capacity) noexcept;
void animation_frame_cache_gpu_reset(AnimationFrameCacheGpu* gpu) noexcept;
void animation_frame_cache_gpu_release(AnimationFrameCacheGpu* gpu) noexcept;
void animation_frame_cache_gpu_begin_visibility(AnimationFrameCacheGpu* gpu) noexcept;
bool animation_frame_cache_gpu_capture(
    AnimationFrameCacheGpu* gpu, GpuDevice* device,
    GpuContext1* context, MediaAtlas* media_atlas,
    GpuPagedTexture* media_surface,
    const AnimationFrameCacheDescriptor& descriptor,
    MediaAtlasHandle* presented_media = nullptr) noexcept;
bool animation_frame_cache_gpu_restore(
    AnimationFrameCacheGpu* gpu, MediaAtlas* media_atlas,
    const AnimationFrameCacheKey& key, AnimationFrameCacheResult* result,
    MediaAtlasHandle* presented_media = nullptr) noexcept;
bool animation_frame_cache_gpu_materialize(
    AnimationFrameCacheGpu* gpu, GpuContext1* context,
    MediaAtlas* media_atlas, GpuPagedTexture* media_surface,
    MediaAssetKey key) noexcept;
bool animation_frame_cache_gpu_resolve(
    AnimationFrameCacheGpu* gpu, const MediaAtlasResult& media,
    GlyphAtlasAllocation* frame) noexcept;

static_assert(sizeof(AnimationFramePresentation) == 8);
static_assert(sizeof(AnimationFrameCacheGpu) < 8 * 1024);

} // namespace maicord
