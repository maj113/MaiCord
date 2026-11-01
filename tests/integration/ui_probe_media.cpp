#include "ui_probe_internal.hpp"
#include "probe_capture.hpp"
#include "../../src/render/text/shaped_text_cache.hpp"
#include "../support/media_fixture.hpp"
#include "../fixtures/webp_fixture.hpp"

namespace maicord {
namespace ui_probe_detail {

// Shaped-text reuse must not change any emitted quad, batch, semantic or hit
// region when cached strings land at new (fractional) origins.
bool shaped_text_cache_equivalence() noexcept {
    auto& renderer = g_probe.renderer;
    auto& text = g_probe.text;
    auto& timeline = g_probe.message_view.timeline;
    static GlyphQuad quads[kRendererContentGlyphCapacity];
    static GlyphDrawBatch batches[kRendererContentBatchCapacity];
    static MessageTextSemantic semantics[kRendererContentSemanticCapacity];
    static TextHitRegion hits[kRendererContentHitRegionCapacity];
    const u64 start = timeline.scroll_units;
    const u64 maximum_scroll =
        timeline.list->total_units > timeline.viewport_units ? timeline.list->total_units - timeline.viewport_units : 0;
    const u32 hits_before = renderer.shaped_text_cache ? renderer.shaped_text_cache->stats.hits : 0;
    bool equal = true;
    for (u32 step = 0; step < 6 && equal; ++step) {
        // 37/64 DIP steps move every cached command to a new subpixel phase.
        const u64 position = minimum(maximum_scroll, start + step * 37u);
        renderer.shaped_text_cache_disabled = false;
        content_text_retention_invalidate(&renderer.content_text_retention);
        if (!message_timeline_set_scroll(&timeline, position, false) || !rebuild_probe_content_text())
            return false;
        const u32 quad_count = text.quad_count, batch_count = text.batch_count;
        const u32 semantic_count = text.content_semantic_count, hit_count = text.content_hit_region_count;
        copy_bytes(quads, text.quads, quad_count * sizeof(GlyphQuad));
        copy_bytes(batches, text.batches, batch_count * sizeof(GlyphDrawBatch));
        copy_bytes(semantics, text.content_semantics, semantic_count * sizeof(MessageTextSemantic));
        copy_bytes(hits, text.content_hit_regions, hit_count * sizeof(TextHitRegion));
        renderer.shaped_text_cache_disabled = true;
        content_text_retention_invalidate(&renderer.content_text_retention);
        if (!rebuild_probe_content_text())
            return false;
        equal = text.quad_count == quad_count && text.batch_count == batch_count &&
                text.content_semantic_count == semantic_count && text.content_hit_region_count == hit_count &&
                bytes_equal(quads, text.quads, quad_count * sizeof(GlyphQuad)) &&
                bytes_equal(batches, text.batches, batch_count * sizeof(GlyphDrawBatch)) &&
                bytes_equal(semantics, text.content_semantics, semantic_count * sizeof(MessageTextSemantic)) &&
                bytes_equal(hits, text.content_hit_regions, hit_count * sizeof(TextHitRegion));
    }
    renderer.shaped_text_cache_disabled = false;
    const bool reused = renderer.shaped_text_cache && renderer.shaped_text_cache->stats.hits > hits_before;
    if (!equal || !reused) {
        log_u64(LogLevel::error, "probe", "shaped_text_cache_equivalence_equal", equal ? 1 : 0);
        log_u64(LogLevel::error, "probe", "shaped_text_cache_equivalence_reused", reused ? 1 : 0);
        return false;
    }
    content_text_retention_invalidate(&renderer.content_text_retention);
    return message_timeline_set_scroll(&timeline, start, false) && rebuild_probe_content_text();
}

} // namespace ui_probe_detail

namespace {

template<class Expected>
bool rgba_pixels_exact(ProbeTargetReadback& readback, u32 left, u32 top,
                       u32 width, u32 height, Expected expected) noexcept {
    bool exact = SUCCEEDED(readback.map());
    const auto& mapped = readback.mapped;
    for (u32 y = 0; y < height && exact; ++y) {
        const auto* row = static_cast<const u8*>(mapped.pData) + (top + y) * mapped.RowPitch + left * 4;
        for (u32 x = 0; x < width; ++x) {
            const u8* pixel = row + x * 4;
            const u32 actual = static_cast<u32>(pixel[0]) | static_cast<u32>(pixel[1]) << 8 |
                               static_cast<u32>(pixel[2]) << 16 | static_cast<u32>(pixel[3]) << 24;
            exact = exact && actual == expected(x, y);
        }
    }
    return exact;
}

// Reads the width x height atlas region back from the main or preview media texture.
template<class Expected>
bool atlas_pixels_exact(const MediaAtlasResult& allocation, bool preview, u32 width, u32 height,
                        Expected expected) noexcept {
    ProbeTargetReadback readback;
    if (!readback.initialize_rgba(g_probe.renderer, width, height))
        return false;
    const D3D11_BOX box{allocation.x, allocation.y, 0, allocation.x + width, allocation.y + height, 1};
    g_probe.renderer.graphics.context->CopySubresourceRegion(
        readback.staging, 0, 0, 0, 0,
        preview ? g_probe.renderer.media_gpu.preview_texture : g_probe.renderer.media_gpu.texture, 0, &box);
    return rgba_pixels_exact(readback, 0, 0, width, height, expected);
}

} // namespace

namespace ui_probe_detail {

bool apng_atlas_pixels_exact(const MediaAtlasResult& allocation, u32 presented_frame, bool preview) noexcept {
    return atlas_pixels_exact(allocation, preview, 4, 4, [presented_frame](u32 x, u32 y) noexcept {
        return apng_fixture_canvas_rgba(presented_frame, x, y);
    });
}

bool gif_atlas_pixels_exact(const MediaAtlasResult& allocation, u32 presented_frame) noexcept {
    return atlas_pixels_exact(allocation, false, 4, 4, [presented_frame](u32 x, u32 y) noexcept {
        return gif_fixture_canvas_rgba(presented_frame, x, y);
    });
}

bool animated_webp_atlas_pixels_exact(const MediaAtlasResult& allocation, u32 presented_frame,
                                      bool preview) noexcept {
    return atlas_pixels_exact(allocation, preview, 4, 4, [presented_frame](u32 x, u32 y) noexcept {
        return animated_webp_fixture_canvas_rgba(presented_frame, x, y);
    });
}

bool mixed_animated_webp_atlas_pixels_exact(const MediaAtlasResult& allocation,
                                            u32 presented_frame) noexcept {
    return atlas_pixels_exact(allocation, false, 4, 4, [presented_frame](u32 x, u32 y) noexcept {
        return mixed_animated_webp_fixture_canvas_rgba(presented_frame, x, y);
    });
}

bool jpeg_atlas_pixels_exact(const MediaAtlasResult& allocation, u16 width, u16 height,
                             bool preview) noexcept {
    return atlas_pixels_exact(allocation, preview, width, height,
                              [](u32, u32) noexcept { return jpeg_fixture_rgba(); });
}

} // namespace ui_probe_detail

namespace {

// Copies a media atlas texture into a mapped staging readback.
bool read_media_texture(ProbeTargetReadback& readback, ID3D11Texture2D* texture) noexcept {
    if (!readback.initialize_rgba(g_probe.renderer, kProbeMediaExtent, kProbeMediaExtent))
        return false;
    g_probe.renderer.graphics.context->CopyResource(readback.staging, texture);
    return SUCCEEDED(readback.map());
}

const u8* atlas_row(const ProbeTargetReadback& readback, const MediaAtlasResult& allocation, u32 y) noexcept {
    return static_cast<const u8*>(readback.mapped.pData) + (allocation.y + y) * readback.mapped.RowPitch +
           allocation.x * 4u;
}

// Largest RGB channel error of the allocation against expected(x, y, channel);
// false unless the read succeeds and every alpha is 255.
template<class Expected>
bool atlas_rgb_error(const MediaAtlasResult& allocation, u32 width, u32 height, Expected expected,
                     u32* maximum_error) noexcept {
    ProbeTargetReadback readback;
    if (!read_media_texture(readback, g_probe.renderer.media_gpu.texture))
        return false;
    bool opaque = true;
    *maximum_error = 0;
    for (u32 y = 0; y < height; ++y) {
        const u8* row = atlas_row(readback, allocation, y);
        for (u32 x = 0; x < width; ++x) {
            for (u32 channel = 0; channel < 3; ++channel) {
                const u8 actual = row[x * 4u + channel];
                const u8 wanted = expected(x, y, channel);
                *maximum_error = maximum<u32>(*maximum_error, actual > wanted ? actual - wanted : wanted - actual);
            }
            opaque = opaque && row[x * 4u + 3u] == 255;
        }
    }
    return opaque;
}

} // namespace

namespace ui_probe_detail {

bool jpeg_atlas_pixels_reference_tolerance(const MediaAtlasResult& allocation, u16 width, u16 height,
                                           ConstSlice<u8> reference_rgb,
                                           JpegOrientation orientation,
                                           bool swap_red_blue) noexcept {
    if (!reference_rgb.data || reference_rgb.count != static_cast<u32>(width) * height * 3u ||
        !jpeg_orientation_valid(orientation)) {
        return false;
    }
    const bool swapped = jpeg_orientation_swaps_dimensions(orientation);
    const u32 output_width = swapped ? height : width;
    const u32 output_height = swapped ? width : height;
    if (allocation.width != output_width || allocation.height != output_height) {
        return false;
    }
    u32 maximum_error = 0;
    return atlas_rgb_error(allocation, output_width, output_height,
                           [&](u32 x, u32 y, u32 channel) noexcept {
                               u32 source_x = x;
                               u32 source_y = y;
                               switch (orientation) {
                               case JpegOrientation::top_right:
                                   source_x = width - 1u - x;
                                   break;
                               case JpegOrientation::bottom_right:
                                   source_x = width - 1u - x;
                                   source_y = height - 1u - y;
                                   break;
                               case JpegOrientation::bottom_left:
                                   source_y = height - 1u - y;
                                   break;
                               case JpegOrientation::left_top:
                                   source_x = y;
                                   source_y = x;
                                   break;
                               case JpegOrientation::right_top:
                                   source_x = y;
                                   source_y = height - 1u - x;
                                   break;
                               case JpegOrientation::right_bottom:
                                   source_x = width - 1u - y;
                                   source_y = height - 1u - x;
                                   break;
                               case JpegOrientation::left_bottom:
                                   source_x = width - 1u - y;
                                   source_y = x;
                                   break;
                               case JpegOrientation::top_left:
                                   break;
                               }
                               const u32 reference_channel = swap_red_blue && channel != 1 ? 2u - channel : channel;
                               return reference_rgb[(source_y * width + source_x) * 3u + reference_channel];
                           },
                           &maximum_error) &&
           maximum_error <= 2;
}

} // namespace ui_probe_detail

namespace {

float probe_clamp_unit(float value) noexcept {
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

float probe_twelfth_root(float value) noexcept {
    float root = 1.0f;
    for (u32 iteration = 0; iteration < 24; ++iteration) {
        float power = root;
        for (u32 exponent = 1; exponent < 11; ++exponent) {
            power *= root;
        }
        root = (11.0f * root + value / power) / 12.0f;
    }
    return root;
}

u8 probe_srgb_byte(float linear) noexcept {
    linear = probe_clamp_unit(linear);
    float encoded = linear * 12.920000076f;
    if (linear >= 0.003130805f) {
        const float root = probe_twelfth_root(1.137283325f * linear);
        const float square = root * root;
        encoded = square * square * root - 0.054969788f;
    }
    const float byte = probe_clamp_unit(encoded) * 255.0f;
    return static_cast<u8>(byte + 0.5f);
}

} // namespace

namespace ui_probe_detail {

void probe_icc_lab_to_srgb(float lightness_normalized, float a_normalized, float b_normalized,
                           u8 expected[3]) noexcept {
    const float lightness = lightness_normalized * 100.0f;
    const float a = a_normalized * 255.0f - 128.0f;
    const float b = b_normalized * 255.0f - 128.0f;
    const float root_y = (lightness + 16.0f) / 116.0f;
    const float roots[3] = {root_y + a / 500.0f, root_y, root_y - b / 200.0f};
    float xyz[3]{};
    for (u32 channel = 0; channel < 3; ++channel) {
        const float cube = roots[channel] * roots[channel] * roots[channel];
        xyz[channel] = cube > 0.008856f ? cube : (roots[channel] - 16.0f / 116.0f) / 7.787f;
    }
    xyz[0] *= 0.9642f;
    xyz[2] *= 0.8249f;
    constexpr float matrix[9] = {
        3.13411215f,   -1.61739242f,  -0.490633398f, -0.978787303f, 1.91627955f,
        0.0334547125f, 0.0719830394f, -0.228985846f, 1.40538514f,
    };
    for (u32 row = 0; row < 3; ++row) {
        const float linear =
            matrix[row * 3u] * xyz[0] + matrix[row * 3u + 1u] * xyz[1] + matrix[row * 3u + 2u] * xyz[2];
        expected[row] = probe_srgb_byte(linear);
    }
}

} // namespace ui_probe_detail

namespace {

void probe_icc_rgb_lut_expected(const u8* source, u8 expected[3]) noexcept {
    const float red = static_cast<float>(source[0]) / 255.0f;
    const float green = static_cast<float>(source[1]) / 255.0f;
    const float blue = static_cast<float>(source[2]) / 255.0f;
    probe_icc_lab_to_srgb(0.25f + 0.50f * red, 0.375f + 0.25f * green, 0.375f + 0.25f * blue, expected);
}

} // namespace

namespace ui_probe_detail {

bool jpeg_icc_rgb_lut_pixels_tolerance(const MediaAtlasResult& allocation,
                                       ConstSlice<u8> reference_rgb) noexcept {
    if (!reference_rgb.data || reference_rgb.count != 16u * 16u * 3u || allocation.width != 16 ||
        allocation.height != 16) {
        return false;
    }
    u32 maximum_error = 0;
    // D3D11 shader interpolation/rounding differs slightly across drivers.
    // The current hardware reaches 8/255; larger errors still fail the probe.
    return atlas_rgb_error(allocation, 16, 16,
                           [&](u32 x, u32 y, u32 channel) noexcept {
                               u8 expected[3]{};
                               probe_icc_rgb_lut_expected(reference_rgb.data + (y * 16u + x) * 3u, expected);
                               return expected[channel];
                           },
                           &maximum_error) &&
           maximum_error <= 8;
}

bool jpeg_atlas_uniform_rgb_tolerance(const MediaAtlasResult& allocation, u16 width, u16 height,
                                      const u8 expected[3], u8 tolerance) noexcept {
    if (!expected || allocation.width != width || allocation.height != height) {
        return false;
    }
    u32 maximum_error = 0;
    return atlas_rgb_error(allocation, width, height, [&](u32, u32, u32 channel) noexcept { return expected[channel]; },
                           &maximum_error) &&
           maximum_error <= tolerance;
}

bool jpeg_atlas_differs_from_reference(const MediaAtlasResult& allocation, u16 width, u16 height,
                                       ConstSlice<u8> reference_rgb, u8 minimum_error) noexcept {
    if (!reference_rgb.data || reference_rgb.count != static_cast<u32>(width) * height * 3u ||
        allocation.width != width || allocation.height != height) {
        return false;
    }
    u32 maximum_error = 0;
    return atlas_rgb_error(allocation, width, height,
                           [&](u32 x, u32 y, u32 channel) noexcept { return reference_rgb[(y * width + x) * 3u + channel]; },
                           &maximum_error) &&
           maximum_error >= minimum_error;
}

} // namespace ui_probe_detail

namespace {

void set_vp8_probe_grid(u32 columns, u32 rows) noexcept {
    g_probe.vp8_modes.columns = g_probe.vp8_coefficients.columns = columns;
    g_probe.vp8_modes.rows = g_probe.vp8_coefficients.rows = rows;
    g_probe.vp8_modes.macroblock_count = g_probe.vp8_coefficients.macroblock_count = columns * rows;
}

// Makes macroblock `index` coefficient-free with the given prediction modes.
void reset_vp8_probe_macroblock(u32 index, Vp8IntraMode luma, Vp8IntraMode chroma,
                                Vp8SubblockMode subblock = Vp8SubblockMode::dc) noexcept {
    Vp8MacroblockMode& mode = g_probe.vp8_modes.macroblocks[index];
    for (auto& value : mode.luma_subblock)
        value = subblock;
    mode.luma = luma;
    mode.chroma = chroma;
    mode.segment_id = 0;
    mode.skip_coefficients = 0;
    Vp8MacroblockCoefficients& coefficient = g_probe.vp8_coefficients.macroblocks[index];
    for (auto& block : coefficient.values)
        for (auto& value : block)
            value = 0;
    coefficient.has_coefficients = 0;
    coefficient.reserved = 0;
}

void reset_vp8_gpu_probe(Vp8IntraMode luma, Vp8IntraMode chroma) noexcept {
    set_vp8_probe_grid(1, 1);
    reset_vp8_probe_macroblock(0, luma, chroma);
}

// A CPU copy of the VP8 YUV sample buffer (one u32 per sample), mapped for the object's lifetime.
struct Vp8YuvReadback {
    ID3D11Buffer* staging = nullptr;
    const u32* samples = nullptr;
    Vp8YuvReadback() noexcept {
        const MediaAtlasGpu& gpu = g_probe.renderer.media_gpu;
        auto* context = g_probe.renderer.graphics.context;
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = gpu.vp8_yuv_capacity;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_probe.renderer.graphics.device->CreateBuffer(&description, nullptr, &staging)))
            return;
        context->CopyResource(staging, gpu.vp8_yuv_buffer);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
            samples = static_cast<const u32*>(mapped.pData);
    }
    Vp8YuvReadback(const Vp8YuvReadback&) = delete;
    Vp8YuvReadback& operator=(const Vp8YuvReadback&) = delete;
    ~Vp8YuvReadback() noexcept {
        if (samples)
            g_probe.renderer.graphics.context->Unmap(staging, 0);
        com_release(staging);
    }
};

bool vp8_gpu_first_block_exact(const u8 expected_luma[16], u8 expected_chroma) noexcept {
    const MediaAtlasGpu& gpu = g_probe.renderer.media_gpu;
    if (!gpu.vp8_yuv_buffer || gpu.vp8_yuv_capacity == 0 || gpu.vp8_luma_stride != 16 ||
        gpu.vp8_luma_rows != 16 || gpu.vp8_chroma_stride != 8 || gpu.vp8_chroma_rows != 8) {
        return false;
    }
    const Vp8YuvReadback yuv;
    if (!yuv.samples)
        return false;
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 4; ++x)
            if (yuv.samples[y * 16u + x] != expected_luma[y * 4u + x])
                return false;
    constexpr u32 u_offset = 16u * 16u;
    constexpr u32 v_offset = u_offset + 8u * 8u;
    for (u32 index = 0; index < 8u * 8u; ++index)
        if (yuv.samples[u_offset + index] != expected_chroma || yuv.samples[v_offset + index] != expected_chroma)
            return false;
    return true;
}

