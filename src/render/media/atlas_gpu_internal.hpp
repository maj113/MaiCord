#pragma once

#include "maicord/render/media/atlas_gpu.hpp"
#include "maicord/media/image/jpeg.hpp"
#include "maicord/media/image/animated_webp.hpp"
#include "maicord/media/image/apng.hpp"
#include "maicord/media/image/gif.hpp"

#include "maicord/gpu/api.hpp"
#include "maicord/media/core/asset.hpp"

namespace maicord::media_atlas_gpu_detail {

inline constexpr float kTransparentColor[4]{};

struct PngAtlasConstants {
    u32 row_stride;
    u32 width;
    u32 height;
    u32 bit_depth;
    u32 color_type;
    u32 destination_x;
    u32 destination_y;
    u32 palette_count;
    u32 transparent_gray;
    u32 transparent_red;
    u32 transparent_green;
    u32 transparent_blue;
    u32 reserved[4];
};

static_assert(sizeof(PngAtlasConstants) == 64);

struct Vp8ReconstructionConstants {
    u32 macroblock_columns;
    u32 macroblock_rows;
    u32 luma_stride;
    u32 chroma_stride;
    u32 chroma_rows;
    u32 u_plane_offset;
    u32 v_plane_offset;
    u32 mode_record_bytes;
    u32 coefficient_record_bytes;
    u32 reserved[3];
};

static_assert(sizeof(Vp8ReconstructionConstants) == 48);

struct Vp8FilterConstants {
    u32 macroblock_columns;
    u32 macroblock_rows;
    u32 luma_stride;
    u32 chroma_stride;
    u32 u_plane_offset;
    u32 v_plane_offset;
    u32 filter_level;
    u32 sharpness;
    u32 simple_filter;
    u32 segmentation_enabled;
    u32 segment_absolute_delta;
    u32 use_filter_delta;
    i32 segment_filter[4];
    i32 reference_delta0;
    i32 mode_delta0;
    u32 mode_record_bytes;
    u32 coefficient_record_bytes;
};

static_assert(sizeof(Vp8FilterConstants) == 80);

struct Vp8AtlasConstants {
    u32 image_width;
    u32 image_height;
    u32 luma_stride;
    u32 chroma_stride;
    u32 u_plane_offset;
    u32 v_plane_offset;
    u32 destination_x;
    u32 destination_y;
    u32 has_alpha;
    u32 alpha_stride;
    u32 blend_over;
    u32 reserved[5];
};

static_assert(sizeof(Vp8AtlasConstants) == 64);

struct JpegIdctComponentConstants {
    u32 block_offset;
    u32 block_count;
    u32 block_columns;
    u32 sample_offset;
    u32 sample_stride;
    u32 quantization_table;
};

struct JpegIdctConstants {
    u32 coefficient_block_count;
    u32 component_count;
    JpegIdctComponentConstants components[kJpegComponentMaximum];
    u32 stored_rows; // the stored-rows layout of JpegCoefficientWorkspace, else whole blocks
    u32 reserved[5];
};

static_assert(sizeof(JpegIdctConstants) == 128);

struct JpegAtlasComponentConstants {
    u32 sample_offset;
    u32 sample_stride;
    u32 sample_width;
    u32 sample_height;
    u32 horizontal_sampling;
    u32 vertical_sampling;
};

struct JpegAtlasConstants {
    u32 image_width;
    u32 image_height;
    u32 destination_x;
    u32 destination_y;
    u32 color_transform_orientation;
    u32 maximum_horizontal_sampling;
    u32 maximum_vertical_sampling;
    u32 component_count;
    JpegAtlasComponentConstants components[kJpegComponentMaximum];
    u32 icc_transform_kind;
    u32 icc_pcs;
    u32 icc_input_channels;
    u32 icc_clut_offset;
    u32 icc_clut_entry_count;
    u32 icc_grid0;
    u32 icc_grid1;
    u32 icc_grid2;
    u32 icc_grid3;
    u32 icc_precision_bytes;
    u32 icc_reserved0;
    u32 icc_reserved1;
    float icc_matrix[12];
    // Kind, table offset, table count and padding for four input/direct
    // curves followed by three LUT output curves.
    u32 icc_curve_layouts[kJpegIccInputChannelMaximum + kJpegIccCurveChannelCount][4];
    float icc_curve_parameters0[kJpegIccInputChannelMaximum + kJpegIccCurveChannelCount][4];
    float icc_curve_parameters1[kJpegIccInputChannelMaximum + kJpegIccCurveChannelCount][4];
};

static_assert(sizeof(JpegAtlasConstants) == 560);

MediaAtlasGpuResult failure(MediaAtlasGpuError error, GpuStatus native_error = kGpuOk,
                            MediaAtlasResult allocation = {}) noexcept;
// A failure that also counts as a rejected request (gpu may be null).
MediaAtlasGpuResult reject(MediaAtlasGpu* gpu, MediaAtlasGpuError error, GpuStatus native_error = kGpuOk,
                           MediaAtlasResult allocation = {}) noexcept;

bool dimensions_valid(u16 width, u16 height) noexcept;

// The surface of `preview` (the primary one otherwise) exists for this atlas's geometry.
inline bool surface_ready(const MediaAtlasGpu* gpu, const MediaAtlas& atlas, bool preview) noexcept {
    return gpu && (preview ? gpu->preview_surface && gpu->preview_width == atlas.allocations.width &&
                                 gpu->preview_height == atlas.allocations.height
                           : gpu->surface && gpu->width == atlas.allocations.width &&
                                 gpu->height == atlas.allocations.height);
}

// The atlas texture exists and matches the allocation grid.
inline bool animation_target_ready(const MediaAtlasGpu* gpu, const MediaAtlas* atlas, GpuDevice* device,
                                   GpuContext1* context, MediaAssetKey key) noexcept {
    return atlas && device && context && surface_ready(gpu, *atlas, false) && media_asset_key_valid(key);
}

// A first frame's canvas is non-empty and fits a page with its padding.
inline bool animation_canvas_fits(const MediaAtlas& atlas, u16 canvas_width, u16 canvas_height) noexcept {
    return canvas_width != 0 && canvas_height != 0 &&
           static_cast<u32>(canvas_width) + atlas.allocations.padding * 2u <= atlas.allocations.width &&
           static_cast<u32>(canvas_height) + atlas.allocations.padding * 2u <= atlas.allocations.page_rows;
}

// An image of this size fits a page with its padding.
inline bool image_fits(const MediaAtlas& atlas, u32 width, u32 height) noexcept {
    return width + atlas.allocations.padding * 2u <= atlas.allocations.width &&
           height + atlas.allocations.padding * 2u <= atlas.allocations.page_rows;
}

bool png_format_valid(const PngImage& image) noexcept;

bool png_scanlines_reconstructed(const PngImage& image, ConstSlice<u8> scanlines) noexcept;

GpuStatus create_png_resources(MediaAtlasGpu* gpu, GpuDevice* device) noexcept;

GpuStatus ensure_dynamic_raw_buffer(GpuDevice* device, u32 required_bytes, GpuBuffer** buffer,
                                    GpuSrv** view, u32* capacity) noexcept;

GpuStatus ensure_vp8_yuv_buffer(MediaAtlasGpu* gpu, GpuDevice* device, u32 required_bytes) noexcept;

GpuStatus create_vp8_resources(MediaAtlasGpu* gpu, GpuDevice* device) noexcept;

GpuStatus create_vp8_filter_resources(MediaAtlasGpu* gpu, GpuDevice* device) noexcept;

GpuStatus create_vp8_atlas_resources(MediaAtlasGpu* gpu, GpuDevice* device) noexcept;

GpuStatus create_jpeg_resources(MediaAtlasGpu* gpu, GpuDevice* device) noexcept;

bool jpeg_icc_curve_valid(const JpegIccCurve& curve, u32 entry_count) noexcept;

void store_jpeg_icc_curve(JpegAtlasConstants* constants, u32 destination, const JpegIccCurve& curve,
                          u32 table_base) noexcept;

bool jpeg_image_valid(const JpegImage& image) noexcept;

bool vp8_reconstruction_valid(const Vp8Keyframe& frame, const Vp8ModeWorkspace& modes,
                              const Vp8CoefficientWorkspace& coefficients) noexcept;

bool vp8_filter_header_valid(const Vp8ControlHeader& header) noexcept;

i32 vp8_macroblock_filter_level(const Vp8ControlHeader& header, const Vp8MacroblockMode& mode) noexcept;

// Allocates `key` on the chosen surface, selects its page and clears the slot with its padding (opaque full-frame
// video skips a cached one). Returns the allocation in virtual rows and `local` in its page's rows.
MediaAtlasResult allocate_and_clear_surface(MediaAtlasGpu* gpu, MediaAtlas* atlas,
                                            GpuContext1* context, bool preview, MediaAssetKey key,
                                            u16 width, u16 height, MediaAtlasResult* local,
                                            bool clear_cached = true) noexcept;

bool apng_control_valid(const ApngFrame& frame, u16 canvas_width, u16 canvas_height) noexcept;

bool apng_frame_valid(const ApngFrame& frame, u16 canvas_width, u16 canvas_height,
                      const PngImage& frame_image) noexcept;

bool gif_control_valid(const GifFrame& frame, u16 canvas_width, u16 canvas_height) noexcept;

bool gif_frame_valid(const GifFrame& frame, u16 canvas_width, u16 canvas_height, const GifPalette& palette,
                     ConstSlice<u8> indices) noexcept;

bool animated_webp_control_valid(const AnimatedWebpFrame& frame, u16 canvas_width,
                                 u16 canvas_height) noexcept;

bool animated_webp_frame_valid(const AnimatedWebpFrame& frame, u16 canvas_width, u16 canvas_height,
                               ConstSlice<u8> rgba8) noexcept;

GpuStatus upload_discard(GpuContext1* context, GpuBuffer* buffer, const void* source,
                         u32 bytes) noexcept;

GpuStatus upload_png_shader_inputs(MediaAtlasGpu* gpu, GpuDevice* device,
                                   GpuContext1* context, ConstSlice<u8> source,
                                   u32 source_bytes, const u32* palette, u32 palette_count) noexcept;

GpuStatus upload_png_inputs(MediaAtlasGpu* gpu, GpuDevice* device, GpuContext1* context,
                            const PngImage& image, ConstSlice<u8> scanlines) noexcept;

GpuStatus upload_gif_inputs(MediaAtlasGpu* gpu, GpuDevice* device, GpuContext1* context,
                            const GifFrame& frame, const GifPalette& palette,
                            ConstSlice<u8> indices) noexcept;

GpuStatus upload_animated_webp_inputs(MediaAtlasGpu* gpu, GpuDevice* device,
                                      GpuContext1* context, const AnimatedWebpFrame& frame,
                                      ConstSlice<u8> rgba8) noexcept;

void bind_compute_2srv(GpuContext1* context, GpuComputeShader* shader,
                       GpuBuffer* constants, GpuSrv* resource0,
                       GpuSrv* resource1,
                       GpuUav* target) noexcept;

void unbind_compute_resources(GpuContext1* context) noexcept;

void unbind_compute(GpuContext1* context) noexcept;

// The primary, animation and preview atlases occupy PS slots 0..2 while media
// is drawn. Every writable atlas path must unbind all three before binding an
// atlas UAV/RTV; unbinding only slot zero leaves the preview atlas readable
// and writable at once. allocate_and_clear_surface and unbind_apng_atlas do it
// once per upload; the dispatch and clear helpers below rely on that.
void unbind_media_pixel_resources(GpuContext1* context) noexcept;

void dispatch_png_shader(MediaAtlasGpu* gpu, GpuContext1* context,
                         const PngAtlasConstants& constants, GpuUav* target,
                         u32 width, u32 height) noexcept;

void dispatch_png(MediaAtlasGpu* gpu, GpuContext1* context, const PngImage& image,
                  u32 destination_x, u32 destination_y, ApngBlend blend) noexcept;

void dispatch_gif(MediaAtlasGpu* gpu, GpuContext1* context, const GifFrame& frame,
                  const GifPalette& palette, u32 destination_x, u32 destination_y) noexcept;

void dispatch_animated_webp(MediaAtlasGpu* gpu, GpuContext1* context,
                            const AnimatedWebpFrame& frame, u32 destination_x, u32 destination_y) noexcept;

// That page of the previous-disposal backup is backed (the backup is made on first use) and selected.
GpuStatus ensure_apng_backup(MediaAtlasGpu* gpu, GpuDevice* device, GpuContext1* context,
                           MediaAtlas* atlas, const MediaAtlasResult& allocation) noexcept;

// A frame's region of an animation canvas and how it leaves it (APNG and GIF dispose alike).
struct CanvasFrame {
    u32 x, y, width, height;
    bool background, previous;
};

// A new animation canvas, allocated and cleared (`local` its slot in that page's rows); a frame that restores the
// canvas after itself saves its region first.
MediaAtlasGpuResult begin_canvas(MediaAtlasGpu* gpu, MediaAtlas* atlas, GpuDevice* device, GpuContext1* context,
                                 MediaAssetKey key, u16 width, u16 height, const CanvasFrame& frame,
                                 MediaAtlasResult* local) noexcept;

void unbind_apng_atlas(GpuContext1* context) noexcept;

void copy_animation_region(GpuContext1* context, GpuTexture* destination,
                           GpuTexture* source, const MediaAtlasResult& allocation,
                           u32 x_offset, u32 y_offset, u32 width, u32 height) noexcept;

void clear_animation_region(MediaAtlasGpu* gpu, GpuContext1* context,
                            const MediaAtlasResult& allocation, u32 x_offset, u32 y_offset,
                            u32 width, u32 height, const float* color) noexcept;

void account_apng_dispatch(MediaAtlasGpu* gpu, const ApngFrame& frame, const PngImage& image) noexcept;

void account_gif_dispatch(MediaAtlasGpu* gpu, const GifFrame& frame) noexcept;

void premultiplied_clear_color(u32 rgba, float* color) noexcept;

void clear_animated_webp_canvas(MediaAtlasGpu* gpu, GpuContext1* context,
                                const MediaAtlasResult& allocation, u32 background_rgba) noexcept;

void clear_animated_webp_region(MediaAtlasGpu* gpu, GpuContext1* context,
                                const MediaAtlasResult& allocation, const AnimatedWebpFrame& frame,
                                u32 background_rgba) noexcept;

void account_animated_webp_dispatch(MediaAtlasGpu* gpu, const AnimatedWebpFrame& frame,
                                    u64 uploaded_bytes) noexcept;

bool vp8_atlas_frame_ready(const MediaAtlasGpu* gpu, const Vp8Keyframe& frame,
                           ConstSlice<u8> alpha) noexcept;

GpuStatus upload_vp8_alpha(MediaAtlasGpu* gpu, GpuDevice* device, GpuContext1* context,
                         ConstSlice<u8> alpha) noexcept;

void dispatch_vp8_atlas(MediaAtlasGpu* gpu, GpuContext1* context, const Vp8Keyframe& frame,
                        ConstSlice<u8> alpha, u32 destination_x, u32 destination_y,
                        bool blend_over, GpuUav* texture_uav) noexcept;

GpuStatus create_media_surface(GpuDevice* device, const MediaAtlas& atlas, GpuPagedTexture** surface,
                               GpuSrv** view) noexcept;

} // namespace maicord::media_atlas_gpu_detail
