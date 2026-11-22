#include "jpeg_internal.hpp"

#include "maicord/core/byte_order.hpp"
#include "maicord/sys/simd_haswell.hpp"

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

// M[s][u] is the mean of kIdctBasis[x][u] over the s-th box of 8 / Samples pixels x: the basis itself at 8 samples.
// The boxes are symmetric, so M[Samples - 1 - s][u] is M[s][u] for even u and -M[s][u] for odd u, also after the
// rounding: sample s is E + O and sample Samples - 1 - s is E - O, with E the sum over the even frequencies and O over
// the odd ones. kBasisPairs<Samples>.values[k][s] holds M[s][u] (low half) and M[s][v] (high half) scaled by 2^15 as
// 16-bit values for _mm256_madd_epi16, for (u, v) = (0, 2), (4, 6), (1, 3), (5, 7) as k = 0 to 3 and the first
// Samples / 2 samples s; samples 4-7 repeat samples 0-3 for the second 128-bit lane.
template <u32 Samples> struct BasisPairs {
    u32 values[4][8];
};
template <u32 Samples> constexpr BasisPairs<Samples> basis_pairs() noexcept {
    BasisPairs<Samples> result{};
    constexpr u32 step = 8 / Samples;
    constexpr u8 kFrequencies[4][2] = {{0, 2}, {4, 6}, {1, 3}, {5, 7}};
    for (u32 k = 0; k < 4; ++k)
        for (u32 s = 0; s < Samples / 2; ++s)
            for (u32 half = 0; half < 2; ++half) {
                const u32 u = kFrequencies[k][half];
                double total = 0;
                for (u32 x = s * step; x < (s + 1) * step; ++x)
                    total += kIdctBasis[x][u];
                const double scaled = total * (32768.0 / step);
                const i32 value = static_cast<i32>(scaled < 0 ? scaled - .5 : scaled + .5);
                const u32 pair = static_cast<u32>(static_cast<u16>(value)) << (half * 16);
                result.values[k][s] |= pair;
                result.values[k][s + 4] |= pair;
            }
    return result;
}
template <u32 Samples> alignas(32) inline constexpr BasisPairs<Samples> kBasisPairs = basis_pairs<Samples>();

// The words of each row in the order 0, 2, 4, 6, 1, 3, 5, 7: the pairs (0, 2), (4, 6), (1, 3) and (5, 7) as dwords.
alignas(32) inline constexpr u8 kPairWords[32] = {0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15,
                                                  0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15};