bool vp8_gpu_luma_block_exact(u32 origin_x, u32 origin_y, const u8 expected[16]) noexcept {
    const u32 stride = g_probe.renderer.media_gpu.vp8_luma_stride;
    const Vp8YuvReadback yuv;
    if (!yuv.samples)
        return false;
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u32 actual = yuv.samples[(origin_y + y) * stride + origin_x + x];
            if (actual != expected[y * 4u + x]) {
                log_u64(LogLevel::error, "probe", "vp8_block_mismatch_x", origin_x + x);
                log_u64(LogLevel::error, "probe", "vp8_block_mismatch_y", origin_y + y);
                log_u64(LogLevel::error, "probe", "vp8_block_actual", actual);
                log_u64(LogLevel::error, "probe", "vp8_block_expected", expected[y * 4u + x]);
                return false;
            }
        }
    }
    return true;
}

bool vp8_gpu_luma_row_exact(u32 origin_x, u32 y, const u8* expected, u32 count) noexcept {
    const u32 stride = g_probe.renderer.media_gpu.vp8_luma_stride;
    const Vp8YuvReadback yuv;
    if (!yuv.samples)
        return false;
    for (u32 index = 0; index < count; ++index)
        if (yuv.samples[y * stride + origin_x + index] != expected[index])
            return false;
    return true;
}

