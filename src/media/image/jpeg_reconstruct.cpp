#include "jpeg_internal.hpp"

namespace maicord {
using namespace jpeg_detail;

namespace {
inline constexpr float kIdctBasis[8][8] = {
    {0.353553391f, 0.490392640f, 0.461939766f, 0.415734806f, 0.353553391f, 0.277785117f, 0.191341716f, 0.097545161f},
    {0.353553391f, 0.415734806f, 0.191341716f, -0.097545161f, -0.353553391f, -0.490392640f, -0.461939766f, -0.277785117f},
    {0.353553391f, 0.277785117f, -0.191341716f, -0.490392640f, -0.353553391f, 0.097545161f, 0.461939766f, 0.415734806f},
    {0.353553391f, 0.097545161f, -0.461939766f, -0.277785117f, 0.353553391f, 0.415734806f, -0.191341716f, -0.490392640f},
    {0.353553391f, -0.097545161f, -0.461939766f, 0.277785117f, 0.353553391f, -0.415734806f, -0.191341716f, 0.490392640f},
    {0.353553391f, -0.277785117f, -0.191341716f, 0.490392640f, -0.353553391f, -0.097545161f, 0.461939766f, -0.415734806f},
    {0.353553391f, -0.415734806f, 0.191341716f, 0.097545161f, -0.353553391f, 0.490392640f, -0.461939766f, 0.277785117f},
    {0.353553391f, -0.490392640f, 0.461939766f, -0.415734806f, 0.353553391f, -0.277785117f, 0.191341716f, -0.097545161f},
};

struct IdctColumns {
    float values[8][8];
};
constexpr IdctColumns transpose_basis() noexcept {
    IdctColumns result{};
    for (u32 row = 0; row < 8; ++row)
        for (u32 column = 0; column < 8; ++column)
            result.values[column][row] = kIdctBasis[row][column];
    return result;
}
// kIdctBasisColumns.values[u][x] == kIdctBasis[x][u]: one row pass step for eight x at once.
alignas(32) inline constexpr IdctColumns kIdctBasisColumns = transpose_basis();

u8 clamp_sample(float value) noexcept {
    value += .5f;
    return value <= 0.f ? 0 : value >= 255.f ? 255 : static_cast<u8>(value);
}

// Box averages of an IDCT block, each summed in y-then-x order. A constant
// sample count unrolls the sums into independent chains.
template <u32 Samples> void store_box_totals(const float* columns, u8* plane, u32 pitch) noexcept {
    constexpr u32 step = 8 / Samples;
    constexpr float scale = 1.f / static_cast<float>(step * step);
    for (u32 sy = 0; sy < Samples; ++sy)
        for (u32 sx = 0; sx < Samples; ++sx) {
            float total = 0;
            for (u32 y = sy * step; y < (sy + 1) * step; ++y)
                for (u32 x = sx * step; x < (sx + 1) * step; ++x)
                    total += columns[y * 8 + x];
            plane[sy * pitch + sx] = clamp_sample(total * scale + 128.f);
        }
}

// Exact 8x8 IDCT, box-averaged to samples x samples (DC alone at one sample).
void reconstruct_block(const JpegCoefficientBlock& block, const JpegQuantizationTable& table, u32 samples,
                       u8* plane, u32 pitch) noexcept {
    if (samples == 1) {
        plane[0] = clamp_sample(static_cast<float>(block.values[0]) * table.values[0] * .125f + 128.f);
        return;
    }
    bool dc_only = true;
    for (u32 i = 1; i < 64; ++i)
        if (block.values[i] != 0) {
            dc_only = false;
            break;
        }
    if (dc_only) {
        // Keep the two basis products and repeated box sum from the full IDCT.
        // DC / 8 can round differently at a sample boundary.
        const float coefficient = static_cast<float>(block.values[0]) * table.values[0];
        const float row = kIdctBasis[0][0] * coefficient;
        const float value = kIdctBasis[0][0] * row;
        const u32 step = 8 / samples;
        const float scale = 1.f / static_cast<float>(step * step);
        float total = 0;
        for (u32 i = 0; i < step * step; ++i)
            total += value;
        const u8 sample = clamp_sample(total * scale + 128.f);
        for (u32 y = 0; y < samples; ++y)
            for (u32 x = 0; x < samples; ++x)
                plane[y * pitch + x] = sample;
        return;
    }
    float coefficients[64];
    for (u32 i = 0; i < 64; ++i)
        coefficients[i] = static_cast<float>(block.values[i]) * table.values[i];
    // Both passes run eight independent outputs per instruction; each output
    // keeps the scalar order (sum from 0, terms in index order), and the box
    // totals below keep the scalar y-then-x order.
    alignas(32) float rows[64];
    for (u32 v = 0; v < 8; ++v) {
        __m256 sum = _mm256_setzero_ps();
        for (u32 u = 0; u < 8; ++u)
            sum = _mm256_add_ps(sum, _mm256_mul_ps(_mm256_load_ps(kIdctBasisColumns.values[u]),
                                                   _mm256_set1_ps(coefficients[v * 8 + u])));
        _mm256_store_ps(rows + v * 8, sum);
    }
    alignas(32) float columns[64];
    for (u32 y = 0; y < 8; ++y) {
        __m256 sum = _mm256_setzero_ps();
        for (u32 v = 0; v < 8; ++v)
            sum = _mm256_add_ps(sum, _mm256_mul_ps(_mm256_set1_ps(kIdctBasis[y][v]), _mm256_load_ps(rows + v * 8)));
        _mm256_store_ps(columns + y * 8, sum);
    }
    switch (samples) {
    case 2: store_box_totals<2>(columns, plane, pitch); break;
    case 4: store_box_totals<4>(columns, plane, pitch); break;
    default: store_box_totals<8>(columns, plane, pitch); break;
    }
}

void reconstruct_component_block(const JpegImage& image, const JpegThumbnailLayout& layout, u8* planes,
                                 u32 component_index, u32 block_x, u32 block_y,
                                 const JpegCoefficientBlock& block) noexcept {
    const JpegComponent& component = image.components[component_index];
    const u32 pitch = layout.plane_pitches[component_index];
    u8* target = planes + layout.plane_offsets[component_index] +
                 static_cast<usize>(block_y) * layout.samples * pitch + static_cast<usize>(block_x) * layout.samples;
    reconstruct_block(block, image.tables->quantization[component.quantization_table], layout.samples, target, pitch);
}

// Four pixels in place: sixteen bytes of component samples (one pixel per four
// bytes) become RGBA. The float operations are the scalar ones, in order.
template <JpegColorTransform Transform> void convert_group(u8* pixels) noexcept {
    const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels));
    const auto sample = [&](char index) noexcept {
        return _mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_shuffle_epi8(
            bytes, _mm_setr_epi8(index, index + 4, index + 8, index + 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))));
    };
    const __m128 zero = _mm_setzero_ps(), maximum_value = _mm_set1_ps(255.f);
    __m128 r = sample(0), g = r, b = r;
    if constexpr (Transform != JpegColorTransform::grayscale) {
        g = sample(1);
        b = sample(2);
    }
    if constexpr (Transform == JpegColorTransform::ycbcr || Transform == JpegColorTransform::ycck) {
        const __m128 luma = r, cb = _mm_sub_ps(g, _mm_set1_ps(128.f)), cr = _mm_sub_ps(b, _mm_set1_ps(128.f));
        r = _mm_add_ps(luma, _mm_mul_ps(_mm_set1_ps(1.402f), cr));
        g = _mm_sub_ps(_mm_sub_ps(luma, _mm_mul_ps(_mm_set1_ps(.344136f), cb)), _mm_mul_ps(_mm_set1_ps(.714136f), cr));
        b = _mm_add_ps(luma, _mm_mul_ps(_mm_set1_ps(1.772f), cb));
    }
    if constexpr (Transform == JpegColorTransform::ycck || Transform == JpegColorTransform::cmyk) {
        // Inverted Adobe CMYK: the YCC triple, like C, M and Y, is already 255 - ink.
        const __m128 k = _mm_div_ps(sample(3), maximum_value);
        if constexpr (Transform == JpegColorTransform::ycck) {
            r = _mm_min_ps(_mm_max_ps(r, zero), maximum_value);
            g = _mm_min_ps(_mm_max_ps(g, zero), maximum_value);
            b = _mm_min_ps(_mm_max_ps(b, zero), maximum_value);
        }
        r = _mm_mul_ps(r, k);
        g = _mm_mul_ps(g, k);
        b = _mm_mul_ps(b, k);
    }
    // clamp_sample: round half up by truncation, clamped to 0..255.
    const auto to_byte = [&](__m128 value) noexcept {
        return _mm_cvttps_epi32(_mm_min_ps(_mm_max_ps(_mm_add_ps(value, _mm_set1_ps(.5f)), zero), maximum_value));
    };
    const __m128i packed = _mm_or_si128(
        _mm_or_si128(to_byte(r), _mm_slli_epi32(to_byte(g), 8)),
        _mm_or_si128(_mm_slli_epi32(to_byte(b), 16), _mm_set1_epi32(static_cast<int>(0xFF000000u))));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(pixels), packed);
}

