#include "maicord/render/animation_frame_cache_gpu.hpp"

#include "maicord/sys/d3d11.hpp"

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
    *storage = AnimationFrameCacheGpuStorage{};
}

void unbind_media_resources(ID3D11DeviceContext1* context) noexcept {
    ID3D11ShaderResourceView* null_views[2]{};
    ID3D11UnorderedAccessView* null_uav = nullptr;
    context->PSSetShaderResources(0, 2, null_views);
    context->CSSetShaderResources(0, 2, null_views);
    context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
}

bool ensure_texture(AnimationFrameCacheGpu* gpu, ID3D11Device* device) noexcept {
    if (gpu->texture && gpu->texture_view) {
        return true;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = gpu->cache.allocations.width;
    description.Height = gpu->cache.allocations.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT result = device->CreateTexture2D(&description, nullptr, &gpu->texture);
    if (SUCCEEDED(result)) {
        result = device->CreateShaderResourceView(gpu->texture, nullptr, &gpu->texture_view);
    }
    if (FAILED(result)) {
        com_release(gpu->texture_view);
        com_release(gpu->texture);
        animation_frame_cache_reset(&gpu->cache);
        return false;
    }
    return true;
}

// Taller cache texture keeping rows [0, old_height); frame handles and
// coordinates are unchanged, so no presentation needs refreshing.
bool grow_texture(AnimationFrameCacheGpu* gpu, ID3D11Device* device, ID3D11DeviceContext1* context,
                  u16 old_height) noexcept {
    if (!gpu->texture) {
        return true;
    }
    D3D11_TEXTURE2D_DESC description{};
    gpu->texture->GetDesc(&description);
    description.Height = gpu->cache.allocations.height;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    HRESULT result = device->CreateTexture2D(&description, nullptr, &texture);
    if (SUCCEEDED(result)) {
        result = device->CreateShaderResourceView(texture, nullptr, &view);
    }
    if (FAILED(result)) {
        com_release(view);
        com_release(texture);
        return false;
    }
    unbind_media_resources(context);
    const D3D11_BOX box{0, 0, 0, description.Width, old_height, 1};
    context->CopySubresourceRegion(texture, 0, 0, 0, 0, gpu->texture, 0, &box);
    com_release(gpu->texture_view);
    com_release(gpu->texture);
    gpu->texture = texture;
    gpu->texture_view = view;
    return true;
}

// Allocation that keeps recently replayed frames: below capacity, a request
// that could only evict recent tiles grows the texture instead.
AnimationFrameCacheResult allocate_frame(AnimationFrameCacheGpu* gpu, ID3D11Device* device,
                                         ID3D11DeviceContext1* context,
                                         const AnimationFrameCacheDescriptor& descriptor) noexcept {
    GlyphAtlasAllocator& allocations = gpu->cache.allocations;
    for (u32 attempt = 0;; ++attempt) {
        const bool protect = gpu->growth.maximum_height != 0 && allocations.height < gpu->growth.maximum_height;
        allocations.protect_after = protect ? glyph_atlas_growth_protect_after(gpu->growth) : 0;
        AnimationFrameCacheResult result = animation_frame_cache_allocate(&gpu->cache, descriptor);
        allocations.protect_after = 0;
        if (result.ok() || !protect ||
            (result.error != AnimationFrameCacheError::no_capacity &&
             result.error != AnimationFrameCacheError::too_large) ||
            attempt != 0) {
            if (!result.ok() && protect && result.error == AnimationFrameCacheError::no_capacity)
                result = animation_frame_cache_allocate(&gpu->cache, descriptor);
            return result;
        }
        const u16 old_height = allocations.height;
        if (!glyph_atlas_growth_prepare(&gpu->growth, &allocations, descriptor.width, descriptor.height)) {
            return animation_frame_cache_allocate(&gpu->cache, descriptor);
        }
        if (!grow_texture(gpu, device, context, old_height)) {
            allocations.height = old_height;
            return result;
        }
    }
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

bool animation_frame_cache_gpu_initialize(AnimationFrameCacheGpu* gpu, u16 width, u16 height,
                                          u16 maximum_frame_extent) noexcept {
    if (!gpu || width < 3 || height < 3 || maximum_frame_extent == 0) {
        return false;
    }
    *gpu = AnimationFrameCacheGpu{};
    gpu->maximum_frame_extent = maximum_frame_extent;
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
    if (animation_frame_cache_initialize(
            &gpu->cache, width, height, 1,
            Slice<AnimationFrameCacheEntry>{
                stable_storage_data<AnimationFrameCacheEntry>(gpu->storage.entries),
                gpu->storage.entries.capacity},
            Slice<u32>{stable_storage_data<u32>(gpu->storage.buckets), gpu->storage.buckets.capacity},
            Slice<GlyphAtlasEntry>{stable_storage_data<GlyphAtlasEntry>(gpu->storage.allocations),
                                   gpu->storage.allocations.capacity},
            Slice<GlyphAtlasRow>{gpu->rows, kAnimationFrameCacheGpuRowCapacity})) {
        return true;
    }
    release_storage(&gpu->storage);
    *gpu = AnimationFrameCacheGpu{};
    return false;
}

bool animation_frame_cache_gpu_configure_growth(AnimationFrameCacheGpu* gpu, u16 initial_height) noexcept {
    return gpu && gpu->storage.entries.arena.base && !gpu->texture && gpu->cache.entry_count == 0 &&
           glyph_atlas_growth_configure(&gpu->growth, &gpu->cache.allocations, initial_height,
                                        kAnimationFrameCacheGpuGrowthStep, kAnimationFrameCacheGpuProtectMs);
}

void animation_frame_cache_gpu_observe_time(AnimationFrameCacheGpu* gpu, u64 now_ms) noexcept {
    if (gpu) {
        glyph_atlas_growth_observe(&gpu->growth, gpu->cache.allocations, now_ms);
    }
}

u64 animation_frame_cache_gpu_trim_deadline(const AnimationFrameCacheGpu& gpu) noexcept {
    return gpu.texture ? glyph_atlas_growth_trim_deadline(gpu.growth, kAnimationFrameCacheGpuIdleMs) : 0;
}

bool animation_frame_cache_gpu_trim(AnimationFrameCacheGpu* gpu, u64 now_ms) noexcept {
    const u64 deadline = gpu ? animation_frame_cache_gpu_trim_deadline(*gpu) : 0;
    if (!deadline || now_ms < deadline) {
        return false;
    }
    // Every replay refreshes the deadline, so nothing on screen is animating
    // from this cache: drop all frames and the texture, restart small.
    animation_frame_cache_gpu_reset(gpu);
    gpu->cache.allocations.height = gpu->growth.minimum_height;
    gpu->growth.trim_pending = false;
    ++gpu->growth.shrinks;
    return true;
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
    const bool cache_grown = animation_frame_cache_grow_storage(
        &gpu->cache,
        Slice<AnimationFrameCacheEntry>{stable_storage_data<AnimationFrameCacheEntry>(gpu->storage.entries),
                                        capacity},
        Slice<u32>{stable_storage_data<u32>(gpu->storage.buckets), capacity * 2u},
        Slice<GlyphAtlasEntry>{stable_storage_data<GlyphAtlasEntry>(gpu->storage.allocations), capacity});
    return cache_grown;
}

void animation_frame_cache_gpu_reset(AnimationFrameCacheGpu* gpu) noexcept {
    if (!gpu) {
        return;
    }
    com_release(gpu->texture_view);
    com_release(gpu->texture);
    animation_frame_cache_reset(&gpu->cache);
    AnimationFramePresentation* values = presentations(gpu);
    for (u32 index = 0; values && index < gpu->storage.presentations.capacity; ++index) {
        values[index] = AnimationFramePresentation{};
    }
}

void animation_frame_cache_gpu_release(AnimationFrameCacheGpu* gpu) noexcept {
    if (!gpu) {
        return;
    }
    com_release(gpu->texture_view);
    com_release(gpu->texture);
    release_storage(&gpu->storage);
    *gpu = AnimationFrameCacheGpu{};
}

void animation_frame_cache_gpu_begin_visibility(AnimationFrameCacheGpu* gpu) noexcept {
    if (gpu) {
        animation_frame_cache_begin_visibility(&gpu->cache);
    }
}

bool animation_frame_cache_gpu_capture(AnimationFrameCacheGpu* gpu, ID3D11Device* device,
                                       ID3D11DeviceContext1* context, MediaAtlas* media_atlas,
                                       ID3D11Texture2D* media_texture,
                                       const AnimationFrameCacheDescriptor& descriptor,
                                       MediaAtlasHandle* presented_media) noexcept {
    if (presented_media) {
        *presented_media = MediaAtlasHandle{};
    }
    if (!gpu || !device || !context || !media_atlas || !media_texture || descriptor.width == 0 ||
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
    const AnimationFrameCacheResult destination = allocate_frame(gpu, device, context, descriptor);
    if (!destination.ok()) {
        ++gpu->failures;
        return false;
    }
    glyph_atlas_growth_note_activity(&gpu->growth);
    if (!ensure_texture(gpu, device)) {
        ++gpu->failures;
        return false;
    }
    if (!destination.cached) {
        unbind_media_resources(context);
        // Copy the atlas's padding texel, when it has one, so linear filtering
        // never samples a neighbouring animation tile.
        const UINT padding = media_atlas->allocations.padding != 0 ? 1u : 0u;
        const D3D11_BOX source_box{static_cast<UINT>(source.x - padding),
                                   static_cast<UINT>(source.y - padding),
                                   0,
                                   static_cast<UINT>(source.x + source.width + padding),
                                   static_cast<UINT>(source.y + source.height + padding),
                                   1};
        context->CopySubresourceRegion(gpu->texture, 0, destination.allocation.x - padding,
                                       destination.allocation.y - padding, 0, media_texture, 0,
                                       &source_box);
        ++gpu->captures;
        const u64 pixels = static_cast<u64>(source.width) * source.height;
        saturating_accumulate(gpu->copied_pixels, pixels);
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
                                       AnimationFrameCacheKey key, AnimationFrameCacheResult* result,
                                       MediaAtlasHandle* presented_media) noexcept {
    if (result) {
        *result = AnimationFrameCacheResult{};
    }
    if (presented_media) {
        *presented_media = MediaAtlasHandle{};
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
    glyph_atlas_growth_note_activity(&gpu->growth);
    *result = source;
    if (presented_media) {
        *presented_media = destination.handle;
    }
    return true;
}

bool animation_frame_cache_gpu_resolve(AnimationFrameCacheGpu* gpu, const MediaAtlasResult& media,
                                       GlyphAtlasAllocation* frame) noexcept {
    if (frame) {
        *frame = GlyphAtlasAllocation{};
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
        presentation = AnimationFramePresentation{};
        return false;
    }
    return true;
}

bool animation_frame_cache_gpu_materialize(AnimationFrameCacheGpu* gpu, ID3D11DeviceContext1* context,
                                           MediaAtlas* media_atlas, ID3D11Texture2D* media_texture,
                                           MediaAssetKey key) noexcept {
    if (!gpu || !context || !media_atlas || !media_texture) {
        return false;
    }
    if (!gpu->texture) {
        return true;
    }
    const MediaAtlasResult destination = media_atlas_find(media_atlas, key);
    if (!destination.ok() || destination.handle.slot >= gpu->storage.presentations.capacity) {
        ++gpu->failures;
        return false;
    }
    AnimationFramePresentation& presentation = presentations(gpu)[destination.handle.slot];
    if (presentation.media_generation != destination.handle.generation) {
        return true;
    }
    if (presentation.materialized) {
        return true;
    }
    GlyphAtlasAllocation source{};
    if (!animation_frame_cache_gpu_resolve(gpu, destination, &source) ||
        source.width != destination.width || source.height != destination.height) {
        ++gpu->failures;
        return false;
    }
    unbind_media_resources(context);
    const UINT padding = media_atlas->allocations.padding != 0 ? 1u : 0u;
    const D3D11_BOX source_box{static_cast<UINT>(source.x - padding),
                               static_cast<UINT>(source.y - padding),
                               0,
                               static_cast<UINT>(source.x + source.width + padding),
                               static_cast<UINT>(source.y + source.height + padding),
                               1};
    context->CopySubresourceRegion(media_texture, 0, destination.x - padding, destination.y - padding, 0, gpu->texture, 0,
                                   &source_box);
    presentation.materialized = true;
    ++gpu->materializations;
    const u64 pixels = static_cast<u64>(source.width) * source.height;
    saturating_accumulate(gpu->copied_pixels, pixels);
    return true;
}

} // namespace maicord