u8 vp8_test_clip_rgb(i32 value) noexcept {
    return static_cast<u8>(minimum<i32>(255, maximum<i32>(0, value >> 6)));
}

void vp8_test_rgb(u8 y, u8 u, u8 v, u8 output[3]) noexcept {
    const i32 scaled_y = (static_cast<i32>(y) * 19077) >> 8;
    output[0] = vp8_test_clip_rgb(scaled_y + (static_cast<i32>(v) * 26149 >> 8) - 14234);
    output[1] = vp8_test_clip_rgb(scaled_y - (static_cast<i32>(u) * 6419 >> 8) -
                                  (static_cast<i32>(v) * 13320 >> 8) + 8708);
    output[2] = vp8_test_clip_rgb(scaled_y + (static_cast<i32>(u) * 33050 >> 8) - 17685);
}

bool vp8_atlas_row_exact(const MediaAtlasResult& allocation, u32 origin_x, const u8* expected_gray,
                         u32 count) noexcept {
    ProbeTargetReadback readback;
    if (!read_media_texture(readback, g_probe.renderer.media_gpu.texture))
        return false;
    const u8* row = atlas_row(readback, allocation, 0) + origin_x * 4u;
    for (u32 x = 0; x < count; ++x) {
        const u8* pixel = row + x * 4u;
        if (pixel[0] != expected_gray[x] || pixel[1] != expected_gray[x] || pixel[2] != expected_gray[x] ||
            pixel[3] != 255)
            return false;
    }
    return true;
}