// Dequantized coefficients to Samples x Samples samples: the 8x8 inverse DCT or its box averages. A vertical pass
// and then a horizontal pass multiply pairs of 16-bit values and add the products in 32 bits. Between the passes
// the values keep 5 fraction bits and saturate at 16 bits. The coefficients from row and column Used on are zero.
// Dequantization wraps at 16 bits, as in libjpeg-turbo: only a damaged file has larger values.
template <u32 Samples, u32 Used>
MAICORD_HASWELL void reconstruct(const JpegCoefficientBlock& block, const JpegQuantizationTable& table, u8* plane,
                                 u32 pitch) noexcept {
    const auto& basis = kBasisPairs<Samples>.values;
    const auto rows_of = [&](u32 first) MAICORD_HASWELL noexcept {
        return _mm256_mullo_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(block.values + first * 8)),
                                  _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(table.values + first * 8)));
    };
    // pairs[k]: coefficient rows (0, 2), (1, 3), (4, 6) and (5, 7) of each column in turns, as k = 0 to 3; columns
    // 0-3 in the low lane and 4-7 in the high lane.
    __m256i pairs[Used / 2];
    for (u32 k = 0; k < Used / 2; k += 2) {
        const __m256i first = rows_of(k * 2), second = rows_of(k * 2 + 2);
        const __m256i low = _mm256_unpacklo_epi16(first, second), high = _mm256_unpackhi_epi16(first, second);
        pairs[k] = _mm256_permute2x128_si256(low, high, 0x20);
        pairs[k + 1] = _mm256_permute2x128_si256(low, high, 0x31);
    }
    // The vertical pass: one row of eight frequencies for each sample row.
    alignas(32) i16 rows[Samples][8];
    for (u32 s = 0; s < Samples / 2; ++s) {
        const auto term = [&](u32 pair, u32 k) MAICORD_HASWELL noexcept {
            return _mm256_madd_epi16(pairs[pair], _mm256_set1_epi32(static_cast<int>(basis[k][s])));
        };
        __m256i even = _mm256_add_epi32(term(0, 0), _mm256_set1_epi32(1 << 9)), odd = term(1, 2);
        if constexpr (Used == 8) {
            even = _mm256_add_epi32(even, term(2, 1));
            odd = _mm256_add_epi32(odd, term(3, 3));
        }
        const __m256i both = _mm256_permute4x64_epi64(
            _mm256_packs_epi32(_mm256_srai_epi32(_mm256_add_epi32(even, odd), 10),
                               _mm256_srai_epi32(_mm256_sub_epi32(even, odd), 10)),
            0xD8);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(rows[s]), _mm256_castsi256_si128(both));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(rows[Samples - 1 - s]), __builtin_shufflevector(both, both, 2, 3));
    }
    // The horizontal pass, two sample rows at once (one in each lane), plus 128 and rounded half up.
    const __m256i order = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(kPairWords));
    for (u32 sy = 0; sy < Samples; sy += 2) {
        const __m256i values =
            _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(rows[sy])), order);
        const auto term = [&](__m256i pair, u32 k) MAICORD_HASWELL noexcept {
            return _mm256_madd_epi16(pair, _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(basis[k])));
        };
        __m256i even = _mm256_add_epi32(term(_mm256_shuffle_epi32(values, 0x00), 0),
                                        _mm256_set1_epi32((128 << 20) + (1 << 19)));
        __m256i odd = term(_mm256_shuffle_epi32(values, 0xAA), 2);
        if constexpr (Used == 8) {
            even = _mm256_add_epi32(even, term(_mm256_shuffle_epi32(values, 0x55), 1));
            odd = _mm256_add_epi32(odd, term(_mm256_shuffle_epi32(values, 0xFF), 3));
        }
        // Samples 0 to Samples / 2 - 1 of each row, then the others from the last.
        const __m256i front = _mm256_srai_epi32(_mm256_add_epi32(even, odd), 20);
        const __m256i back = _mm256_srai_epi32(_mm256_sub_epi32(even, odd), 20);
        __m256i words;
        if constexpr (Samples == 8) {
            words = _mm256_packs_epi32(front, _mm256_shuffle_epi32(back, 0x1B));
        } else {
            const __v8si row = Samples == 4
                                   ? __builtin_shufflevector((__v8si)front, (__v8si)back, 0, 1, 9, 8, 4, 5, 13, 12)
                                   : __builtin_shufflevector((__v8si)front, (__v8si)back, 0, 8, 0, 8, 4, 12, 4, 12);
            words = _mm256_packs_epi32((__m256i)row, (__m256i)row);
        }
        const __m256i bytes = _mm256_packus_epi16(words, words);
        const __m128i high = __builtin_shufflevector(bytes, bytes, 2, 3);
        u8* const first = plane + sy * pitch;
        if constexpr (Samples == 8) {
            _mm_storel_epi64(reinterpret_cast<__m128i*>(first), _mm256_castsi256_si128(bytes));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(first + pitch), high);
        } else if constexpr (Samples == 4) {
            _mm_storeu_si32(first, _mm256_castsi256_si128(bytes));
            _mm_storeu_si32(first + pitch, high);
        } else {
            byte_store_le16(first, static_cast<u16>(_mm_cvtsi128_si32(_mm256_castsi256_si128(bytes))));
            byte_store_le16(first + pitch, static_cast<u16>(_mm_cvtsi128_si32(high)));
        }
    }
}

template <u32 Used>
void reconstruct_used(const JpegCoefficientBlock& block, const JpegQuantizationTable& table, u32 samples, u8* plane,
                      u32 pitch) noexcept {
    if (samples == 2)
        return reconstruct<2, Used>(block, table, plane, pitch);
    if (samples == 4)
        return reconstruct<4, Used>(block, table, plane, pitch);
    reconstruct<8, Used>(block, table, plane, pitch);
}

