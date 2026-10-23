#include "maicord/render/d3d11_renderer.hpp"

#include "maicord/core/log.hpp"
#include "maicord/render/colr_program.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/media/jpeg.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {

void log_media_upload_failure(const MediaAtlasGpuResult& result, const char* event) noexcept {
    // Capacity pressure is recoverable and rate-limited by the media owner;
    // logging it here would repeat on every render wake.
    if (result.error == MediaAtlasGpuError::atlas_failure &&
        result.allocation.error == MediaAtlasError::no_capacity) return;
    log_write(LogLevel::error, "render", event);
}

void note_media_mutation(D3D11Renderer* renderer, const MediaAtlasGpuResult& result,
                         u8 domain = 0) noexcept {
    if (renderer && result.ok() &&
        !presentation_damage_note_media(&renderer->presentation_damage, result.allocation.handle, domain)) {
        presentation_damage_invalidate_full(&renderer->presentation_damage);
        log_write(LogLevel::warning, "render", "media_damage_handle_out_of_range");
    }
}

void note_media_mutation(D3D11Renderer* renderer, MediaAtlasHandle handle, u8 domain = 0) noexcept {
    if (renderer && !presentation_damage_note_media(&renderer->presentation_damage, handle, domain)) {
        presentation_damage_invalidate_full(&renderer->presentation_damage);
        log_write(LogLevel::warning, "render", "media_damage_handle_out_of_range");
    }
}

MediaAtlasGpuResult reject_media_request(D3D11Renderer* renderer) noexcept {
    if (renderer) {
        ++renderer->media_rejected;
    }
    return MediaAtlasGpuResult{{}, E_INVALIDARG, MediaAtlasGpuError::invalid_input};
}

bool media_request_ready(const D3D11Renderer* renderer, MediaAssetKey key) noexcept {
    return renderer && renderer->graphics.device && renderer->graphics.context1 && renderer->media_atlas &&
           media_asset_key_valid(key);
}

// Large previews below the original tier use the preview atlas once it is bound.
bool uses_preview_atlas(const D3D11Renderer* renderer, MediaAssetKey key) noexcept {
    return media_asset_large_preview(key) && media_asset_resolution_tier(key) != MediaResolutionTier::original &&
           renderer && renderer->preview_media_atlas;
}

MediaAtlas* upload_atlas(D3D11Renderer* renderer, bool preview) noexcept {
    return preview ? renderer->preview_media_atlas : renderer ? renderer->media_atlas : nullptr;
}

MediaAtlasGpuResult ensure_primary_media_gpu(D3D11Renderer* renderer) noexcept {
    if (renderer->media_gpu.texture) {
        return MediaAtlasGpuResult{{}, S_OK, MediaAtlasGpuError::none};
    }
    const MediaAtlasGpuResult initialized = media_atlas_gpu_initialize(
        &renderer->media_gpu, renderer->graphics.device, renderer->graphics.context1,
        renderer->media_atlas->allocations.width, renderer->media_atlas->allocations.height);
    if (!initialized.ok()) {
        ++renderer->media_rejected;
        log_write(LogLevel::error, "render", "media_atlas_gpu_initialization_failed");
    }
    return initialized;
}

MediaAtlasGpuResult ensure_preview_media_gpu(D3D11Renderer* renderer, const MediaAtlas& atlas) noexcept {
    if (renderer->media_gpu.preview_texture) {
        return MediaAtlasGpuResult{{}, S_OK, MediaAtlasGpuError::none};
    }
    // A preview can finish before any primary media, so the primary surface
    // is created first.
    const MediaAtlasGpuResult primary = ensure_primary_media_gpu(renderer);
    if (!primary.ok()) return primary;
    const MediaAtlasGpuResult initialized = media_atlas_gpu_initialize_preview(
        &renderer->media_gpu, renderer->graphics.device, renderer->graphics.context1,
        atlas.allocations.width, atlas.allocations.height);
    if (!initialized.ok()) {
        ++renderer->media_rejected;
        log_write(LogLevel::error, "render", "preview_media_atlas_gpu_initialization_failed");
    }
    return initialized;
}

MediaAtlasGpuResult ensure_upload_preview_gpu(D3D11Renderer* renderer, bool preview, const MediaAtlas* atlas) noexcept {
    return preview ? ensure_preview_media_gpu(renderer, *atlas) : MediaAtlasGpuResult{{}, S_OK, MediaAtlasGpuError::none};
}

// Growable atlases: make room for one request before any pixel is written.
// The CPU allocator grows first; the matching GPU surface is recreated with
// its existing rows copied. A failed resize reverts the allocator, so the
// upload fails like any other capacity miss.
MediaAtlasGpuResult prepare_media_surface(D3D11Renderer* renderer, MediaAtlas* atlas, bool preview,
                                          MediaAssetKey key, u32 width, u32 height) noexcept {
    const MediaAtlasGpuResult ready{{}, S_OK, MediaAtlasGpuError::none};
    if (!atlas || width == 0 || height == 0 || width > 0xFFFFu || height > 0xFFFFu)
        return ready;
    const u16 before = atlas->allocations.height;
    if (!media_atlas_prepare(atlas, key, static_cast<u16>(width), static_cast<u16>(height)))
        return ready;
    const MediaAtlasGpuResult resized = media_atlas_gpu_resize(&renderer->media_gpu, renderer->graphics.device,
                                                               renderer->graphics.context1, preview,
                                                               atlas->allocations.height);
    if (!resized.ok()) {
        media_atlas_revert_growth(atlas, before);
        ++renderer->media_rejected;
        log_write(LogLevel::warning, "render", "media_atlas_growth_failed");
        return resized;
    }
    ++renderer->media_atlas_resizes;
    return ready;
}

