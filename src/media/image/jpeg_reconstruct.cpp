#include "jpeg_internal.hpp"

#include "maicord/core/byte_order.hpp"

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

// The box average of the 8x8 IDCT over step x step pixels is separable too:
// sample (sy, sx) = sum over v, u of M[sy][v] M[sx][u] C[v][u], where M[s][u]
// is the mean of kIdctBasis[x][u] over the s-th box of x (zero for u = 4, and
// for u = 2 and 6 at two samples, which the vertical pass skips).
template <u32 Samples> struct BoxBasis {
    float values[Samples][8];
};
template <u32 Samples> constexpr BoxBasis<Samples> box_basis() noexcept {
    BoxBasis<Samples> result{};
    constexpr u32 step = 8 / Samples;
    for (u32 s = 0; s < Samples; ++s)
        for (u32 u = 0; u < 8; ++u) {
            double total = 0;
            for (u32 x = s * step; x < (s + 1) * step; ++x)
                total += kIdctBasis[x][u];
            result.values[s][u] = static_cast<float>(total / step);
        }
    return result;
}
template <u32 Samples> alignas(32) inline constexpr BoxBasis<Samples> kBoxBasis = box_basis<Samples>();

// {sum a, sum b, sum c, sum d} over eight lanes each.
inline __m128 lane_sums(__m256 a, __m256 b, __m256 c, __m256 d) noexcept {
    // {a0+a2, b0+b2, a1+a3, b1+b3 | a4+a6, b4+b6, a5+a7, b5+b7}, and likewise for c and d.
    const __m256 ab = _mm256_add_ps(_mm256_unpacklo_ps(a, b), _mm256_unpackhi_ps(a, b));
    const __m256 cd = _mm256_add_ps(_mm256_unpacklo_ps(c, d), _mm256_unpackhi_ps(c, d));
    // {a0..3, b0..3, c0..3, d0..3 | a4..7, b4..7, c4..7, d4..7}
    const __m256 sums = _mm256_add_ps(_mm256_shuffle_ps(ab, cd, 0x44), _mm256_shuffle_ps(ab, cd, 0xEE));
    const __m256 totals = _mm256_add_ps(sums, _mm256_permute2f128_ps(sums, sums, 0x01));
    return __builtin_shufflevector(totals, totals, 0, 1, 2, 3);
}

// clamp_sample(value + 128) of a then b, as eight bytes in the low half.
inline __m128i sample_bytes(__m128 a, __m128 b) noexcept {
    const __m128 bias = _mm_set1_ps(128.f), half = _mm_set1_ps(.5f), zero = _mm_setzero_ps(),
                 top = _mm_set1_ps(255.f);
    const auto lanes = [&](__m128 value) noexcept {
        return _mm_cvttps_epi32(
            _mm_min_ps(_mm_max_ps(_mm_add_ps(_mm_add_ps(value, bias), half), zero), top));
    };
    const __m128i words = _mm_packs_epi32(lanes(a), lanes(b));
    return _mm_packus_epi16(words, words);
}