// One block into its plane: the 8x8 IDCT, or its box averages to samples x samples (DC alone at one sample).
// `end` is one past the last coefficient of the block in zigzag order, or 64 when the caller does not know it.
void reconstruct_component_block(const JpegImage& image, const JpegThumbnailLayout& layout, u8* planes,
                                 u32 component_index, u32 block_x, u32 block_y, const JpegCoefficientBlock& block,
                                 u32 end) noexcept {
    const JpegComponent& component = image.components[component_index];
    const JpegQuantizationTable& table = image.tables->quantization[component.quantization_table];
    const u32 samples = layout.samples;
    const u32 pitch = layout.plane_pitches[component_index];
    u8* plane = planes + layout.plane_offsets[component_index] + static_cast<usize>(block_y) * samples * pitch +
                static_cast<usize>(block_x) * samples;
    // DC / 8 + 128, rounded half up: each pixel of a block without other coefficients, and so the mean of each box.
    // The product of an i16 and a u16 fits in 32 bits.
    const auto dc_sample = [&]() noexcept {
        const i32 value = (block.values[0] * table.values[0] + 1028) >> 3;
        return static_cast<u8>(value < 0 ? 0 : value > 255 ? 255 : value);
    };
    if (samples == 1) {
        plane[0] = dc_sample();
        return;
    }
    // Ten coefficients or fewer have no run of 16 zeroes: one of them is not DC, and all lie in the first four
    // rows and columns. A longer block shows in its values where the coefficients are.
    bool dc_only = end == 1, small = end <= 10;
    if (!small) {
        // Row i is eight coefficients: the pointer moves in coefficients, then the load reads sixteen bytes.
        const auto row = [&](u32 i) noexcept {
            return _mm_loadu_si128(reinterpret_cast<const __m128i*>(block.values + i * 8));
        };
        const auto zero = [](__m128i values) noexcept {
            return _mm_movemask_epi8(_mm_cmpeq_epi8(values, _mm_setzero_si128())) == 0xFFFF;
        };
        const __m128i first = row(0);
        const __m128i top = _mm_or_si128(_mm_or_si128(row(1), row(2)), row(3));
        const __m128i bottom = _mm_or_si128(_mm_or_si128(row(4), row(5)), _mm_or_si128(row(6), row(7)));
        small = zero(_mm_or_si128(bottom, _mm_srli_si128(_mm_or_si128(first, top), 8)));
        dc_only = small && zero(_mm_or_si128(top, _mm_srli_si128(first, 2)));
    }
    if (dc_only) {
        const u8 sample = dc_sample();
        const __m128i fill = _mm_set1_epi8(static_cast<char>(sample));
        for (u32 y = 0; y < samples; ++y, plane += pitch) {
            if (samples == 8)
                _mm_storel_epi64(reinterpret_cast<__m128i*>(plane), fill);
            else if (samples == 4)
                _mm_storeu_si32(plane, fill);
            else
                byte_store_le16(plane, static_cast<u16>(sample * 0x101u));
        }
        return;
    }
    if (small)
        return reconstruct_used<4>(block, table, samples, plane, pitch);
    reconstruct_used<8>(block, table, samples, plane, pitch);
}

// In each 128-bit lane, bytes R0-3, G0-3, B0-3, A0-3 to the four bytes of each of the four pixels.
alignas(32) inline constexpr u8 kPixelBytes[32] = {0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15,
                                                   0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15};
// Bytes 1 and 2 of each pixel (Cb and Cr) to two words.
alignas(32) inline constexpr u8 kChromaWords[32] = {1, 128, 2, 128, 5, 128, 6, 128, 9, 128, 10, 128, 13, 128, 14, 128,
                                                    1, 128, 2, 128, 5, 128, 6, 128, 9, 128, 10, 128, 13, 128, 14, 128};