// Codec shaders/buffers are shared, but each atlas owns its canvas and
// disposal backup. Borrow the chosen surface for one synchronous render-thread
// operation. This swaps metadata only: no texture copies, allocation or AddRef.
class AnimationAtlasTarget {
    D3D11Renderer* renderer_;
    template<class T> static void exchange(T& a, T& b) noexcept {
        const T temporary = a;
        a = b;
        b = temporary;
    }
    void exchange_surface() noexcept {
        auto& gpu = renderer_->media_gpu;
        exchange(gpu.texture, gpu.preview_texture);
        exchange(gpu.texture_view, gpu.preview_texture_view);
        exchange(gpu.texture_uav, gpu.preview_texture_uav);
        exchange(gpu.target, gpu.preview_target);
        exchange(gpu.width, gpu.preview_width);
        exchange(gpu.height, gpu.preview_height);
        exchange(gpu.apng_backup_texture, gpu.preview_animation_backup_texture);
    }
public:
    bool preview;
    MediaAtlas* atlas;
    MediaAtlasGpuResult initialized;
    // A nonzero extent (new animations) makes room before the slots swap, so
    // a resize always targets the surface slot that owns this atlas.
    AnimationAtlasTarget(D3D11Renderer* renderer, MediaAssetKey key, u32 width = 0, u32 height = 0) noexcept
        : renderer_(renderer) {
        preview = media_asset_large_preview(key) && renderer->preview_media_atlas;
        atlas = preview ? renderer->preview_media_atlas : renderer->media_atlas;
        initialized = preview ? ensure_preview_media_gpu(renderer, *atlas) : ensure_primary_media_gpu(renderer);
        if (initialized.ok() && width && height) {
            const MediaAtlasGpuResult prepared = prepare_media_surface(renderer, atlas, preview, key, width, height);
            if (!prepared.ok()) {
                initialized = prepared;
                return;
            }
        }
        if (preview && initialized.ok()) exchange_surface();
    }
    ~AnimationAtlasTarget() noexcept {
        if (preview && initialized.ok()) exchange_surface();
    }
    AnimationAtlasTarget(const AnimationAtlasTarget&) = delete;
    AnimationAtlasTarget& operator=(const AnimationAtlasTarget&) = delete;
};

// Shared tail of the still-image uploads: both surfaces up, room made, the
// upload run on the chosen atlas, failure and damage accounted.
template <class Upload>
MediaAtlasGpuResult upload_to_atlas(D3D11Renderer* renderer, MediaAssetKey key, bool preview, MediaAtlas* atlas,
                                    u32 width, u32 height, const char* failure_event, Upload upload) noexcept {
    const MediaAtlasGpuResult initialized = ensure_primary_media_gpu(renderer);
    if (!initialized.ok()) return initialized;
    const MediaAtlasGpuResult preview_initialized = ensure_upload_preview_gpu(renderer, preview, atlas);
    if (!preview_initialized.ok()) return preview_initialized;
    const MediaAtlasGpuResult prepared = prepare_media_surface(renderer, atlas, preview, key, width, height);
    if (!prepared.ok()) return prepared;
    const MediaAtlasGpuResult uploaded = upload();
    if (!uploaded.ok()) {
        ++renderer->media_rejected;
        log_media_upload_failure(uploaded, failure_event);
    } else if (preview) {
        ++renderer->preview_media_uploads;
        saturating_accumulate(renderer->preview_media_uploaded_pixels, static_cast<u64>(width) * height);
    }
    note_media_mutation(renderer, uploaded, preview ? 1 : 0);
    return uploaded;
}

// Animation entry points: the surface owning `key` is borrowed for `upload`.
template <class Upload>
MediaAtlasGpuResult upload_animation(D3D11Renderer* renderer, MediaAssetKey key, u32 canvas_width,
                                     u32 canvas_height, const char* failure_event, Upload upload) noexcept {
    AnimationAtlasTarget target(renderer, key, canvas_width, canvas_height);
    if (!target.initialized.ok()) return target.initialized;
    const MediaAtlasGpuResult uploaded = upload(target.atlas);
    if (!uploaded.ok()) {
        ++renderer->media_rejected;
        log_media_upload_failure(uploaded, failure_event);
    }
    note_media_mutation(renderer, uploaded, target.preview ? 1 : 0);
    return uploaded;
}

} // namespace

MediaAtlasGpuResult renderer_present_video_frame(D3D11Renderer* renderer,
    MediaAssetKey key, const GpuVideoFrame& frame, u32 maximum_extent) noexcept {
    if (!renderer || !renderer->graphics.device || !renderer->graphics.context1 ||
        !renderer->media_atlas || !media_asset_key_valid(key)) return reject_media_request(renderer);
    // Call video lives in the large preview atlas, reduced to its tile.
    const bool call = key.kind == MediaAssetKind::call_video;
    if (call && !renderer->preview_media_atlas) return reject_media_request(renderer);
    const bool preview = call || uses_preview_atlas(renderer, key);
    MediaAtlas* atlas = preview ? renderer->preview_media_atlas : renderer->media_atlas;
    const MediaAtlasGpuResult primary = ensure_primary_media_gpu(renderer);
    if (!primary.ok()) return primary;
    if (preview) {
        const MediaAtlasGpuResult ready = ensure_preview_media_gpu(renderer, *atlas);
        if (!ready.ok()) return ready;
    }
    // Local draft video keys are composer posters: a thumbnail, not playback.
    // A GIF picker tile (a VIDEO favourite) plays at the same small extent.
    const u32 extent = key.kind == MediaAssetKind::local_draft_preview ||
                               key.kind == MediaAssetKind::catalog_preview
                           ? kGpuVideoPosterExtent
                       : call ? maximum_extent
                              : 0;
    if (gpu_video_frame_visible_valid(frame.width, frame.height)) {
        const u32 step = gpu_video_frame_reduction(frame.width, frame.height, extent);
        const u32 width = gpu_video_frame_output_width(frame, step);
        const u32 height = gpu_video_frame_output_height(frame, step);
        // A tile resize or new stream resolution changes the extent under the
        // same key; retire the old allocation, which cannot hold the new size.
        const MediaAtlasResult resident = media_atlas_peek(atlas, key);
        if (resident.ok() && (resident.width != width || resident.height != height))
            media_atlas_forget(atlas, key);
        const MediaAtlasGpuResult prepared = prepare_media_surface(renderer, atlas, preview, key, width, height);
        if (!prepared.ok()) return prepared;
    }
    const MediaAtlasGpuResult result = media_atlas_gpu_present_video(&renderer->media_gpu, atlas,
        renderer->graphics.device, renderer->graphics.context1, key, frame, preview, extent);
    note_media_mutation(renderer, result, preview ? 1 : 0);
    return result;
}

