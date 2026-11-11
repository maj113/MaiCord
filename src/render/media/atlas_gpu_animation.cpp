#include "maicord/render/media/atlas_gpu.hpp"

#include "atlas_gpu_internal.hpp"
// Device command recording, once per animation frame: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {

using namespace media_atlas_gpu_detail;

MediaAtlasGpuResult media_atlas_gpu_detail::begin_canvas(MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
                                                         GpuContext1* context, MediaAssetKey key, u16 width,
                                                         u16 height, const CanvasFrame& frame,
                                                         MediaAtlasResult* local) noexcept {
    unbind_apng_atlas(context);
    const MediaAtlasResult allocation =
        allocate_and_clear_surface(gpu, atlas, context, false, key, width, height, local);
    if (!allocation.ok()) {
        return reject(gpu, MediaAtlasGpuError::atlas_failure, S_OK, allocation);
    }
    if (frame.previous) {
        const GpuStatus result = ensure_apng_backup(gpu, device, context, atlas, allocation);
        if (FAILED(result)) {
            return reject(gpu, MediaAtlasGpuError::device_failure, result, allocation);
        }
        copy_animation_region(context, gpu->backup_texture, gpu->texture, *local, frame.x, frame.y, frame.width,
                              frame.height);
    }
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

namespace {

template <typename Frame>
CanvasFrame canvas_frame(const Frame& frame) noexcept {
    using Dispose = decltype(frame.dispose);
    return {frame.x_offset, frame.y_offset, frame.width, frame.height, frame.dispose == Dispose::background,
            frame.dispose == Dispose::previous};
}

// The resident canvas selected (`local` its slot in that page's rows), the previous frame disposed of, and the
// frame's region saved when the frame restores the canvas after itself.
GpuStatus advance_canvas(MediaAtlasGpu* gpu, GpuDevice* device, GpuContext1* context, MediaAtlas* atlas,
                         const MediaAtlasResult& allocation, const CanvasFrame& previous, const CanvasFrame& frame,
                         u32* background_disposals, u32* previous_disposals, MediaAtlasResult* local) noexcept {
    *local = allocation;
    GpuStatus result = media_atlas_gpu_select(gpu, context, *atlas, false, local) ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(result) && (previous.previous || frame.previous)) {
        result = ensure_apng_backup(gpu, device, context, atlas, allocation);
    }
    if (FAILED(result)) {
        return result;
    }
    unbind_apng_atlas(context);
    if (previous.background) {
        clear_animation_region(gpu, context, *local, previous.x, previous.y, previous.width, previous.height,
                               kTransparentColor);
        ++*background_disposals;
    } else if (previous.previous) {
        copy_animation_region(context, gpu->texture, gpu->backup_texture, *local, previous.x, previous.y,
                              previous.width, previous.height);
        ++*previous_disposals;
    }
    if (frame.previous) {
        copy_animation_region(context, gpu->backup_texture, gpu->texture, *local, frame.x, frame.y, frame.width,
                              frame.height);
    }
    return S_OK;
}

} // namespace

MediaAtlasGpuResult media_atlas_gpu_begin_apng(MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
                                               GpuContext1* context, MediaAssetKey key,
                                               u16 canvas_width, u16 canvas_height, const ApngFrame& frame,
                                               const PngImage& frame_image,
                                               ConstSlice<u8> reconstructed_scanlines) noexcept {
    if (!animation_target_ready(gpu, atlas, device, context, key) ||
        !animation_canvas_fits(*atlas, canvas_width, canvas_height) ||
        !apng_frame_valid(frame, canvas_width, canvas_height, frame_image) ||
        !png_scanlines_reconstructed(frame_image, reconstructed_scanlines)) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    const GpuStatus result = upload_png_inputs(gpu, device, context, frame_image, reconstructed_scanlines);
    if (FAILED(result)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, result);
    }
    MediaAtlasResult local{};
    const MediaAtlasGpuResult canvas = begin_canvas(gpu, atlas, device, context, key, canvas_width, canvas_height,
                                                    canvas_frame(frame), &local);
    if (canvas.ok()) {
        dispatch_png(gpu, context, frame_image, local.x + frame.x_offset, local.y + frame.y_offset, frame.blend);
        account_apng_dispatch(gpu, frame, frame_image);
    }
    return canvas;
}

MediaAtlasGpuResult media_atlas_gpu_advance_apng(MediaAtlasGpu* gpu, MediaAtlas* atlas,
                                                 GpuDevice* device, GpuContext1* context,
                                                 MediaAssetKey key, const ApngFrame& previous_frame,
                                                 const ApngFrame& frame, const PngImage& frame_image,
                                                 ConstSlice<u8> reconstructed_scanlines) noexcept {
    const MediaAtlasResult allocation = atlas ? media_atlas_find(atlas, key) : MediaAtlasResult{};
    if (!animation_target_ready(gpu, atlas, device, context, key) || !allocation.ok() ||
        !apng_control_valid(previous_frame, allocation.width, allocation.height) ||
        !apng_frame_valid(frame, allocation.width, allocation.height, frame_image) ||
        !png_scanlines_reconstructed(frame_image, reconstructed_scanlines)) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG, allocation);
    }
    MediaAtlasResult local{};
    GpuStatus result = upload_png_inputs(gpu, device, context, frame_image, reconstructed_scanlines);
    if (SUCCEEDED(result)) {
        result = advance_canvas(gpu, device, context, atlas, allocation, canvas_frame(previous_frame),
                                canvas_frame(frame), &gpu->apng_background_disposals, &gpu->apng_previous_disposals,
                                &local);
    }
    if (FAILED(result)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, result, allocation);
    }
    dispatch_png(gpu, context, frame_image, local.x + frame.x_offset, local.y + frame.y_offset, frame.blend);
    account_apng_dispatch(gpu, frame, frame_image);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_begin_gif(MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
                                              GpuContext1* context, MediaAssetKey key,
                                              u16 canvas_width, u16 canvas_height, const GifFrame& frame,
                                              const GifPalette& palette, ConstSlice<u8> indices) noexcept {
    if (!animation_target_ready(gpu, atlas, device, context, key) ||
        !animation_canvas_fits(*atlas, canvas_width, canvas_height) ||
        !gif_frame_valid(frame, canvas_width, canvas_height, palette, indices)) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    const GpuStatus result = upload_gif_inputs(gpu, device, context, frame, palette, indices);
    if (FAILED(result)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, result);
    }
    MediaAtlasResult local{};
    const MediaAtlasGpuResult canvas = begin_canvas(gpu, atlas, device, context, key, canvas_width, canvas_height,
                                                    canvas_frame(frame), &local);
    if (canvas.ok()) {
        dispatch_gif(gpu, context, frame, palette, local.x + frame.x_offset, local.y + frame.y_offset);
        account_gif_dispatch(gpu, frame);
    }
    return canvas;
}

