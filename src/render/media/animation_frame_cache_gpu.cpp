#include "maicord/render/media/animation_frame_cache_gpu.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/gpu/api.hpp"
#include "maicord/render/media/atlas.hpp"

namespace maicord {
namespace {

AnimationFramePresentation* presentations(AnimationFrameCacheGpu* gpu) noexcept {
    return stable_storage_data<AnimationFramePresentation>(gpu->storage.presentations);
}

void release_storage(AnimationFrameCacheGpuStorage* storage) noexcept {
    stable_storage_release(&storage->entries);
    stable_storage_release(&storage->buckets);
    stable_storage_release(&storage->allocations);
    stable_storage_release(&storage->presentations);
    reset_value<AnimationFrameCacheGpuStorage>(storage);
}

void unbind_media_resources(GpuContext1* context) noexcept {
    GpuSrv* null_views[2]{};
    GpuUav* null_uav = nullptr;
    gpu_set_srvs(context, GpuStage::pixel, 0, 2, null_views);
    gpu_set_srvs(context, GpuStage::compute, 0, 2, null_views);
    gpu_set_uavs(context, 0, 1, &null_uav);
}

// Copies a tile with its padding texel between two paged surfaces, each in its own page's rows.
void copy_tile(GpuContext1* context, GpuPagedTexture* to, u32 to_x, u32 to_y, GpuPagedTexture* from,
               u32 x, u32 y, u32 width, u32 height, u32 padding) noexcept {
    unbind_media_resources(context);
    const GpuPage destination = gpu_paged_texture_page(to, to_y);
    const GpuPage source = gpu_paged_texture_page(from, y);
    if (!destination.texture || !source.texture) {
        return;
    }
    y -= source.first_row;
    const GpuBox box{x - padding, y - padding, x + width + padding, y + height + padding};
    gpu_copy_region(context, destination.texture, to_x - padding, to_y - destination.first_row - padding,
                    source.texture, box);
}

// The surface is made on the first frame and backs the rows the cache allocated.
bool sync_surface(AnimationFrameCacheGpu* gpu, GpuDevice* device, GpuContext1* context) noexcept {
    const GlyphAtlasAllocator& pages = gpu->cache.allocations;
    if (!gpu->surface) {
        if (FAILED(gpu_paged_texture_create(
                device, {pages.width, pages.page_rows, pages.page_count, GpuFormat::rgba8, gpu_bind_srv},
                &gpu->surface))) {
            animation_frame_cache_reset(&gpu->cache);
            return false;
        }
        gpu->texture_view = gpu_paged_texture_view(gpu->surface);
    }
    bool ok = true;
    for (u32 page = 0; page < pages.page_count; ++page) {
        ok = gpu_ok(gpu_paged_texture_commit(context, gpu->surface, page, pages.pages[page].committed)) && ok;
    }
    return ok;
}

bool publish_presentation(AnimationFrameCacheGpu* gpu, const MediaAtlasResult& media,
                          GlyphAtlasHandle frame, bool materialized) noexcept {
    if (media.handle.slot >= kAnimationFrameCacheGpuEntryCapacity || media.handle.generation == 0 ||
        frame.generation == 0) {
        return false;
    }
    if (!animation_frame_cache_gpu_reserve(gpu, static_cast<u32>(media.handle.slot) + 1u)) {
        return false;
    }
    presentations(gpu)[media.handle.slot] =
        AnimationFramePresentation{frame, media.handle.generation, materialized, 0};
    return true;
}

} // namespace

bool animation_frame_cache_gpu_initialize(AnimationFrameCacheGpu* gpu, bool preview) noexcept {
    if (!gpu) {
        return false;
    }
    reset_value<AnimationFrameCacheGpu>(gpu);
    gpu->maximum_frame_extent = preview ? kAnimationFrameCacheGpuPreviewFrameExtent
                                        : kAnimationFrameCacheGpuMaximumFrameExtent;
    if (!stable_storage_initialize(&gpu->storage.entries, kAnimationFrameCacheGpuEntryCapacity,
                                   kAnimationFrameCacheGpuInitialEntryCapacity,
                                   sizeof(AnimationFrameCacheEntry), alignof(AnimationFrameCacheEntry),
                                   true) ||
        !stable_storage_initialize(&gpu->storage.buckets, kAnimationFrameCacheGpuBucketCapacity,
                                   kAnimationFrameCacheGpuInitialEntryCapacity * 2u, sizeof(u32),
                                   alignof(u32), true) ||
        !stable_storage_initialize(&gpu->storage.allocations, kAnimationFrameCacheGpuEntryCapacity,
                                   kAnimationFrameCacheGpuInitialEntryCapacity, sizeof(GlyphAtlasEntry),
                                   alignof(GlyphAtlasEntry), true) ||
        !stable_storage_initialize(&gpu->storage.presentations, kAnimationFrameCacheGpuEntryCapacity,
                                   kAnimationFrameCacheGpuInitialEntryCapacity,
                                   sizeof(AnimationFramePresentation), alignof(AnimationFramePresentation),
                                   true)) {
        release_storage(&gpu->storage);
        return false;
    }
    const u16 commit_rows = gpu_paged_commit_rows(4);
    const GlyphAtlasPaging paging{kAnimationFrameCacheGpuPageRows, static_cast<u8>(preview ? 4 : 2), commit_rows == 0,
                                  commit_rows, kAnimationFrameCacheGpuPageRows, kAnimationFrameCacheGpuProtectMs,
                                  false, {}};
    if (animation_frame_cache_initialize(
            &gpu->cache, kAnimationFrameCacheGpuExtent, kAnimationFrameCacheGpuPageRows, 1,
            Slice<AnimationFrameCacheEntry>{
                stable_storage_data<AnimationFrameCacheEntry>(gpu->storage.entries),
                gpu->storage.entries.capacity},
            Slice<u32>{stable_storage_data<u32>(gpu->storage.buckets), gpu->storage.buckets.capacity},
            Slice<GlyphAtlasEntry>{stable_storage_data<GlyphAtlasEntry>(gpu->storage.allocations),
                                   gpu->storage.allocations.capacity},
            Slice<GlyphAtlasRow>{gpu->rows, kAnimationFrameCacheGpuRowCapacity}) &&
        glyph_atlas_configure_paging(&gpu->cache.allocations, paging)) {
        return true;
    }
    release_storage(&gpu->storage);
    reset_value<AnimationFrameCacheGpu>(gpu);
    return false;
}

void animation_frame_cache_gpu_observe_time(AnimationFrameCacheGpu* gpu, u64 now_ms) noexcept {
    if (gpu) {
        glyph_atlas_observe_time(&gpu->cache.allocations, now_ms);
    }
}

u64 animation_frame_cache_gpu_trim_deadline(const AnimationFrameCacheGpu& gpu) noexcept {
    return gpu.surface ? glyph_atlas_trim_deadline(gpu.cache.allocations, kAnimationFrameCacheGpuIdleMs) : 0;
}

bool animation_frame_cache_gpu_trim(AnimationFrameCacheGpu* gpu, GpuContext1* context, u64 now_ms) noexcept {
    const u64 deadline = gpu ? animation_frame_cache_gpu_trim_deadline(*gpu) : 0;
    if (!deadline || now_ms < deadline) {
        return false;
    }
    const u32 evicted = animation_frame_cache_trim(&gpu->cache);
    for (u32 page = 0; page < gpu->cache.allocations.page_count; ++page) {
        gpu_paged_texture_commit(context, gpu->surface, page, gpu->cache.allocations.pages[page].committed);
    }
    return evicted != 0;
}

bool animation_frame_cache_gpu_reserve(AnimationFrameCacheGpu* gpu, u32 required_capacity) noexcept {
    if (!gpu || !gpu->storage.entries.arena.base || required_capacity == 0 ||
        required_capacity > kAnimationFrameCacheGpuEntryCapacity) {
        return false;
    }
    if (required_capacity <= gpu->storage.entries.capacity) {
        return true;
    }
    u32 capacity = gpu->storage.entries.capacity;
    while (capacity < required_capacity) {
        capacity = minimum(kAnimationFrameCacheGpuEntryCapacity, maximum(capacity * 2u, required_capacity));
    }

    const u32 previous_capacity = gpu->storage.entries.capacity;
    const ArenaMark entries_mark = arena_mark(&gpu->storage.entries.arena);
    const ArenaMark buckets_mark = arena_mark(&gpu->storage.buckets.arena);
    const ArenaMark allocations_mark = arena_mark(&gpu->storage.allocations.arena);
    const ArenaMark presentations_mark = arena_mark(&gpu->storage.presentations.arena);
    if (!stable_storage_reserve(&gpu->storage.entries, capacity) ||
        !stable_storage_reserve(&gpu->storage.buckets, capacity * 2u) ||
        !stable_storage_reserve(&gpu->storage.allocations, capacity) ||
        !stable_storage_reserve(&gpu->storage.presentations, capacity)) {
        arena_rewind(&gpu->storage.entries.arena, entries_mark);
        arena_rewind(&gpu->storage.buckets.arena, buckets_mark);
        arena_rewind(&gpu->storage.allocations.arena, allocations_mark);
        arena_rewind(&gpu->storage.presentations.arena, presentations_mark);
        gpu->storage.entries.capacity = previous_capacity;
        gpu->storage.buckets.capacity = previous_capacity * 2u;
        gpu->storage.allocations.capacity = previous_capacity;
        gpu->storage.presentations.capacity = previous_capacity;
        return false;
    }
    return animation_frame_cache_grow_storage(
        &gpu->cache,
        Slice<AnimationFrameCacheEntry>{stable_storage_data<AnimationFrameCacheEntry>(gpu->storage.entries),
                                        capacity},
        Slice<u32>{stable_storage_data<u32>(gpu->storage.buckets), capacity * 2u},
        Slice<GlyphAtlasEntry>{stable_storage_data<GlyphAtlasEntry>(gpu->storage.allocations), capacity});
}

void animation_frame_cache_gpu_reset(AnimationFrameCacheGpu* gpu) noexcept {
    if (!gpu) {
        return;
    }
    gpu_paged_texture_release(&gpu->surface);
    gpu->texture_view = nullptr;
    animation_frame_cache_reset(&gpu->cache);
    AnimationFramePresentation* values = presentations(gpu);
    for (u32 index = 0; values && index < gpu->storage.presentations.capacity; ++index) {
        reset_value<AnimationFramePresentation>(&values[index]);
    }
}

void animation_frame_cache_gpu_release(AnimationFrameCacheGpu* gpu) noexcept {
    if (!gpu) {
        return;
    }
    gpu_paged_texture_release(&gpu->surface);
    release_storage(&gpu->storage);
    reset_value<AnimationFrameCacheGpu>(gpu);
}

void animation_frame_cache_gpu_begin_visibility(AnimationFrameCacheGpu* gpu) noexcept {
    if (gpu) {
        animation_frame_cache_begin_visibility(&gpu->cache);
    }
}

bool animation_frame_cache_gpu_capture(AnimationFrameCacheGpu* gpu, GpuDevice* device,
                                       GpuContext1* context, MediaAtlas* media_atlas,
                                       GpuPagedTexture* media_surface,
                                       const AnimationFrameCacheDescriptor& descriptor,
                                       MediaAtlasHandle* presented_media) noexcept {
    if (presented_media) {
        reset_value<MediaAtlasHandle>(presented_media);
    }
    if (!gpu || !device || !context || !media_atlas || !media_surface || descriptor.width == 0 ||
        descriptor.height == 0 || descriptor.width > gpu->maximum_frame_extent ||
        descriptor.height > gpu->maximum_frame_extent) {
        return false;
    }
    const MediaAtlasResult source = media_atlas_find(media_atlas, descriptor.key.asset);
    if (!source.ok() || source.width != descriptor.width || source.height != descriptor.height) {
        ++gpu->failures;
        return false;
    }
    u32 required_capacity = static_cast<u32>(source.handle.slot) + 1u;
    if (gpu->cache.entry_count >= gpu->cache.entry_capacity &&
        gpu->cache.entry_capacity < kAnimationFrameCacheGpuEntryCapacity) {
        required_capacity = maximum(required_capacity, gpu->cache.entry_capacity + 1u);
    }
    if (required_capacity > gpu->storage.entries.capacity &&
        !animation_frame_cache_gpu_reserve(gpu, required_capacity)) {
        ++gpu->failures;
        return false;
    }
    const AnimationFrameCacheResult destination = animation_frame_cache_allocate(&gpu->cache, descriptor);
    if (!destination.ok() || !sync_surface(gpu, device, context)) {
        ++gpu->failures;
        return false;
    }
    if (!destination.cached) {
        // Includes the atlas's padding texel, so linear filtering never samples a neighbouring tile.
        copy_tile(context, gpu->surface, destination.allocation.x, destination.allocation.y, media_surface,
                  source.x, source.y, source.width, source.height,
                  media_atlas->allocations.padding != 0 ? 1u : 0u);
        ++gpu->captures;
        saturating_accumulate(gpu->copied_pixels, u64{source.width} * source.height);
    }
    if (!publish_presentation(gpu, source, destination.allocation.handle, true)) {
        ++gpu->failures;
        return false;
    }
    if (presented_media) {
        *presented_media = source.handle;
    }
    return true;
}

bool animation_frame_cache_gpu_restore(AnimationFrameCacheGpu* gpu, MediaAtlas* media_atlas,
                                       const AnimationFrameCacheKey& key, AnimationFrameCacheResult* result,
                                       MediaAtlasHandle* presented_media) noexcept {
    if (result) {
        reset_value<AnimationFrameCacheResult>(result);
    }
    if (presented_media) {
        reset_value<MediaAtlasHandle>(presented_media);
    }
    if (!gpu || !media_atlas || !gpu->texture_view || !result) {
        return false;
    }
    const AnimationFrameCacheResult source = animation_frame_cache_find(&gpu->cache, key);
    if (!source.ok()) {
        return false;
    }
    const MediaAtlasResult destination = media_atlas_find(media_atlas, key.asset);
    if (!destination.ok() || destination.width != source.allocation.width ||
        destination.height != source.allocation.height ||
        !publish_presentation(gpu, destination, source.allocation.handle, false)) {
        ++gpu->failures;
        return false;
    }
    ++gpu->restores;
    *result = source;
    if (presented_media) {
        *presented_media = destination.handle;
    }
    return true;
}

bool animation_frame_cache_gpu_resolve(AnimationFrameCacheGpu* gpu, const MediaAtlasResult& media,
                                       GlyphAtlasAllocation* frame) noexcept {
    if (frame) {
        reset_value<GlyphAtlasAllocation>(frame);
    }
    if (!gpu || !frame || media.handle.generation == 0 ||
        media.handle.slot >= gpu->storage.presentations.capacity || !gpu->texture_view) {
        return false;
    }
    AnimationFramePresentation& presentation = presentations(gpu)[media.handle.slot];
    if (presentation.frame.generation == 0) {
        return false;
    }
    if (presentation.media_generation != media.handle.generation ||
        !glyph_atlas_validate(gpu->cache.allocations, presentation.frame, frame) ||
        !glyph_atlas_pin(&gpu->cache.allocations, presentation.frame)) {
        reset_value<AnimationFramePresentation>(&presentation);
        return false;
    }
    return true;
}

bool animation_frame_cache_gpu_materialize(AnimationFrameCacheGpu* gpu, GpuContext1* context,
                                           MediaAtlas* media_atlas, GpuPagedTexture* media_surface,
                                           MediaAssetKey key) noexcept {
    if (!gpu || !context || !media_atlas || !media_surface) {
        return false;
    }
    if (!gpu->surface) {
        return true;
    }
    // Materializing shows nothing: neither the entry nor its frame is pinned.
    const MediaAtlasResult destination = media_atlas_peek(media_atlas, key);
    if (!destination.ok() || destination.handle.slot >= gpu->storage.presentations.capacity) {
        ++gpu->failures;
        return false;
    }
    AnimationFramePresentation& presentation = presentations(gpu)[destination.handle.slot];
    if (presentation.media_generation != destination.handle.generation || presentation.materialized) {
        return true;
    }
    GlyphAtlasAllocation source{};
    if (!glyph_atlas_validate(gpu->cache.allocations, presentation.frame, &source) ||
        source.width != destination.width || source.height != destination.height) {
        reset_value<AnimationFramePresentation>(&presentation);
        ++gpu->failures;
        return false;
    }
    // Includes the atlas's padding texel, so linear filtering never samples a neighbouring tile.
    copy_tile(context, media_surface, destination.x, destination.y, gpu->surface, source.x, source.y,
              source.width, source.height, media_atlas->allocations.padding != 0 ? 1u : 0u);
    presentation.materialized = true;
    saturating_accumulate(gpu->copied_pixels, u64{source.width} * source.height);
    return true;
}

} // namespace maicord