bool renderer_bind_media_atlas(D3D11Renderer* renderer, MediaAtlas* atlas) noexcept {
    if (!renderer || !atlas || renderer->media_atlas || !atlas->entries || !atlas->allocations.entries ||
        atlas->allocations.width < 16 || atlas->allocations.height < 16 ||
        atlas->allocations.width > 4'096 || atlas->allocations.height > 4'096) {
        return false;
    }
    renderer->media_atlas = atlas;
    presentation_damage_reset(&renderer->presentation_damage);
    presentation_damage_invalidate_full(&renderer->presentation_damage);
    return true;
}

bool renderer_bind_preview_media_atlas(D3D11Renderer* renderer, MediaAtlas* atlas) noexcept {
    if (!renderer || !atlas || !renderer->media_atlas || renderer->preview_media_atlas || !atlas->entries ||
        !atlas->allocations.entries || atlas->allocations.width < 16 || atlas->allocations.height < 16 ||
        atlas->allocations.width > 4'096 || atlas->allocations.height > 4'096) {
        return false;
    }
    renderer->preview_media_atlas = atlas;
    presentation_damage_reset(&renderer->presentation_damage);
    presentation_damage_invalidate_full(&renderer->presentation_damage);
    return true;
}

void renderer_unbind_media_atlas(D3D11Renderer* renderer) noexcept {
    if (!renderer) {
        return;
    }
    media_atlas_gpu_release(&renderer->media_gpu);
    animation_frame_cache_gpu_reset(&renderer->animation_gpu);
    animation_frame_cache_gpu_reset(&renderer->preview_animation_gpu);
    // Surfaces are released, so growable atlases restart at their initial height.
    if (renderer->animation_gpu.growth.maximum_height)
        renderer->animation_gpu.cache.allocations.height = renderer->animation_gpu.growth.minimum_height;
    if (renderer->media_atlas) {
        media_atlas_reset(renderer->media_atlas);
        media_atlas_reset_growth(renderer->media_atlas);
    }
    if (renderer->preview_media_atlas) {
        media_atlas_reset(renderer->preview_media_atlas);
        media_atlas_reset_growth(renderer->preview_media_atlas);
    }
    renderer->media_atlas = nullptr;
    renderer->preview_media_atlas = nullptr;
    renderer->media_quad_count = 0;
    renderer->media_batch_count = 0;
    renderer->popup_media_first_batch = 0;
    renderer->media_draws_last_frame = 0;
    presentation_damage_reset(&renderer->presentation_damage);
}

bool renderer_capture_animation_frame(D3D11Renderer* renderer,
                                      const AnimationFrameCacheDescriptor& descriptor) noexcept {
    if (renderer && media_asset_large_preview(descriptor.key.asset)) {
        auto& cache = renderer->preview_animation_gpu;
        // Lazy preview cache sized to the viewport (8..32 MiB), starting at 256
        // rows and grown only for recently replayed frames. Identity slots are
        // a separate namespace; identical encoded animations share frame tiles.
        if (!cache.storage.entries.arena.base &&
            (!animation_frame_cache_gpu_initialize(
                 &cache, 4096, animation_frame_cache_gpu_preview_height(renderer->width, renderer->height), 512) ||
             !animation_frame_cache_gpu_configure_growth(&cache, kAnimationFrameCacheGpuGrowthStep))) {
            animation_frame_cache_gpu_release(&cache);
            return false;
        }
        animation_frame_cache_gpu_begin_visibility(&cache);
        return animation_frame_cache_gpu_capture(&cache, renderer->graphics.device,
            renderer->graphics.context1, renderer->preview_media_atlas,
            renderer->media_gpu.preview_texture, descriptor);
    }
    MediaAtlasHandle presented_media{};
    const bool captured =
        renderer && animation_frame_cache_gpu_capture(
                        &renderer->animation_gpu, renderer->graphics.device, renderer->graphics.context1,
                        renderer->media_atlas, renderer->media_gpu.texture, descriptor, &presented_media);
    if (captured) {
        note_media_mutation(renderer, presented_media);
    }
    return captured;
}

bool renderer_restore_animation_frame(D3D11Renderer* renderer, const AnimationFrameCacheKey& key,
                                      AnimationFrameCacheResult* result) noexcept {
    if (renderer && media_asset_large_preview(key.asset)) {
        auto& cache = renderer->preview_animation_gpu;
        animation_frame_cache_gpu_begin_visibility(&cache);
        MediaAtlasHandle presented{};
        if (!animation_frame_cache_gpu_restore(&cache, renderer->preview_media_atlas,
                key, result, &presented) ||
            !animation_frame_cache_gpu_materialize(&cache, renderer->graphics.context1,
                renderer->preview_media_atlas, renderer->media_gpu.preview_texture, key.asset)) return false;
        // Preview drawing keeps its live-atlas UVs. This is a GPU-only copy and
        // cannot alias identity slots.
        note_media_mutation(renderer, presented, 1);
        return true;
    }
    MediaAtlasHandle presented_media{};
    const bool restored =
        renderer && animation_frame_cache_gpu_restore(&renderer->animation_gpu, renderer->media_atlas, key,
                                                      result, &presented_media);
    if (restored) {
        note_media_mutation(renderer, presented_media);
    }
    return restored;
}

