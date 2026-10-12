#pragma once

#include "maicord/render/media_atlas_gpu.hpp"

#include <d3d11_1.h>

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
    u32 image_width;
    u32 image_height;
    u32 macroblock_columns;
    u32 macroblock_rows;
    u32 final_wave_rank;
    u32 luma_stride;
    u32 luma_rows;
    u32 chroma_stride;
    u32 chroma_rows;
    u32 u_plane_offset;
    u32 v_plane_offset;
    u32 mode_record_bytes;
    u32 coefficient_record_bytes;
    u32 reserved[3];
};

static_assert(sizeof(Vp8ReconstructionConstants) == 64);

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
    u32 reserved[6];
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

MediaAtlasGpuResult failure(MediaAtlasGpuError error, HRESULT native_error = S_OK,
                            MediaAtlasResult allocation = {}) noexcept;

bool dimensions_valid(u16 width, u16 height) noexcept;

u32 retained_upload_capacity(u32 current, u32 required) noexcept;

bool png_format_valid(const PngImage& image) noexcept;

bool png_scanlines_reconstructed(const PngImage& image, ConstSlice<u8> scanlines) noexcept;

HRESULT create_png_scanline_buffer(MediaAtlasGpu* gpu, ID3D11Device* device, u32 required_bytes) noexcept;

HRESULT create_png_resources(MediaAtlasGpu* gpu, ID3D11Device* device) noexcept;

HRESULT ensure_dynamic_raw_buffer(ID3D11Device* device, u32 required_bytes, ID3D11Buffer** buffer,
                                  ID3D11ShaderResourceView** view, u32* capacity) noexcept;

HRESULT ensure_vp8_yuv_buffer(MediaAtlasGpu* gpu, ID3D11Device* device, u32 required_bytes) noexcept;

HRESULT create_vp8_resources(MediaAtlasGpu* gpu, ID3D11Device* device) noexcept;

HRESULT create_vp8_filter_resources(MediaAtlasGpu* gpu, ID3D11Device* device) noexcept;

HRESULT create_vp8_atlas_resources(MediaAtlasGpu* gpu, ID3D11Device* device) noexcept;

HRESULT create_jpeg_resources(MediaAtlasGpu* gpu, ID3D11Device* device) noexcept;

bool jpeg_icc_curve_valid(const JpegIccCurve& curve, u32 entry_count) noexcept;

void store_jpeg_icc_curve(JpegAtlasConstants* constants, u32 destination, const JpegIccCurve& curve,
                          u32 table_base) noexcept;

bool jpeg_image_valid(const JpegImage& image) noexcept;

bool vp8_reconstruction_valid(const Vp8Keyframe& frame, const Vp8ModeWorkspace& modes,
                              const Vp8CoefficientWorkspace& coefficients) noexcept;

bool vp8_filter_header_valid(const Vp8ControlHeader& header) noexcept;

i32 vp8_macroblock_filter_level(const Vp8ControlHeader& header, const Vp8MacroblockMode& mode) noexcept;

MediaAtlasResult allocate_and_clear_surface(MediaAtlasGpu* gpu, MediaAtlas* atlas,
                                            ID3D11DeviceContext1* context, ID3D11RenderTargetView* target,
                                            u16 surface_width, u16 surface_height, MediaAssetKey key,
                                            u16 width, u16 height,
                                            bool clear_cached = true) noexcept;

MediaAtlasResult allocate_and_clear(MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11DeviceContext1* context,
                                    MediaAssetKey key, u16 width, u16 height) noexcept;

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

HRESULT upload_discard(ID3D11DeviceContext1* context, ID3D11Buffer* buffer, const void* source,
                       u32 bytes) noexcept;

HRESULT upload_png_shader_inputs(MediaAtlasGpu* gpu, ID3D11Device* device,
                                 ID3D11DeviceContext1* context, ConstSlice<u8> source,
                                 u32 source_bytes, const u32* palette, u32 palette_count) noexcept;

HRESULT upload_png_inputs(MediaAtlasGpu* gpu, ID3D11Device* device, ID3D11DeviceContext1* context,
                          const PngImage& image, ConstSlice<u8> scanlines) noexcept;

HRESULT upload_gif_inputs(MediaAtlasGpu* gpu, ID3D11Device* device, ID3D11DeviceContext1* context,
                          const GifFrame& frame, const GifPalette& palette,
                          ConstSlice<u8> indices) noexcept;