// Eight pixels: the component samples of each (one pixel per four bytes) become RGBA. Gray and RGB samples need
// no arithmetic. YCbCr uses 14 fraction bits and rounds half up.
template <JpegColorTransform Transform>
MAICORD_HASWELL __forceinline __m256i convert_pixels(__m256i pixels) noexcept {
    const __m256i opaque = _mm256_set1_epi32(static_cast<int>(0xFF000000u)), low = _mm256_set1_epi32(0xFF),
                  words = _mm256_set1_epi32(0x00FF00FF);
    if constexpr (Transform == JpegColorTransform::rgb) {
        return _mm256_or_si256(pixels, opaque);
    } else if constexpr (Transform == JpegColorTransform::grayscale) {
        const __m256i gray = _mm256_and_si256(pixels, low);
        return _mm256_or_si256(_mm256_or_si256(gray, _mm256_slli_epi32(gray, 8)),
                               _mm256_or_si256(_mm256_slli_epi32(gray, 16), opaque));
    } else {
        if constexpr (Transform == JpegColorTransform::ycbcr || Transform == JpegColorTransform::ycck) {
            // Cb - 128 and Cr - 128 in the two words of each pixel, and Y + 1/2 with 14 fraction bits.
            const __m256i chroma = _mm256_sub_epi16(
                _mm256_shuffle_epi8(pixels, _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(kChromaWords))),
                _mm256_set1_epi16(128));
            const __m256i luma =
                _mm256_add_epi32(_mm256_slli_epi32(_mm256_and_si256(pixels, low), 14), _mm256_set1_epi32(1 << 13));
            const auto channel = [&](i16 cb, i16 cr) MAICORD_HASWELL noexcept {
                const u32 factors = static_cast<u16>(cb) | u32{static_cast<u16>(cr)} << 16;
                return _mm256_srai_epi32(
                    _mm256_add_epi32(luma, _mm256_madd_epi16(chroma, _mm256_set1_epi32(static_cast<int>(factors)))),
                    14);
            };
            // The factors are 1.402, 0.344136, 0.714136 and 1.772 times 2^14. The packs clamp to 0..255. Byte 3
            // keeps K for YCCK.
            const __m256i rg = _mm256_packs_epi32(channel(0, 22970), channel(-5638, -11700));
            const __m256i last = Transform == JpegColorTransform::ycck ? _mm256_srli_epi32(pixels, 24) : low;
            pixels = _mm256_shuffle_epi8(_mm256_packus_epi16(rg, _mm256_packs_epi32(channel(29032, 0), last)),
                                         _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(kPixelBytes)));
            if constexpr (Transform == JpegColorTransform::ycbcr)
                return pixels;
        }
        // Inverted Adobe CMYK: the YCC triple, like C, M and Y, is already 255 - ink. Each channel times K / 255,
        // rounded half up: (t + 128) * 257 / 2^16 is exact for t = channel * K.
        const __m256i k = _mm256_srli_epi32(pixels, 24), factors = _mm256_or_si256(k, _mm256_slli_epi32(k, 16));
        const auto scale = [&](__m256i values) MAICORD_HASWELL noexcept {
            return _mm256_mulhi_epu16(_mm256_add_epi16(_mm256_mullo_epi16(values, factors), _mm256_set1_epi16(128)),
                                      _mm256_set1_epi16(257));
        };
        return _mm256_or_si256(_mm256_or_si256(scale(_mm256_and_si256(pixels, words)),
                                               _mm256_slli_epi16(scale(_mm256_and_si256(_mm256_srli_epi32(pixels, 8), words)), 8)),
                               opaque);
    }
}

// Sixteen pixels of the image's transform: one switch for each step, which takes the same case all image long.
MAICORD_HASWELL __forceinline void convert_pixels(JpegColorTransform transform, __m256i pixels[2]) noexcept {
    switch (transform) {
    case JpegColorTransform::grayscale:
        for (u32 i = 0; i < 2; ++i) pixels[i] = convert_pixels<JpegColorTransform::grayscale>(pixels[i]);
        return;
    case JpegColorTransform::rgb:
        for (u32 i = 0; i < 2; ++i) pixels[i] = convert_pixels<JpegColorTransform::rgb>(pixels[i]);
        return;
    case JpegColorTransform::ycbcr:
        for (u32 i = 0; i < 2; ++i) pixels[i] = convert_pixels<JpegColorTransform::ycbcr>(pixels[i]);
        return;
    case JpegColorTransform::cmyk:
        for (u32 i = 0; i < 2; ++i) pixels[i] = convert_pixels<JpegColorTransform::cmyk>(pixels[i]);
        return;
    case JpegColorTransform::ycck: break;
    }
    for (u32 i = 0; i < 2; ++i) pixels[i] = convert_pixels<JpegColorTransform::ycck>(pixels[i]);
}

