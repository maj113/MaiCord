#include "png_common.hpp"

#include "maicord/media/png.hpp"

#include <immintrin.h>

extern "C" __declspec(dllimport) unsigned long __stdcall RtlComputeCrc32(
    unsigned long initial, const void* bytes, unsigned long count);

namespace maicord {
u32 png_crc32(const u8* bytes, u32 count) noexcept {
    // ntdll selects its CRC-32 implementation for the host CPU. PNG/APNG and
    // gzip use the same IEEE polynomial and an initial published CRC of zero.
    return count ? static_cast<u32>(RtlComputeCrc32(0, bytes, count)) : 0;
}

bool png_format_channels(u8 color_type, u8 bit_depth, u8* channels) noexcept {
    switch (color_type) {
    case 0:
        *channels = 1;
        return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8 || bit_depth == 16;
    case 2:
        *channels = 3;
        return bit_depth == 8 || bit_depth == 16;
    case 3:
        *channels = 1;
        return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8;
    case 4:
        *channels = 2;
        return bit_depth == 8 || bit_depth == 16;
    case 6:
        *channels = 4;
        return bit_depth == 8 || bit_depth == 16;
    default:
        return false;
    }
}

bool png_apply_palette(PngImage* image, const u8* data, u32 length) noexcept {
    if (image->color_type == 0 || image->color_type == 4 || length == 0 || length % 3 != 0 ||
        length > 768) {
        return false;
    }
    const u32 entries = length / 3;
    if (image->color_type == 3 && entries > (1u << image->bit_depth)) {
        return false;
    }
    for (u32 index = 0; index < entries; ++index) {
        image->palette_rgba[index] = static_cast<u32>(data[index * 3]) |
                                     static_cast<u32>(data[index * 3 + 1]) << 8 |
                                     static_cast<u32>(data[index * 3 + 2]) << 16 | 0xFF000000u;
    }
    image->palette_count = static_cast<u16>(entries);
    return true;
}

bool png_apply_transparency(PngImage* image, const u8* data, u32 length) noexcept {
    const u32 maximum_sample = image->bit_depth == 16 ? 65'535u : (1u << image->bit_depth) - 1u;
    if (image->color_type == 0 && length == 2) {
        image->transparent_gray = byte_load_be16(data);
        if (image->transparent_gray > maximum_sample) {
            return false;
        }
    } else if (image->color_type == 2 && length == 6) {
        image->transparent_red = byte_load_be16(data);
        image->transparent_green = byte_load_be16(data + 2);
        image->transparent_blue = byte_load_be16(data + 4);
        if (image->transparent_red > maximum_sample || image->transparent_green > maximum_sample ||
            image->transparent_blue > maximum_sample) {
            return false;
        }
    } else if (image->color_type == 3 && length != 0 && length <= image->palette_count) {
        for (u32 index = 0; index < length; ++index) {
            image->palette_rgba[index] =
                (image->palette_rgba[index] & 0x00FFFFFFu) | static_cast<u32>(data[index]) << 24;
        }
    } else {
        return false;
    }
    image->has_transparency = true;
    return true;
}

namespace {

__forceinline __m128i load16(const u8* bytes) noexcept {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes));
}
__forceinline void store16(u8* bytes, __m128i value) noexcept {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(bytes), value);
}

