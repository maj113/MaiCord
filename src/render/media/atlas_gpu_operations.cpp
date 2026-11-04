#include "maicord/render/media/atlas_gpu.hpp"

#include "atlas_gpu_internal.hpp"
// Device command recording, once per image upload: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {

using namespace media_atlas_gpu_detail;

MediaAtlasGpuResult media_atlas_gpu_present_video(MediaAtlasGpu* gpu, MediaAtlas* atlas,
    GpuDevice* device, GpuContext1* context, MediaAssetKey key,
    const GpuVideoFrame& frame, bool preview, u32 maximum_extent) noexcept {
    if (!gpu || !atlas || !device || !context || !media_asset_key_valid(key) ||
        !frame.texture || !gpu_video_frame_visible_valid(frame.width, frame.height)) {
        return failure(MediaAtlasGpuError::invalid_input);
    }
    const auto [target, uav, width, height] = upload_surface(gpu, preview);
    if (!uav || !target || width != atlas->allocations.width || height != atlas->allocations.height) {
        return failure(MediaAtlasGpuError::invalid_input);
    }
    // A poster entry holds only the reduced picture, never the native frame,
    // already turned upright for a rotated container track.
    const u32 step = gpu_video_frame_reduction(frame.width, frame.height, maximum_extent);
    const MediaAtlasResult allocation = allocate_and_clear_surface(gpu, atlas, context, target,
        width, height, key, static_cast<u16>(gpu_video_frame_output_width(frame, step)),
        static_cast<u16>(gpu_video_frame_output_height(frame, step)), false);
    if (!allocation.ok()) return failure(MediaAtlasGpuError::atlas_failure, S_OK, allocation);
    const GpuStatus status = video_frame_gpu_present(&gpu->video, device, context, frame, uav,
                                                   allocation.x, allocation.y, step);
    return SUCCEEDED(status) ? MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none}
                              : failure(MediaAtlasGpuError::device_failure, status, allocation);
}