bool renderer_materialize_animation_frame(D3D11Renderer* renderer, MediaAssetKey key) noexcept {
    if (renderer && media_asset_large_preview(key)) return true;
    return renderer &&
           animation_frame_cache_gpu_materialize(&renderer->animation_gpu, renderer->graphics.context1,
                                                 renderer->media_atlas, renderer->media_gpu.texture, key);
}

MediaAtlasGpuResult renderer_upload_media_rgba8(D3D11Renderer* renderer, MediaAssetKey key, u16 width,
                                                u16 height, u32 row_pitch,
                                                ConstSlice<u8> premultiplied_rgba8) noexcept {
    const u64 minimum_pitch = static_cast<u64>(width) * 4;
    const u64 required = height == 0 ? 0 : static_cast<u64>(height - 1) * row_pitch + minimum_pitch;
    if (!renderer || !renderer->graphics.device || !renderer->graphics.context1 || !renderer->media_atlas ||
        !media_asset_key_valid(key) || width == 0 || height == 0 || row_pitch < minimum_pitch ||
        required > premultiplied_rgba8.count ||
        (!premultiplied_rgba8.data && premultiplied_rgba8.count != 0) ||
        static_cast<u32>(width) + renderer->media_atlas->allocations.padding * 2u >
            renderer->media_atlas->allocations.width ||
        static_cast<u32>(height) + renderer->media_atlas->allocations.padding * 2u >
            media_atlas_capacity_height(*renderer->media_atlas)) {
        return reject_media_request(renderer);
    }
    const MediaAtlasGpuResult initialized = ensure_primary_media_gpu(renderer);
    if (!initialized.ok()) {
        return initialized;
    }
    const MediaAtlasGpuResult prepared =
        prepare_media_surface(renderer, renderer->media_atlas, false, key, width, height);
    if (!prepared.ok()) {
        return prepared;
    }
    const MediaAtlasGpuResult uploaded = media_atlas_gpu_upload_rgba8(
        &renderer->media_gpu, renderer->media_atlas, renderer->graphics.context1, key, width, height,
        row_pitch, premultiplied_rgba8);
    if (!uploaded.ok()) {
        ++renderer->media_rejected;
        log_media_upload_failure(uploaded, "media_atlas_upload_failed");
    }
    note_media_mutation(renderer, uploaded);
    return uploaded;
}

MediaAtlasGpuResult renderer_upload_regional_flag(D3D11Renderer* renderer, MediaAssetKey key,
    const ColrPaintProgram& program) noexcept {
    if (!renderer || !renderer->media_atlas || !media_asset_key_valid(key) ||
        key.kind != MediaAssetKind::regional_flag || !program.storage.base || !program.storage.offset)
        return reject_media_request(renderer);
    char metadata_target[kRegionalFlagSvgTargetBytes];
    if (!regional_flag_svg_target(static_cast<u16>(key.id),{metadata_target,sizeof(metadata_target)}))
        return reject_media_request(renderer);
    const auto initialized = ensure_primary_media_gpu(renderer);if (!initialized.ok()) return initialized;
    const auto prepared = prepare_media_surface(renderer, renderer->media_atlas, false, key, 64, 64);
    if (!prepared.ok()) return prepared;
    const auto allocation = media_atlas_allocate(renderer->media_atlas,key,64,64);
    if (!allocation.ok()) return {allocation,E_FAIL,MediaAtlasGpuError::atlas_failure};
    GlyphAtlasAllocation target{};target.x=allocation.x;target.y=allocation.y;target.width=target.height=64;
    target.error=GlyphAtlasError::none;
    if (!d3d11_glyph_text_rasterize_flag_program(&renderer->glyph_text,renderer->graphics.context,program,target,
        renderer->media_atlas->allocations.width,renderer->media_atlas->allocations.height,
        renderer->media_atlas->allocations.padding,renderer->media_gpu.texture_uav)) {
        media_atlas_reset(renderer->media_atlas);presentation_damage_invalidate_full(&renderer->presentation_damage);
        return {allocation,E_FAIL,MediaAtlasGpuError::device_failure};
    }
    const MediaAtlasGpuResult uploaded{allocation,S_OK,MediaAtlasGpuError::none};
    note_media_mutation(renderer,uploaded);return uploaded;
}