// Each component's sample for pixel x is the plane sample at (x * h / max_h, y * v / max_v). The frame admits
// sampling factors 1 and 2 only, so each component has full or half width. A pixel gets four component bytes:
// past the image's components it repeats the first one, which no transform reads.
MAICORD_HASWELL bool convert_planes(const JpegImage& image, const JpegThumbnailLayout& layout, const u8* planes,
                                    u8* row, JpegThumbnailRowSink sink, void* context) noexcept {
    const u32 max_h = image.maximum_horizontal_sampling, max_v = image.maximum_vertical_sampling;
    const JpegColorTransform transform = image.color_transform;
    const u32 width = layout.width;
    for (u32 y = 0; y < layout.height; ++y) {
        const u8* sources[kJpegComponentMaximum];
        u32 shifts[kJpegComponentMaximum];
        for (u32 c = 0; c < kJpegComponentMaximum; ++c) {
            const u32 index = c < image.component_count ? c : 0u;
            const JpegComponent& component = image.components[index];
            const u32 py = y * component.vertical_sampling / max_v;
            sources[c] = planes + layout.plane_offsets[index] + static_cast<usize>(py) * layout.plane_pitches[index];
            shifts[c] = component.horizontal_sampling == max_h ? 0 : 1;
        }
        u32 x = 0;
        for (; x + 16 <= width; x += 16) {
            __m128i samples[kJpegComponentMaximum];
            for (u32 c = 0; c < kJpegComponentMaximum; ++c) {
                const __m128i* const source = reinterpret_cast<const __m128i*>(sources[c] + (x >> shifts[c]));
                // A half width sample two times.
                const __m128i half = _mm_loadl_epi64(source);
                samples[c] = shifts[c] ? _mm_unpacklo_epi8(half, half) : _mm_loadu_si128(source);
            }
            const __m128i low01 = _mm_unpacklo_epi8(samples[0], samples[1]);
            const __m128i high01 = _mm_unpackhi_epi8(samples[0], samples[1]);
            const __m128i low23 = _mm_unpacklo_epi8(samples[2], samples[3]);
            const __m128i high23 = _mm_unpackhi_epi8(samples[2], samples[3]);
            __m256i pixels[2] = {
                __builtin_shufflevector(_mm_unpacklo_epi16(low01, low23), _mm_unpackhi_epi16(low01, low23), 0, 1, 2,
                                        3),
                __builtin_shufflevector(_mm_unpacklo_epi16(high01, high23), _mm_unpackhi_epi16(high01, high23), 0, 1,
                                        2, 3)};
            convert_pixels(transform, pixels);
            _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(row + x * 4u), pixels[0]);
            _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(row + x * 4u) + 1, pixels[1]);
        }
        if (x < width) {
            // The last pixels go through a padded copy, one pixel for each move.
            u8 tail[64]{};
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
            for (u32 i = x; i < width; ++i)
                for (u32 c = 0; c < kJpegComponentMaximum; ++c)
                    tail[(i - x) * 4u + c] = sources[c][i >> shifts[c]];
            __m256i_u* const unaligned = reinterpret_cast<__m256i_u*>(tail);
            __m256i pixels[2] = {_mm256_loadu_si256(unaligned), _mm256_loadu_si256(unaligned + 1)};
            convert_pixels(transform, pixels);
            _mm256_storeu_si256(unaligned, pixels[0]);
            _mm256_storeu_si256(unaligned + 1, pixels[1]);
            copy_bytes(row + x * 4u, tail, (width - x) * 4u);
        }
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
                                                blocks[component.block_offset + by * component.block_columns + bx], 64);
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
                        const u32 end = decode_block(&reader, dc_decode[component.dc_table],
                                                     ac_decode[component.ac_table], &predictor[component_index], &block);
                        if (!end) {
                            result.error = reader.error == JpegError::none ? JpegError::coefficient : reader.error;
                            return result;
                        }
                        reconstruct_component_block(image, layout, planes, component_index,
                                                    mcu_x * component.horizontal_sampling + horizontal,
                                                    mcu_y * component.vertical_sampling + vertical, block, end);
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
