#include "maicord/render/media/atlas_gpu.hpp"

#include "atlas_gpu_internal.hpp"
// Device command recording, once per VP8 keyframe: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {

using namespace media_atlas_gpu_detail;

namespace {
// D3D11.1 constant-buffer ranges start at 256-byte boundaries. One upload owns
// all dispatch constants; changes of image geometry reuse the same GPU buffer.
struct Vp8TileWaveConstants { u32 diagonal, first_row, rank_span, reserved; u32 padding[60]; };
static_assert(sizeof(Vp8TileWaveConstants) == 256);
static_assert(kVp8MacroblockDimensionMaximum <= 32);
// Waves: ceil((columns + 2 * rows - 2) / 8) + ceil(rows / 2) - 1 diagonals at most.
static_assert((kVp8MacroblockDimensionMaximum * 3 + 5) / 8 + (kVp8MacroblockDimensionMaximum + 1) / 2 <= 32);
GpuStatus prepare_vp8_tiles(MediaAtlasGpu* gpu, GpuDevice* device,
                            GpuContext1* context, u32 columns, u32 rows) noexcept {
    if (gpu->vp8_tile_constants && gpu->vp8_tile_columns == columns && gpu->vp8_tile_rows == rows)
        return S_OK;
    constexpr u32 tile_rows = 2, span = 8;
    const u32 ranks = columns + 2 * rows - 2;
    const u32 rank_tiles = (ranks + span - 1) / span;
    const u32 row_tiles = (rows + tile_rows - 1) / tile_rows;
    if (!gpu->vp8_tile_constants) {
        const GpuStatus result = gpu_buffer_create(
            device, {32 * sizeof(Vp8TileWaveConstants), GpuUsage::dynamic, gpu_bind_constant, false}, nullptr,
            &gpu->vp8_tile_constants);
        if (FAILED(result)) return result;
    }
    // Only the first `count` 256-byte ranges are ever bound, so the rest stay unwritten.
    GpuStatus mapped_result = kGpuOk;
    void* mapped = gpu_map(context, gpu->vp8_tile_constants, GpuMap::discard, &mapped_result);
    if (!mapped) return FAILED(mapped_result) ? mapped_result : kGpuFail;
    auto* constants = static_cast<Vp8TileWaveConstants*>(mapped);
    u32 count = 0;
    for (u32 diagonal = 0; diagonal < rank_tiles + row_tiles - 1; ++diagonal) {
        const u32 first = diagonal >= rank_tiles ? diagonal - rank_tiles + 1 : 0;
        const u32 last = minimum(diagonal, row_tiles - 1);
        bool intersects = false;
        for (u32 row = first; row <= last; ++row) {
            const u32 y0 = row * tile_rows, y1 = minimum(y0 + tile_rows, rows);
            const u32 r0 = (diagonal - row) * span;
            intersects = intersects || (r0 < columns + 2 * (y1 - 1) && r0 + span > 2 * y0);
        }
        if (!intersects) continue;
        constants[count].diagonal = diagonal;
        constants[count].first_row = first;
        constants[count].rank_span = span;
        gpu->vp8_tile_groups[count++] = last - first + 1;
    }
    gpu_unmap(context, gpu->vp8_tile_constants);
    gpu->vp8_tile_columns = static_cast<u16>(columns);
    gpu->vp8_tile_rows = static_cast<u16>(rows);
    gpu->vp8_tile_wave_count = count;
    return S_OK;
}
} // namespace