MediaAtlasGpuResult renderer_upload_lottie_program(D3D11Renderer* renderer, MediaAssetKey key,
                                          const ColrPaintProgram& program) noexcept {
    if (!renderer || !renderer->media_atlas || !media_asset_key_valid(key) ||
        !((key.kind == MediaAssetKind::catalog_preview &&
           media_asset_catalog_format(key) == MediaAttachmentFormat::lottie) ||
          (key.kind == MediaAssetKind::sticker && key.variant == 3)) ||
        !program.storage.base || !program.storage.offset) return reject_media_request(renderer);
    AnimationAtlasTarget surface(renderer, key, 128, 128);
    if (!surface.initialized.ok()) return surface.initialized;
    const auto allocation = media_atlas_allocate(surface.atlas, key, 128, 128);
    if (!allocation.ok()) return {allocation, E_FAIL, MediaAtlasGpuError::atlas_failure};
    GlyphAtlasAllocation target{};
    target.x=allocation.x; target.y=allocation.y; target.width=target.height=128;
    target.error=GlyphAtlasError::none;
    if (!d3d11_glyph_text_rasterize_lottie_program(&renderer->glyph_text, renderer->graphics.context, program, target,
        surface.atlas->allocations.width, surface.atlas->allocations.height,
        surface.atlas->allocations.padding, renderer->media_gpu.texture_uav)) {
        media_atlas_reset(surface.atlas);
        presentation_damage_invalidate_full(&renderer->presentation_damage);
        return {allocation, E_FAIL, MediaAtlasGpuError::device_failure};
    }
    const MediaAtlasGpuResult uploaded{allocation, S_OK, MediaAtlasGpuError::none};
    note_media_mutation(renderer, uploaded,surface.preview ? 1 : 0);
    return uploaded;
}
MediaAtlasGpuResult renderer_upload_media_straight_rgba8(D3D11Renderer* renderer, MediaAssetKey key,
                                                         u16 width, u16 height, u32 row_pitch,
                                                         ConstSlice<u8> straight_rgba8) noexcept {
    const bool preview = uses_preview_atlas(renderer, key);
    MediaAtlas* atlas = upload_atlas(renderer, preview);
    const u64 minimum_pitch = static_cast<u64>(width) * 4;
    const u64 required = height == 0 ? 0 : static_cast<u64>(height - 1) * row_pitch + minimum_pitch;
    if (!renderer || !renderer->graphics.device || !renderer->graphics.context1 || !atlas ||
        !media_asset_key_valid(key) || width == 0 || height == 0 || row_pitch < minimum_pitch ||
        required > straight_rgba8.count || (!straight_rgba8.data && straight_rgba8.count != 0) ||
        static_cast<u32>(width) + atlas->allocations.padding * 2u > atlas->allocations.width ||
        static_cast<u32>(height) + atlas->allocations.padding * 2u > media_atlas_capacity_height(*atlas)) {
        return reject_media_request(renderer);
    }
    return upload_to_atlas(renderer, key, preview, atlas, width, height, "straight_rgba8_atlas_upload_failed",
        [&]() noexcept {
            return preview ? media_atlas_gpu_upload_preview_straight_rgba8(
                                 &renderer->media_gpu, atlas, renderer->graphics.device,
                                 renderer->graphics.context1, key, width, height, row_pitch, straight_rgba8)
                           : media_atlas_gpu_upload_straight_rgba8(
                                 &renderer->media_gpu, atlas, renderer->graphics.device,
                                 renderer->graphics.context1, key, width, height, row_pitch, straight_rgba8);
        });
}

MediaAtlasGpuResult renderer_upload_png_scanlines(D3D11Renderer* renderer, MediaAssetKey key,
                                                  const PngImage& image,
                                                  ConstSlice<u8> reconstructed_scanlines) noexcept {
    const bool preview = uses_preview_atlas(renderer, key);
    MediaAtlas* atlas = upload_atlas(renderer, preview);
    if (!renderer || !renderer->graphics.device || !renderer->graphics.context1 || !atlas ||
        !media_asset_key_valid(key)) {
        return reject_media_request(renderer);
    }
    return upload_to_atlas(renderer, key, preview, atlas, image.width, image.height, "png_atlas_upload_failed",
        [&]() noexcept {
            return preview ? media_atlas_gpu_upload_preview_png(&renderer->media_gpu, atlas,
                                                                renderer->graphics.device,
                                                                renderer->graphics.context1, key, image,
                                                                reconstructed_scanlines)
                           : media_atlas_gpu_upload_png(&renderer->media_gpu, atlas, renderer->graphics.device,
                                                        renderer->graphics.context1, key, image,
                                                        reconstructed_scanlines);
        });
}

MediaAtlasGpuResult renderer_upload_jpeg(D3D11Renderer* renderer, MediaAssetKey key, const JpegImage& image,
                                         const JpegCoefficientWorkspace& coefficients) noexcept {
    const bool preview = uses_preview_atlas(renderer, key);
    MediaAtlas* atlas = upload_atlas(renderer, preview);
    if (!renderer || !renderer->graphics.device || !renderer->graphics.context1 || !atlas ||
        !media_asset_key_valid(key)) {
        return reject_media_request(renderer);
    }
    return upload_to_atlas(renderer, key, preview, atlas, jpeg_display_width(image), jpeg_display_height(image),
        "jpeg_atlas_upload_failed", [&]() noexcept {
            return preview ? media_atlas_gpu_upload_preview_jpeg(&renderer->media_gpu, atlas,
                                                                 renderer->graphics.device,
                                                                 renderer->graphics.context1, key, image,
                                                                 coefficients)
                           : media_atlas_gpu_upload_jpeg(&renderer->media_gpu, atlas, renderer->graphics.device,
                                                         renderer->graphics.context1, key, image, coefficients);
        });
}

MediaAtlasGpuResult renderer_begin_apng(D3D11Renderer* renderer, MediaAssetKey key, u16 canvas_width,
                                        u16 canvas_height, const ApngFrame& frame,
                                        const PngImage& frame_image,
                                        ConstSlice<u8> reconstructed_scanlines) noexcept {
    if (!media_request_ready(renderer, key)) {
        return reject_media_request(renderer);
    }
    return upload_animation(renderer, key, canvas_width, canvas_height, "apng_atlas_begin_failed",
        [&](MediaAtlas* atlas) noexcept {
            return media_atlas_gpu_begin_apng(&renderer->media_gpu, atlas, renderer->graphics.device,
                                              renderer->graphics.context1, key, canvas_width, canvas_height,
                                              frame, frame_image, reconstructed_scanlines);
        });
}

MediaAtlasGpuResult renderer_advance_apng(D3D11Renderer* renderer, MediaAssetKey key,
                                          const ApngFrame& previous_frame, const ApngFrame& frame,
                                          const PngImage& frame_image,
                                          ConstSlice<u8> reconstructed_scanlines) noexcept {
    if (!media_request_ready(renderer, key) || !renderer->media_gpu.texture) {
        return reject_media_request(renderer);
    }
    return upload_animation(renderer, key, 0, 0, "apng_atlas_advance_failed", [&](MediaAtlas* atlas) noexcept {
        return media_atlas_gpu_advance_apng(&renderer->media_gpu, atlas, renderer->graphics.device,
                                            renderer->graphics.context1, key, previous_frame, frame,
                                            frame_image, reconstructed_scanlines);
    });
}