MediaAtlasGpuResult media_atlas_gpu_advance_gif(MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
                                                GpuContext1* context, MediaAssetKey key,
                                                const GifFrame& previous_frame, const GifFrame& frame,
                                                const GifPalette& palette,
                                                ConstSlice<u8> indices) noexcept {
    const MediaAtlasResult allocation = atlas ? media_atlas_find(atlas, key) : MediaAtlasResult{};
    if (!animation_target_ready(gpu, atlas, device, context, key) || !allocation.ok() ||
        !gif_control_valid(previous_frame, allocation.width, allocation.height) ||
        !gif_frame_valid(frame, allocation.width, allocation.height, palette, indices)) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG, allocation);
    }
    MediaAtlasResult local{};
    GpuStatus result = upload_gif_inputs(gpu, device, context, frame, palette, indices);
    if (SUCCEEDED(result)) {
        result = advance_canvas(gpu, device, context, atlas, allocation, canvas_frame(previous_frame),
                                canvas_frame(frame), &gpu->gif_background_disposals, &gpu->gif_previous_disposals,
                                &local);
    }
    if (FAILED(result)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, result, allocation);
    }
    dispatch_gif(gpu, context, frame, palette, local.x + frame.x_offset, local.y + frame.y_offset);
    account_gif_dispatch(gpu, frame);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_begin_animated_webp(MediaAtlasGpu* gpu, MediaAtlas* atlas,
                                                        GpuDevice* device, GpuContext1* context,
                                                        MediaAssetKey key, u16 canvas_width,
                                                        u16 canvas_height, u32 background_rgba,
                                                        const AnimatedWebpFrame& frame,
                                                        ConstSlice<u8> straight_rgba8) noexcept {
    if (!animation_target_ready(gpu, atlas, device, context, key) ||
        !animation_canvas_fits(*atlas, canvas_width, canvas_height) ||
        !animated_webp_frame_valid(frame, canvas_width, canvas_height, straight_rgba8)) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    const GpuStatus uploaded = upload_animated_webp_inputs(gpu, device, context, frame, straight_rgba8);
    if (FAILED(uploaded)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, uploaded);
    }
    MediaAtlasResult local{};
    const MediaAtlasGpuResult canvas =
        begin_canvas(gpu, atlas, device, context, key, canvas_width, canvas_height, {}, &local);
    if (canvas.ok()) {
        clear_animated_webp_canvas(gpu, context, local, background_rgba);
        dispatch_animated_webp(gpu, context, frame, local.x + frame.x_offset, local.y + frame.y_offset);
        account_animated_webp_dispatch(gpu, frame, static_cast<u64>(frame.width) * frame.height * 4u);
    }
    return canvas;
}

MediaAtlasGpuResult media_atlas_gpu_advance_animated_webp(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device, GpuContext1* context,
    MediaAssetKey key, u32 background_rgba, const AnimatedWebpFrame& previous_frame,
    const AnimatedWebpFrame& frame, ConstSlice<u8> straight_rgba8) noexcept {
    const MediaAtlasResult allocation = atlas ? media_atlas_find(atlas, key) : MediaAtlasResult{};
    if (!animation_target_ready(gpu, atlas, device, context, key) || !allocation.ok() ||
        !animated_webp_control_valid(previous_frame, allocation.width, allocation.height) ||
        !animated_webp_frame_valid(frame, allocation.width, allocation.height, straight_rgba8)) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG, allocation);
    }
    MediaAtlasResult local = allocation;
    GpuStatus uploaded = upload_animated_webp_inputs(gpu, device, context, frame, straight_rgba8);
    if (SUCCEEDED(uploaded) && !media_atlas_gpu_select(gpu, context, *atlas, false, &local)) {
        uploaded = E_OUTOFMEMORY;
    }
    if (FAILED(uploaded)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, uploaded, allocation);
    }
    unbind_apng_atlas(context);
    if (previous_frame.dispose == AnimatedWebpDispose::background) {
        clear_animated_webp_region(gpu, context, local, previous_frame, background_rgba);
        ++gpu->animated_webp_background_disposals;
    }
    dispatch_animated_webp(gpu, context, frame, local.x + frame.x_offset, local.y + frame.y_offset);
    account_animated_webp_dispatch(gpu, frame, static_cast<u64>(frame.width) * frame.height * 4u);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

} // namespace maicord
#pragma clang attribute pop
