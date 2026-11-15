#include "png_common.hpp"

#include "maicord/media/image/png.hpp"

#include "maicord/sys/simd.hpp"

namespace maicord {

bool png_format_channels(u8 color_type, u8 bit_depth, u8* channels) noexcept {
    // For color types 0 to 6 (1 and 5 are not valid): the channels, and the sum of the valid bit depths.
    static constexpr u8 kChannels[7] = {1, 0, 3, 1, 2, 0, 4}, kDepths[7] = {31, 0, 24, 15, 24, 0, 24};
    if (color_type > 6 || kChannels[color_type] == 0) {
        return false;
    }
    *channels = kChannels[color_type];
    return (bit_depth & (bit_depth - 1)) == 0 && (kDepths[color_type] & bit_depth) != 0;
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
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
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
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
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

// Sub: every byte adds the reconstructed byte one pixel to its left. Each 16-byte step is a prefix sum over its
// whole pixels (12 bytes of 3 or 6 byte pixels) plus the last pixel of the step before. Whole pixels are stored,
// so later loads see raw bytes. Returns the first byte not done.
template <u32 kBpp> __forceinline u32 unfilter_sub_wide(u8* bytes, u32 count) noexcept {
    constexpr u32 kStep = kBpp % 3 == 0 ? 12 : 16;
    // The last pixel of a step in every lane.
    constexpr auto r = [](u32 lane) noexcept { return static_cast<char>(kStep - kBpp + lane % kBpp); };
    const __m128i repeat = _mm_setr_epi8(r(0), r(1), r(2), r(3), r(4), r(5), r(6), r(7), r(8), r(9), r(10), r(11),
                                         r(12), r(13), r(14), r(15));
    u32 x = 0;
    __m128i last = _mm_setzero_si128();
    for (; x + 16 <= count; x += kStep) {
        __m128i value = load16(bytes + x);
        // Shifts of 1, 2, 4 and 8 pixels, those that stay in the step.
        value = _mm_add_epi8(value, _mm_slli_si128(value, kBpp));
        if constexpr (kBpp * 2 < kStep)
            value = _mm_add_epi8(value, _mm_slli_si128(value, kBpp * 2));
        if constexpr (kBpp * 4 < kStep)
            value = _mm_add_epi8(value, _mm_slli_si128(value, kBpp * 4));
        if constexpr (kBpp * 8 < kStep)
            value = _mm_add_epi8(value, _mm_slli_si128(value, kBpp * 8));
        value = _mm_add_epi8(value, last);
        if constexpr (kStep == 16) {
            store16(bytes + x, value);
        } else {
            _mm_storel_epi64(reinterpret_cast<__m128i*>(bytes + x), value);
            _mm_storeu_si32(bytes + x + 8, _mm_srli_si128(value, 8));
        }
        last = _mm_shuffle_epi8(value, repeat);
    }
    return x;
}

void unfilter_sub(u8* bytes, u32 count, u32 bpp) noexcept {
    u32 x = 0;
    switch (bpp) {
    case 1: x = unfilter_sub_wide<1>(bytes, count); break;
    case 2: x = unfilter_sub_wide<2>(bytes, count); break;
    case 3: x = unfilter_sub_wide<3>(bytes, count); break;
    case 4: x = unfilter_sub_wide<4>(bytes, count); break;
    case 6: x = unfilter_sub_wide<6>(bytes, count); break;
    case 8: x = unfilter_sub_wide<8>(bytes, count); break;
    default: break;
    }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (x = maximum(x, bpp); x < count; ++x) {
        bytes[x] = static_cast<u8>(bytes[x] + bytes[x - bpp]);
    }
}

void unfilter_up(u8* bytes, const u8* previous, u32 count) noexcept {
    u32 x = 0;
    for (; x + 16 <= count; x += 16) {
        store16(bytes + x, _mm_add_epi8(load16(bytes + x), load16(previous + x)));
    }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (; x < count; ++x) {
        bytes[x] = static_cast<u8>(bytes[x] + previous[x]);
    }
}

// The whole pixels of kBpp bytes of a row from byte x on, one for each step: 4-byte loads and stores for
// 3/4-byte pixels, 8-byte for 6/8. `step` gets the raw bytes of a pixel and the bytes above it, and returns the
// reconstructed bytes. Returns the first byte not done.
template <u32 kBpp, typename Step>
__forceinline u32 unfilter_pixels(u8* bytes, const u8* previous, u32 x, u32 count, Step step) noexcept {
    constexpr u32 kLoad = kBpp <= 4 ? 4 : 8;
    const auto load = [](const u8* at) noexcept {
        return kLoad == 4 ? _mm_loadu_si32(at) : _mm_loadl_epi64(reinterpret_cast<const __m128i*>(at));
    };
    const auto store = [](u8* at, __m128i pixel) noexcept {
        if constexpr (kLoad == 4) {
            _mm_storeu_si32(at, pixel);
        } else {
            _mm_storel_epi64(reinterpret_cast<__m128i*>(at), pixel);
        }
    };
    if constexpr (kBpp == kLoad) {
        for (; x + kLoad <= count; x += kBpp) {
            store(bytes + x, step(load(bytes + x), load(previous + x)));
        }
        return x;
    } else {
        // The bytes past a 3 or 6 byte pixel belong to the next pixel. The store keeps them, and the raw bytes
        // of that pixel load one step early: a load that shares only some bytes with a store before it waits
        // for that store.
        if (x + kLoad > count) {
            return x;
        }
        const __m128i keep = _mm_cmpgt_epi8(_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15),
                                            _mm_set1_epi8(static_cast<char>(kBpp - 1u)));
        __m128i next = load(bytes + x);
        for (;;) {
            const __m128i raw = next;
            const u32 following = x + kBpp;
            const bool more = following + kLoad <= count;
            if (more) {
                next = load(bytes + following);
            }
            store(bytes + x, _mm_blendv_epi8(step(raw, load(previous + x)), raw, keep));
            x = following;
            if (!more) {
                return x;
            }
        }
    }
}

// Average: floor((left + above) / 2) (pavgb rounds up; subtracting the low bit of left ^ above floors it).
template <u32 kBpp> u32 unfilter_average_wide(u8* bytes, const u8* previous, u32 count) noexcept {
    // The first pixel is done: it is the left pixel of the first step.
    if (kBpp + (kBpp <= 4 ? 4u : 8u) > count) {
        return kBpp;
    }
    __m128i left = kBpp <= 4 ? _mm_loadu_si32(bytes) : _mm_loadl_epi64(reinterpret_cast<const __m128i*>(bytes));
    return unfilter_pixels<kBpp>(bytes, previous, kBpp, count, [&](__m128i raw, __m128i above) noexcept {
        const __m128i average =
            _mm_sub_epi8(_mm_avg_epu8(left, above), _mm_and_si128(_mm_xor_si128(left, above), _mm_set1_epi8(1)));
        left = _mm_add_epi8(raw, average);
        return left;
    });
}

void unfilter_average(u8* bytes, const u8* previous, u32 count, u32 bpp) noexcept {
#pragma clang loop vectorize(disable) interleave(disable)
    for (u32 x = 0; x < bpp && x < count; ++x) {
        bytes[x] = static_cast<u8>(bytes[x] + (previous[x] >> 1));
    }
    u32 x = bpp;
    switch (bpp) {
    case 3: x = unfilter_average_wide<3>(bytes, previous, count); break;
    case 4: x = unfilter_average_wide<4>(bytes, previous, count); break;
    case 6: x = unfilter_average_wide<6>(bytes, previous, count); break;
    case 8: x = unfilter_average_wide<8>(bytes, previous, count); break;
    default: break;
    }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
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

// One Paeth step on 16-bit lanes: the reconstructed bytes of the pixels with the raw bytes `raw`. Only `left`
// comes from the step before. The step adds raw to each of the three candidates first, so the chain from left
// to the result is a subtract, an add, an absolute value, a compare and two blends. The empty assembly
// statement keeps the two sums: without it the compiler adds raw after the blends, as three more operations
// on the chain.
__forceinline __m128i paeth_step(__m128i raw, __m128i left, __m128i above, __m128i upper_left) noexcept {
    const __m128i low = _mm_set1_epi16(0xFF);
    const __m128i to_above = _mm_sub_epi16(above, upper_left);
    const __m128i to_left = _mm_sub_epi16(left, upper_left);
    const __m128i distance_left = _mm_abs_epi16(to_above);
    const __m128i distance_above = _mm_abs_epi16(to_left);
    const __m128i distance_upper_left = _mm_abs_epi16(_mm_add_epi16(to_above, to_left));
    const __m128i not_left = _mm_or_si128(_mm_cmpgt_epi16(distance_left, distance_above),
                                          _mm_cmpgt_epi16(distance_left, distance_upper_left));
    const __m128i not_above = _mm_cmpgt_epi16(distance_above, distance_upper_left);
    __m128i from_above = _mm_and_si128(_mm_add_epi16(raw, above), low);
    __m128i from_upper_left = _mm_and_si128(_mm_add_epi16(raw, upper_left), low);
    __asm__("" : "+x"(from_above), "+x"(from_upper_left));
    return _mm_blendv_epi8(_mm_and_si128(_mm_add_epi16(raw, left), low),
                           _mm_blendv_epi8(from_above, from_upper_left, not_above), not_left);
}

// Paeth on 16-bit lanes, one whole pixel per step.
template <u32 kBpp> u32 unfilter_paeth_wide(u8* bytes, const u8* previous, u32 count) noexcept {
    __m128i left = _mm_setzero_si128();
    __m128i upper_left = _mm_setzero_si128();
    return unfilter_pixels<kBpp>(bytes, previous, 0, count, [&](__m128i raw, __m128i above_bytes) noexcept {
        const __m128i above = _mm_cvtepu8_epi16(above_bytes);
        left = paeth_step(_mm_cvtepu8_epi16(raw), left, above, upper_left);
        upper_left = above;
        return _mm_packus_epi16(left, left);
    });
}

void unfilter_paeth(u8* bytes, const u8* previous, u32 count, u32 bpp) noexcept {
    u32 x = 0;
    switch (bpp) {
    case 3: x = unfilter_paeth_wide<3>(bytes, previous, count); break;
    case 4: x = unfilter_paeth_wide<4>(bytes, previous, count); break;
    case 6: x = unfilter_paeth_wide<6>(bytes, previous, count); break;
    case 8: x = unfilter_paeth_wide<8>(bytes, previous, count); break;
    default: break;
    }
    // The first pixel has no left or upper-left neighbour: Paeth picks above.
#pragma clang loop vectorize(disable) interleave(disable)
    for (; x < bpp && x < count; ++x) {
        bytes[x] = static_cast<u8>(bytes[x] + previous[x]);
    }
    if (bpp == 1 && x < count) {
        // Palette and 8-bit gray rows: the left byte stays in a register
        // rather than being reloaded right after it was stored.
        u32 left = bytes[x - 1];
        u32 upper_left = previous[x - 1];
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
        for (; x < count; ++x) {
            const u32 above = previous[x];
            left = static_cast<u8>(bytes[x] + paeth_predictor(static_cast<i32>(left), static_cast<i32>(above),
                                                              static_cast<i32>(upper_left)));
            bytes[x] = static_cast<u8>(left);
            upper_left = above;
        }
        return;
    }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
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