MediaAtlasGpuResult media_atlas_gpu_reconstruct_vp8_keyframe(
    MediaAtlasGpu* gpu, GpuDevice* device, GpuContext1* context, const Vp8Keyframe& frame,
    const Vp8ModeWorkspace& modes, const Vp8CoefficientWorkspace& coefficients) noexcept {
    if (!gpu || !device || !context || !gpu->surface ||
        !vp8_reconstruction_valid(frame, modes, coefficients)) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }

    const u32 columns = modes.columns;
    const u32 rows = modes.rows;
    const u32 macroblock_count = modes.macroblock_count;
    const u32 mode_bytes = macroblock_count * sizeof(Vp8MacroblockMode);
    const u32 coefficient_bytes = macroblock_count * sizeof(Vp8MacroblockCoefficients);
    const u32 luma_stride = columns * 16u;
    const u32 luma_rows = rows * 16u;
    const u32 chroma_stride = columns * 8u;
    const u32 chroma_rows = rows * 8u;
    const u32 luma_samples = luma_stride * luma_rows;
    const u32 chroma_samples = chroma_stride * chroma_rows;
    const u32 yuv_samples = luma_samples + chroma_samples * 2u;
    const u32 yuv_bytes = yuv_samples * sizeof(u32) * 2u;

    GpuStatus result = create_vp8_resources(gpu, device);
    if (SUCCEEDED(result)) {
        result = ensure_dynamic_raw_buffer(device, mode_bytes, &gpu->vp8_mode_buffer, &gpu->vp8_mode_view,
                                           &gpu->vp8_mode_capacity);
    }
    if (SUCCEEDED(result)) {
        result = ensure_dynamic_raw_buffer(device, coefficient_bytes, &gpu->vp8_coefficient_buffer,
                                           &gpu->vp8_coefficient_view, &gpu->vp8_coefficient_capacity);
    }
    if (SUCCEEDED(result)) {
        result = ensure_vp8_yuv_buffer(gpu, device, yuv_bytes);
    }
    if (FAILED(result)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, result);
    }

    result = prepare_vp8_tiles(gpu, device, context, columns, rows);
    if (FAILED(result)) { return reject(gpu, MediaAtlasGpuError::device_failure, result); }
    result = upload_discard(context, gpu->vp8_mode_buffer, modes.macroblocks, mode_bytes);
    if (SUCCEEDED(result)) {
        result = upload_discard(context, gpu->vp8_coefficient_buffer, coefficients.macroblocks,
                                coefficient_bytes);
    }
    if (FAILED(result)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, result);
    }

    bind_compute_2srv(context, gpu->vp8_idct_shader, gpu->vp8_constants,
                      gpu->vp8_mode_view, gpu->vp8_coefficient_view, gpu->vp8_yuv_uav);

    const u32 u_plane_offset = luma_samples;
    const u32 v_plane_offset = luma_samples + chroma_samples;
    const Vp8ReconstructionConstants constants{
        columns,
        rows,
        luma_stride,
        chroma_stride,
        chroma_rows,
        u_plane_offset,
        v_plane_offset,
        sizeof(Vp8MacroblockMode),
        sizeof(Vp8MacroblockCoefficients),
        {0, 0, 0},
    };
    gpu_update_buffer(context, gpu->vp8_constants, &constants, sizeof(constants));
    gpu_dispatch(context, (macroblock_count * 24u + 63u) / 64u, 1, 1);
    gpu_set_shader(context, gpu->vp8_reconstruct_shader);
    for (u32 wave = 0; wave < gpu->vp8_tile_wave_count; ++wave) {
        gpu_set_constants_range(context, 1, gpu->vp8_tile_constants, wave * 16, 16);
        gpu_dispatch(context, gpu->vp8_tile_groups[wave], 1, 1);
    }
    GpuBuffer* no_tiles = nullptr;
    gpu_set_constants(context, GpuStage::compute, 1, 1, &no_tiles);
    unbind_compute(context);

    gpu->vp8_width = frame.width;
    gpu->vp8_height = frame.height;
    gpu->vp8_luma_stride = static_cast<u16>(luma_stride);
    gpu->vp8_luma_rows = static_cast<u16>(luma_rows);
    gpu->vp8_chroma_stride = static_cast<u16>(chroma_stride);
    gpu->vp8_chroma_rows = static_cast<u16>(chroma_rows);
    ++gpu->vp8_reconstructions;
    saturating_accumulate(gpu->vp8_reconstruction_dispatches, 1u + gpu->vp8_tile_wave_count);
    const u64 uploaded = static_cast<u64>(mode_bytes) + coefficient_bytes;
    saturating_accumulate(gpu->vp8_uploaded_bytes, uploaded);
    const u64 pixels = static_cast<u64>(frame.width) * frame.height;
    saturating_accumulate(gpu->vp8_reconstructed_pixels, pixels);
    return MediaAtlasGpuResult{{}, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_filter_vp8_keyframe(
    MediaAtlasGpu* gpu, GpuDevice* device, GpuContext1* context, const Vp8Keyframe& frame,
    const Vp8ControlHeader& header, const Vp8ModeWorkspace& modes,
    const Vp8CoefficientWorkspace& coefficients) noexcept {
    const u32 expected_luma_stride = ((static_cast<u32>(frame.width) + 15u) / 16u) * 16u;
    const u32 expected_luma_rows = ((static_cast<u32>(frame.height) + 15u) / 16u) * 16u;
    if (!gpu || !device || !context || !gpu->vp8_mode_view || !gpu->vp8_coefficient_view ||
        !gpu->vp8_yuv_uav || !vp8_reconstruction_valid(frame, modes, coefficients) ||
        !vp8_filter_header_valid(header) || gpu->vp8_width != frame.width ||
        gpu->vp8_height != frame.height || gpu->vp8_luma_stride != expected_luma_stride ||
        gpu->vp8_luma_rows != expected_luma_rows || gpu->vp8_reconstructions == 0 ||
        gpu->vp8_last_filtered_reconstruction == gpu->vp8_reconstructions) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }

    bool active = false;
    u32 filtered_edges = 0;
    for (u32 macroblock = 0; macroblock < modes.macroblock_count; ++macroblock) {
        const Vp8MacroblockMode& mode = modes.macroblocks[macroblock];
        if (vp8_macroblock_filter_level(header, mode) == 0) {
            continue;
        }
        active = true;
        const u32 macroblock_x = macroblock % modes.columns;
        const u32 macroblock_y = macroblock / modes.columns;
        const u32 plane_edges = header.loop_filter.simple ? 1u : 3u;
        filtered_edges += macroblock_x != 0 ? plane_edges : 0u;
        filtered_edges += macroblock_y != 0 ? plane_edges : 0u;
        const bool inner = mode.luma == Vp8IntraMode::subblocks ||
                           coefficients.macroblocks[macroblock].has_coefficients != 0;
        if (inner) {
            filtered_edges += header.loop_filter.simple ? 6u : 10u;
        }
    }
    if (!active) {
        gpu->vp8_last_filtered_reconstruction = gpu->vp8_reconstructions;
        ++gpu->vp8_filter_skips;
        return MediaAtlasGpuResult{{}, S_OK, MediaAtlasGpuError::none};
    }

    const GpuStatus result = create_vp8_filter_resources(gpu, device);
    if (FAILED(result)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, result);
    }
    const u32 luma_samples = static_cast<u32>(gpu->vp8_luma_stride) * gpu->vp8_luma_rows;
    const u32 chroma_samples = static_cast<u32>(gpu->vp8_chroma_stride) * gpu->vp8_chroma_rows;
    const Vp8FilterConstants constants{
        modes.columns,
        modes.rows,
        gpu->vp8_luma_stride,
        gpu->vp8_chroma_stride,
        luma_samples,
        luma_samples + chroma_samples,
        header.loop_filter.level,
        header.loop_filter.sharpness,
        header.loop_filter.simple,
        header.segmentation.enabled,
        header.segmentation.absolute_delta,
        header.loop_filter.use_delta,
        {header.segmentation.filter_strength[0], header.segmentation.filter_strength[1],
         header.segmentation.filter_strength[2], header.segmentation.filter_strength[3]},
        header.loop_filter.reference_delta[0],
        header.loop_filter.mode_delta[0],
        sizeof(Vp8MacroblockMode),
        sizeof(Vp8MacroblockCoefficients),
    };
    // vp8_filter.hlsl gives each wavefront rank (x + 2y) 16 thread rows and no
    // inner loop: a rank holds at most ceil(columns / 2) macroblocks.
    static_assert((kWebpDimensionMaximum + 15u) / 16u <= 32u, "VP8 filter rank exceeds 16 macroblocks");
    gpu_update_buffer(context, gpu->vp8_filter_constants, &constants, sizeof(constants));
    bind_compute_2srv(context, gpu->vp8_filter_shader, gpu->vp8_filter_constants,
                      gpu->vp8_mode_view, gpu->vp8_coefficient_view, gpu->vp8_yuv_uav);
    gpu_dispatch(context, 1, 1, 1);
    unbind_compute(context);

    gpu->vp8_last_filtered_reconstruction = gpu->vp8_reconstructions;
    ++gpu->vp8_filter_dispatches;
    saturating_accumulate(gpu->vp8_filtered_edges, filtered_edges);
    const u64 pixels = static_cast<u64>(frame.width) * frame.height;
    saturating_accumulate(gpu->vp8_filtered_pixels, pixels);
    return MediaAtlasGpuResult{{}, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_upload_vp8_keyframe(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key, const Vp8Keyframe& frame,
    ConstSlice<u8> alpha, bool preview) noexcept {
    if ((preview && !media_asset_large_preview(key)) || !gpu || !atlas || !device || !context ||
        !surface_ready(gpu, *atlas, preview) || !vp8_atlas_frame_ready(gpu, frame, alpha) ||
        !image_fits(*atlas, frame.width, frame.height)) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }

    const GpuStatus result = upload_vp8_alpha(gpu, device, context, alpha);
    if (FAILED(result)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, result);
    }

    MediaAtlasResult local{};
    const MediaAtlasResult allocation =
        allocate_and_clear_surface(gpu, atlas, context, preview, key, frame.width, frame.height, &local);
    if (!allocation.ok()) {
        return reject(gpu, MediaAtlasGpuError::atlas_failure, S_OK, allocation);
    }
    dispatch_vp8_atlas(gpu, context, frame, alpha, local.x, local.y, false, gpu->texture_uav);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_begin_animated_webp_vp8(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device, GpuContext1* context,
    MediaAssetKey key, u16 canvas_width, u16 canvas_height, u32 background_rgba,
    const AnimatedWebpFrame& animation_frame, const Vp8Keyframe& vp8_frame, ConstSlice<u8> alpha) noexcept {
    if (!animation_target_ready(gpu, atlas, device, context, key) ||
        !animation_canvas_fits(*atlas, canvas_width, canvas_height) ||
        !animated_webp_control_valid(animation_frame, canvas_width, canvas_height) ||
        animation_frame.codec != AnimatedWebpCodec::vp8_lossy || animation_frame.width != vp8_frame.width ||
        animation_frame.height != vp8_frame.height || animation_frame.has_alpha != (alpha.count != 0) ||
        !vp8_atlas_frame_ready(gpu, vp8_frame, alpha)) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    const GpuStatus uploaded = upload_vp8_alpha(gpu, device, context, alpha);
    if (FAILED(uploaded)) {
        return reject(gpu, MediaAtlasGpuError::device_failure, uploaded);
    }
    unbind_apng_atlas(context);
    MediaAtlasResult local{};
    const MediaAtlasResult allocation =
        allocate_and_clear_surface(gpu, atlas, context, false, key, canvas_width, canvas_height, &local);
    if (!allocation.ok()) {
        return reject(gpu, MediaAtlasGpuError::atlas_failure, S_OK, allocation);
    }
    clear_animated_webp_canvas(gpu, context, local, background_rgba);
    dispatch_vp8_atlas(gpu, context, vp8_frame, alpha, local.x + animation_frame.x_offset,
                       local.y + animation_frame.y_offset,
                       animation_frame.blend == AnimatedWebpBlend::over, gpu->texture_uav);
    account_animated_webp_dispatch(gpu, animation_frame, alpha.count);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

MediaAtlasGpuResult media_atlas_gpu_advance_animated_webp_vp8(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device, GpuContext1* context,
    MediaAssetKey key, u32 background_rgba, const AnimatedWebpFrame& previous_frame,
    const AnimatedWebpFrame& animation_frame, const Vp8Keyframe& vp8_frame, ConstSlice<u8> alpha) noexcept {
    const MediaAtlasResult allocation = atlas ? media_atlas_find(atlas, key) : MediaAtlasResult{};
    if (!animation_target_ready(gpu, atlas, device, context, key) || !allocation.ok() ||
        !animated_webp_control_valid(previous_frame, allocation.width, allocation.height) ||
        !animated_webp_control_valid(animation_frame, allocation.width, allocation.height) ||
        animation_frame.codec != AnimatedWebpCodec::vp8_lossy || animation_frame.width != vp8_frame.width ||
        animation_frame.height != vp8_frame.height || animation_frame.has_alpha != (alpha.count != 0) ||
        !vp8_atlas_frame_ready(gpu, vp8_frame, alpha)) {
        return reject(gpu, MediaAtlasGpuError::invalid_input, E_INVALIDARG, allocation);
    }
    MediaAtlasResult local = allocation;
    GpuStatus uploaded = upload_vp8_alpha(gpu, device, context, alpha);
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
    dispatch_vp8_atlas(gpu, context, vp8_frame, alpha, local.x + animation_frame.x_offset,
                       local.y + animation_frame.y_offset,
                       animation_frame.blend == AnimatedWebpBlend::over, gpu->texture_uav);
    account_animated_webp_dispatch(gpu, animation_frame, alpha.count);
    return MediaAtlasGpuResult{allocation, S_OK, MediaAtlasGpuError::none};
}

void media_atlas_gpu_release(MediaAtlasGpu* gpu) noexcept {
    if (!gpu) {
        return;
    }
    video_frame_gpu_release(&gpu->video);
    gpu_release(gpu->jpeg_atlas_shader);
    gpu_release(gpu->jpeg_atlas_constants);
    gpu_release(gpu->jpeg_idct_rows_shader);
    gpu_release(gpu->jpeg_idct_shader);
    gpu_release(gpu->jpeg_idct_constants);
    gpu_release(gpu->vp8_atlas_shader);
    gpu_release(gpu->vp8_atlas_constants);
    gpu_release(gpu->vp8_filter_shader);
    gpu_release(gpu->vp8_filter_constants);
    gpu_release(gpu->vp8_reconstruct_shader);
    gpu_release(gpu->vp8_idct_shader);
    gpu_release(gpu->vp8_constants);
    gpu_release(gpu->vp8_tile_constants);
    gpu->vp8_tile_columns = gpu->vp8_tile_rows = 0;
    gpu->vp8_tile_wave_count = 0;
    gpu_release(gpu->vp8_yuv_uav);
    gpu_release(gpu->vp8_yuv_view);
    gpu_release(gpu->vp8_yuv_buffer);
    gpu_release(gpu->vp8_coefficient_view);
    gpu_release(gpu->vp8_coefficient_buffer);
    gpu_release(gpu->vp8_mode_view);
    gpu_release(gpu->vp8_mode_buffer);
    gpu_release(gpu->png_shader);
    gpu_release(gpu->png_constants);
    gpu_release(gpu->png_palette_view);
    gpu_release(gpu->png_palette_buffer);
    gpu_release(gpu->png_scanline_view);
    gpu_release(gpu->png_scanline_buffer);
    gpu_paged_texture_release(&gpu->backup);
    gpu_paged_texture_release(&gpu->preview_backup);
    gpu_paged_texture_release(&gpu->preview_surface);
    gpu_paged_texture_release(&gpu->surface);
    gpu->texture_view = nullptr;
    gpu->preview_texture_view = nullptr;
    gpu->texture = nullptr;
    gpu->texture_uav = nullptr;
    gpu->backup_texture = nullptr;
    gpu->width = 0;
    gpu->height = 0;
    gpu->preview_width = 0;
    gpu->preview_height = 0;
    gpu->png_scanline_capacity = 0;
    gpu->vp8_mode_capacity = 0;
    gpu->vp8_coefficient_capacity = 0;
    gpu->vp8_yuv_capacity = 0;
    gpu->vp8_width = 0;
    gpu->vp8_height = 0;
    gpu->vp8_luma_stride = 0;
    gpu->vp8_luma_rows = 0;
    gpu->vp8_chroma_stride = 0;
    gpu->vp8_chroma_rows = 0;
    gpu->vp8_last_filtered_reconstruction = 0;
    gpu->vp8_last_atlas_reconstruction = 0;
}

} // namespace maicord
#pragma clang attribute pop
