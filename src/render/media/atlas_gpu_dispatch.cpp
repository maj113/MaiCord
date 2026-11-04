#include "atlas_gpu_internal.hpp"
// Device command recording, once per image upload: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord::media_atlas_gpu_detail {

GpuStatus upload_discard(GpuContext1* context, GpuBuffer* buffer, const void* source,
                         u32 bytes) noexcept {
    GpuStatus result = kGpuOk;
    void* mapped = gpu_map(context, buffer, GpuMap::discard, &result);
    if (mapped) {
        CopyMemory(mapped, source, bytes);
        gpu_unmap(context, buffer);
    }
    return result;
}

GpuStatus upload_png_shader_inputs(MediaAtlasGpu* gpu, GpuDevice* device,
                                   GpuContext1* context, ConstSlice<u8> source,
                                   u32 source_bytes, const u32* palette, u32 palette_count) noexcept {
    GpuStatus result = create_png_resources(gpu, device);
    if (SUCCEEDED(result)) {
        result = ensure_dynamic_raw_buffer(device, source_bytes, &gpu->png_scanline_buffer,
                                           &gpu->png_scanline_view, &gpu->png_scanline_capacity);
    }
    if (FAILED(result)) {
        return result;
    }
    result = upload_discard(context, gpu->png_scanline_buffer, source.data, source_bytes);
    return SUCCEEDED(result) && palette_count != 0
               ? upload_discard(context, gpu->png_palette_buffer, palette, palette_count * sizeof(u32))
               : result;
}

GpuStatus upload_png_inputs(MediaAtlasGpu* gpu, GpuDevice* device, GpuContext1* context,
                            const PngImage& image, ConstSlice<u8> scanlines) noexcept {
    const bool indexed = image.color_type == 3;
    return upload_png_shader_inputs(gpu, device, context, scanlines, image.inflated_bytes,
                                    indexed ? image.palette_rgba : nullptr,
                                    indexed ? image.palette_count : 0);
}

GpuStatus upload_gif_inputs(MediaAtlasGpu* gpu, GpuDevice* device, GpuContext1* context,
                            const GifFrame& frame, const GifPalette& palette,
                            ConstSlice<u8> indices) noexcept {
    return upload_png_shader_inputs(gpu, device, context, indices, static_cast<u32>(frame.width) * frame.height,
                                    palette.rgba, palette.count);
}

GpuStatus upload_animated_webp_inputs(MediaAtlasGpu* gpu, GpuDevice* device,
                                      GpuContext1* context, const AnimatedWebpFrame& frame,
                                      ConstSlice<u8> rgba8) noexcept {
    return upload_png_shader_inputs(gpu, device, context, rgba8,
                                     static_cast<u32>(frame.width) * frame.height * 4u, nullptr, 0);
}

void bind_compute_2srv(GpuContext1* context, GpuComputeShader* shader,
                       GpuBuffer* constants, GpuSrv* resource0,
                       GpuSrv* resource1,
                       GpuUav* target) noexcept {
    GpuSrv* resources[2] = {resource0, resource1};
    gpu_set_shader(context, shader);
    gpu_set_constants(context, GpuStage::compute, 0, 1, &constants);
    gpu_set_srvs(context, GpuStage::compute, 0, 2, resources);
    gpu_set_uavs(context, 0, 1, &target);
}

void unbind_compute_resources(GpuContext1* context) noexcept {
    GpuUav* null_uav = nullptr;
    GpuSrv* null_resources[2] = {nullptr, nullptr};
    GpuBuffer* null_buffer = nullptr;
    gpu_set_uavs(context, 0, 1, &null_uav);
    gpu_set_srvs(context, GpuStage::compute, 0, 2, null_resources);
    gpu_set_constants(context, GpuStage::compute, 0, 1, &null_buffer);
}

void unbind_compute(GpuContext1* context) noexcept {
    unbind_compute_resources(context);
    gpu_set_shader(context, static_cast<GpuComputeShader*>(nullptr));
}

