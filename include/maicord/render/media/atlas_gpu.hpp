#pragma once

#include "maicord/render/media/atlas.hpp"
#include "maicord/render/media/video_frame_gpu.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/gpu/gpu.hpp"

namespace maicord {
struct PngImage;

struct Vp8CoefficientWorkspace;
struct Vp8ControlHeader;
struct Vp8Keyframe;
struct Vp8ModeWorkspace;

struct AnimatedWebpFrame;
struct ApngFrame;
struct GifFrame;
struct GifPalette;
struct JpegCoefficientWorkspace;
struct JpegImage;

struct MediaAtlasGpu {
    VideoFrameGpu video;
    GpuTexture* texture;
    GpuTexture* preview_texture;
    GpuTexture* apng_backup_texture;
    GpuTexture* preview_animation_backup_texture;
    GpuSrv* texture_view;
    GpuSrv* preview_texture_view;
    GpuRtv* target;
    GpuRtv* preview_target;
    GpuUav* texture_uav;
    GpuUav* preview_texture_uav;
    GpuBuffer* png_scanline_buffer;
    GpuSrv* png_scanline_view;
    GpuBuffer* png_palette_buffer;
    GpuSrv* png_palette_view;
    GpuBuffer* png_constants;
    GpuComputeShader* png_shader;
    GpuBuffer* vp8_mode_buffer;
    GpuSrv* vp8_mode_view;
    GpuBuffer* vp8_coefficient_buffer;
    GpuSrv* vp8_coefficient_view;
    GpuBuffer* vp8_yuv_buffer;
    GpuSrv* vp8_yuv_view;
    GpuUav* vp8_yuv_uav;
    GpuBuffer* vp8_constants;
    GpuBuffer* vp8_tile_constants;
    u16 vp8_tile_columns;
    u16 vp8_tile_rows;
    u32 vp8_tile_wave_count;
    u32 vp8_tile_groups[32];
    GpuComputeShader* vp8_reconstruct_shader;
    GpuComputeShader* vp8_idct_shader;
    GpuBuffer* vp8_filter_constants;
    GpuComputeShader* vp8_filter_shader;
    GpuBuffer* vp8_atlas_constants;
    GpuComputeShader* vp8_atlas_shader;
    GpuBuffer* jpeg_idct_constants;
    GpuComputeShader* jpeg_idct_shader;
    GpuBuffer* jpeg_atlas_constants;
    GpuComputeShader* jpeg_atlas_shader;
    u64 png_uploaded_bytes;
    u64 apng_uploaded_bytes;
    u64 png_dispatched_pixels;
    u64 straight_rgba8_uploaded_bytes;
    u64 apng_dispatched_pixels;
    u64 gif_uploaded_bytes;
    u64 gif_dispatched_pixels;
    u64 animated_webp_uploaded_bytes;
    u64 animated_webp_dispatched_pixels;
    u64 vp8_uploaded_bytes;
    u64 vp8_reconstructed_pixels;
    u64 vp8_filtered_pixels;
    u64 vp8_atlas_pixels;
    u64 vp8_alpha_uploaded_bytes;
    u64 jpeg_uploaded_bytes;
    u64 jpeg_reconstructed_pixels;
    u32 png_uploads;
    u32 straight_rgba8_uploads;
    u32 cleared_regions;
    u32 rejected;
    u32 png_scanline_capacity;
    u32 apng_frames;
    u32 apng_source_blends;
    u32 apng_over_blends;
    u32 apng_background_disposals;
    u32 apng_previous_disposals;
    u32 gif_frames;
    u32 gif_background_disposals;
    u32 gif_previous_disposals;
    u32 animated_webp_frames;
    u32 animated_webp_source_blends;
    u32 animated_webp_over_blends;
    u32 animated_webp_background_disposals;
    u32 vp8_mode_capacity;
    u32 vp8_coefficient_capacity;
    u32 vp8_yuv_capacity;
    u32 vp8_reconstructions;
    u32 vp8_reconstruction_dispatches;
    u32 vp8_filter_dispatches;
    u32 vp8_filtered_edges;
    u32 vp8_last_filtered_reconstruction;
    u32 vp8_filter_skips;
    u32 vp8_atlas_uploads;
    u32 vp8_alpha_uploads;
    u32 vp8_last_atlas_reconstruction;
    u32 jpeg_uploads;
    u32 jpeg_idct_dispatches;
    u32 jpeg_atlas_dispatches;
    u16 width;
    u16 height;
    u16 preview_width;
    u16 preview_height;
    u16 vp8_width;
    u16 vp8_height;
    u16 vp8_luma_stride;
    u16 vp8_luma_rows;
    u16 vp8_chroma_stride;
    u16 vp8_chroma_rows;
};

enum class MediaAtlasGpuError : u8 {
    none,
    invalid_input,
    device_failure,
    atlas_failure,
};

struct MediaAtlasGpuResult {
    MediaAtlasResult allocation;
    GpuStatus native_error;
    MediaAtlasGpuError error;

