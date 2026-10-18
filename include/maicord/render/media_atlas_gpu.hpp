#pragma once

#include "maicord/media/png.hpp"
#include "maicord/media/vp8.hpp"
#include "maicord/media/webp.hpp"
#include "maicord/render/media_atlas.hpp"
#include "maicord/render/video_frame_gpu.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/sys/d3d11.hpp"

struct ID3D11Buffer;
struct ID3D11ComputeShader;
struct ID3D11Device;
struct ID3D11DeviceContext1;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11UnorderedAccessView;

namespace maicord {
struct AnimatedWebpFrame;
struct ApngFrame;
struct GifFrame;
struct GifPalette;
struct JpegCoefficientWorkspace;
struct JpegImage;

struct MediaAtlasGpu {
    VideoFrameGpu video;
    ID3D11Texture2D* texture;
    ID3D11Texture2D* preview_texture;
    ID3D11Texture2D* apng_backup_texture;
    ID3D11Texture2D* preview_animation_backup_texture;
    ID3D11ShaderResourceView* texture_view;
    ID3D11ShaderResourceView* preview_texture_view;
    ID3D11RenderTargetView* target;
    ID3D11RenderTargetView* preview_target;
    ID3D11UnorderedAccessView* texture_uav;
    ID3D11UnorderedAccessView* preview_texture_uav;
    ID3D11Buffer* png_scanline_buffer;
    ID3D11ShaderResourceView* png_scanline_view;
    ID3D11Buffer* png_palette_buffer;
    ID3D11ShaderResourceView* png_palette_view;
    ID3D11Buffer* png_constants;
    ID3D11ComputeShader* png_shader;
    ID3D11Buffer* vp8_mode_buffer;
    ID3D11ShaderResourceView* vp8_mode_view;
    ID3D11Buffer* vp8_coefficient_buffer;
    ID3D11ShaderResourceView* vp8_coefficient_view;
    ID3D11Buffer* vp8_yuv_buffer;
    ID3D11ShaderResourceView* vp8_yuv_view;
    ID3D11UnorderedAccessView* vp8_yuv_uav;
    ID3D11Buffer* vp8_constants;
    ID3D11Buffer* vp8_tile_constants;
    u16 vp8_tile_columns;
    u16 vp8_tile_rows;
    u32 vp8_tile_wave_count;
    u32 vp8_tile_groups[32];
    ID3D11ComputeShader* vp8_reconstruct_shader;
    ID3D11ComputeShader* vp8_idct_shader;
    ID3D11Buffer* vp8_filter_constants;
    ID3D11ComputeShader* vp8_filter_shader;
    ID3D11Buffer* vp8_atlas_constants;
    ID3D11ComputeShader* vp8_atlas_shader;
    ID3D11Buffer* jpeg_idct_constants;
    ID3D11ComputeShader* jpeg_idct_shader;
    ID3D11Buffer* jpeg_atlas_constants;
    ID3D11ComputeShader* jpeg_atlas_shader;
    u64 uploaded_bytes;
    u64 png_uploaded_bytes;
    u64 png_dispatched_pixels;
    u64 straight_rgba8_uploaded_bytes;
    u64 straight_rgba8_dispatched_pixels;
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
    u32 uploads;
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
    HRESULT native_error;
    MediaAtlasGpuError error;