// Dequantized coefficients (rows of eight) to Samples x Samples box averages:
// a vertical pass per sample row (lanes u), then dot products with M[sx]. Box
// Samples - 1 - s mirrors box s (M[Samples - 1 - s][v] = (-1)^v M[s][v]), so the
// vertical pass sums even and odd v apart and takes both rows from the two sums.
template <u32 Samples> void reconstruct_boxes(const float* coefficients, u8* plane, u32 pitch) noexcept {
    const auto& m = kBoxBasis<Samples>.values;
    __m256 q[Samples];
    for (u32 s = 0; s < Samples / 2; ++s) {
        __m256 even = _mm256_mul_ps(_mm256_set1_ps(m[s][0]), _mm256_load_ps(coefficients));
        __m256 odd = _mm256_mul_ps(_mm256_set1_ps(m[s][1]), _mm256_load_ps(coefficients + 8));
        for (u32 v = 2; v < 8; ++v) {
            if (m[s][v] == 0.f)
                continue;
            const __m256 term = _mm256_mul_ps(_mm256_set1_ps(m[s][v]), _mm256_load_ps(coefficients + v * 8));
            if (v & 1)
                odd = _mm256_add_ps(odd, term);
            else
                even = _mm256_add_ps(even, term);
        }
        q[s] = _mm256_add_ps(even, odd);
        q[Samples - 1 - s] = _mm256_sub_ps(even, odd);
    }
    const auto basis = [&](u32 s) noexcept { return _mm256_load_ps(m[s]); };
    if constexpr (Samples == 2) {
        const __m128 samples = lane_sums(_mm256_mul_ps(q[0], basis(0)), _mm256_mul_ps(q[0], basis(1)),
                                         _mm256_mul_ps(q[1], basis(0)), _mm256_mul_ps(q[1], basis(1)));
        const u32 bytes = static_cast<u32>(_mm_cvtsi128_si32(sample_bytes(samples, samples)));
        byte_store_le16(plane, static_cast<u16>(bytes));
        byte_store_le16(plane + pitch, static_cast<u16>(bytes >> 16));
    } else {
        const auto row = [&](u32 sy) noexcept {
            return lane_sums(_mm256_mul_ps(q[sy], basis(0)), _mm256_mul_ps(q[sy], basis(1)),
                             _mm256_mul_ps(q[sy], basis(2)), _mm256_mul_ps(q[sy], basis(3)));
        };
        for (u32 sy = 0; sy < Samples; sy += 2) {
            const u64 bytes = static_cast<u64>(_mm_cvtsi128_si64(sample_bytes(row(sy), row(sy + 1))));
            byte_store_le32(plane + sy * pitch, static_cast<u32>(bytes));
            byte_store_le32(plane + (sy + 1) * pitch, static_cast<u32>(bytes >> 32));
        }
    }
}

// The exact 8x8 IDCT, or its box averages to samples x samples (DC alone at one sample).
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
    alignas(32) float coefficients[64];
    for (u32 i = 0; i < 64; ++i)
        coefficients[i] = static_cast<float>(block.values[i]) * table.values[i];
    if (samples == 2)
        return reconstruct_boxes<2>(coefficients, plane, pitch);
    if (samples == 4)
        return reconstruct_boxes<4>(coefficients, plane, pitch);
    // Both passes run eight independent outputs per instruction; each output
    // keeps the scalar order (sum from 0, terms in index order).
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
    for (u32 y = 0; y < 8; ++y)
        _mm_storel_epi64(reinterpret_cast<__m128i*>(plane + y * pitch),
                         sample_bytes(_mm_loadu_ps(columns + y * 8), _mm_loadu_ps(columns + y * 8 + 4)));
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
    const u32 width = layout.width;
    for (u32 y = 0; y < layout.height; ++y) {
        // Each component's sample for pixel x is the plane sample at
        // (x * h / max_h, y * v / max_v). Locals and a stepped output pointer
        // keep the u8 stores from reloading them.
        for (u32 c = 0; c < image.component_count; ++c) {
            const JpegComponent& component = image.components[c];
            const u32 h = component.horizontal_sampling;
            const u32 py = y * component.vertical_sampling / max_v;
            const u8* source = planes + layout.plane_offsets[c] + static_cast<usize>(py) * layout.plane_pitches[c];
            u8* out = row + c;
            if (h == max_h || 2 * h == max_h) {
                const u32 shift = h == max_h ? 0 : 1;
                for (u32 x = 0; x < width; ++x, out += 4)
                    *out = source[x >> shift];
                continue;
            }
            // Other ratios step the column index instead of dividing it.
            u32 accumulated = 0;
            for (u32 x = 0; x < width; ++x, out += 4) {
                *out = *source;
                accumulated += h;
                if (accumulated >= max_h) {
                    accumulated -= max_h;
                    ++source;
                }
            }
        }
        convert_row(convert, row, width);
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