HRESULT upload_animated_webp_inputs(MediaAtlasGpu* gpu, ID3D11Device* device,
                                    ID3D11DeviceContext1* context, const AnimatedWebpFrame& frame,
                                    ConstSlice<u8> rgba8) noexcept;

void bind_compute_2srv(ID3D11DeviceContext1* context, ID3D11ComputeShader* shader,
                       ID3D11Buffer* constants, ID3D11ShaderResourceView* resource0,
                       ID3D11ShaderResourceView* resource1,
                       ID3D11UnorderedAccessView* target) noexcept;

void unbind_compute_resources(ID3D11DeviceContext1* context) noexcept;

void unbind_compute(ID3D11DeviceContext1* context) noexcept;

// The primary, animation and preview atlases occupy PS slots 0..2 while media
// is drawn.  Atlas mutation can happen later in the same immediate-context
// command stream, so every writable atlas path must remove all three aliases
// before binding an atlas UAV/RTV.  Unbinding only slot zero leaves the preview
// atlas simultaneously readable and writable.
void unbind_media_pixel_resources(ID3D11DeviceContext1* context) noexcept;

void dispatch_png_shader(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context,
                         const PngAtlasConstants& constants, ID3D11UnorderedAccessView* target,
                         u32 width, u32 height) noexcept;

void dispatch_png(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context, const PngImage& image,
                  u32 destination_x, u32 destination_y, ApngBlend blend) noexcept;

void dispatch_gif(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context, const GifFrame& frame,
                  const GifPalette& palette, u32 destination_x, u32 destination_y) noexcept;

void dispatch_animated_webp(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context,
                            const AnimatedWebpFrame& frame, u32 destination_x, u32 destination_y) noexcept;

// Previous-disposal backup covering atlas rows [0, required_bottom); grows in
// 256-row steps (keeping its content) and is never larger than the surface.
HRESULT ensure_apng_backup(MediaAtlasGpu* gpu, ID3D11Device* device, ID3D11DeviceContext1* context,
                           MediaAtlas* atlas, u32 required_bottom) noexcept;

void unbind_apng_atlas(ID3D11DeviceContext1* context) noexcept;

void copy_animation_region(ID3D11DeviceContext1* context, ID3D11Texture2D* destination,
                           ID3D11Texture2D* source, const MediaAtlasResult& allocation,
                           u32 x_offset, u32 y_offset, u32 width, u32 height) noexcept;

void clear_animation_region(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context,
                            const MediaAtlasResult& allocation, u32 x_offset, u32 y_offset,
                            u32 width, u32 height, const float* color) noexcept;

void account_apng_dispatch(MediaAtlasGpu* gpu, const ApngFrame& frame, const PngImage& image) noexcept;

void account_gif_dispatch(MediaAtlasGpu* gpu, const GifFrame& frame) noexcept;

void premultiplied_clear_color(u32 rgba, float* color) noexcept;

void clear_animated_webp_canvas(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context,
                                const MediaAtlasResult& allocation, u32 background_rgba) noexcept;

void clear_animated_webp_region(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context,
                                const MediaAtlasResult& allocation, const AnimatedWebpFrame& frame,
                                u32 background_rgba) noexcept;

void account_animated_webp_dispatch(MediaAtlasGpu* gpu, const AnimatedWebpFrame& frame,
                                    u64 uploaded_bytes) noexcept;

bool vp8_atlas_frame_ready(const MediaAtlasGpu* gpu, const Vp8Keyframe& frame,
                           ConstSlice<u8> alpha) noexcept;

HRESULT upload_vp8_alpha(MediaAtlasGpu* gpu, ID3D11Device* device, ID3D11DeviceContext1* context,
                         ConstSlice<u8> alpha) noexcept;

void dispatch_vp8_atlas(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context, const Vp8Keyframe& frame,
                        ConstSlice<u8> alpha, u32 destination_x, u32 destination_y,
                        bool blend_over, ID3D11UnorderedAccessView* texture_uav) noexcept;

HRESULT create_media_surface(ID3D11Device* device, ID3D11DeviceContext1* context, u16 width, u16 height,
                             ID3D11Texture2D** texture, ID3D11ShaderResourceView** texture_view,
                             ID3D11RenderTargetView** target,
                             ID3D11UnorderedAccessView** texture_uav) noexcept;

} // namespace maicord::media_atlas_gpu_detail