MediaAtlasGpuResult media_atlas_gpu_initialize(MediaAtlasGpu* gpu, GpuDevice* device,
                                               GpuContext1* context, u16 width,
                                               u16 height) noexcept {
    if (!gpu || !device || !context || !dimensions_valid(width, height) || gpu->texture) {
        return reject(gpu, MediaAtlasGpuError::invalid_input);
    }
    const GpuStatus result = create_media_surface(device, context, width, height, &gpu->texture,
                                                &gpu->texture_view, &gpu->target, &gpu->texture_uav);

    if (FAILED(result)) {
        media_atlas_gpu_release(gpu);
        return failure(MediaAtlasGpuError::device_failure, result);
    }
    gpu->width = width;
    gpu->height = height;
    return MediaAtlasGpuResult{{}, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_initialize_preview(MediaAtlasGpu* gpu, GpuDevice* device,
                                                       GpuContext1* context, u16 width,
                                                       u16 height) noexcept {
    if (!gpu || !device || !context || !gpu->texture || !dimensions_valid(width, height) ||
        gpu->preview_texture) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    const GpuStatus result =
        create_media_surface(device, context, width, height, &gpu->preview_texture,
                             &gpu->preview_texture_view, &gpu->preview_target, &gpu->preview_texture_uav);
    if (FAILED(result)) {
        gpu_release(gpu->preview_texture_uav);
        gpu_release(gpu->preview_target);
        gpu_release(gpu->preview_texture_view);
        gpu_release(gpu->preview_texture);
        return reject(gpu, MediaAtlasGpuError::device_failure, result);
    }
    gpu->preview_width = width;
    gpu->preview_height = height;
    return MediaAtlasGpuResult{{}, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_upload_straight_rgba8(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key, u16 width, u16 height,
    u32 row_pitch, ConstSlice<u8> straight_rgba8, bool preview) noexcept {
    const auto [target, texture_uav, surface_width, surface_height] = upload_surface(gpu, preview);
    const u64 minimum_pitch = static_cast<u64>(width) * 4;
    const u64 required = height == 0 ? 0 : static_cast<u64>(height - 1) * row_pitch + minimum_pitch;
    if ((preview && !media_asset_large_preview(key)) || !gpu || !atlas || !device || !context || !target ||
        !texture_uav ||
        !media_asset_key_valid(key) || width == 0 || height == 0 ||
        width > kPngDimensionMaximum || height > kPngDimensionMaximum || row_pitch < minimum_pitch ||
        required > straight_rgba8.count || (!straight_rgba8.data && straight_rgba8.count != 0) ||
        surface_width != atlas->allocations.width || surface_height != atlas->allocations.height ||
        static_cast<u32>(width) + atlas->allocations.padding * 2u > atlas->allocations.width ||
        static_cast<u32>(height) + atlas->allocations.padding * 2u > atlas->allocations.height) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }

    const GpuStatus result = upload_png_shader_inputs(gpu, device, context, straight_rgba8,
                                                    static_cast<u32>(required), nullptr, 0);
    if (FAILED(result)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, result);
    }

    const MediaAtlasResult allocation = allocate_and_clear_surface(
        gpu, atlas, context, target, surface_width, surface_height, key, width, height);
    if (!allocation.ok()) {
        return reject(gpu, MediaAtlasGpuError::atlas_failure, S_OK, allocation);
    }
    const PngAtlasConstants constants{row_pitch, width, height, 8, 7, allocation.x, allocation.y, 0,
                                       0,         0,     0,      0, {}};
    dispatch_png_shader(gpu, context, constants, texture_uav, width, height);

    ++gpu->straight_rgba8_uploads;
    saturating_accumulate(gpu->straight_rgba8_uploaded_bytes, required);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_upload_png(MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
                                               GpuContext1* context, MediaAssetKey key,
                                               const PngImage& image, ConstSlice<u8> reconstructed_scanlines,
                                               bool preview) noexcept {
    const auto [target, texture_uav, surface_width, surface_height] = upload_surface(gpu, preview);
    if ((preview && !media_asset_large_preview(key)) || !gpu || !atlas || !device || !context || !target ||
        !texture_uav || !media_asset_key_valid(key) ||
        !png_format_valid(image) || !png_scanlines_reconstructed(image, reconstructed_scanlines) ||
        surface_width != atlas->allocations.width || surface_height != atlas->allocations.height ||
        image.width + atlas->allocations.padding * 2u > atlas->allocations.width ||
        image.height + atlas->allocations.padding * 2u > atlas->allocations.height) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    const GpuStatus result = upload_png_inputs(gpu, device, context, image, reconstructed_scanlines);
    if (FAILED(result)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, result);
    }

    const MediaAtlasResult allocation =
        allocate_and_clear_surface(gpu, atlas, context, target, surface_width, surface_height, key,
                                   static_cast<u16>(image.width), static_cast<u16>(image.height));
    if (!allocation.ok()) {
        return reject(gpu, MediaAtlasGpuError::atlas_failure, S_OK, allocation);
    }
    const PngAtlasConstants constants{
        image.row_stride,
        image.width,
        image.height,
        image.bit_depth,
        image.color_type,
        allocation.x,
        allocation.y,
        image.palette_count,
        image.transparent_gray,
        image.transparent_red,
        image.transparent_green,
        image.transparent_blue,
        {},
    };
    dispatch_png_shader(gpu, context, constants, texture_uav, image.width, image.height);

    ++gpu->png_uploads;
    saturating_accumulate(gpu->png_uploaded_bytes, image.inflated_bytes);
    const u64 pixels = static_cast<u64>(image.width) * image.height;
    saturating_accumulate(gpu->png_dispatched_pixels, pixels);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_upload_jpeg(MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
                                                GpuContext1* context, MediaAssetKey key,
                                                const JpegImage& image,
                                                const JpegCoefficientWorkspace& coefficients,
                                                bool preview) noexcept {
    const auto [target, texture_uav, surface_width, surface_height] = upload_surface(gpu, preview);
    if ((preview && !media_asset_large_preview(key)) || !gpu || !atlas || !device || !context || !target ||
        !texture_uav || !media_asset_key_valid(key) ||
        !jpeg_image_valid(image) || surface_width != atlas->allocations.width ||
        surface_height != atlas->allocations.height ||
        static_cast<u32>(jpeg_display_width(image)) + atlas->allocations.padding * 2u >
            atlas->allocations.width ||
        static_cast<u32>(jpeg_display_height(image)) + atlas->allocations.padding * 2u >
            atlas->allocations.height) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }

    JpegIdctConstants idct_constants{};
    idct_constants.coefficient_block_count = image.coefficient_blocks;
    idct_constants.component_count = image.component_count;
    JpegAtlasConstants atlas_constants{};
    u32 sample_count = 0;
    for (u32 index = 0; index < image.component_count; ++index) {
        const JpegComponent& component = image.components[index];
        const u32 stride = component.block_columns * 8u;
        const u32 count = stride * component.block_rows * 8u;
        if (count > ~0u - sample_count) {
            return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG);
        }
        idct_constants.components[index] = JpegIdctComponentConstants{
            component.block_offset, component.block_count, component.block_columns,
            sample_count,           stride,                component.quantization_table,
        };
        atlas_constants.components[index] = JpegAtlasComponentConstants{
            sample_count,
            stride,
            (static_cast<u32>(image.width) * component.horizontal_sampling + image.maximum_horizontal_sampling - 1u) /
                image.maximum_horizontal_sampling,
            (static_cast<u32>(image.height) * component.vertical_sampling + image.maximum_vertical_sampling - 1u) /
                image.maximum_vertical_sampling,
            component.horizontal_sampling,
            component.vertical_sampling,
        };
        sample_count += count;
    }
    const u32 coefficient_bytes = image.coefficient_blocks * sizeof(JpegCoefficientBlock);
    const u32 quantization_bytes = kJpegTableMaximum * 64u * sizeof(u16);
    const JpegIccProfile* icc_profile =
        image.tables->icc && image.tables->icc->profile.disposition == JpegIccDisposition::applied
            ? &image.tables->icc->profile
            : nullptr;
    const u32 icc_curve_bytes = icc_profile ? icc_profile->entry_count * sizeof(u16) : 0;
    const u32 quantization_and_curve_bytes = quantization_bytes + icc_curve_bytes;
    const u32 sample_bytes = sample_count * sizeof(u32);

    GpuStatus result = create_jpeg_resources(gpu, device);
    if (SUCCEEDED(result)) {
        result = ensure_dynamic_raw_buffer(device, coefficient_bytes, &gpu->vp8_coefficient_buffer,
                                           &gpu->vp8_coefficient_view, &gpu->vp8_coefficient_capacity);
    }
    if (SUCCEEDED(result)) {
        result = ensure_dynamic_raw_buffer(device, quantization_and_curve_bytes, &gpu->vp8_mode_buffer,
                                           &gpu->vp8_mode_view, &gpu->vp8_mode_capacity);
    }
    if (SUCCEEDED(result)) {
        result = ensure_vp8_yuv_buffer(gpu, device, sample_bytes);
    }
    if (FAILED(result)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, result);
    }

    result = upload_discard(context, gpu->vp8_coefficient_buffer, coefficients.blocks,
                            coefficient_bytes);
    if (SUCCEEDED(result)) {
        void* mapped = gpu_map(context, gpu->vp8_mode_buffer, GpuMap::discard, &result);
        if (mapped) {
            u16* destination = static_cast<u16*>(mapped);
            for (u32 table = 0; table < kJpegTableMaximum; ++table) {
                for (u32 index = 0; index < 64; ++index) {
                    destination[table * 64u + index] = image.tables->quantization[table].present
                                                           ? image.tables->quantization[table].values[index]
                                                           : 0;
                }
            }
            if (icc_profile) {
                const u32 curve_base = quantization_bytes / sizeof(u16);
                CopyMemory(destination + curve_base, icc_profile->entries,
                           static_cast<usize>(icc_profile->entry_count) * sizeof(u16));
            }
            gpu_unmap(context, gpu->vp8_mode_buffer);
        }
    }
    if (FAILED(result)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, result);
    }

    const u16 display_width = jpeg_display_width(image);
    const u16 display_height = jpeg_display_height(image);
    const MediaAtlasResult allocation = allocate_and_clear_surface(
        gpu, atlas, context, target, surface_width, surface_height, key, display_width, display_height);
    if (!allocation.ok()) {
        return reject(gpu, MediaAtlasGpuError::atlas_failure, S_OK, allocation);
    }

    gpu_update_buffer(context, gpu->jpeg_idct_constants, &idct_constants, sizeof(idct_constants));
    bind_compute_2srv(context, gpu->jpeg_idct_shader, gpu->jpeg_idct_constants,
                      gpu->vp8_coefficient_view, gpu->vp8_mode_view, gpu->vp8_yuv_uav);
    gpu_dispatch(context, image.coefficient_blocks, 1, 1);
    unbind_compute_resources(context);

    atlas_constants.image_width = image.width;
    atlas_constants.image_height = image.height;
    atlas_constants.destination_x = allocation.x;
    atlas_constants.destination_y = allocation.y;
    atlas_constants.color_transform_orientation =
        static_cast<u8>(image.color_transform) | static_cast<u32>(static_cast<u8>(image.orientation)) << 8u;
    atlas_constants.maximum_horizontal_sampling = image.maximum_horizontal_sampling;
    atlas_constants.maximum_vertical_sampling = image.maximum_vertical_sampling;
    atlas_constants.component_count = image.component_count;
    if (icc_profile) {
        const u32 curve_base = quantization_bytes / sizeof(u16);
        atlas_constants.icc_transform_kind = static_cast<u8>(icc_profile->transform_kind);
        atlas_constants.icc_pcs = static_cast<u8>(icc_profile->pcs);
        for (u32 row = 0; row < 3; ++row) {
            for (u32 column = 0; column < 3; ++column) {
                atlas_constants.icc_matrix[row * 4u + column] =
                    icc_profile->to_linear_srgb[row * 3u + column];
            }
        }
        if (icc_profile->transform_kind == JpegIccTransformKind::matrix_trc) {
            atlas_constants.icc_input_channels = 3;
            for (u32 channel = 0; channel < 3; ++channel) {
                store_jpeg_icc_curve(&atlas_constants, channel, icc_profile->curves[channel], curve_base);
            }
        } else if (icc_profile->transform_kind == JpegIccTransformKind::gray_trc) {
            atlas_constants.icc_input_channels = 1;
            store_jpeg_icc_curve(&atlas_constants, 0, icc_profile->curves[0], curve_base);
        } else {
            const JpegIccLut& lut = icc_profile->lut;
            atlas_constants.icc_input_channels = lut.input_channels;
            atlas_constants.icc_clut_offset = curve_base + lut.clut_offset;
            atlas_constants.icc_clut_entry_count = lut.clut_entry_count;
            atlas_constants.icc_grid0 = lut.grid_points[0];
            atlas_constants.icc_grid1 = lut.grid_points[1];
            atlas_constants.icc_grid2 = lut.grid_points[2];
            atlas_constants.icc_grid3 = lut.grid_points[3];
            atlas_constants.icc_precision_bytes = lut.precision_bytes;
            for (u32 channel = 0; channel < lut.input_channels; ++channel) {
                store_jpeg_icc_curve(&atlas_constants, channel, lut.input_curves[channel], curve_base);
            }
            for (u32 channel = 0; channel < 3; ++channel) {
                store_jpeg_icc_curve(&atlas_constants, kJpegIccInputChannelMaximum + channel,
                                     lut.output_curves[channel], curve_base);
            }
        }
    }
    gpu_update_buffer(context, gpu->jpeg_atlas_constants, &atlas_constants, sizeof(atlas_constants));
    bind_compute_2srv(context, gpu->jpeg_atlas_shader, gpu->jpeg_atlas_constants,
                      gpu->vp8_yuv_view, gpu->vp8_mode_view, texture_uav);
    gpu_dispatch(context, (display_width + 7u) / 8u, (display_height + 7u) / 8u, 1);
    unbind_compute(context);

    ++gpu->jpeg_uploads;
    ++gpu->jpeg_idct_dispatches;
    ++gpu->jpeg_atlas_dispatches;
    const u64 uploaded = static_cast<u64>(coefficient_bytes) + quantization_and_curve_bytes;
    saturating_accumulate(gpu->jpeg_uploaded_bytes, uploaded);
    saturating_accumulate(gpu->jpeg_reconstructed_pixels, sample_count);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

