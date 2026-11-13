#pragma once

#include "maicord/render/media/atlas.hpp"
#include "maicord/render/media/video_frame_gpu.hpp"
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

// What the VP8 decode shaders need of a GPU (every Direct3D 11 device has it): vp8_predict.hlsl and vp8_filter.hlsl
// each run one group of 16 x 32 invocations, and vp8_filter.hlsl holds 30208 bytes of shared memory, the most of the
// VP8 shaders (src/gpu/gles/README.md).
inline constexpr u32 kVp8GpuInvocations = 512;
inline constexpr u32 kVp8GpuSharedBytes = 30208;
constexpr bool media_atlas_gpu_vp8_supported(const GpuCaps& caps) noexcept {
    return caps.max_compute_invocations >= kVp8GpuInvocations && caps.max_shared_bytes >= kVp8GpuSharedBytes;
}

// The primary and preview surfaces are paged textures (gpu_paged_texture_*) of their atlases' geometry, so they
// never grow, move or copy: each upload backs the pages its atlas allocated and writes the page holding it.
struct MediaAtlasGpu {
    VideoFrameGpu video;
    GpuPagedTexture* surface;
    GpuPagedTexture* preview_surface;
    // Previous-disposal backups: copy-only paged textures of the same pages, backed page by page as animations
    // need them and released when unused for kMediaAtlasIdleTrimMs.
    GpuPagedTexture* backup;
    GpuPagedTexture* preview_backup;
    // What draws sample: the surfaces' views (owned by the surfaces).
    GpuSrv* texture_view;
    GpuSrv* preview_texture_view;
    // The page an upload writes and that page of the backup (select_page), in the page's own rows.
    GpuTexture* texture;
    GpuUav* texture_uav;
    GpuTexture* backup_texture;
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

// Creates the primary surface for `atlas`'s pages (rows are backed as the atlas allocates them).
MediaAtlasGpuResult media_atlas_gpu_initialize(
    MediaAtlasGpu* gpu, GpuDevice* device, const MediaAtlas& atlas) noexcept;
// Adds a second surface that shares the decoder shaders, upload buffers
// and workspaces, so large previews cannot evict small identity media.
MediaAtlasGpuResult media_atlas_gpu_initialize_preview(
    MediaAtlasGpu* gpu, GpuDevice* device, const MediaAtlas& atlas) noexcept;
// Backs or returns the surface's rows to match the atlas's pages (after a trim).
bool media_atlas_gpu_sync(MediaAtlasGpu* gpu, GpuContext1* context, const MediaAtlas& atlas, bool preview) noexcept;
// Backs the rows the atlas allocated on the primary surface (`preview`: the preview one), points texture,
// texture_uav and backup_texture at the page holding `allocation` and turns it into that page's rows. False when
// the rows could not be backed: the caller then forgets the key, which nothing may sample.
bool media_atlas_gpu_select(MediaAtlasGpu* gpu, GpuContext1* context, const MediaAtlas& atlas, bool preview,
                            MediaAtlasResult* allocation) noexcept;
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
// Releases one surface's disposal backup (idle trim when unused).
void media_atlas_gpu_release_backup(MediaAtlasGpu* gpu, bool preview) noexcept;
// maximum_extent > 0 reduces the frame on the GPU so its longer side fits
// (composer posters); zero presents the native visible extent. A picture a
// page cannot hold with its padding (a portrait 1440p source, a wide 4K one)
// is reduced further, by media_atlas_video_step, rather than refused.
MediaAtlasGpuResult media_atlas_gpu_present_video(MediaAtlasGpu* gpu, MediaAtlas* atlas,
    GpuDevice* device, GpuContext1* context, MediaAssetKey key,
    const GpuVideoFrame& frame, bool preview, u32 maximum_extent = 0) noexcept;
// The integer reduction media_atlas_gpu_present_video converts `frame` at:
// gpu_video_frame_reduction's for maximum_extent, or the smallest step whose
// picture fits one of the atlas's pages with its padding when that is larger.
inline u32 media_atlas_video_step(const MediaAtlas& atlas, const GpuVideoFrame& frame, u32 maximum_extent) noexcept {
    const GlyphAtlasAllocator& a = atlas.allocations;
    u32 step = gpu_video_frame_reduction(frame.width, frame.height, maximum_extent);
    const u32 width = gpu_video_frame_output_width(frame, 1), height = gpu_video_frame_output_height(frame, 1);
    // A native picture a page holds (every stream up to 2560 wide in the preview atlas) costs two comparisons.
    if ((width + a.padding * 2u > a.width || height + a.padding * 2u > a.page_rows) && a.width > a.padding * 2u &&
        a.page_rows > a.padding * 2u) {
        const u32 room_width = a.width - a.padding * 2u, room_height = a.page_rows - a.padding * 2u;
        const u32 across = (width + room_width - 1) / room_width, down = (height + room_height - 1) / room_height;
        step = maximum(step, maximum(across, down));
    }
    return minimum(step, kGpuVideoReductionMaximum);
}

static_assert(sizeof(MediaAtlasGpu) == 584);

} // namespace maicord