using ConvertGroup = void (*)(u8*) noexcept;

void convert_row(ConvertGroup convert, u8* row, u32 width) noexcept {
    u32 x = 0;
    for (; x + 4 <= width; x += 4)
        convert(row + x * 4u);
    if (x < width) {
        u8 tail[16]{};
        copy_bytes(tail, row + x * 4u, (width - x) * 4u);
        convert(tail);
        copy_bytes(row + x * 4u, tail, (width - x) * 4u);
    }
}

bool convert_planes(const JpegImage& image, const JpegThumbnailLayout& layout, const u8* planes, u8* row,
                    JpegThumbnailRowSink sink, void* context) noexcept {
    const u32 max_h = image.maximum_horizontal_sampling, max_v = image.maximum_vertical_sampling;
    ConvertGroup convert = convert_group<JpegColorTransform::grayscale>;
    switch (image.color_transform) {
    case JpegColorTransform::grayscale: break;
    case JpegColorTransform::rgb: convert = convert_group<JpegColorTransform::rgb>; break;
    case JpegColorTransform::ycbcr: convert = convert_group<JpegColorTransform::ycbcr>; break;
    case JpegColorTransform::cmyk: convert = convert_group<JpegColorTransform::cmyk>; break;
    case JpegColorTransform::ycck: convert = convert_group<JpegColorTransform::ycck>; break;
    }
    for (u32 y = 0; y < layout.height; ++y) {
        // Each component's sample for pixel x is the plane sample at
        // (x * h / max_h, y * v / max_v); the column index is stepped, not divided.
        for (u32 c = 0; c < image.component_count; ++c) {
            const JpegComponent& component = image.components[c];
            const u32 py = y * component.vertical_sampling / max_v;
            const u8* source = planes + layout.plane_offsets[c] + static_cast<usize>(py) * layout.plane_pitches[c];
            u32 accumulated = 0;
            for (u32 x = 0; x < layout.width; ++x) {
                row[x * 4u + c] = *source;
                accumulated += component.horizontal_sampling;
                if (accumulated >= max_h) {
                    accumulated -= max_h;
                    ++source;
                }
            }
        }
        convert_row(convert, row, layout.width);
        if (!sink(context, y, row)) return false;
    }
    return true;
}
} // namespace