void unbind_media_pixel_resources(GpuContext1* context) noexcept {
    GpuSrv* null_views[3]{};
    gpu_set_srvs(context, GpuStage::pixel, 0, 3, null_views);
}

void dispatch_png_shader(MediaAtlasGpu* gpu, GpuContext1* context,
                         const PngAtlasConstants& constants, GpuUav* target,
                         u32 width, u32 height) noexcept {
    gpu_update_buffer(context, gpu->png_constants, &constants, sizeof(constants));
    bind_compute_2srv(context, gpu->png_shader, gpu->png_constants, gpu->png_scanline_view,
                      gpu->png_palette_view, target);
    gpu_dispatch(context, (width + 7u) / 8u, (height + 7u) / 8u, 1);
    unbind_compute(context);
}

void dispatch_png(MediaAtlasGpu* gpu, GpuContext1* context, const PngImage& image,
                  u32 destination_x, u32 destination_y, ApngBlend blend) noexcept {
    const PngAtlasConstants constants{
        image.row_stride,       image.width,             image.height, image.bit_depth,
        image.color_type,       destination_x,           destination_y, image.palette_count,
        image.transparent_gray, image.transparent_red,   image.transparent_green,
        image.transparent_blue, {blend == ApngBlend::over ? 1u : 0u, 0, 0, 0},
    };
    dispatch_png_shader(gpu, context, constants, gpu->texture_uav, image.width, image.height);
}

void dispatch_gif(MediaAtlasGpu* gpu, GpuContext1* context, const GifFrame& frame,
                  const GifPalette& palette, u32 destination_x, u32 destination_y) noexcept {
    const PngAtlasConstants constants{
        frame.width, frame.width, frame.height, 8, 8, destination_x, destination_y, palette.count, 0, 0,
        0,           0,           {1, 0, 0, 0},
    };
    dispatch_png_shader(gpu, context, constants, gpu->texture_uav, frame.width, frame.height);
}

void dispatch_animated_webp(MediaAtlasGpu* gpu, GpuContext1* context,
                            const AnimatedWebpFrame& frame, u32 destination_x, u32 destination_y) noexcept {
    const PngAtlasConstants constants{
        static_cast<u32>(frame.width) * 4u,
        frame.width,
        frame.height,
        8,
        7,
        destination_x,
        destination_y,
        0,
        0,
        0,
        0,
        0,
        {frame.blend == AnimatedWebpBlend::over ? 1u : 0u, 0, 0, 0},
    };
    dispatch_png_shader(gpu, context, constants, gpu->texture_uav, frame.width, frame.height);
}

GpuStatus ensure_apng_backup(MediaAtlasGpu* gpu, GpuDevice* device, GpuContext1* context,
                             MediaAtlas* atlas, u32 required_bottom) noexcept {
    if (atlas) {
        atlas->disposal_backup_used_ms = atlas->growth.now_ms;
    }
    u32 current = 0;
    GpuTextureInfo info{};
    if (gpu->apng_backup_texture) {
        gpu_texture_info(gpu->apng_backup_texture, &info);
        current = gpu_texture_info_height(info);
    }
    if (current >= required_bottom) {
        return S_OK;
    }
    gpu_texture_info(gpu->texture, &info);
    const u32 height = minimum<u32>(gpu_texture_info_height(info), (required_bottom + 255u) & ~255u);
    if (height < required_bottom) {
        return E_INVALIDARG;
    }
    GpuTexture* replacement = nullptr;
    const GpuStatus result = gpu_texture_create_from_info(device, info, height, true, &replacement);
    if (FAILED(result)) {
        return result;
    }
    if (current) {
        gpu_copy_region(context, replacement, 0, 0, gpu->apng_backup_texture,
                        {0, 0, gpu_texture_info_width(info), current});
    }
    gpu_release(gpu->apng_backup_texture);
    gpu->apng_backup_texture = replacement;
    return S_OK;
}