// Sub: every byte adds the reconstructed byte one pixel to its left. Each
// 16-byte step is a prefix sum over its whole pixels plus the last pixel of
// the previous step; whole pixels are stored, so later loads see raw bytes.
void unfilter_sub(u8* bytes, u32 count, u32 bpp) noexcept {
    u32 x = 0;
    __m128i last = _mm_setzero_si128();
    switch (bpp) {
    case 1:
        for (; x + 16 <= count; x += 16) {
            __m128i value = load16(bytes + x);
            value = _mm_add_epi8(value, _mm_slli_si128(value, 1));
            value = _mm_add_epi8(value, _mm_slli_si128(value, 2));
            value = _mm_add_epi8(value, _mm_slli_si128(value, 4));
            value = _mm_add_epi8(value, _mm_slli_si128(value, 8));
            value = _mm_add_epi8(value, last);
            store16(bytes + x, value);
            last = _mm_shuffle_epi8(value, _mm_set1_epi8(15));
        }
        break;
    case 2:
        for (; x + 16 <= count; x += 16) {
            __m128i value = load16(bytes + x);
            value = _mm_add_epi8(value, _mm_slli_si128(value, 2));
            value = _mm_add_epi8(value, _mm_slli_si128(value, 4));
            value = _mm_add_epi8(value, _mm_slli_si128(value, 8));
            value = _mm_add_epi8(value, last);
            store16(bytes + x, value);
            last = _mm_shuffle_epi8(value, _mm_set1_epi16(0x0F0E));
        }
        break;
    case 3: {
        const __m128i repeat = _mm_setr_epi8(9, 10, 11, 9, 10, 11, 9, 10, 11, 9, 10, 11, 9, 10, 11, 9);
        for (; x + 16 <= count; x += 12) {
            __m128i value = load16(bytes + x);
            value = _mm_add_epi8(value, _mm_slli_si128(value, 3));
            value = _mm_add_epi8(value, _mm_slli_si128(value, 6));
            value = _mm_add_epi8(value, last);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(bytes + x), value);
            _mm_storeu_si32(bytes + x + 8, _mm_srli_si128(value, 8));
            last = _mm_shuffle_epi8(value, repeat);
        }
        break;
    }
    case 4:
        for (; x + 16 <= count; x += 16) {
            __m128i value = load16(bytes + x);
            value = _mm_add_epi8(value, _mm_slli_si128(value, 4));
            value = _mm_add_epi8(value, _mm_slli_si128(value, 8));
            value = _mm_add_epi8(value, last);
            store16(bytes + x, value);
            last = _mm_shuffle_epi32(value, _MM_SHUFFLE(3, 3, 3, 3));
        }
        break;
    case 6: {
        const __m128i repeat = _mm_setr_epi8(6, 7, 8, 9, 10, 11, 6, 7, 8, 9, 10, 11, 6, 7, 8, 9);
        for (; x + 16 <= count; x += 12) {
            __m128i value = load16(bytes + x);
            value = _mm_add_epi8(value, _mm_slli_si128(value, 6));
            value = _mm_add_epi8(value, last);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(bytes + x), value);
            _mm_storeu_si32(bytes + x + 8, _mm_srli_si128(value, 8));
            last = _mm_shuffle_epi8(value, repeat);
        }
        break;
    }
    case 8:
        for (; x + 16 <= count; x += 16) {
            __m128i value = load16(bytes + x);
            value = _mm_add_epi8(value, _mm_slli_si128(value, 8));
            value = _mm_add_epi8(value, last);
            store16(bytes + x, value);
            last = _mm_unpackhi_epi64(value, value);
        }
        break;
    default:
        break;
    }
    for (x = maximum(x, bpp); x < count; ++x) {
        bytes[x] = static_cast<u8>(bytes[x] + bytes[x - bpp]);
    }
}

void unfilter_up(u8* bytes, const u8* previous, u32 count) noexcept {
    u32 x = 0;
    for (; x + 16 <= count; x += 16) {
        store16(bytes + x, _mm_add_epi8(load16(bytes + x), load16(previous + x)));
    }
    for (; x < count; ++x) {
        bytes[x] = static_cast<u8>(bytes[x] + previous[x]);
    }
}

// Average: floor((left + above) / 2), one whole pixel per step for 3/4/6/8
// byte pixels (pavgb rounds up; subtracting the low bit of left ^ above
// floors it). Bytes of the next pixel ride along unchanged and are restored.
void unfilter_average(u8* bytes, const u8* previous, u32 count, u32 bpp) noexcept {
    for (u32 x = 0; x < bpp && x < count; ++x) {
        bytes[x] = static_cast<u8>(bytes[x] + (previous[x] >> 1));
    }
    u32 x = bpp;
    const __m128i one = _mm_set1_epi8(1);
    if (bpp == 3 || bpp == 4) {
        const __m128i keep = bpp == 3 ? _mm_setr_epi8(0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
                                      : _mm_setzero_si128();
        __m128i left = _mm_loadu_si32(bytes);
        for (; x + 4 <= count; x += bpp) {
            const __m128i above = _mm_loadu_si32(previous + x);
            const __m128i raw = _mm_loadu_si32(bytes + x);
            const __m128i average =
                _mm_sub_epi8(_mm_avg_epu8(left, above), _mm_and_si128(_mm_xor_si128(left, above), one));
            left = _mm_add_epi8(raw, average);
            _mm_storeu_si32(bytes + x, _mm_blendv_epi8(left, raw, keep));
        }
    } else if (bpp == 6 || bpp == 8) {
        const __m128i keep = bpp == 6 ? _mm_setr_epi8(0, 0, 0, 0, 0, 0, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0)
                                      : _mm_setzero_si128();
        __m128i left = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(bytes));
        for (; x + 8 <= count; x += bpp) {
            const __m128i above = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(previous + x));
            const __m128i raw = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(bytes + x));
            const __m128i average =
                _mm_sub_epi8(_mm_avg_epu8(left, above), _mm_and_si128(_mm_xor_si128(left, above), one));
            left = _mm_add_epi8(raw, average);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(bytes + x), _mm_blendv_epi8(left, raw, keep));
        }
    }
    for (; x < count; ++x) {
        bytes[x] = static_cast<u8>(bytes[x] + ((static_cast<u32>(bytes[x - bpp]) + previous[x]) >> 1));
    }
}