    constexpr bool ok() const noexcept {
        return error == MediaAtlasGpuError::none;
    }
};

MediaAtlasGpuResult media_atlas_gpu_initialize(
    MediaAtlasGpu* gpu, GpuDevice* device,
    GpuContext1* context, u16 width, u16 height) noexcept;
// Adds a second texture owner that shares the decoder shaders, upload buffers
// and workspaces, so large previews cannot evict small identity media.
MediaAtlasGpuResult media_atlas_gpu_initialize_preview(
    MediaAtlasGpu* gpu, GpuDevice* device,
    GpuContext1* context, u16 width, u16 height) noexcept;
// The straight RGBA8, PNG, JPEG and VP8 keyframe uploads target the primary
// surface, or the preview surface (large-preview keys only) when `preview` is set.
MediaAtlasGpuResult media_atlas_gpu_upload_straight_rgba8(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key, u16 width,
    u16 height, u32 row_pitch, ConstSlice<u8> straight_rgba8, bool preview = false) noexcept;
MediaAtlasGpuResult media_atlas_gpu_upload_png(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key,
    const PngImage& image, ConstSlice<u8> reconstructed_scanlines, bool preview = false) noexcept;
MediaAtlasGpuResult media_atlas_gpu_upload_jpeg(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key,
    const JpegImage& image,
    const JpegCoefficientWorkspace& coefficients, bool preview = false) noexcept;
MediaAtlasGpuResult media_atlas_gpu_begin_apng(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key, u16 canvas_width,
    u16 canvas_height, const ApngFrame& frame, const PngImage& frame_image,
    ConstSlice<u8> reconstructed_scanlines) noexcept;
MediaAtlasGpuResult media_atlas_gpu_advance_apng(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key,
    const ApngFrame& previous_frame, const ApngFrame& frame,
    const PngImage& frame_image,
    ConstSlice<u8> reconstructed_scanlines) noexcept;
MediaAtlasGpuResult media_atlas_gpu_begin_gif(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key, u16 canvas_width,
    u16 canvas_height, const GifFrame& frame, const GifPalette& palette,
    ConstSlice<u8> indices) noexcept;
MediaAtlasGpuResult media_atlas_gpu_advance_gif(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key,
    const GifFrame& previous_frame, const GifFrame& frame,
    const GifPalette& palette, ConstSlice<u8> indices) noexcept;
MediaAtlasGpuResult media_atlas_gpu_begin_animated_webp(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key, u16 canvas_width,
    u16 canvas_height, u32 background_rgba,
    const AnimatedWebpFrame& frame,
    ConstSlice<u8> straight_rgba8) noexcept;
MediaAtlasGpuResult media_atlas_gpu_advance_animated_webp(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key,
    u32 background_rgba, const AnimatedWebpFrame& previous_frame,
    const AnimatedWebpFrame& frame,
    ConstSlice<u8> straight_rgba8) noexcept;
MediaAtlasGpuResult media_atlas_gpu_begin_animated_webp_vp8(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key, u16 canvas_width,
    u16 canvas_height, u32 background_rgba,
    const AnimatedWebpFrame& animation_frame,
    const Vp8Keyframe& vp8_frame, ConstSlice<u8> alpha) noexcept;
MediaAtlasGpuResult media_atlas_gpu_advance_animated_webp_vp8(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key,
    u32 background_rgba, const AnimatedWebpFrame& previous_frame,
    const AnimatedWebpFrame& animation_frame,
    const Vp8Keyframe& vp8_frame, ConstSlice<u8> alpha) noexcept;
// Reconstructs the padded keyframe Y/U/V planes entirely on the GPU. Samples
// are u32 values in vp8_yuv_buffer: Y first, then U and V. Loop filtering,
// chroma upsampling, alpha and atlas composition are separate bounded stages.
MediaAtlasGpuResult media_atlas_gpu_reconstruct_vp8_keyframe(
    MediaAtlasGpu* gpu, GpuDevice* device,
    GpuContext1* context, const Vp8Keyframe& frame,
    const Vp8ModeWorkspace& modes,
    const Vp8CoefficientWorkspace& coefficients) noexcept;
MediaAtlasGpuResult media_atlas_gpu_filter_vp8_keyframe(
    MediaAtlasGpu* gpu, GpuDevice* device,
    GpuContext1* context, const Vp8Keyframe& frame,
    const Vp8ControlHeader& header, const Vp8ModeWorkspace& modes,
    const Vp8CoefficientWorkspace& coefficients) noexcept;
MediaAtlasGpuResult media_atlas_gpu_upload_vp8_keyframe(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device,
    GpuContext1* context, MediaAssetKey key,
    const Vp8Keyframe& frame, ConstSlice<u8> alpha, bool preview = false) noexcept;
void media_atlas_gpu_release(MediaAtlasGpu* gpu) noexcept;
// Recreates the primary or preview surface at `height` (its atlas changed
// height): rows [0, min(old, new)) are copied, then each trim move from the old
// surface. The previous-disposal backup follows the same moves and never
// grows here; it only covers rows that actually needed it.
MediaAtlasGpuResult media_atlas_gpu_resize(MediaAtlasGpu* gpu, GpuDevice* device,
    GpuContext1* context, bool preview, u16 height,
    ConstSlice<GlyphAtlasRowMove> moves = {}) noexcept;
// Releases one surface's disposal backup (idle trim when unused).
void media_atlas_gpu_release_backup(MediaAtlasGpu* gpu, bool preview) noexcept;
// maximum_extent > 0 reduces the frame on the GPU so its longer side fits
// (composer posters); zero presents the native visible extent.
MediaAtlasGpuResult media_atlas_gpu_present_video(MediaAtlasGpu* gpu, MediaAtlas* atlas,
    GpuDevice* device, GpuContext1* context, MediaAssetKey key,
    const GpuVideoFrame& frame, bool preview, u32 maximum_extent = 0) noexcept;

static_assert(sizeof(MediaAtlasGpu) == 736);

} // namespace maicord