bool vp8_alpha_atlas_exact(const MediaAtlasResult& allocation) noexcept {
    constexpr u8 positions[7][2] = {
        {0, 0}, {7, 0}, {8, 0}, {0, 7}, {0, 8}, {7, 7}, {8, 8},
    };
    constexpr u8 expected_u[7] = {124, 126, 130, 127, 133, 128, 126};
    ProbeTargetReadback readback;
    if (!read_media_texture(readback, g_probe.renderer.media_gpu.texture))
        return false;
    for (u32 index = 0; index < 7; ++index) {
        const u32 x = positions[index][0];
        const u32 y = positions[index][1];
        const u8* pixel = atlas_row(readback, allocation, y) + x * 4u;
        u8 rgb[3]{};
        vp8_test_rgb(128, expected_u[index], 128, rgb);
        const u8 alpha = g_probe.png_scanlines[y * 16u + x];
        for (u32 channel = 0; channel < 3; ++channel) {
            const u8 premultiplied = static_cast<u8>((static_cast<u32>(rgb[channel]) * alpha + 127u) / 255u);
            if (pixel[channel] != premultiplied)
                return false;
        }
        if (pixel[3] != alpha)
            return false;
    }
    return true;
}

bool media_surface_rgba_exact(ID3D11Texture2D* texture, const MediaAtlasResult& allocation,
                              ConstSlice<u8> expected_rgba) noexcept {
    if (!texture || !expected_rgba.data || expected_rgba.count == 0 || (expected_rgba.count & 3u) != 0) {
        return false;
    }
    ProbeTargetReadback readback;
    if (!read_media_texture(readback, texture))
        return false;
    const u32 pixel_count = expected_rgba.count / 4u;
    const u32 width = allocation.width;
    if (width == 0 || pixel_count % width != 0 || pixel_count / width != allocation.height)
        return false;
    for (u32 y = 0; y < allocation.height; ++y)
        if (!bytes_equal(atlas_row(readback, allocation, y), expected_rgba.data + y * width * 4u, width * 4u))
            return false;
    return true;
}

bool vp8_atlas_rgba_exact(const MediaAtlasResult& allocation, ConstSlice<u8> expected_rgba) noexcept {
    return media_surface_rgba_exact(g_probe.renderer.media_gpu.texture, allocation, expected_rgba);
}

