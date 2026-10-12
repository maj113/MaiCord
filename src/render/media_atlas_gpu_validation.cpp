#include "media_atlas_gpu_internal.hpp"

namespace maicord::media_atlas_gpu_detail {

bool jpeg_icc_curve_valid(const JpegIccCurve& curve, u32 entry_count) noexcept {
    if (static_cast<u8>(curve.kind) > static_cast<u8>(JpegIccCurveKind::table)) {
        return false;
    }
    return curve.kind != JpegIccCurveKind::table ||
           (curve.table_count >= 2 && curve.table_count <= kJpegIccCurveEntryMaximum &&
            curve.table_offset <= entry_count && curve.table_count <= entry_count - curve.table_offset);
}

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

bool jpeg_image_valid(const JpegImage& image) noexcept {
    if (!image.tables || image.width == 0 || image.height == 0 || image.width > kJpegDimensionMaximum ||
        image.height > kJpegDimensionMaximum ||
        (image.component_count != 1 && image.component_count != 3 && image.component_count != 4) ||
        image.maximum_horizontal_sampling == 0 || image.maximum_horizontal_sampling > 2 ||
        image.maximum_vertical_sampling == 0 || image.maximum_vertical_sampling > 2 ||
        image.coefficient_blocks == 0 || image.coefficient_blocks > kJpegCoefficientBlockMaximum ||
        !jpeg_orientation_valid(image.orientation) ||
        static_cast<u8>(image.color_transform) > static_cast<u8>(JpegColorTransform::ycck)) {
        return false;
    }
    if (image.tables->icc && static_cast<u8>(image.tables->icc->profile.disposition) >
                                 static_cast<u8>(JpegIccDisposition::unsupported_ignored)) {
        return false;
    }
    if (image.tables->icc && image.tables->icc->profile.disposition == JpegIccDisposition::applied) {
        const JpegIccWorkspace& icc = *image.tables->icc;
        const JpegIccProfile& profile = icc.profile;
        if (profile.entries != icc.entries || profile.profile_bytes == 0 ||
            (profile.major_version != 2 && profile.major_version != 4) ||
            profile.entry_count > kJpegIccEntryMaximum ||
            static_cast<u8>(profile.pcs) > static_cast<u8>(JpegIccPcs::lab) ||
            static_cast<u8>(profile.data_color_space) > static_cast<u8>(JpegIccDataColorSpace::cmyk)) {
            return false;
        }
        if (profile.transform_kind == JpegIccTransformKind::matrix_trc) {
            if (profile.pcs != JpegIccPcs::xyz || profile.data_color_space != JpegIccDataColorSpace::rgb ||
                image.component_count != 3) {
                return false;
            }
            for (u32 channel = 0; channel < kJpegIccCurveChannelCount; ++channel) {
                if (!jpeg_icc_curve_valid(profile.curves[channel], profile.entry_count)) {
                    return false;
                }
            }
        } else if (profile.transform_kind == JpegIccTransformKind::gray_trc) {
            if (profile.data_color_space != JpegIccDataColorSpace::gray || image.component_count != 1 ||
                !jpeg_icc_curve_valid(profile.curves[0], profile.entry_count)) {
                return false;
            }
        } else if (profile.transform_kind == JpegIccTransformKind::lut) {
            const JpegIccLut& lut = profile.lut;
            if (lut.input_channels != image.component_count || lut.input_channels == 0 ||
                lut.input_channels > kJpegIccInputChannelMaximum ||
                lut.output_channels != kJpegIccCurveChannelCount ||
                (lut.precision_bytes != 1 && lut.precision_bytes != 2) ||
                (lut.precision_bytes == 1 && profile.pcs == JpegIccPcs::xyz) ||
                (image.component_count == 1 && profile.data_color_space != JpegIccDataColorSpace::gray) ||
                (image.component_count == 3 && profile.data_color_space != JpegIccDataColorSpace::rgb) ||
                (image.component_count == 4 && profile.data_color_space != JpegIccDataColorSpace::cmyk)) {
                return false;
            }
            u64 grid_product = 1;
            for (u32 channel = 0; channel < lut.input_channels; ++channel) {
                if (lut.grid_points[channel] < 2 || lut.grid_points[channel] > kJpegIccLutGridMaximum ||
                    !jpeg_icc_curve_valid(lut.input_curves[channel], profile.entry_count)) {
                    return false;
                }
                grid_product *= lut.grid_points[channel];
            }
            for (u32 channel = 0; channel < kJpegIccCurveChannelCount; ++channel) {
                if (!jpeg_icc_curve_valid(lut.output_curves[channel], profile.entry_count)) {
                    return false;
                }
            }
            if (grid_product * kJpegIccCurveChannelCount != lut.clut_entry_count ||
                lut.clut_entry_count > kJpegIccLutClutEntryMaximum ||
                lut.clut_offset > profile.entry_count ||
                lut.clut_entry_count > profile.entry_count - lut.clut_offset) {
                return false;
            }
        } else {
            return false;
        }
    }
    const u32 expected_mcu_columns =
        (static_cast<u32>(image.width) + 8u * image.maximum_horizontal_sampling - 1u) /
        (8u * image.maximum_horizontal_sampling);
    const u32 expected_mcu_rows =
        (static_cast<u32>(image.height) + 8u * image.maximum_vertical_sampling - 1u) /
        (8u * image.maximum_vertical_sampling);
    if (image.mcu_columns != expected_mcu_columns || image.mcu_rows != expected_mcu_rows) {
        return false;
    }
    u32 expected_offset = 0;
    u8 maximum_horizontal = 0;
    u8 maximum_vertical = 0;
    for (u32 index = 0; index < image.component_count; ++index) {
        const JpegComponent& component = image.components[index];
        const u32 expected_columns = expected_mcu_columns * component.horizontal_sampling;
        const u32 expected_rows = expected_mcu_rows * component.vertical_sampling;
        const u32 expected_count = static_cast<u32>(component.block_columns) * component.block_rows;
        if (component.block_offset != expected_offset || component.block_count != expected_count ||
            component.block_columns != expected_columns || component.block_rows != expected_rows ||
            expected_count == 0 || component.horizontal_sampling == 0 ||
            component.horizontal_sampling > 2 || component.vertical_sampling == 0 ||
            component.vertical_sampling > 2 || component.quantization_table >= kJpegTableMaximum ||
            !image.tables->quantization[component.quantization_table].present) {
            return false;
        }
        maximum_horizontal = maximum(maximum_horizontal, component.horizontal_sampling);
        maximum_vertical = maximum(maximum_vertical, component.vertical_sampling);
        expected_offset += expected_count;
    }
    return expected_offset == image.coefficient_blocks &&
           maximum_horizontal == image.maximum_horizontal_sampling &&
           maximum_vertical == image.maximum_vertical_sampling &&
           (image.component_count != 1 || image.color_transform == JpegColorTransform::grayscale) &&
           (image.component_count != 3 || (image.color_transform == JpegColorTransform::ycbcr ||
                                           image.color_transform == JpegColorTransform::rgb)) &&
           (image.component_count != 4 || image.color_transform == JpegColorTransform::cmyk ||
            image.color_transform == JpegColorTransform::ycck) &&
           (image.color_transform != JpegColorTransform::rgb ||
            (image.maximum_horizontal_sampling == 1 && image.maximum_vertical_sampling == 1));
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
                                            ID3D11DeviceContext1* context, ID3D11RenderTargetView* target,
                                            u16 surface_width, u16 surface_height, MediaAssetKey key,
                                            u16 width, u16 height, bool clear_cached) noexcept {
    const MediaAtlasResult allocation = media_atlas_allocate(atlas, key, width, height);
    if (!allocation.ok()) {
        return allocation;
    }
    // Opaque full-frame video overwrites every pixel, and cached allocation
    // padding was already cleared. Partial/disposal codecs keep the default.
    if (allocation.cached && !clear_cached) {
        return allocation;
    }
    const LONG padding = atlas->allocations.padding;
    const D3D11_RECT rectangle{
        maximum<LONG>(0, static_cast<LONG>(allocation.x) - padding),
        maximum<LONG>(0, static_cast<LONG>(allocation.y) - padding),
        minimum<LONG>(surface_width, static_cast<LONG>(allocation.x + allocation.width) + padding),
        minimum<LONG>(surface_height, static_cast<LONG>(allocation.y + allocation.height) + padding),
    };
    const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    unbind_media_pixel_resources(context);
    context->ClearView(target, transparent, &rectangle, 1);
    ++gpu->cleared_regions;
    return allocation;
}

MediaAtlasResult allocate_and_clear(MediaAtlasGpu* gpu, MediaAtlas* atlas, ID3D11DeviceContext1* context,
                                    MediaAssetKey key, u16 width, u16 height) noexcept {
    return allocate_and_clear_surface(gpu, atlas, context, gpu->target, gpu->width, gpu->height, key, width,
                                      height);
}

bool apng_control_valid(const ApngFrame& frame, u16 canvas_width, u16 canvas_height) noexcept {
    return frame.width != 0 && frame.height != 0 && frame.x_offset <= canvas_width &&
           frame.y_offset <= canvas_height &&
           frame.width <= static_cast<u32>(canvas_width) - frame.x_offset &&
           frame.height <= static_cast<u32>(canvas_height) - frame.y_offset &&
           static_cast<u8>(frame.dispose) <= static_cast<u8>(ApngDispose::previous) &&
           static_cast<u8>(frame.blend) <= static_cast<u8>(ApngBlend::over);
}

bool apng_frame_valid(const ApngFrame& frame, u16 canvas_width, u16 canvas_height,
                      const PngImage& frame_image) noexcept {
    return apng_control_valid(frame, canvas_width, canvas_height) && frame.width == frame_image.width &&
           frame.height == frame_image.height && png_format_valid(frame_image);
}

bool gif_control_valid(const GifFrame& frame, u16 canvas_width, u16 canvas_height) noexcept {
    return frame.width != 0 && frame.height != 0 && frame.width <= kGifDimensionMaximum &&
           frame.height <= kGifDimensionMaximum && frame.x_offset <= canvas_width &&
           frame.y_offset <= canvas_height &&
           frame.width <= static_cast<u32>(canvas_width) - frame.x_offset &&
           frame.height <= static_cast<u32>(canvas_height) - frame.y_offset &&
           static_cast<u8>(frame.dispose) <= static_cast<u8>(GifDispose::previous);
}

bool gif_frame_valid(const GifFrame& frame, u16 canvas_width, u16 canvas_height, const GifPalette& palette,
                     ConstSlice<u8> indices) noexcept {
    const u64 pixels = static_cast<u64>(frame.width) * frame.height;
    if (!gif_control_valid(frame, canvas_width, canvas_height) || palette.count < 2 ||
        palette.count > 256 || pixels > indices.count || !indices.data) {
        return false;
    }
    if (palette.has_transparency && palette.transparent_index >= palette.count) {
        return false;
    }
    // Every u8 index is valid for a full palette. Avoid rereading the entire
    // decoded image on the UI thread before its upload in this common case.
    if (palette.count == 256) return true;
    for (u32 index = 0; index < pixels; ++index) {
        if (indices[index] >= palette.count) {
            return false;
        }
    }
    return true;
}

bool animated_webp_control_valid(const AnimatedWebpFrame& frame, u16 canvas_width,
                                 u16 canvas_height) noexcept {
    return frame.width != 0 && frame.height != 0 && frame.width <= kWebpDimensionMaximum &&
           frame.height <= kWebpDimensionMaximum && frame.x_offset <= canvas_width &&
           frame.y_offset <= canvas_height &&
           frame.width <= static_cast<u32>(canvas_width) - frame.x_offset &&
           frame.height <= static_cast<u32>(canvas_height) - frame.y_offset &&
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