MediaAtlasGpuResult renderer_begin_gif(D3D11Renderer* renderer, MediaAssetKey key, u16 canvas_width,
                                       u16 canvas_height, const GifFrame& frame, const GifPalette& palette,
                                       ConstSlice<u8> indices) noexcept {
    if (!media_request_ready(renderer, key)) {
        return reject_media_request(renderer);
    }
    return upload_animation(renderer, key, canvas_width, canvas_height, "gif_atlas_begin_failed",
        [&](MediaAtlas* atlas) noexcept {
            return media_atlas_gpu_begin_gif(&renderer->media_gpu, atlas, renderer->graphics.device,
                                             renderer->graphics.context1, key, canvas_width, canvas_height,
                                             frame, palette, indices);
        });
}

MediaAtlasGpuResult renderer_advance_gif(D3D11Renderer* renderer, MediaAssetKey key,
                                         const GifFrame& previous_frame, const GifFrame& frame,
                                         const GifPalette& palette, ConstSlice<u8> indices) noexcept {
    if (!media_request_ready(renderer, key)) {
        return reject_media_request(renderer);
    }
    return upload_animation(renderer, key, 0, 0, "gif_atlas_advance_failed", [&](MediaAtlas* atlas) noexcept {
        return media_atlas_gpu_advance_gif(&renderer->media_gpu, atlas, renderer->graphics.device,
                                           renderer->graphics.context1, key, previous_frame, frame, palette,
                                           indices);
    });
}

MediaAtlasGpuResult renderer_begin_animated_webp(D3D11Renderer* renderer, MediaAssetKey key,
                                                 u16 canvas_width, u16 canvas_height, u32 background_rgba,
                                                 const AnimatedWebpFrame& frame,
                                                 ConstSlice<u8> straight_rgba8) noexcept {
    if (!media_request_ready(renderer, key)) {
        return reject_media_request(renderer);
    }
    return upload_animation(renderer, key, canvas_width, canvas_height, "animated_webp_atlas_begin_failed",
        [&](MediaAtlas* atlas) noexcept {
            return media_atlas_gpu_begin_animated_webp(&renderer->media_gpu, atlas, renderer->graphics.device,
                                                       renderer->graphics.context1, key, canvas_width,
                                                       canvas_height, background_rgba, frame, straight_rgba8);
        });
}

MediaAtlasGpuResult renderer_advance_animated_webp(D3D11Renderer* renderer, MediaAssetKey key,
                                                   u32 background_rgba,
                                                   const AnimatedWebpFrame& previous_frame,
                                                   const AnimatedWebpFrame& frame,
                                                   ConstSlice<u8> straight_rgba8) noexcept {
    if (!media_request_ready(renderer, key)) {
        return reject_media_request(renderer);
    }
    return upload_animation(renderer, key, 0, 0, "animated_webp_atlas_advance_failed",
        [&](MediaAtlas* atlas) noexcept {
            return media_atlas_gpu_advance_animated_webp(&renderer->media_gpu, atlas,
                                                         renderer->graphics.device,
                                                         renderer->graphics.context1, key, background_rgba,
                                                         previous_frame, frame, straight_rgba8);
        });
}

MediaAtlasGpuResult renderer_reconstruct_vp8_keyframe(
    D3D11Renderer* renderer, const Vp8Keyframe& frame, const Vp8ModeWorkspace& modes,
    const Vp8CoefficientWorkspace& coefficients) noexcept {
    if (!renderer || !renderer->graphics.device || !renderer->graphics.context1 || !renderer->media_atlas) {
        return reject_media_request(renderer);
    }
    const MediaAtlasGpuResult initialized = ensure_primary_media_gpu(renderer);
    if (!initialized.ok()) {
        return initialized;
    }
    const MediaAtlasGpuResult reconstructed =
        media_atlas_gpu_reconstruct_vp8_keyframe(&renderer->media_gpu, renderer->graphics.device,
                                                 renderer->graphics.context1, frame, modes, coefficients);
    if (!reconstructed.ok()) {
        ++renderer->media_rejected;
        log_write(LogLevel::error, "render", "vp8_gpu_reconstruction_failed");
    }
    return reconstructed;
}

MediaAtlasGpuResult renderer_filter_vp8_keyframe(D3D11Renderer* renderer, const Vp8Keyframe& frame,
                                                 const Vp8ControlHeader& header,
                                                 const Vp8ModeWorkspace& modes,
                                                 const Vp8CoefficientWorkspace& coefficients) noexcept {
    if (!renderer || !renderer->graphics.device || !renderer->graphics.context1 || !renderer->media_atlas ||
        !renderer->media_gpu.texture) {
        return reject_media_request(renderer);
    }
    const MediaAtlasGpuResult filtered = media_atlas_gpu_filter_vp8_keyframe(
        &renderer->media_gpu, renderer->graphics.device, renderer->graphics.context1, frame, header, modes,
        coefficients);
    if (!filtered.ok()) {
        ++renderer->media_rejected;
        log_write(LogLevel::error, "render", "vp8_gpu_filter_failed");
    }
    return filtered;
}