JpegParseResult jpeg_thumbnail_parse(ConstSlice<u8> encoded, u32 target_extent, JpegImage* image,
                                     JpegWorkspace* workspace, JpegThumbnailLayout* layout) noexcept {
    if (!layout || !target_extent)
        return parse_failure({}, JpegError::invalid_input);
    reset_value(layout);
    JpegParseResult result = parse_jpeg(encoded, image, workspace, true);
    if (!result.ok())
        return result;
    const u32 longest = maximum<u32>(image->width, image->height);
    u32 samples = 1;
    while (samples < 8 && longest * samples / 8u < target_extent)
        samples *= 2;
    layout->samples = static_cast<u8>(samples);
    layout->width = (static_cast<u32>(image->width) * samples + 7u) / 8u;
    layout->height = (static_cast<u32>(image->height) * samples + 7u) / 8u;
    u64 plane = 0;
    for (u32 c = 0; c < image->component_count; ++c) {
        const JpegComponent& component = image->components[c];
        const u64 pitch = static_cast<u64>(component.block_columns) * samples;
        const u64 rows = static_cast<u64>(component.block_rows) * samples;
        if (plane > 0xffffffffull || pitch > 0xffffffffull)
            return parse_failure(result, JpegError::dimensions);
        layout->plane_offsets[c] = static_cast<u32>(plane);
        layout->plane_pitches[c] = static_cast<u32>(pitch);
        plane += pitch * rows;
    }
    layout->plane_bytes = (plane + 15u) & ~15ull;
    layout->coefficient_bytes =
        image->progressive ? static_cast<u64>(image->coefficient_blocks) * sizeof(JpegCoefficientBlock) : 0;
    layout->rgba_bytes = static_cast<u64>(layout->width) * layout->height * 4u;
    if (layout->plane_bytes + layout->coefficient_bytes > kJpegThumbnailScratchMaximumBytes ||
        layout->plane_bytes > 0xffffffffull)
        return parse_failure(result, JpegError::dimensions);
    return result;
}