void unbind_apng_atlas(GpuContext1* context) noexcept {
    GpuUav* null_uav = nullptr;
    unbind_media_pixel_resources(context);
    gpu_set_uavs(context, 0, 1, &null_uav);
}

void copy_animation_region(GpuContext1* context, GpuTexture* destination,
                           GpuTexture* source, const MediaAtlasResult& allocation,
                           u32 x_offset, u32 y_offset, u32 width, u32 height) noexcept {
    const GpuBox box{
        allocation.x + x_offset,
        allocation.y + y_offset,
        allocation.x + x_offset + width,
        allocation.y + y_offset + height,
    };
    gpu_copy_region(context, destination, box.left, box.top, source, box);
}

void clear_animation_region(MediaAtlasGpu* gpu, GpuContext1* context,
                            const MediaAtlasResult& allocation, u32 x_offset, u32 y_offset,
                            u32 width, u32 height, const float* color) noexcept {
    const GpuRect rectangle{
        static_cast<i32>(allocation.x + x_offset),
        static_cast<i32>(allocation.y + y_offset),
        static_cast<i32>(allocation.x + x_offset + width),
        static_cast<i32>(allocation.y + y_offset + height),
    };
    gpu_clear_rtv_rect(context, gpu->target, color, rectangle);
}

void account_apng_dispatch(MediaAtlasGpu* gpu, const ApngFrame& frame, const PngImage& image) noexcept {
    ++gpu->apng_frames;
    if (frame.blend == ApngBlend::over) {
        ++gpu->apng_over_blends;
    } else {
        ++gpu->apng_source_blends;
    }
    const u64 pixels = static_cast<u64>(image.width) * image.height;
    saturating_accumulate(gpu->apng_dispatched_pixels, pixels);
    saturating_accumulate(gpu->apng_uploaded_bytes, image.inflated_bytes);
}

void account_gif_dispatch(MediaAtlasGpu* gpu, const GifFrame& frame) noexcept {
    ++gpu->gif_frames;
    const u64 pixels = static_cast<u64>(frame.width) * frame.height;
    saturating_accumulate(gpu->gif_uploaded_bytes, pixels);
    saturating_accumulate(gpu->gif_dispatched_pixels, pixels);
}

void premultiplied_clear_color(u32 rgba, float* color) noexcept {
    const u32 alpha = rgba >> 24;
    color[0] = static_cast<float>(((rgba & 255u) * alpha + 127u) / 255u) / 255.0f;
    color[1] = static_cast<float>((((rgba >> 8) & 255u) * alpha + 127u) / 255u) / 255.0f;
    color[2] = static_cast<float>((((rgba >> 16) & 255u) * alpha + 127u) / 255u) / 255.0f;
    color[3] = static_cast<float>(alpha) / 255.0f;
}

void clear_animated_webp_canvas(MediaAtlasGpu* gpu, GpuContext1* context,
                                const MediaAtlasResult& allocation, u32 background_rgba) noexcept {
    float color[4]{};
    premultiplied_clear_color(background_rgba, color);
    clear_animation_region(gpu, context, allocation, 0, 0, allocation.width, allocation.height, color);
}

void clear_animated_webp_region(MediaAtlasGpu* gpu, GpuContext1* context,
                                const MediaAtlasResult& allocation, const AnimatedWebpFrame& frame,
                                u32 background_rgba) noexcept {
    float color[4]{};
    premultiplied_clear_color(background_rgba, color);
    clear_animation_region(gpu, context, allocation, frame.x_offset, frame.y_offset, frame.width,
                           frame.height, color);
}