MediaAtlasGpuResult renderer_upload_vp8_keyframe(D3D11Renderer* renderer, MediaAssetKey key,
                                                 const Vp8Keyframe& frame, const Vp8ControlHeader& header,
                                                 const Vp8ModeWorkspace& modes,
                                                 const Vp8CoefficientWorkspace& coefficients,
                                                 ConstSlice<u8> alpha) noexcept {
    const bool preview = uses_preview_atlas(renderer, key);
    MediaAtlas* atlas = upload_atlas(renderer, preview);
    if (!renderer || !renderer->graphics.device || !renderer->graphics.context1 || !atlas ||
        !media_asset_key_valid(key)) {
        return reject_media_request(renderer);
    }
    MediaAtlasGpuResult result = renderer_reconstruct_vp8_keyframe(renderer, frame, modes, coefficients);
    if (!result.ok()) {
        return result;
    }
    result = renderer_filter_vp8_keyframe(renderer, frame, header, modes, coefficients);
    if (!result.ok()) {
        return result;
    }
    return upload_to_atlas(renderer, key, preview, atlas, frame.width, frame.height, "vp8_atlas_upload_failed",
        [&]() noexcept {
            return preview ? media_atlas_gpu_upload_preview_vp8_keyframe(
                                 &renderer->media_gpu, atlas, renderer->graphics.device,
                                 renderer->graphics.context1, key, frame, alpha)
                           : media_atlas_gpu_upload_vp8_keyframe(
                                 &renderer->media_gpu, atlas, renderer->graphics.device,
                                 renderer->graphics.context1, key, frame, alpha);
        });
}

MediaAtlasGpuResult renderer_begin_animated_webp_vp8(
    D3D11Renderer* renderer, MediaAssetKey key, u16 canvas_width, u16 canvas_height, u32 background_rgba,
    const AnimatedWebpFrame& animation_frame, const Vp8Keyframe& vp8_frame, const Vp8ControlHeader& header,
    const Vp8ModeWorkspace& modes, const Vp8CoefficientWorkspace& coefficients,
    ConstSlice<u8> alpha) noexcept {
    if (!renderer || !media_asset_key_valid(key)) {
        return reject_media_request(renderer);
    }
    MediaAtlasGpuResult result =
        renderer_reconstruct_vp8_keyframe(renderer, vp8_frame, modes, coefficients);
    if (!result.ok()) {
        return result;
    }
    result = renderer_filter_vp8_keyframe(renderer, vp8_frame, header, modes, coefficients);
    if (!result.ok()) {
        return result;
    }
    return upload_animation(renderer, key, canvas_width, canvas_height, "animated_webp_vp8_atlas_begin_failed",
        [&](MediaAtlas* atlas) noexcept {
            return media_atlas_gpu_begin_animated_webp_vp8(
                &renderer->media_gpu, atlas, renderer->graphics.device, renderer->graphics.context1, key,
                canvas_width, canvas_height, background_rgba, animation_frame, vp8_frame, alpha);
        });
}

MediaAtlasGpuResult renderer_advance_animated_webp_vp8(
    D3D11Renderer* renderer, MediaAssetKey key, u32 background_rgba,
    const AnimatedWebpFrame& previous_frame, const AnimatedWebpFrame& animation_frame,
    const Vp8Keyframe& vp8_frame, const Vp8ControlHeader& header, const Vp8ModeWorkspace& modes,
    const Vp8CoefficientWorkspace& coefficients, ConstSlice<u8> alpha) noexcept {
    if (!renderer || !media_asset_key_valid(key)) {
        return reject_media_request(renderer);
    }
    MediaAtlasGpuResult result =
        renderer_reconstruct_vp8_keyframe(renderer, vp8_frame, modes, coefficients);
    if (!result.ok()) {
        return result;
    }
    result = renderer_filter_vp8_keyframe(renderer, vp8_frame, header, modes, coefficients);
    if (!result.ok()) {
        return result;
    }
    return upload_animation(renderer, key, 0, 0, "animated_webp_vp8_atlas_advance_failed",
        [&](MediaAtlas* atlas) noexcept {
            return media_atlas_gpu_advance_animated_webp_vp8(
                &renderer->media_gpu, atlas, renderer->graphics.device, renderer->graphics.context1, key,
                background_rgba, previous_frame, animation_frame, vp8_frame, alpha);
        });
}

bool renderer_read_media_rgba(D3D11Renderer* renderer, MediaAssetKey key, Slice<u8> rgba, u32* width,
                              u32* height) noexcept {
    if (!renderer || !width || !height || !rgba.data || !renderer->graphics.device ||
        !renderer->graphics.context || !media_asset_key_valid(key))
        return false;
    // Same domain selection as presentation: large previews live in the preview atlas.
    const bool preview = media_asset_large_preview(key) && renderer->preview_media_atlas;
    MediaAtlas* atlas = preview ? renderer->preview_media_atlas : renderer->media_atlas;
    ID3D11Texture2D* source = preview ? renderer->media_gpu.preview_texture : renderer->media_gpu.texture;
    if (!atlas || !source)
        return false;
    const MediaAtlasResult allocation = media_atlas_peek(atlas, key);
    if (!allocation.ok() || !allocation.width || !allocation.height ||
        u64{allocation.width} * allocation.height * 4 > rgba.count)
        return false;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = allocation.width;
    description.Height = allocation.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(renderer->graphics.device->CreateTexture2D(&description, nullptr, &staging)))
        return false;
    const D3D11_BOX box{allocation.x, allocation.y, 0, static_cast<UINT>(allocation.x + allocation.width),
                        static_cast<UINT>(allocation.y + allocation.height), 1};
    renderer->graphics.context->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const bool ok = SUCCEEDED(renderer->graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    if (ok) {
        // Atlas texels are premultiplied; clipboard consumers expect straight alpha.
        for (u32 y = 0; y < allocation.height; ++y) {
            const u8* row = static_cast<const u8*>(mapped.pData) + u64{y} * mapped.RowPitch;
            u8* out = rgba.data + u64{y} * allocation.width * 4;
            for (u32 x = 0; x < allocation.width; ++x) {
                const u32 a = row[x * 4 + 3];
                for (u32 c = 0; c < 3; ++c)
                    out[x * 4 + c] = a ? static_cast<u8>(minimum<u32>(255, (row[x * 4 + c] * 255u + a / 2) / a)) : 0;
                out[x * 4 + 3] = static_cast<u8>(a);
            }
        }
        renderer->graphics.context->Unmap(staging, 0);
        *width = allocation.width;
        *height = allocation.height;
    }
    staging->Release();
    return ok;
}