JpegDecodeResult jpeg_thumbnail_decode_rows(ConstSlice<u8> encoded, const JpegImage& image,
                                            const JpegThumbnailLayout& layout, Slice<u8> scratch,
                                            Slice<u8> row, JpegThumbnailRowSink sink, void* context) noexcept {
    JpegDecodeResult result{};
    const u64 required = layout.coefficient_bytes + layout.plane_bytes;
    if (!sink || !image.tables || !layout.samples || (!encoded.data && encoded.count) || !scratch.data ||
        required > scratch.count || !row.data || u64{layout.width} * 4 > row.count ||
        (reinterpret_cast<usize>(scratch.data) & 15u) || !image.component_count ||
        static_cast<u64>(image.entropy_offset) + image.entropy_bytes > encoded.count) {
        result.error = JpegError::invalid_input;
        return result;
    }
    auto* blocks = reinterpret_cast<JpegCoefficientBlock*>(scratch.data);
    u8* planes = scratch.data + layout.coefficient_bytes;
    if (image.progressive) {
        result = decode_progressive_scans(encoded, image, blocks);
        if (!result.ok()) {
            return result;
        }
        for (u32 c = 0; c < image.component_count; ++c) {
            const JpegComponent& component = image.components[c];
            for (u32 by = 0; by < component.block_rows; ++by)
                for (u32 bx = 0; bx < component.block_columns; ++bx)
                    reconstruct_component_block(image, layout, planes, c, bx, by,
                                                blocks[component.block_offset + by * component.block_columns + bx]);
        }
    } else {
        JpegEntropyReader reader =
            entropy_reader(ConstSlice<u8>{encoded.data + image.entropy_offset, image.entropy_bytes});
        JpegDecodeTable dc_decode[kJpegTableMaximum];
        JpegAcDecodeTable ac_decode[kJpegTableMaximum];
        build_component_tables(image, dc_decode, ac_decode);
        i32 predictor[kJpegComponentMaximum]{};
        const u32 total_mcus = static_cast<u32>(image.mcu_columns) * image.mcu_rows;
        u8 expected_restart = 0;
        JpegCoefficientBlock block{};
        u32 mcu_x = 0, mcu_y = 0;
        for (u32 mcu = 0; mcu < total_mcus; ++mcu) {
            if (image.restart_interval != 0 && mcu != 0 && mcu % image.restart_interval == 0) {
                if (!entropy_consume_restart(&reader, expected_restart)) {
                    result.error = reader.error;
                    return result;
                }
                expected_restart = static_cast<u8>((expected_restart + 1) & 7);
                for (auto& value : predictor)
                    value = 0;
            }
            // mcu_x/mcu_y step with the MCU index instead of dividing it.
            for (u32 scan = 0; scan < image.component_count; ++scan) {
                const u32 component_index = image.scan_order[scan];
                const JpegComponent& component = image.components[component_index];
                for (u32 vertical = 0; vertical < component.vertical_sampling; ++vertical)
                    for (u32 horizontal = 0; horizontal < component.horizontal_sampling; ++horizontal) {
                        if (!decode_block(&reader, dc_decode[component.dc_table],
                                          ac_decode[component.ac_table], &predictor[component_index], &block)) {
                            result.error = reader.error == JpegError::none ? JpegError::coefficient : reader.error;
                            return result;
                        }
                        reconstruct_component_block(image, layout, planes, component_index,
                                                    mcu_x * component.horizontal_sampling + horizontal,
                                                    mcu_y * component.vertical_sampling + vertical, block);
                        ++result.decoded_blocks;
                    }
            }
            if (++mcu_x == image.mcu_columns) {
                mcu_x = 0;
                ++mcu_y;
            }
        }
        if (!entropy_at_end(&reader)) {
            result.error = reader.error == JpegError::none ? JpegError::entropy : reader.error;
            return result;
        }
    }
    if (!convert_planes(image, layout, planes, row.data, sink, context)) {
        result.error = JpegError::sink;
        return result;
    }
    result.decoded_scans = image.scan_count;
    result.error = JpegError::none;
    return result;
}
} // namespace maicord