void account_animated_webp_dispatch(MediaAtlasGpu* gpu, const AnimatedWebpFrame& frame,
                                    u64 uploaded_bytes) noexcept {
    ++gpu->animated_webp_frames;
    if (frame.blend == AnimatedWebpBlend::source) {
        ++gpu->animated_webp_source_blends;
    } else {
        ++gpu->animated_webp_over_blends;
    }
    const u64 pixels = static_cast<u64>(frame.width) * frame.height;
    saturating_accumulate(gpu->animated_webp_uploaded_bytes, uploaded_bytes);
    saturating_accumulate(gpu->animated_webp_dispatched_pixels, pixels);
}

bool vp8_atlas_frame_ready(const MediaAtlasGpu* gpu, const Vp8Keyframe& frame,
                           ConstSlice<u8> alpha) noexcept {
    const u64 pixel_count = static_cast<u64>(frame.width) * frame.height;
    return gpu && gpu->vp8_yuv_view && frame.width != 0 && frame.height != 0 &&
           gpu->vp8_width == frame.width && gpu->vp8_height == frame.height &&
           gpu->vp8_last_filtered_reconstruction == gpu->vp8_reconstructions &&
           gpu->vp8_last_atlas_reconstruction != gpu->vp8_reconstructions &&
           (alpha.count == 0 || (alpha.data && alpha.count == pixel_count));
}

GpuStatus upload_vp8_alpha(MediaAtlasGpu* gpu, GpuDevice* device, GpuContext1* context,
                           ConstSlice<u8> alpha) noexcept {
    GpuStatus result = create_vp8_atlas_resources(gpu, device);
    if (SUCCEEDED(result) && alpha.count != 0) {
        result = ensure_dynamic_raw_buffer(device, static_cast<u32>(alpha.count), &gpu->png_scanline_buffer,
                                           &gpu->png_scanline_view, &gpu->png_scanline_capacity);
        if (SUCCEEDED(result)) {
            result = upload_discard(context, gpu->png_scanline_buffer, alpha.data, static_cast<u32>(alpha.count));
        }
    }
    return result;
}

void dispatch_vp8_atlas(MediaAtlasGpu* gpu, GpuContext1* context, const Vp8Keyframe& frame,
                        ConstSlice<u8> alpha, u32 destination_x, u32 destination_y,
                        bool blend_over, GpuUav* texture_uav) noexcept {
    const u32 luma_samples = static_cast<u32>(gpu->vp8_luma_stride) * gpu->vp8_luma_rows;
    const u32 chroma_samples = static_cast<u32>(gpu->vp8_chroma_stride) * gpu->vp8_chroma_rows;
    const Vp8AtlasConstants constants{
        frame.width,
        frame.height,
        gpu->vp8_luma_stride,
        gpu->vp8_chroma_stride,
        luma_samples,
        luma_samples + chroma_samples,
        destination_x,
        destination_y,
        alpha.count != 0 ? 1u : 0u,
        frame.width,
        blend_over ? 1u : 0u,
        {0, 0, 0, 0, 0},
    };
    gpu_update_buffer(context, gpu->vp8_atlas_constants, &constants, sizeof(constants));
    bind_compute_2srv(context, gpu->vp8_atlas_shader, gpu->vp8_atlas_constants,
                      gpu->vp8_yuv_view, alpha.count != 0 ? gpu->png_scanline_view : nullptr,
                      texture_uav);
    gpu_dispatch(context, (frame.width + 7u) / 8u, (frame.height + 7u) / 8u, 1);
    unbind_compute(context);

    gpu->vp8_last_atlas_reconstruction = gpu->vp8_reconstructions;
    ++gpu->vp8_atlas_uploads;
    const u64 pixel_count = static_cast<u64>(frame.width) * frame.height;
    saturating_accumulate(gpu->vp8_atlas_pixels, pixel_count);
    if (alpha.count != 0) {
        ++gpu->vp8_alpha_uploads;
        saturating_accumulate(gpu->vp8_alpha_uploaded_bytes, alpha.count);
    }
}

} // namespace maicord::media_atlas_gpu_detail
#pragma clang attribute pop
