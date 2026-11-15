#include "maicord/media/image/webp_alpha.hpp"

#include "png_common.hpp"

#include "maicord/sys/simd.hpp"

namespace maicord {
namespace {

WebpAlphaDecodeResult alpha_failure(
    WebpAlphaDecodeResult result, WebpAlphaError error) noexcept {
    result.error = error;
    return result;
}

__forceinline u8 gradient_predictor(u8 left, u8 above, u8 upper_left) noexcept {
    const i32 value = static_cast<i32>(left) + above - upper_left;
    return value < 0 ? 0 : value > 255 ? 255 : static_cast<u8>(value);
}

// Gradient: each byte adds clamp(left + above - upper left). In the high byte of a 16-bit lane the clamp is two
// operations that saturate: add max(above - upper left, 0), then subtract max(upper left - above, 0). The low
// byte of the lane stays 0x00 or 0xFF and changes neither. Only `left` goes from pixel to pixel.
__forceinline void unfilter_gradient(u8* row, const u8* above, u32 width) noexcept {
    const auto high_bytes = [](const u8* bytes) noexcept {
        return _mm_unpacklo_epi8(_mm_setzero_si128(), _mm_loadl_epi64(reinterpret_cast<const __m128i*>(bytes)));
    };
    u32 x = 1;
    __m128i left = _mm_cvtsi32_si128(row[0] << 8);
    for (; x + 8 <= width; x += 8) {
        const __m128i top = high_bytes(above + x), top_left = high_bytes(above + x - 1);
        __m128i up = _mm_subs_epu16(top, top_left), down = _mm_subs_epu16(top_left, top), raw = high_bytes(row + x);
#pragma clang loop unroll(disable)
        for (u32 pixel = 0; pixel < 8; ++pixel) {
            left = _mm_add_epi16(_mm_subs_epu16(_mm_adds_epu16(left, up), down), raw);
            row[x + pixel] = static_cast<u8>(_mm_cvtsi128_si32(left) >> 8);
            up = _mm_srli_si128(up, 2);
            down = _mm_srli_si128(down, 2);
            raw = _mm_srli_si128(raw, 2);
        }
    }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (; x < width; ++x) {
        row[x] = static_cast<u8>(row[x] + gradient_predictor(row[x - 1], above[x], above[x - 1]));
    }
}

} // namespace

// The first row predicts from the left (its first pixel from zero); later rows
// start from the pixel above, then apply the filter's own predictor.
void webp_alpha_unfilter_row(WebpAlphaFilter filter, u8* row, const u8* above, u32 width) noexcept {
    if (above && filter == WebpAlphaFilter::vertical) {
        u32 x = 0;
        for (; x + 16 <= width; x += 16) {
            const __m128i sum = _mm_add_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(row + x)),
                                             _mm_loadu_si128(reinterpret_cast<const __m128i*>(above + x)));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(row + x), sum);
        }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
        for (; x < width; ++x) {
            row[x] = static_cast<u8>(row[x] + above[x]);
        }
        return;
    }
    if (above) {
        row[0] = static_cast<u8>(row[0] + above[0]);
    }
    if (!above || filter == WebpAlphaFilter::horizontal) {
        // The Sub filter of PNG for pixels of one byte: 16 bytes for each step.
        png_unfilter_row(1, row, nullptr, width, 1);
        return;
    }
    unfilter_gradient(row, above, width);
}

WebpAlphaDecodeResult webp_alpha_decode(
    ConstSlice<u8> payload, u32 width, u32 height,
    Vp8lWorkspace* workspace, Slice<u8> rgba_scratch,
    Slice<u8> alpha_output) noexcept {
    WebpAlphaDecodeResult result{};
    if (!payload.data || payload.count == 0 || !alpha_output.data) {
        return alpha_failure(result, WebpAlphaError::invalid_input);
    }
    if (width == 0 || height == 0 ||
        width > kWebpLosslessDimensionMaximum ||
        height > kWebpLosslessDimensionMaximum ||
        static_cast<u64>(width) * height > kWebpLosslessPixelMaximum) {
        return alpha_failure(result, WebpAlphaError::dimensions);
    }
    const u32 pixel_count = width * height;
    if (alpha_output.count < pixel_count) {
        return alpha_failure(result, WebpAlphaError::output_capacity);
    }

    const u8 control = payload[0];
    if ((control & 0xC0u) != 0) {
        return alpha_failure(result, WebpAlphaError::reserved_bits);
    }
    const u8 preprocessing = (control >> 4) & 3u;
    const u8 filter = (control >> 2) & 3u;
    const u8 compression = control & 3u;
    if (preprocessing > 1) {
        return alpha_failure(
            result, WebpAlphaError::unsupported_preprocessing);
    }
    if (compression > 1) {
        return alpha_failure(
            result, WebpAlphaError::unsupported_compression);
    }
    result.preprocessing =
        static_cast<WebpAlphaPreprocessing>(preprocessing);
    result.filter = static_cast<WebpAlphaFilter>(filter);
    result.compression =
        static_cast<WebpAlphaCompression>(compression);

    if (result.compression == WebpAlphaCompression::raw) {
        if (payload.count != pixel_count + 1) {
            return alpha_failure(result, WebpAlphaError::data_size);
        }
        copy_bytes(alpha_output.data, payload.data + 1, pixel_count);
    } else {
        const u64 required_scratch =
            static_cast<u64>(pixel_count) * 4u;
        if (!workspace) {
            return alpha_failure(result,
                                 WebpAlphaError::invalid_input);
        }
        if (!rgba_scratch.data ||
            rgba_scratch.count < required_scratch ||
            (reinterpret_cast<usize>(rgba_scratch.data) & 3u) != 0) {
            return alpha_failure(result,
                                 WebpAlphaError::scratch_capacity);
        }
        if (payload.count == 1) {
            return alpha_failure(result, WebpAlphaError::data_size);
        }
        WebpImage image{};
        result.lossless = webp_lossless_decode_image_stream_argb32(
            ConstSlice<u8>{payload.data + 1, payload.count - 1},
            width, height, &image, workspace, rgba_scratch);
        if (!result.lossless.ok()) {
            return alpha_failure(result,
                                 WebpAlphaError::lossless_decode);
        }
        // Green stays at byte one in the native packed pixels on this platform.
        const __m128i greens = _mm_setr_epi8(1, 5, 9, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
        u32 index = 0;
        for (; index + 4 <= pixel_count; index += 4) {
            const __m128i packed = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rgba_scratch.data + index * 4));
            _mm_storeu_si32(alpha_output.data + index, _mm_shuffle_epi8(packed, greens));
        }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
        for (; index < pixel_count; ++index) {
            alpha_output[index] = rgba_scratch[index * 4 + 1];
        }
    }

    for (u32 y = 0; y < height && result.filter != WebpAlphaFilter::none; ++y) {
        u8* const row = alpha_output.data + y * width;
        webp_alpha_unfilter_row(result.filter, row, y ? row - width : nullptr, width);
    }
    result.alpha_bytes = pixel_count;
    result.error = WebpAlphaError::none;
    return result;
}

} // namespace maicord
