#include "maicord/media/yuv_convert.hpp"
#include "maicord/media/yuv_convert_common.hpp"

#include "maicord/sys/simd.hpp"

namespace maicord {
namespace {

__m128i pack_four_rgba(__m128i r, __m128i g, __m128i b,
                       __m128i a) noexcept {
    const __m128i rg_low = _mm_unpacklo_epi16(r, g);
    const __m128i ba_low = _mm_unpacklo_epi16(b, a);
    const __m128i rgba_01 = _mm_unpacklo_epi32(rg_low, ba_low);
    const __m128i rgba_23 = _mm_unpackhi_epi32(rg_low, ba_low);
    return _mm_packus_epi16(rgba_01, rgba_23);
}

} // namespace

void nv12_to_rgba8_avx2(const u8* y_plane, const u8* uv_plane,
                        u32 source_pitch, u8* destination,
                        u32 destination_pitch, u32 width, u32 height,
                        bool bt709, bool full_range) noexcept {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i byte_max = _mm256_set1_epi32(255);
    const __m256i rounding = _mm256_set1_epi32(128);
    const __m256i offset_128 = _mm256_set1_epi32(128);
    const YuvRgbCoefficients c = yuv_rgb_coefficients(bt709, full_range);
    const __m256i y_offset = _mm256_set1_epi32(c.y_offset);
    const __m256i luma_scale = _mm256_set1_epi32(c.y_scale);
    const __m256i red_scale = _mm256_set1_epi32(c.red_cr);
    const __m256i green_cb_scale = _mm256_set1_epi32(c.green_cb);
    const __m256i green_cr_scale = _mm256_set1_epi32(c.green_cr);
    const __m256i blue_scale = _mm256_set1_epi32(c.blue_cb);
    const __m128i u_shuffle = _mm_setr_epi8(
        0, 0, 2, 2, 4, 4, 6, 6,
        static_cast<char>(0x80), static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x80));
    const __m128i v_shuffle = _mm_setr_epi8(
        1, 1, 3, 3, 5, 5, 7, 7,
        static_cast<char>(0x80), static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x80));
    const __m128i alpha = _mm_set1_epi16(255);
    for (u32 y = 0; y < height; ++y) {
        const u8* y_row = y_plane + static_cast<u64>(y) * source_pitch;
        const u8* uv_row = uv_plane +
                           static_cast<u64>(y / 2u) * source_pitch;
        u8* output = destination + static_cast<u64>(y) * destination_pitch;
        u32 x = 0;
        for (; x + 8u <= width; x += 8u) {
            const __m128i y8 = _mm_loadl_epi64(
                reinterpret_cast<const __m128i*>(y_row + x));
            const __m128i uv8 = _mm_loadl_epi64(
                reinterpret_cast<const __m128i*>(uv_row + x));
            __m256i luma = _mm256_cvtepu8_epi32(y8);
            __m256i cb = _mm256_cvtepu8_epi32(
                _mm_shuffle_epi8(uv8, u_shuffle));
            __m256i cr = _mm256_cvtepu8_epi32(
                _mm_shuffle_epi8(uv8, v_shuffle));
            cb = _mm256_sub_epi32(cb, offset_128);
            cr = _mm256_sub_epi32(cr, offset_128);
            luma = _mm256_sub_epi32(luma, y_offset);
            luma = _mm256_mullo_epi32(luma, luma_scale);
            __m256i red = _mm256_add_epi32(
                rounding,
                _mm256_add_epi32(
                    luma, _mm256_mullo_epi32(cr, red_scale)));
            __m256i green = _mm256_add_epi32(
                rounding,
                _mm256_sub_epi32(
                    _mm256_sub_epi32(
                        luma,
                        _mm256_mullo_epi32(cb, green_cb_scale)),
                    _mm256_mullo_epi32(cr, green_cr_scale)));
            __m256i blue = _mm256_add_epi32(
                rounding,
                _mm256_add_epi32(
                    luma, _mm256_mullo_epi32(cb, blue_scale)));
            red = _mm256_srai_epi32(red, 8);
            green = _mm256_srai_epi32(green, 8);
            blue = _mm256_srai_epi32(blue, 8);
            red = _mm256_min_epi32(byte_max, _mm256_max_epi32(zero, red));
            green = _mm256_min_epi32(byte_max,
                                     _mm256_max_epi32(zero, green));
            blue = _mm256_min_epi32(byte_max,
                                    _mm256_max_epi32(zero, blue));
            const __m128i r16 = _mm_packus_epi32(
                _mm256_castsi256_si128(red),
                _mm256_extracti128_si256(red, 1));
            const __m128i g16 = _mm_packus_epi32(
                _mm256_castsi256_si128(green),
                _mm256_extracti128_si256(green, 1));
            const __m128i b16 = _mm_packus_epi32(
                _mm256_castsi256_si128(blue),
                _mm256_extracti128_si256(blue, 1));
            const __m128i low = pack_four_rgba(r16, g16, b16, alpha);
            const __m128i high = pack_four_rgba(
                _mm_srli_si128(r16, 8), _mm_srli_si128(g16, 8),
                _mm_srli_si128(b16, 8), alpha);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(output + x * 4u),
                             low);
            _mm_storeu_si128(
                reinterpret_cast<__m128i*>(output + x * 4u + 16u), high);
        }
        for (; x < width; ++x) {
            const i32 luma = y_row[x];
            const i32 cb = uv_row[x & ~1u] - 128;
            const i32 cr = uv_row[(x & ~1u) + 1u] - 128;
            yuv_rgb_pixel(output + x * 4u, luma, cb, cr, c);
        }
    }
}

} // namespace maicord