namespace {

// Last resort when a trimmed atlas cannot get its smaller surface: drop all
// media surfaces and residency (as on unbind) so lazy initialization restarts
// at the allocators' heights. Visible media is requested again by its owners.
void release_media_surfaces(D3D11Renderer* renderer) noexcept {
    media_atlas_gpu_release(&renderer->media_gpu);
    animation_frame_cache_gpu_reset(&renderer->animation_gpu);
    animation_frame_cache_gpu_reset(&renderer->preview_animation_gpu);
    MediaAtlas* const atlases[] = {renderer->media_atlas, renderer->preview_media_atlas};
    for (MediaAtlas* atlas : atlases) {
        if (atlas) {
            media_atlas_reset(atlas);
            media_atlas_reset_growth(atlas);
        }
    }
    presentation_damage_invalidate_full(&renderer->presentation_damage);
    log_write(LogLevel::warning, "render", "media_atlas_trim_released_surfaces");
}

ID3D11Texture2D* disposal_backup(const D3D11Renderer& renderer, bool preview) noexcept {
    return preview ? renderer.media_gpu.preview_animation_backup_texture : renderer.media_gpu.apng_backup_texture;
}

u64 backup_release_deadline(const D3D11Renderer& renderer, const MediaAtlas* atlas, bool preview) noexcept {
    return atlas && disposal_backup(renderer, preview) ? atlas->disposal_backup_used_ms + kMediaAtlasIdleTrimMs : 0;
}

bool trim_media_atlas(D3D11Renderer* renderer, MediaAtlas* atlas, bool preview, u64 now_ms) noexcept {
    if (!atlas) {
        return false;
    }
    media_atlas_observe_time(atlas, now_ms);
    bool changed = false;
    // The previous-disposal backup lives only while some animation needs it.
    const u64 backup_deadline = backup_release_deadline(*renderer, atlas, preview);
    if (backup_deadline && now_ms >= backup_deadline) {
        media_atlas_gpu_release_backup(&renderer->media_gpu, preview);
        changed = true;
    }
    const u64 deadline = media_atlas_trim_deadline(*atlas);
    if (!deadline || now_ms < deadline) {
        return changed;
    }
    GlyphAtlasRowMove moves[256];
    bool resized = false;
    const u32 moved = media_atlas_trim(atlas, {moves, 256}, &resized);
    if ((resized || moved) &&
        !media_atlas_gpu_resize(&renderer->media_gpu, renderer->graphics.device, renderer->graphics.context1, preview,
                                atlas->allocations.height, {moves, moved})
             .ok()) {
        release_media_surfaces(renderer);
        return true;
    }
    return changed || resized || moved;
}
} // namespace

bool renderer_media_maintain(D3D11Renderer* renderer, u64 now_ms) noexcept {
    if (!renderer || !renderer->graphics.device || !renderer->graphics.context1) {
        return false;
    }
    bool changed = false;
    animation_frame_cache_gpu_observe_time(&renderer->animation_gpu, now_ms);
    animation_frame_cache_gpu_observe_time(&renderer->preview_animation_gpu, now_ms);
    const u64 identity_deadline = animation_frame_cache_gpu_trim_deadline(renderer->animation_gpu);
    if (identity_deadline && now_ms >= identity_deadline && renderer->media_atlas && renderer->media_gpu.texture) {
        // A paused, visible animation may show a cached frame whose live atlas
        // slot holds an older picture: copy those frames back before release.
        // Stops after the last occupied entry: unused capacity stays untouched.
        const MediaAtlas& atlas = *renderer->media_atlas;
        for (u32 slot = 0, live = atlas.entry_count; slot < atlas.entry_capacity && live; ++slot) {
            if (!atlas.entries[slot].occupied)
                continue;
            --live;
            animation_frame_cache_gpu_materialize(&renderer->animation_gpu, renderer->graphics.context1,
                                                  renderer->media_atlas, renderer->media_gpu.texture,
                                                  atlas.entries[slot].key);
        }
    }
    changed = animation_frame_cache_gpu_trim(&renderer->animation_gpu, now_ms) || changed;
    changed = animation_frame_cache_gpu_trim(&renderer->preview_animation_gpu, now_ms) || changed;
    changed = trim_media_atlas(renderer, renderer->media_atlas, false, now_ms) || changed;
    changed = trim_media_atlas(renderer, renderer->preview_media_atlas, true, now_ms) || changed;
    if (changed) {
        ++renderer->media_atlas_trims;
    }
    return changed;
}

u64 renderer_media_next_trim_ms(const D3D11Renderer& renderer) noexcept {
    u64 deadline = 0;
    const auto take = [&deadline](u64 value) noexcept {
        if (value && (!deadline || value < deadline))
            deadline = value;
    };
    if (renderer.media_atlas)
        take(media_atlas_trim_deadline(*renderer.media_atlas));
    if (renderer.preview_media_atlas)
        take(media_atlas_trim_deadline(*renderer.preview_media_atlas));
    take(backup_release_deadline(renderer, renderer.media_atlas, false));
    take(backup_release_deadline(renderer, renderer.preview_media_atlas, true));
    take(animation_frame_cache_gpu_trim_deadline(renderer.animation_gpu));
    take(animation_frame_cache_gpu_trim_deadline(renderer.preview_animation_gpu));
    return deadline;
}

} // namespace maicord

