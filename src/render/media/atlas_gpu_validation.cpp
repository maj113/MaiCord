#include "atlas_gpu_internal.hpp"
// Per-image header validation, once per upload: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord::media_atlas_gpu_detail {

void store_jpeg_icc_curve(JpegAtlasConstants* constants, u32 destination, const JpegIccCurve& curve,
                          u32 table_base) noexcept {
    constants->icc_curve_layouts[destination][0] = static_cast<u8>(curve.kind);
    constants->icc_curve_layouts[destination][1] = table_base + curve.table_offset;
    constants->icc_curve_layouts[destination][2] = curve.table_count;
    float* parameters0 = constants->icc_curve_parameters0[destination];
    float* parameters1 = constants->icc_curve_parameters1[destination];
    parameters0[0] = curve.g;
    parameters0[1] = curve.a;
    parameters0[2] = curve.b;
    parameters0[3] = curve.c;
    parameters1[0] = curve.d;
    parameters1[1] = curve.e;
    parameters1[2] = curve.f;
}

bool vp8_reconstruction_valid(const Vp8Keyframe& frame, const Vp8ModeWorkspace& modes,
                              const Vp8CoefficientWorkspace& coefficients) noexcept {
    if (frame.width == 0 || frame.height == 0 || frame.width > kWebpDimensionMaximum ||
        frame.height > kWebpDimensionMaximum) {
        return false;
    }
    const u32 columns = (static_cast<u32>(frame.width) + 15u) / 16u;
    const u32 rows = (static_cast<u32>(frame.height) + 15u) / 16u;
    const u32 count = columns * rows;
    if (modes.columns != columns || modes.rows != rows || modes.macroblock_count != count ||
        coefficients.columns != columns || coefficients.rows != rows ||
        coefficients.macroblock_count != count) {
        return false;
    }
    for (u32 macroblock = 0; macroblock < count; ++macroblock) {
        const Vp8MacroblockMode& mode = modes.macroblocks[macroblock];
        const u8 luma = static_cast<u8>(mode.luma);
        const u8 chroma = static_cast<u8>(mode.chroma);
        if (!((luma <= static_cast<u8>(Vp8IntraMode::horizontal)) ||
              luma == static_cast<u8>(Vp8IntraMode::subblocks)) ||
            chroma > static_cast<u8>(Vp8IntraMode::horizontal) || mode.segment_id > 3 ||
            mode.skip_coefficients > 1 || coefficients.macroblocks[macroblock].has_coefficients > 1) {
            return false;
        }
        if (mode.luma == Vp8IntraMode::subblocks) {
            for (u32 block = 0; block < 16; ++block) {
                if (static_cast<u8>(mode.luma_subblock[block]) >
                    static_cast<u8>(Vp8SubblockMode::horizontal_up)) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool vp8_filter_header_valid(const Vp8ControlHeader& header) noexcept {
    if (header.loop_filter.level > 63 || header.loop_filter.sharpness > 7 ||
        header.loop_filter.simple > 1 || header.loop_filter.use_delta > 1 ||
        header.segmentation.enabled > 1 || header.segmentation.absolute_delta > 1) {
        return false;
    }
    for (u32 segment = 0; segment < 4; ++segment) {
        if (header.segmentation.filter_strength[segment] < -63 ||
            header.segmentation.filter_strength[segment] > 63 ||
            header.loop_filter.reference_delta[segment] < -63 ||
            header.loop_filter.reference_delta[segment] > 63 ||
            header.loop_filter.mode_delta[segment] < -63 || header.loop_filter.mode_delta[segment] > 63) {
            return false;
        }
    }
    return true;
}

i32 vp8_macroblock_filter_level(const Vp8ControlHeader& header, const Vp8MacroblockMode& mode) noexcept {
    i32 level = header.loop_filter.level;
    if (header.segmentation.enabled) {
        const i32 segment = header.segmentation.filter_strength[mode.segment_id];
        level = header.segmentation.absolute_delta ? segment : level + segment;
    }
    if (header.loop_filter.use_delta) {
        level += header.loop_filter.reference_delta[0];
        if (mode.luma == Vp8IntraMode::subblocks) {
            level += header.loop_filter.mode_delta[0];
        }
    }
    return minimum<i32>(63, maximum<i32>(0, level));
}

MediaAtlasResult allocate_and_clear_surface(MediaAtlasGpu* gpu, MediaAtlas* atlas,
                                            GpuContext1* context, bool preview, MediaAssetKey key,
                                            u16 width, u16 height, MediaAtlasResult* local,
                                            bool clear_cached) noexcept {
    MediaAtlasResult allocation = media_atlas_allocate(atlas, key, width, height);
    if (!allocation.ok()) {
        return allocation;
    }
    *local = allocation;
    if (!media_atlas_gpu_select(gpu, context, *atlas, preview, local)) {
        // The rows could not be backed: nothing may sample or write them.
        media_atlas_forget(atlas, key);
        allocation.error = MediaAtlasError::no_capacity;
        return allocation;
    }
    // Opaque full-frame video overwrites every pixel, and cached allocation
    // padding was already cleared. Partial/disposal codecs keep the default.
    if (allocation.cached && !clear_cached) {
        return allocation;
    }
    const LONG padding = atlas->allocations.padding;
    const GpuRect rectangle{
        maximum<LONG>(0, static_cast<LONG>(local->x) - padding),
        maximum<LONG>(0, static_cast<LONG>(local->y) - padding),
        static_cast<LONG>(local->x + local->width) + padding,
        static_cast<LONG>(local->y + local->height) + padding,
    };
    unbind_media_pixel_resources(context);
    gpu_clear_uav_rect(context, gpu->texture_uav, kTransparentColor, rectangle);
    return allocation;
}

// A non-empty frame rectangle inside the canvas.
static bool frame_fits_canvas(u32 x_offset, u32 y_offset, u32 width, u32 height, u32 canvas_width,
                              u32 canvas_height) noexcept {
    return width != 0 && height != 0 && x_offset <= canvas_width && y_offset <= canvas_height &&
           width <= canvas_width - x_offset && height <= canvas_height - y_offset;
}

bool apng_control_valid(const ApngFrame& frame, u16 canvas_width, u16 canvas_height) noexcept {
    return frame_fits_canvas(frame.x_offset, frame.y_offset, frame.width, frame.height, canvas_width, canvas_height) &&
           static_cast<u8>(frame.dispose) <= static_cast<u8>(ApngDispose::previous) &&
           static_cast<u8>(frame.blend) <= static_cast<u8>(ApngBlend::over);
}

// The frame image comes from the APNG decode (configure_frame_image) or the profile fit (png_image_configure):
// its format and row sizes are valid.
bool apng_frame_valid(const ApngFrame& frame, u16 canvas_width, u16 canvas_height,
                      const PngImage& frame_image) noexcept {
    return apng_control_valid(frame, canvas_width, canvas_height) && frame.width == frame_image.width &&
           frame.height == frame_image.height;
}

bool gif_control_valid(const GifFrame& frame, u16 canvas_width, u16 canvas_height) noexcept {
    return frame_fits_canvas(frame.x_offset, frame.y_offset, frame.width, frame.height, canvas_width, canvas_height) &&
           frame.width <= kGifDimensionMaximum && frame.height <= kGifDimensionMaximum &&
           static_cast<u8>(frame.dispose) <= static_cast<u8>(GifDispose::previous);
}

bool gif_frame_valid(const GifFrame& frame, u16 canvas_width, u16 canvas_height, const GifPalette& palette,
                     ConstSlice<u8> indices) noexcept {
    const u64 pixels = static_cast<u64>(frame.width) * frame.height;
    if (!gif_control_valid(frame, canvas_width, canvas_height) || palette.count < 2 ||
        palette.count > 256 || pixels > indices.count || !indices.data) {
        return false;
    }
    // The decoder rejects indices past the palette; the shader clamps any
    // other source.
    return !palette.has_transparency || palette.transparent_index < palette.count;
}

bool animated_webp_control_valid(const AnimatedWebpFrame& frame, u16 canvas_width,
                                 u16 canvas_height) noexcept {
    return frame_fits_canvas(frame.x_offset, frame.y_offset, frame.width, frame.height, canvas_width, canvas_height) &&
           frame.width <= kWebpDimensionMaximum && frame.height <= kWebpDimensionMaximum &&
           static_cast<u8>(frame.blend) <= static_cast<u8>(AnimatedWebpBlend::source) &&
           static_cast<u8>(frame.dispose) <= static_cast<u8>(AnimatedWebpDispose::background);
}

bool animated_webp_frame_valid(const AnimatedWebpFrame& frame, u16 canvas_width, u16 canvas_height,
                               ConstSlice<u8> rgba8) noexcept {
    const u64 required = static_cast<u64>(frame.width) * frame.height * 4u;
    return animated_webp_control_valid(frame, canvas_width, canvas_height) && rgba8.data &&
           required <= rgba8.count;
}
} // namespace maicord::media_atlas_gpu_detail
#pragma clang attribute pop