bool vp8_atlas_rgba_hash(const MediaAtlasResult& allocation, u64* output) noexcept {
    if (!output || allocation.width == 0 || allocation.height == 0) {
        return false;
    }
    ProbeTargetReadback readback;
    if (!read_media_texture(readback, g_probe.renderer.media_gpu.texture))
        return false;
    u64 hash = 1'469'598'103'934'665'603ull;
    for (u32 y = 0; y < allocation.height; ++y) {
        const u8* row = atlas_row(readback, allocation, y);
        for (u32 byte = 0; byte < allocation.width * 4u; ++byte)
            hash = (hash ^ row[byte]) * 1'099'511'628'211ull;
    }
    *output = hash;
    return true;
}

void setup_vp8_inner_filter_probe(bool high_edge_variance) noexcept {
    reset_vp8_gpu_probe(Vp8IntraMode::dc, Vp8IntraMode::dc);
    Vp8MacroblockCoefficients& coefficient = g_probe.vp8_coefficients.macroblocks[0];
    for (u32 block = 0; block < 16; ++block) {
        const u32 block_x = block & 3u;
        if (block_x == 0u) {
            if (high_edge_variance) {
                coefficient.values[block][1] = 16;
            } else {
                coefficient.values[block][0] = -32;
            }
        } else {
            coefficient.values[block][0] = 32;
        }
    }
    coefficient.has_coefficients = 1;
}

void setup_vp8_outer_filter_probe() noexcept {
    set_vp8_probe_grid(2, 1);
    for (u32 macroblock = 0; macroblock < 2; ++macroblock) {
        reset_vp8_probe_macroblock(macroblock, macroblock == 0 ? Vp8IntraMode::dc : Vp8IntraMode::horizontal,
                                   Vp8IntraMode::dc);
        Vp8MacroblockCoefficients& coefficient = g_probe.vp8_coefficients.macroblocks[macroblock];
        for (u32 block = 0; block < 16; ++block) {
            coefficient.values[block][0] = macroblock == 0 ? -32 : 64;
        }
        coefficient.has_coefficients = 1;
    }
}

} // namespace