__forceinline u32 paeth_predictor(i32 left, i32 above, i32 upper_left) noexcept {
    const i32 to_above = above - upper_left;
    const i32 to_left = left - upper_left;
    const i32 distance_left = to_above < 0 ? -to_above : to_above;    // |p - left|
    const i32 distance_above = to_left < 0 ? -to_left : to_left;      // |p - above|
    const i32 sum = to_above + to_left;
    const i32 distance_upper_left = sum < 0 ? -sum : sum;             // |p - upper_left|
    const i32 other = distance_above <= distance_upper_left ? above : upper_left;
    return static_cast<u32>(distance_left <= distance_above && distance_left <= distance_upper_left ? left : other);
}

// Paeth on 16-bit lanes, one whole pixel per step: 4-byte loads and stores
// for 3/4-byte pixels, 8-byte for 6/8 (bytes past a 3/6-byte pixel are kept).
template <u32 kLoad> void unfilter_paeth_wide(u8* bytes, const u8* previous, u32 count, u32 bpp, u32* done) noexcept {
    const __m128i keep = _mm_cmpgt_epi8(_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15),
                                        _mm_set1_epi8(static_cast<char>(bpp - 1u)));
    const __m128i low = _mm_set1_epi16(0xFF);
    __m128i left = _mm_setzero_si128();
    __m128i upper_left = _mm_setzero_si128();
    u32 x = 0;
    for (; x + kLoad <= count; x += bpp) {
        __m128i raw, above;
        if constexpr (kLoad == 4) {
            raw = _mm_loadu_si32(bytes + x);
            above = _mm_cvtepu8_epi16(_mm_loadu_si32(previous + x));
        } else {
            raw = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(bytes + x));
            above = _mm_cvtepu8_epi16(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(previous + x)));
        }
        const __m128i to_above = _mm_sub_epi16(above, upper_left);
        const __m128i to_left = _mm_sub_epi16(left, upper_left);
        const __m128i distance_left = _mm_abs_epi16(to_above);
        const __m128i distance_above = _mm_abs_epi16(to_left);
        const __m128i distance_upper_left = _mm_abs_epi16(_mm_add_epi16(to_above, to_left));
        const __m128i not_left = _mm_or_si128(_mm_cmpgt_epi16(distance_left, distance_above),
                                              _mm_cmpgt_epi16(distance_left, distance_upper_left));
        const __m128i not_above = _mm_cmpgt_epi16(distance_above, distance_upper_left);
        const __m128i predicted =
            _mm_blendv_epi8(left, _mm_blendv_epi8(above, upper_left, not_above), not_left);
        left = _mm_and_si128(_mm_add_epi16(_mm_cvtepu8_epi16(raw), predicted), low);
        upper_left = above;
        const __m128i packed = _mm_blendv_epi8(_mm_packus_epi16(left, left), raw, keep);
        if constexpr (kLoad == 4) {
            _mm_storeu_si32(bytes + x, packed);
        } else {
            _mm_storel_epi64(reinterpret_cast<__m128i*>(bytes + x), packed);
        }
    }
    *done = x;
}

void unfilter_paeth(u8* bytes, const u8* previous, u32 count, u32 bpp) noexcept {
    u32 x = 0;
    switch (bpp) {
    case 3:
    case 4: unfilter_paeth_wide<4>(bytes, previous, count, bpp, &x); break;
    case 6:
    case 8: unfilter_paeth_wide<8>(bytes, previous, count, bpp, &x); break;
    default: break;
    }
    // The first pixel has no left or upper-left neighbour: Paeth picks above.
    for (; x < bpp && x < count; ++x) {
        bytes[x] = static_cast<u8>(bytes[x] + previous[x]);
    }
    for (; x < count; ++x) {
        bytes[x] = static_cast<u8>(bytes[x] + paeth_predictor(bytes[x - bpp], previous[x], previous[x - bpp]));
    }
}

} // namespace

bool png_unfilter_row(u8 filter, u8* bytes, const u8* previous, u32 count, u32 bpp) noexcept {
    switch (filter) {
    case 0:
        return true;
    case 1:
        unfilter_sub(bytes, count, bpp);
        return true;
    case 2:
        if (previous) {
            unfilter_up(bytes, previous, count);
        }
        return true;
    case 3:
        if (previous) {
            unfilter_average(bytes, previous, count, bpp);
        } else {
            for (u32 x = bpp; x < count; ++x) {
                bytes[x] = static_cast<u8>(bytes[x] + (bytes[x - bpp] >> 1));
            }
        }
        return true;
    case 4:
        // Without a previous row, above and upper-left are zero and Paeth is Sub.
        if (previous) {
            unfilter_paeth(bytes, previous, count, bpp);
        } else {
            unfilter_sub(bytes, count, bpp);
        }
        return true;
    default:
        return false;
    }
}

} // namespace maicord