namespace {

struct SurfaceSlot {
    GpuTexture** texture;
    GpuSrv** view;
    GpuRtv** target;
    GpuUav** uav;
    GpuTexture** backup;
    u16* width;
    u16* height;
};

SurfaceSlot surface_slot(MediaAtlasGpu* gpu, bool preview) noexcept {
    return preview ? SurfaceSlot{&gpu->preview_texture, &gpu->preview_texture_view, &gpu->preview_target,
                                 &gpu->preview_texture_uav, &gpu->preview_animation_backup_texture,
                                 &gpu->preview_width, &gpu->preview_height}
                   : SurfaceSlot{&gpu->texture, &gpu->texture_view, &gpu->target, &gpu->texture_uav,
                                 &gpu->apng_backup_texture, &gpu->width, &gpu->height};
}

// Old -> new rows: the kept prefix, then each trim move (sources and
// destinations already checked against both heights by the caller).
void copy_rows(GpuContext1* context, GpuTexture* destination, GpuTexture* source, u32 width,
               u32 prefix, ConstSlice<GlyphAtlasRowMove> moves, u32 source_height, u32 destination_height) noexcept {
    if (prefix) {
        gpu_copy_region(context, destination, 0, 0, source, {0, 0, width, prefix});
    }
    for (u32 index = 0; index < moves.count; ++index) {
        const GlyphAtlasRowMove& move = moves[index];
        if (static_cast<u32>(move.source_y) + move.height > source_height ||
            static_cast<u32>(move.destination_y) + move.height > destination_height) {
            continue;
        }
        gpu_copy_region(context, destination, 0, move.destination_y, source,
                        {0, move.source_y, width, static_cast<u32>(move.source_y + move.height)});
    }
}

} // namespace