namespace ui_probe_detail {

bool vp8_gpu_reconstruction_exact() noexcept {
    const Vp8Keyframe frame{{}, {}, 0, 0, 16, 16, 0, 0, 0, 0};
    const u32 rejected_before = g_probe.renderer.media_gpu.rejected;
    const u32 reconstructions_before = g_probe.renderer.media_gpu.vp8_reconstructions;
    const u32 dispatches_before = g_probe.renderer.media_gpu.vp8_reconstruction_dispatches;
    const u64 uploaded_before = g_probe.renderer.media_gpu.vp8_uploaded_bytes;
    const u64 pixels_before = g_probe.renderer.media_gpu.vp8_reconstructed_pixels;
    const u32 filter_dispatches_before = g_probe.renderer.media_gpu.vp8_filter_dispatches;
    const u32 filtered_edges_before = g_probe.renderer.media_gpu.vp8_filtered_edges;
    const u32 filter_skips_before = g_probe.renderer.media_gpu.vp8_filter_skips;
    const u64 filtered_pixels_before = g_probe.renderer.media_gpu.vp8_filtered_pixels;
    const u32 atlas_uploads_before = g_probe.renderer.media_gpu.vp8_atlas_uploads;
    const u32 alpha_uploads_before = g_probe.renderer.media_gpu.vp8_alpha_uploads;
    const u64 atlas_pixels_before = g_probe.renderer.media_gpu.vp8_atlas_pixels;
    const u64 alpha_bytes_before = g_probe.renderer.media_gpu.vp8_alpha_uploaded_bytes;
    Vp8Keyframe invalid_frame = frame;
    invalid_frame.width = 0;
    if (renderer_reconstruct_vp8_keyframe(&g_probe.renderer, invalid_frame, g_probe.vp8_modes,
                                          g_probe.vp8_coefficients)
            .ok() ||
        g_probe.renderer.media_gpu.rejected != rejected_before + 1u ||
        g_probe.renderer.media_gpu.vp8_reconstructions != reconstructions_before) {
        return false;
    }

    constexpr u8 macroblock_values[4] = {128, 129, 127, 129};
    u8 expected[16]{};
    for (u32 mode_index = 0; mode_index < 4; ++mode_index) {
        reset_vp8_gpu_probe(static_cast<Vp8IntraMode>(mode_index), static_cast<Vp8IntraMode>(mode_index));
        for (u32 index = 0; index < 16; ++index) {
            expected[index] = macroblock_values[mode_index];
        }
        if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, frame, g_probe.vp8_modes,
                                               g_probe.vp8_coefficients)
                 .ok() ||
            !vp8_gpu_first_block_exact(expected, macroblock_values[mode_index])) {
            log_u64(LogLevel::error, "probe", "vp8_plane_mode_failed", mode_index);
            return false;
        }
    }

    constexpr u8 subblock_expected[10][16] = {
        {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
        {129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129},
        {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127},
        {129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129},
        {128, 127, 127, 127, 129, 128, 127, 127, 129, 129, 128, 127, 129, 129, 129, 128},
        {127, 127, 127, 127, 128, 127, 127, 127, 129, 127, 127, 127, 129, 128, 127, 127},
        {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127},
        {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127},
        {128, 128, 127, 127, 129, 129, 128, 128, 129, 129, 129, 129, 129, 129, 129, 129},
        {129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129},
    };
    for (u32 mode_index = 0; mode_index < 10; ++mode_index) {
        reset_vp8_gpu_probe(Vp8IntraMode::subblocks, Vp8IntraMode::dc);
        for (u32 block = 0; block < 16; ++block) {
            g_probe.vp8_modes.macroblocks[0].luma_subblock[block] =
                static_cast<Vp8SubblockMode>(mode_index);
        }
        if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, frame, g_probe.vp8_modes,
                                               g_probe.vp8_coefficients)
                 .ok() ||
            !vp8_gpu_first_block_exact(subblock_expected[mode_index], 128)) {
            log_u64(LogLevel::error, "probe", "vp8_subblock_mode_failed", mode_index);
            return false;
        }
    }

    reset_vp8_gpu_probe(Vp8IntraMode::dc, Vp8IntraMode::dc);
    g_probe.vp8_coefficients.macroblocks[0].values[0][1] = 16;
    g_probe.vp8_coefficients.macroblocks[0].has_coefficients = 1;
    constexpr u8 transform_expected[16] = {
        131, 129, 127, 126, 131, 129, 127, 126, 131, 129, 127, 126, 131, 129, 127, 126,
    };
    if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, frame, g_probe.vp8_modes,
                                           g_probe.vp8_coefficients)
             .ok() ||
        !vp8_gpu_first_block_exact(transform_expected, 128)) {
        log_write(LogLevel::error, "probe", "vp8_transform_failed");
        return false;
    }

    set_vp8_probe_grid(2, 2);
    for (u32 macroblock = 0; macroblock < 4; ++macroblock)
        reset_vp8_probe_macroblock(macroblock, Vp8IntraMode::dc, Vp8IntraMode::dc, Vp8SubblockMode::vertical_edge);
    g_probe.vp8_modes.macroblocks[0].luma = Vp8IntraMode::vertical;
    g_probe.vp8_modes.macroblocks[2].luma = Vp8IntraMode::subblocks;
    g_probe.vp8_coefficients.macroblocks[1].values[12][1] = 16;
    g_probe.vp8_coefficients.macroblocks[1].has_coefficients = 1;
    Vp8Keyframe schedule_frame = frame;
    schedule_frame.width = 32;
    schedule_frame.height = 32;
    constexpr u8 top_right_dependency[16] = {
        127, 127, 127, 128, 127, 127, 127, 128, 127, 127, 127, 128, 127, 127, 127, 128,
    };
    if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, schedule_frame, g_probe.vp8_modes,
                                           g_probe.vp8_coefficients)
             .ok() ||
        !vp8_gpu_luma_block_exact(12, 16, top_right_dependency)) {
        log_write(LogLevel::error, "probe", "vp8_top_right_dependency_failed");
        return false;
    }

    Vp8ControlHeader filter_header{};
    g_probe.failure_stage = 16;
    if (!renderer_filter_vp8_keyframe(&g_probe.renderer, schedule_frame, filter_header, g_probe.vp8_modes,
                                      g_probe.vp8_coefficients)
             .ok() ||
        g_probe.renderer.media_gpu.vp8_filter_skips != filter_skips_before + 1u ||
        renderer_filter_vp8_keyframe(&g_probe.renderer, schedule_frame, filter_header, g_probe.vp8_modes,
                                     g_probe.vp8_coefficients)
            .ok()) {
        return false;
    }

    setup_vp8_inner_filter_probe(false);
    g_probe.failure_stage = 17;
    if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, frame, g_probe.vp8_modes,
                                           g_probe.vp8_coefficients)
             .ok()) {
        return false;
    }
    constexpr u8 inner_unfiltered_expected[8] = {
        124, 124, 124, 124, 132, 132, 132, 132,
    };
    if (!vp8_gpu_luma_row_exact(0, 0, inner_unfiltered_expected, sizeof(inner_unfiltered_expected))) {
        g_probe.failure_stage = 22;
        return false;
    }
    filter_header = {};
    filter_header.loop_filter.simple = 1;
    filter_header.segmentation.enabled = 1;
    filter_header.segmentation.absolute_delta = 1;
    filter_header.segmentation.filter_strength[0] = 10;
    constexpr u8 simple_inner_expected[8] = {
        124, 124, 124, 126, 130, 132, 132, 132,
    };
    if (!renderer_filter_vp8_keyframe(&g_probe.renderer, frame, filter_header, g_probe.vp8_modes,
                                      g_probe.vp8_coefficients)
             .ok() ||
        !vp8_gpu_luma_row_exact(0, 0, simple_inner_expected, sizeof(simple_inner_expected))) {
        return false;
    }

    setup_vp8_inner_filter_probe(false);
    g_probe.failure_stage = 18;
    if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, frame, g_probe.vp8_modes,
                                           g_probe.vp8_coefficients)
             .ok()) {
        return false;
    }
    filter_header = {};
    filter_header.loop_filter.level = 7;
    filter_header.loop_filter.use_delta = 1;
    filter_header.loop_filter.reference_delta[0] = 3;
    constexpr u8 normal_inner_expected[8] = {
        124, 124, 126, 127, 129, 130, 132, 132,
    };
    if (!renderer_filter_vp8_keyframe(&g_probe.renderer, frame, filter_header, g_probe.vp8_modes,
                                      g_probe.vp8_coefficients)
             .ok() ||
        !vp8_gpu_luma_row_exact(0, 0, normal_inner_expected, sizeof(normal_inner_expected))) {
        return false;
    }

    setup_vp8_inner_filter_probe(true);
    g_probe.failure_stage = 19;
    if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, frame, g_probe.vp8_modes,
                                           g_probe.vp8_coefficients)
             .ok()) {
        return false;
    }
    filter_header = {};
    filter_header.loop_filter.level = 10;
    constexpr u8 normal_hev_expected[8] = {
        131, 129, 127, 128, 130, 132, 132, 132,
    };
    if (!renderer_filter_vp8_keyframe(&g_probe.renderer, frame, filter_header, g_probe.vp8_modes,
                                      g_probe.vp8_coefficients)
             .ok() ||
        !vp8_gpu_luma_row_exact(0, 0, normal_hev_expected, sizeof(normal_hev_expected))) {
        return false;
    }

    setup_vp8_outer_filter_probe();
    g_probe.failure_stage = 20;
    Vp8Keyframe outer_frame = frame;
    outer_frame.width = 32;
    if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, outer_frame, g_probe.vp8_modes,
                                           g_probe.vp8_coefficients)
             .ok()) {
        return false;
    }
    filter_header = {};
    filter_header.loop_filter.level = 10;
    constexpr u8 normal_outer_expected[8] = {
        124, 125, 126, 127, 129, 130, 131, 132,
    };
    if (!renderer_filter_vp8_keyframe(&g_probe.renderer, outer_frame, filter_header, g_probe.vp8_modes,
                                      g_probe.vp8_coefficients)
             .ok() ||
        !vp8_gpu_luma_row_exact(12, 0, normal_outer_expected, sizeof(normal_outer_expected))) {
        return false;
    }

    constexpr u8 normal_outer_gray[8] = {
        126, 127, 128, 129, 132, 133, 134, 135,
    };
    g_probe.failure_stage = 23;
    const MediaAtlasGpuResult opaque_atlas = media_atlas_gpu_upload_vp8_keyframe(
        &g_probe.renderer.media_gpu, &g_probe.media_atlas, g_probe.renderer.graphics.device,
        g_probe.renderer.graphics.context1, media_asset_custom_emoji(102, true), outer_frame, {});
    if (!opaque_atlas.ok()) {
        g_probe.failure_stage = 25;
        return false;
    }
    if (!vp8_atlas_row_exact(opaque_atlas.allocation, 12, normal_outer_gray, sizeof(normal_outer_gray))) {
        return false;
    }
    if (media_atlas_gpu_upload_vp8_keyframe(
            &g_probe.renderer.media_gpu, &g_probe.media_atlas, g_probe.renderer.graphics.device,
            g_probe.renderer.graphics.context1, media_asset_custom_emoji(103, true), outer_frame, {})
            .ok()) {
        g_probe.failure_stage = 27;
        return false;
    }

    reset_vp8_gpu_probe(Vp8IntraMode::dc, Vp8IntraMode::dc);
    g_probe.failure_stage = 24;
    Vp8MacroblockCoefficients& alpha_coefficients = g_probe.vp8_coefficients.macroblocks[0];
    alpha_coefficients.values[16][0] = -32;
    alpha_coefficients.values[17][0] = 32;
    alpha_coefficients.values[18][0] = 64;
    alpha_coefficients.values[19][0] = -64;
    alpha_coefficients.has_coefficients = 1;
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            g_probe.png_scanlines[y * 16u + x] = static_cast<u8>((x * 13u + y * 17u) & 255u);
        }
    }
    filter_header = {};
    const MediaAtlasGpuResult alpha_atlas = renderer_upload_vp8_keyframe(
        &g_probe.renderer, media_asset_custom_emoji(104, true), frame, filter_header, g_probe.vp8_modes,
        g_probe.vp8_coefficients, ConstSlice<u8>{g_probe.png_scanlines, 16u * 16u});
    if (!alpha_atlas.ok() || !vp8_alpha_atlas_exact(alpha_atlas.allocation)) {
        return false;
    }

    const MediaAtlasGpu& gpu = g_probe.renderer.media_gpu;
    g_probe.failure_stage = 21;
    constexpr u32 single_macroblock_reconstructions = 19;
    constexpr u64 single_macroblock_bytes = sizeof(Vp8MacroblockMode) + sizeof(Vp8MacroblockCoefficients);
    return gpu.vp8_reconstructions == reconstructions_before + single_macroblock_reconstructions + 2u &&
           gpu.vp8_reconstruction_dispatches ==
               dispatches_before + (single_macroblock_reconstructions + 2u) * 2u &&
           gpu.vp8_uploaded_bytes == uploaded_before +
                                         single_macroblock_bytes * single_macroblock_reconstructions +
                                         single_macroblock_bytes * 6u &&
           gpu.vp8_reconstructed_pixels == pixels_before +
                                               16ull * 16ull * single_macroblock_reconstructions +
                                               32ull * 32ull + 32ull * 16ull &&
           gpu.vp8_filter_dispatches == filter_dispatches_before + 4u &&
           gpu.vp8_filtered_edges == filtered_edges_before + 49u &&
           gpu.vp8_filter_skips == filter_skips_before + 2u &&
           gpu.vp8_filtered_pixels == filtered_pixels_before + 1'280u &&
           gpu.vp8_atlas_uploads == atlas_uploads_before + 2u &&
           gpu.vp8_alpha_uploads == alpha_uploads_before + 1u &&
           gpu.vp8_atlas_pixels == atlas_pixels_before + 768u &&
           gpu.vp8_alpha_uploaded_bytes == alpha_bytes_before + 256u &&
           gpu.vp8_last_filtered_reconstruction == gpu.vp8_reconstructions &&
           gpu.vp8_last_atlas_reconstruction == gpu.vp8_reconstructions && gpu.vp8_width == 16 &&
           gpu.vp8_height == 16;
}