    constexpr bool ok() const noexcept {
        return error == MediaAtlasGpuError::none;
    }
};

MediaAtlasGpuResult media_atlas_gpu_initialize(
    MediaAtlasGpu* gpu, ID3D11Device* device,
    ID3D11DeviceContext1* context, u16 width, u16 height) noexcept;
// Adds a second texture owner that shares the decoder shaders, upload buffers
// and workspaces, so large previews cannot evict small identity media.
MediaAtlasGpuResult media_atlas_gpu_initialize_preview(
    MediaAtlasGpu* gpu, ID3D11Device* device,
    ID3D11DeviceContext1* context, u16 width, u16 height) noexcept;
MediaAtlasGpuResult media_atlas_gpu_upload_rgba8(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11DeviceContext1* context,
    MediaAssetKey key, u16 width, u16 height, u32 row_pitch,
    ConstSlice<u8> premultiplied_rgba8) noexcept;
MediaAtlasGpuResult media_atlas_gpu_upload_straight_rgba8(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key, u16 width,
    u16 height, u32 row_pitch, ConstSlice<u8> straight_rgba8) noexcept;
MediaAtlasGpuResult media_atlas_gpu_upload_preview_straight_rgba8(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key, u16 width,
    u16 height, u32 row_pitch, ConstSlice<u8> straight_rgba8) noexcept;
MediaAtlasGpuResult media_atlas_gpu_upload_png(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key,
    const PngImage& image, ConstSlice<u8> reconstructed_scanlines) noexcept;
MediaAtlasGpuResult media_atlas_gpu_upload_preview_png(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key,
    const PngImage& image, ConstSlice<u8> reconstructed_scanlines) noexcept;
MediaAtlasGpuResult media_atlas_gpu_upload_jpeg(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key,
    const JpegImage& image,
    const JpegCoefficientWorkspace& coefficients) noexcept;
MediaAtlasGpuResult media_atlas_gpu_upload_preview_jpeg(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key,
    const JpegImage& image,
    const JpegCoefficientWorkspace& coefficients) noexcept;
MediaAtlasGpuResult media_atlas_gpu_begin_apng(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key, u16 canvas_width,
    u16 canvas_height, const ApngFrame& frame, const PngImage& frame_image,
    ConstSlice<u8> reconstructed_scanlines) noexcept;
MediaAtlasGpuResult media_atlas_gpu_advance_apng(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key,
    const ApngFrame& previous_frame, const ApngFrame& frame,
    const PngImage& frame_image,
    ConstSlice<u8> reconstructed_scanlines) noexcept;
MediaAtlasGpuResult media_atlas_gpu_begin_gif(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key, u16 canvas_width,
    u16 canvas_height, const GifFrame& frame, const GifPalette& palette,
    ConstSlice<u8> indices) noexcept;
MediaAtlasGpuResult media_atlas_gpu_advance_gif(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key,
    const GifFrame& previous_frame, const GifFrame& frame,
    const GifPalette& palette, ConstSlice<u8> indices) noexcept;
MediaAtlasGpuResult media_atlas_gpu_begin_animated_webp(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key, u16 canvas_width,
    u16 canvas_height, u32 background_rgba,
    const AnimatedWebpFrame& frame,
    ConstSlice<u8> straight_rgba8) noexcept;
MediaAtlasGpuResult media_atlas_gpu_advance_animated_webp(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key,
    u32 background_rgba, const AnimatedWebpFrame& previous_frame,
    const AnimatedWebpFrame& frame,
    ConstSlice<u8> straight_rgba8) noexcept;
MediaAtlasGpuResult media_atlas_gpu_begin_animated_webp_vp8(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key, u16 canvas_width,
    u16 canvas_height, u32 background_rgba,
    const AnimatedWebpFrame& animation_frame,
    const Vp8Keyframe& vp8_frame, ConstSlice<u8> alpha) noexcept;
MediaAtlasGpuResult media_atlas_gpu_advance_animated_webp_vp8(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key,
    u32 background_rgba, const AnimatedWebpFrame& previous_frame,
    const AnimatedWebpFrame& animation_frame,
    const Vp8Keyframe& vp8_frame, ConstSlice<u8> alpha) noexcept;
// Reconstructs the padded keyframe Y/U/V planes entirely on the GPU. Samples
// are u32 values in vp8_yuv_buffer: Y first, then U and V. Loop filtering,
// chroma upsampling, alpha and atlas composition are separate bounded stages.
MediaAtlasGpuResult media_atlas_gpu_reconstruct_vp8_keyframe(
    MediaAtlasGpu* gpu, ID3D11Device* device,
    ID3D11DeviceContext1* context, const Vp8Keyframe& frame,
    const Vp8ModeWorkspace& modes,
    const Vp8CoefficientWorkspace& coefficients) noexcept;
MediaAtlasGpuResult media_atlas_gpu_filter_vp8_keyframe(
    MediaAtlasGpu* gpu, ID3D11Device* device,
    ID3D11DeviceContext1* context, const Vp8Keyframe& frame,
    const Vp8ControlHeader& header, const Vp8ModeWorkspace& modes,
    const Vp8CoefficientWorkspace& coefficients) noexcept;
MediaAtlasGpuResult media_atlas_gpu_upload_vp8_keyframe(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key,
    const Vp8Keyframe& frame, ConstSlice<u8> alpha) noexcept;
MediaAtlasGpuResult media_atlas_gpu_upload_preview_vp8_keyframe(
    MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11Device* device,
    ID3D11DeviceContext1* context, MediaAssetKey key,
    const Vp8Keyframe& frame, ConstSlice<u8> alpha) noexcept;
void media_atlas_gpu_release(MediaAtlasGpu* gpu) noexcept;
// Recreates the primary or preview surface at `height` (its atlas changed
// height): rows [0, min(old, new)) are copied, then each trim move from the old
// surface. The previous-disposal backup follows the same moves and never
// grows here; it only covers rows that actually needed it.
MediaAtlasGpuResult media_atlas_gpu_resize(MediaAtlasGpu* gpu, ID3D11Device* device,
    ID3D11DeviceContext1* context, bool preview, u16 height,
    ConstSlice<GlyphAtlasRowMove> moves = {}) noexcept;
// Releases one surface's disposal backup (idle trim when unused).
void media_atlas_gpu_release_backup(MediaAtlasGpu* gpu, bool preview) noexcept;
// maximum_extent > 0 reduces the frame on the GPU so its longer side fits
// (composer posters); zero presents the native visible extent.
MediaAtlasGpuResult media_atlas_gpu_present_video(MediaAtlasGpu* gpu, MediaAtlas* atlas,
    ID3D11Device* device, ID3D11DeviceContext1* context, MediaAssetKey key,
    const GpuVideoFrame& frame, bool preview, u32 maximum_extent = 0) noexcept;

static_assert(sizeof(MediaAtlasGpu) == 784);

} // namespace maicord
