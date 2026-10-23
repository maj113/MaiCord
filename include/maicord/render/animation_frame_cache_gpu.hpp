#pragma once

#include "maicord/core/stable_storage.hpp"
#include "maicord/render/animation_frame_cache.hpp"
#include "maicord/render/media_atlas.hpp"
#include "maicord/sys/d3d11.hpp"

struct ID3D11Device;
struct ID3D11DeviceContext1;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

namespace maicord {

inline constexpr u32 kAnimationFrameCacheGpuEntryCapacity = 4'096;
inline constexpr u32 kAnimationFrameCacheGpuBucketCapacity =
    kAnimationFrameCacheGpuEntryCapacity * 2;
inline constexpr u32 kAnimationFrameCacheGpuRowCapacity = 128;
inline constexpr u16 kAnimationFrameCacheGpuExtent = 2'048;
inline constexpr u16 kAnimationFrameCacheGpuMaximumFrameExtent = 128;
inline constexpr u32 kAnimationFrameCacheGpuInitialEntryCapacity = 128;

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
    ID3D11Texture2D* texture;
    ID3D11ShaderResourceView* texture_view;
    AnimationFrameCacheGpuStorage storage;
    GlyphAtlasRow rows[kAnimationFrameCacheGpuRowCapacity];
    u64 captures;
    u64 restores;
    u64 materializations;
    u64 copied_pixels;
    u32 failures;
    u16 maximum_frame_extent;
    // Height growth by recently replayed frames, released when idle.
    GlyphAtlasGrowth growth;
};

// Frames replayed within this window keep their tiles (the cache grows
// instead); a cache unused for kAnimationFrameCacheGpuIdleMs is released.
inline constexpr u32 kAnimationFrameCacheGpuProtectMs = 8'000;
inline constexpr u32 kAnimationFrameCacheGpuIdleMs = 10'000;
inline constexpr u16 kAnimationFrameCacheGpuGrowthStep = 256;

// Preview cache capacity: two viewports of RGBA frame tiles (physical
// pixels) in a 4096-wide texture, 512..2048 rows (8..32 MiB).
constexpr u16 animation_frame_cache_gpu_preview_height(u32 viewport_width, u32 viewport_height) noexcept {
    const u64 rows = (u64{viewport_width} * viewport_height * 2u + 4'095u) / 4'096u;
    const u64 stepped = (rows + kAnimationFrameCacheGpuGrowthStep - 1u) / kAnimationFrameCacheGpuGrowthStep *
                        kAnimationFrameCacheGpuGrowthStep;
    return static_cast<u16>(stepped < 512u ? 512u : stepped > 2'048u ? 2'048u : stepped);
}

bool animation_frame_cache_gpu_initialize(AnimationFrameCacheGpu* gpu,
    u16 width = kAnimationFrameCacheGpuExtent, u16 height = kAnimationFrameCacheGpuExtent,
    u16 maximum_frame_extent = kAnimationFrameCacheGpuMaximumFrameExtent) noexcept;
// Starts the (empty) cache at initial_height rows; it grows by whole steps.
bool animation_frame_cache_gpu_configure_growth(AnimationFrameCacheGpu* gpu, u16 initial_height) noexcept;
void animation_frame_cache_gpu_observe_time(AnimationFrameCacheGpu* gpu, u64 now_ms) noexcept;
// Zero when nothing is resident; otherwise when the idle release may run.
u64 animation_frame_cache_gpu_trim_deadline(const AnimationFrameCacheGpu& gpu) noexcept;
// Releases the texture and every frame when idle past the deadline.
bool animation_frame_cache_gpu_trim(AnimationFrameCacheGpu* gpu, u64 now_ms) noexcept;
bool animation_frame_cache_gpu_reserve(AnimationFrameCacheGpu* gpu,
                                       u32 required_capacity) noexcept;
void animation_frame_cache_gpu_reset(AnimationFrameCacheGpu* gpu) noexcept;
void animation_frame_cache_gpu_release(AnimationFrameCacheGpu* gpu) noexcept;
void animation_frame_cache_gpu_begin_visibility(AnimationFrameCacheGpu* gpu) noexcept;
bool animation_frame_cache_gpu_capture(
    AnimationFrameCacheGpu* gpu, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAtlas* media_atlas,
    ID3D11Texture2D* media_texture,
    const AnimationFrameCacheDescriptor& descriptor,
    MediaAtlasHandle* presented_media = nullptr) noexcept;
bool animation_frame_cache_gpu_restore(
    AnimationFrameCacheGpu* gpu, MediaAtlas* media_atlas,
    const AnimationFrameCacheKey& key, AnimationFrameCacheResult* result,
    MediaAtlasHandle* presented_media = nullptr) noexcept;
bool animation_frame_cache_gpu_materialize(
    AnimationFrameCacheGpu* gpu, ID3D11DeviceContext1* context,
    MediaAtlas* media_atlas, ID3D11Texture2D* media_texture,
    MediaAssetKey key) noexcept;
bool animation_frame_cache_gpu_resolve(
    AnimationFrameCacheGpu* gpu, const MediaAtlasResult& media,
    GlyphAtlasAllocation* frame) noexcept;

static_assert(sizeof(AnimationFramePresentation) == 8);
static_assert(sizeof(AnimationFrameCacheGpu) < 8 * 1024);

} // namespace maicord
