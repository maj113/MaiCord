#include "maicord/render/media_atlas_gpu.hpp"

#include "media_atlas_gpu_internal.hpp"

namespace maicord {

using namespace media_atlas_gpu_detail;

MediaAtlasGpuResult media_atlas_gpu_begin_apng(MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
                                               ID3D11DeviceContext1* context, MediaAssetKey key,
                                               u16 canvas_width, u16 canvas_height, const ApngFrame& frame,
                                               const PngImage& frame_image,
                                               ConstSlice<u8> reconstructed_scanlines) noexcept {
    if (!gpu || !atlas || !device || !context || !gpu->texture || !gpu->texture_view || !gpu->target ||
        !gpu->texture_uav || !media_asset_key_valid(key) || canvas_width == 0 || canvas_height == 0 ||
        !apng_frame_valid(frame, canvas_width, canvas_height, frame_image) ||
        !png_scanlines_reconstructed(frame_image, reconstructed_scanlines) ||
        gpu->width != atlas->allocations.width || gpu->height != atlas->allocations.height ||
        static_cast<u32>(canvas_width) + atlas->allocations.padding * 2u > atlas->allocations.width ||
        static_cast<u32>(canvas_height) + atlas->allocations.padding * 2u > atlas->allocations.height) {
        if (gpu) {
            ++gpu->rejected;
        }
        return failure(MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    HRESULT result = upload_png_inputs(gpu, device, context, frame_image, reconstructed_scanlines);
    if (FAILED(result)) {
        ++gpu->rejected;
        return failure(MediaAtlasGpuError::device_failure, result);
    }
    unbind_apng_atlas(context);
    const MediaAtlasResult allocation =
        allocate_and_clear(gpu, atlas, context, key, canvas_width, canvas_height);
    if (!allocation.ok()) {
        ++gpu->rejected;
        return failure(MediaAtlasGpuError::atlas_failure, S_OK, allocation);
    }
    if (frame.dispose == ApngDispose::previous) {
        result = ensure_apng_backup(gpu, device, context, atlas, static_cast<u32>(allocation.y) + allocation.height);
        if (FAILED(result)) {
            ++gpu->rejected;
            return failure(MediaAtlasGpuError::device_failure, result, allocation);
        }
    }
    if (frame.dispose == ApngDispose::previous) {
        copy_animation_region(context, gpu->apng_backup_texture, gpu->texture, allocation,
                              frame.x_offset, frame.y_offset, frame.width, frame.height);
    }
    dispatch_png(gpu, context, frame_image, allocation.x + frame.x_offset, allocation.y + frame.y_offset,
                 frame.blend);
    account_apng_dispatch(gpu, frame, frame_image);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_advance_apng(MediaAtlasGpu* gpu, MediaAtlas* atlas,
                                                 ID3D11Device* device, ID3D11DeviceContext1* context,
                                                 MediaAssetKey key, const ApngFrame& previous_frame,
                                                 const ApngFrame& frame, const PngImage& frame_image,
                                                 ConstSlice<u8> reconstructed_scanlines) noexcept {
    const MediaAtlasResult allocation = atlas ? media_atlas_find(atlas, key) : MediaAtlasResult{};
    if (!gpu || !atlas || !device || !context || !gpu->texture || !gpu->texture_view || !gpu->target ||
        !gpu->texture_uav || !media_asset_key_valid(key) || !allocation.ok() ||
        !apng_control_valid(previous_frame, allocation.width, allocation.height) ||
        !apng_frame_valid(frame, allocation.width, allocation.height, frame_image) ||
        !png_scanlines_reconstructed(frame_image, reconstructed_scanlines) ||
        gpu->width != atlas->allocations.width || gpu->height != atlas->allocations.height) {
        if (gpu) {
            ++gpu->rejected;
        }
        return failure(MediaAtlasGpuError::invalid_input, E_INVALIDARG, allocation);
    }
    HRESULT result = upload_png_inputs(gpu, device, context, frame_image, reconstructed_scanlines);
    if (SUCCEEDED(result) &&
        (previous_frame.dispose == ApngDispose::previous || frame.dispose == ApngDispose::previous)) {
        result = ensure_apng_backup(gpu, device, context, atlas, static_cast<u32>(allocation.y) + allocation.height);
    }
    if (FAILED(result)) {
        ++gpu->rejected;
        return failure(MediaAtlasGpuError::device_failure, result, allocation);
    }

    unbind_apng_atlas(context);
    if (previous_frame.dispose == ApngDispose::background) {
        clear_animation_region(gpu, context, allocation, previous_frame.x_offset,
                               previous_frame.y_offset, previous_frame.width, previous_frame.height,
                               kTransparentColor);
        ++gpu->apng_background_disposals;
    } else if (previous_frame.dispose == ApngDispose::previous) {
        copy_animation_region(context, gpu->texture, gpu->apng_backup_texture, allocation,
                              previous_frame.x_offset, previous_frame.y_offset, previous_frame.width,
                              previous_frame.height);
        ++gpu->apng_previous_disposals;
    }
    if (frame.dispose == ApngDispose::previous) {
        copy_animation_region(context, gpu->apng_backup_texture, gpu->texture, allocation,
                              frame.x_offset, frame.y_offset, frame.width, frame.height);
    }
    dispatch_png(gpu, context, frame_image, allocation.x + frame.x_offset, allocation.y + frame.y_offset,
                 frame.blend);
    account_apng_dispatch(gpu, frame, frame_image);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_begin_gif(MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
                                              ID3D11DeviceContext1* context, MediaAssetKey key,
                                              u16 canvas_width, u16 canvas_height, const GifFrame& frame,
                                              const GifPalette& palette, ConstSlice<u8> indices) noexcept {
    if (!gpu || !atlas || !device || !context || !gpu->texture || !gpu->texture_view || !gpu->target ||
        !gpu->texture_uav || !media_asset_key_valid(key) || canvas_width == 0 || canvas_height == 0 ||
        !gif_frame_valid(frame, canvas_width, canvas_height, palette, indices) ||
        gpu->width != atlas->allocations.width || gpu->height != atlas->allocations.height ||
        static_cast<u32>(canvas_width) + atlas->allocations.padding * 2u > atlas->allocations.width ||
        static_cast<u32>(canvas_height) + atlas->allocations.padding * 2u > atlas->allocations.height) {
        if (gpu) {
            ++gpu->rejected;
        }
        return failure(MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    HRESULT result = upload_gif_inputs(gpu, device, context, frame, palette, indices);
    if (FAILED(result)) {
        ++gpu->rejected;
        return failure(MediaAtlasGpuError::device_failure, result);
    }
    unbind_apng_atlas(context);
    const MediaAtlasResult allocation =
        allocate_and_clear(gpu, atlas, context, key, canvas_width, canvas_height);
    if (!allocation.ok()) {
        ++gpu->rejected;
        return failure(MediaAtlasGpuError::atlas_failure, S_OK, allocation);
    }
    if (frame.dispose == GifDispose::previous) {
        result = ensure_apng_backup(gpu, device, context, atlas, static_cast<u32>(allocation.y) + allocation.height);
        if (FAILED(result)) {
            ++gpu->rejected;
            return failure(MediaAtlasGpuError::device_failure, result, allocation);
        }
    }
    if (frame.dispose == GifDispose::previous) {
        copy_animation_region(context, gpu->apng_backup_texture, gpu->texture, allocation,
                              frame.x_offset, frame.y_offset, frame.width, frame.height);
    }
    dispatch_gif(gpu, context, frame, palette, allocation.x + frame.x_offset,
                 allocation.y + frame.y_offset);
    account_gif_dispatch(gpu, frame);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_advance_gif(MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
                                                ID3D11DeviceContext1* context, MediaAssetKey key,
                                                const GifFrame& previous_frame, const GifFrame& frame,
                                                const GifPalette& palette,
                                                ConstSlice<u8> indices) noexcept {
    const MediaAtlasResult allocation = atlas ? media_atlas_find(atlas, key) : MediaAtlasResult{};
    if (!gpu || !atlas || !device || !context || !gpu->texture || !gpu->texture_view || !gpu->target ||
        !gpu->texture_uav || !media_asset_key_valid(key) || !allocation.ok() ||
        !gif_control_valid(previous_frame, allocation.width, allocation.height) ||
        !gif_frame_valid(frame, allocation.width, allocation.height, palette, indices) ||
        gpu->width != atlas->allocations.width || gpu->height != atlas->allocations.height) {
        if (gpu) {
            ++gpu->rejected;
        }
        return failure(MediaAtlasGpuError::invalid_input, E_INVALIDARG, allocation);
    }
    HRESULT result = upload_gif_inputs(gpu, device, context, frame, palette, indices);
    if (SUCCEEDED(result) &&
        (previous_frame.dispose == GifDispose::previous || frame.dispose == GifDispose::previous)) {
        result = ensure_apng_backup(gpu, device, context, atlas, static_cast<u32>(allocation.y) + allocation.height);
    }
    if (FAILED(result)) {
        ++gpu->rejected;
        return failure(MediaAtlasGpuError::device_failure, result, allocation);
    }
    unbind_apng_atlas(context);
    if (previous_frame.dispose == GifDispose::background) {
        clear_animation_region(gpu, context, allocation, previous_frame.x_offset,
                               previous_frame.y_offset, previous_frame.width, previous_frame.height,
                               kTransparentColor);
        ++gpu->gif_background_disposals;
    } else if (previous_frame.dispose == GifDispose::previous) {
        copy_animation_region(context, gpu->texture, gpu->apng_backup_texture, allocation,
                              previous_frame.x_offset, previous_frame.y_offset, previous_frame.width,
                              previous_frame.height);
        ++gpu->gif_previous_disposals;
    }
    if (frame.dispose == GifDispose::previous) {
        copy_animation_region(context, gpu->apng_backup_texture, gpu->texture, allocation,
                              frame.x_offset, frame.y_offset, frame.width, frame.height);
    }
    dispatch_gif(gpu, context, frame, palette, allocation.x + frame.x_offset,
                 allocation.y + frame.y_offset);
    account_gif_dispatch(gpu, frame);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_begin_animated_webp(MediaAtlasGpu* gpu, MediaAtlas* atlas,
                                                        ID3D11Device* device, ID3D11DeviceContext1* context,
                                                        MediaAssetKey key, u16 canvas_width,
                                                        u16 canvas_height, u32 background_rgba,
                                                        const AnimatedWebpFrame& frame,
                                                        ConstSlice<u8> straight_rgba8) noexcept {
    if (!gpu || !atlas || !device || !context || !gpu->texture || !gpu->texture_view || !gpu->target ||
        !gpu->texture_uav || !media_asset_key_valid(key) || canvas_width == 0 || canvas_height == 0 ||
        !animated_webp_frame_valid(frame, canvas_width, canvas_height, straight_rgba8) ||
        gpu->width != atlas->allocations.width || gpu->height != atlas->allocations.height ||
        static_cast<u32>(canvas_width) + atlas->allocations.padding * 2u > atlas->allocations.width ||
        static_cast<u32>(canvas_height) + atlas->allocations.padding * 2u > atlas->allocations.height) {
        if (gpu) {
            ++gpu->rejected;
        }
        return failure(MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    const HRESULT uploaded = upload_animated_webp_inputs(gpu, device, context, frame, straight_rgba8);
    if (FAILED(uploaded)) {
        ++gpu->rejected;
        return failure(MediaAtlasGpuError::device_failure, uploaded);
    }
    unbind_apng_atlas(context);
    const MediaAtlasResult allocation =
        allocate_and_clear(gpu, atlas, context, key, canvas_width, canvas_height);
    if (!allocation.ok()) {
        ++gpu->rejected;
        return failure(MediaAtlasGpuError::atlas_failure, S_OK, allocation);
    }
    clear_animated_webp_canvas(gpu, context, allocation, background_rgba);
    dispatch_animated_webp(gpu, context, frame, allocation.x + frame.x_offset,
                           allocation.y + frame.y_offset);
    account_animated_webp_dispatch(gpu, frame, static_cast<u64>(frame.width) * frame.height * 4u);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_advance_animated_webp(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device, ID3D11DeviceContext1* context,
    MediaAssetKey key, u32 background_rgba, const AnimatedWebpFrame& previous_frame,
    const AnimatedWebpFrame& frame, ConstSlice<u8> straight_rgba8) noexcept {
    const MediaAtlasResult allocation = atlas ? media_atlas_find(atlas, key) : MediaAtlasResult{};
    if (!gpu || !atlas || !device || !context || !gpu->texture || !gpu->texture_view || !gpu->target ||
        !gpu->texture_uav || !media_asset_key_valid(key) || !allocation.ok() ||
        !animated_webp_control_valid(previous_frame, allocation.width, allocation.height) ||
        !animated_webp_frame_valid(frame, allocation.width, allocation.height, straight_rgba8) ||
        gpu->width != atlas->allocations.width || gpu->height != atlas->allocations.height) {
        if (gpu) {
            ++gpu->rejected;
        }
        return failure(MediaAtlasGpuError::invalid_input, E_INVALIDARG, allocation);
    }
    const HRESULT uploaded = upload_animated_webp_inputs(gpu, device, context, frame, straight_rgba8);
    if (FAILED(uploaded)) {
        ++gpu->rejected;
        return failure(MediaAtlasGpuError::device_failure, uploaded, allocation);
    }
    unbind_apng_atlas(context);
    if (previous_frame.dispose == AnimatedWebpDispose::background) {
        clear_animated_webp_region(gpu, context, allocation, previous_frame, background_rgba);
        ++gpu->animated_webp_background_disposals;
    }
    dispatch_animated_webp(gpu, context, frame, allocation.x + frame.x_offset,
                           allocation.y + frame.y_offset);
    account_animated_webp_dispatch(gpu, frame, static_cast<u64>(frame.width) * frame.height * 4u);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

} // namespace maicord