bool vp8_real_fixture_gpu_exact() noexcept {
    WebpImage image{};
    Vp8Keyframe frame{};
    Vp8ControlHeader header{};
    Vp8ProbabilityWorkspace probabilities{};
    if (!lossy_webp_decode_keyframe(ConstSlice<u8>{kWebpLossy4x4, sizeof(kWebpLossy4x4)}, &image, &frame,
                                    &header, &probabilities, &g_probe.vp8_modes, &g_probe.vp8_coefficients,
                                    nullptr, {}, {})
             .ok()) {
        return false;
    }
    const MediaAtlasGpuResult uploaded =
        renderer_upload_vp8_keyframe(&g_probe.renderer, media_asset_custom_emoji(105, true), frame, header,
                                     g_probe.vp8_modes, g_probe.vp8_coefficients, {});
    // Oracle bytes are the authored fixture decoded by libwebp 1.6.0
    // through Pillow 12.3.0. Alpha is opaque, so atlas premultiplication
    // preserves RGB exactly.
    constexpr u8 expected_rgba[] = {
        4,  2,   0,  255, 59,  30,  40,  255, 130, 44,  83,  255, 185, 72,  124, 255,
        32, 59,  34, 255, 87,  86,  75,  255, 157, 100, 117, 255, 212, 127, 158, 255,
        42, 126, 57, 255, 97,  154, 98,  255, 166, 168, 140, 255, 221, 196, 181, 255,
        69, 182, 89, 255, 124, 209, 130, 255, 194, 225, 173, 255, 248, 252, 215, 255,
    };
    if (!uploaded.ok() || uploaded.allocation.width != frame.width ||
        uploaded.allocation.height != frame.height ||
        !vp8_atlas_rgba_exact(uploaded.allocation, ConstSlice<u8>{expected_rgba, sizeof(expected_rgba)})) {
        return false;
    }

    // Large message WebP must land in the preview atlas used by message draw
    // commands. Routing it into the identity atlas makes the placeholder stay
    // blank and causes the visible-media pump to decode/upload the same cached
    // image once per frame.
    const MediaAssetKey preview_vp8_key =
        media_asset_embed(10'008, 2, 0, MediaEmbedResource::image, MediaAttachmentFormat::webp, false,
                          MediaResolutionTier::px64);
    const MediaAtlasGpuResult preview_vp8 = renderer_upload_vp8_keyframe(
        &g_probe.renderer, preview_vp8_key, frame, header, g_probe.vp8_modes, g_probe.vp8_coefficients, {});
    if (!preview_vp8.ok() || !media_atlas_find(&g_probe.preview_media_atlas, preview_vp8_key).ok() ||
        media_atlas_find(&g_probe.media_atlas, preview_vp8_key).ok() ||
        !media_surface_rgba_exact(g_probe.renderer.media_gpu.preview_texture, preview_vp8.allocation,
                                  ConstSlice<u8>{expected_rgba, sizeof(expected_rgba)})) {
        return false;
    }
    const MediaAssetKey preview_lossless_key =
        media_asset_embed(10'009, 3, 0, MediaEmbedResource::image, MediaAttachmentFormat::webp, false,
                          MediaResolutionTier::px64);
    const MediaAtlasGpuResult preview_lossless =
        renderer_upload_media_straight_rgba8(&g_probe.renderer, preview_lossless_key, 4, 4, 16,
                                             ConstSlice<u8>{expected_rgba, sizeof(expected_rgba)});
    if (!preview_lossless.ok() ||
        !media_atlas_find(&g_probe.preview_media_atlas, preview_lossless_key).ok() ||
        media_atlas_find(&g_probe.media_atlas, preview_lossless_key).ok() ||
        !media_surface_rgba_exact(g_probe.renderer.media_gpu.preview_texture, preview_lossless.allocation,
                                  ConstSlice<u8>{expected_rgba, sizeof(expected_rgba)})) {
        return false;
    }

    if (!lossy_webp_decode_keyframe(ConstSlice<u8>{kWebpLossy32x24, sizeof(kWebpLossy32x24)}, &image,
                                    &frame, &header, &probabilities, &g_probe.vp8_modes,
                                    &g_probe.vp8_coefficients, nullptr, {}, {})
             .ok()) {
        return false;
    }
    const MediaAtlasGpuResult multi_uploaded =
        renderer_upload_vp8_keyframe(&g_probe.renderer, media_asset_custom_emoji(10'007, true), frame,
                                     header, g_probe.vp8_modes, g_probe.vp8_coefficients, {});
    u64 multi_hash = 0;
    if (!multi_uploaded.ok() || frame.width != 32 || frame.height != 24 ||
        !vp8_atlas_rgba_hash(multi_uploaded.allocation, &multi_hash) ||
        multi_hash != kWebpLossy32x24RgbaFnv1a) {
        log_u64(LogLevel::error, "probe", "vp8_multi_fixture_hash", multi_hash);
        log_u64(LogLevel::error, "probe", "vp8_multi_fixture_expected_hash", kWebpLossy32x24RgbaFnv1a);
        return false;
    }

    constexpr u32 alpha_encoded_bytes = 136;
    auto write_u32 = [](u8* output, u32 value) noexcept {
        output[0] = static_cast<u8>(value);
        output[1] = static_cast<u8>(value >> 8);
        output[2] = static_cast<u8>(value >> 16);
        output[3] = static_cast<u8>(value >> 24);
    };
    for (u32 index = 0; index < alpha_encoded_bytes; ++index) {
        g_probe.png_encoded[index] = 0;
    }
    write_u32(g_probe.png_encoded, 0x46464952u);
    write_u32(g_probe.png_encoded + 4, alpha_encoded_bytes - 8);
    write_u32(g_probe.png_encoded + 8, 0x50424557u);
    write_u32(g_probe.png_encoded + 12, 0x58385056u);
    write_u32(g_probe.png_encoded + 16, 10);
    g_probe.png_encoded[20] = 0x10;
    g_probe.png_encoded[24] = 3;
    g_probe.png_encoded[27] = 3;
    write_u32(g_probe.png_encoded + 30, 0x48504C41u);
    write_u32(g_probe.png_encoded + 34, 17);
    g_probe.png_encoded[38] = 0;
    for (u32 index = 0; index < 16; ++index) {
        g_probe.png_encoded[39 + index] = static_cast<u8>(index * 13u + 7u);
    }
    write_u32(g_probe.png_encoded + 56, 0x20385056u);
    write_u32(g_probe.png_encoded + 60, sizeof(kWebpLossy4x4) - 20);
    for (u32 index = 20; index < sizeof(kWebpLossy4x4); ++index) {
        g_probe.png_encoded[64 + index - 20] = kWebpLossy4x4[index];
    }
    const LossyWebpDecodeResult alpha_decoded = lossy_webp_decode_keyframe(
        ConstSlice<u8>{g_probe.png_encoded, alpha_encoded_bytes}, &image, &frame, &header, &probabilities,
        &g_probe.vp8_modes, &g_probe.vp8_coefficients, &g_probe.vp8l_workspace,
        Slice<u8>{g_probe.png_scanlines, 16u * 4u}, Slice<u8>{g_probe.png_scanlines + 16u * 4u, 16u});
    if (!alpha_decoded.ok() || alpha_decoded.alpha_bytes != 16) {
        return false;
    }
    const MediaAtlasGpuResult alpha_uploaded = renderer_upload_vp8_keyframe(
        &g_probe.renderer, media_asset_custom_emoji(106, true), frame, header, g_probe.vp8_modes,
        g_probe.vp8_coefficients, ConstSlice<u8>{g_probe.png_scanlines + 16u * 4u, 16u});
    u8 expected_premultiplied[sizeof(expected_rgba)]{};
    for (u32 pixel = 0; pixel < 16; ++pixel) {
        const u8 alpha = static_cast<u8>(pixel * 13u + 7u);
        for (u32 channel = 0; channel < 3; ++channel) {
            expected_premultiplied[pixel * 4u + channel] = static_cast<u8>(
                (static_cast<u32>(expected_rgba[pixel * 4u + channel]) * alpha + 127u) / 255u);
        }
        expected_premultiplied[pixel * 4u + 3u] = alpha;
    }
    return alpha_uploaded.ok() &&
           vp8_atlas_rgba_exact(alpha_uploaded.allocation,
                                ConstSlice<u8>{expected_premultiplied, sizeof(expected_premultiplied)});
}

} // namespace ui_probe_detail
} // namespace maicord