MediaAtlasGpuResult media_atlas_gpu_resize(MediaAtlasGpu* gpu, GpuDevice* device,
                                           GpuContext1* context, bool preview, u16 height,
                                           ConstSlice<GlyphAtlasRowMove> moves) noexcept {
    if (!gpu || !device || !context || (!moves.data && moves.count)) {
        return failure(MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    const SurfaceSlot slot = surface_slot(gpu, preview);
    if (!*slot.texture) {
        return MediaAtlasGpuResult{{}, S_OK, MediaAtlasGpuError::none}; // Created lazily at the atlas height.
    }
    const u16 width = *slot.width;
    const u16 old_height = *slot.height;
    if (height == old_height && moves.count == 0) {
        return MediaAtlasGpuResult{{}, S_OK, MediaAtlasGpuError::none};
    }
    if (!dimensions_valid(width, height)) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    GpuTexture* texture = nullptr;
    GpuSrv* view = nullptr;
    GpuRtv* target = nullptr;
    GpuUav* uav = nullptr;
    const GpuStatus result = create_media_surface(device, context, width, height, &texture, &view, &target, &uav);
    if (FAILED(result)) {
        gpu_release(uav);
        gpu_release(target);
        gpu_release(view);
        gpu_release(texture);
        return reject(gpu, MediaAtlasGpuError::device_failure, result);
    }
    unbind_apng_atlas(context);
    copy_rows(context, texture, *slot.texture, width, minimum<u32>(old_height, height), moves, old_height, height);
    if (*slot.backup) {
        GpuTextureInfo info{};
        gpu_texture_info(*slot.backup, &info);
        const u32 backup_height = gpu_texture_info_height(info);
        u32 next_height = minimum<u32>(backup_height, height);
        for (u32 index = 0; index < moves.count; ++index) {
            if (static_cast<u32>(moves[index].source_y) + moves[index].height <= backup_height)
                next_height = maximum<u32>(next_height, static_cast<u32>(moves[index].destination_y) + moves[index].height);
        }
        next_height = minimum<u32>(next_height, height);
        GpuTexture* backup = nullptr;
        if (next_height && SUCCEEDED(gpu_texture_create_from_info(device, info, next_height, false, &backup))) {
            copy_rows(context, backup, *slot.backup, width, minimum(backup_height, next_height), moves, backup_height,
                      next_height);
        }
        // A failed backup allocation only loses one pending previous-disposal state.
        gpu_release(*slot.backup);
        *slot.backup = backup;
    }
    gpu_release(*slot.uav);
    gpu_release(*slot.target);
    gpu_release(*slot.view);
    gpu_release(*slot.texture);
    *slot.texture = texture;
    *slot.view = view;
    *slot.target = target;
    *slot.uav = uav;
    *slot.height = height;
    return MediaAtlasGpuResult{{}, S_OK, MediaAtlasGpuError::none};
}

void media_atlas_gpu_release_backup(MediaAtlasGpu* gpu, bool preview) noexcept {
    if (gpu) {
        gpu_release(*surface_slot(gpu, preview).backup);
    }
}

} // namespace maicord
#pragma clang attribute pop
