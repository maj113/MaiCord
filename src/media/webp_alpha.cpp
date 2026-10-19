#include "maicord/media/webp_alpha.hpp"

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

// The first row predicts from the left (its first pixel from zero); later rows
// start from the pixel above, then apply the filter's own predictor.
void unfilter_alpha(Slice<u8> alpha, u32 width, u32 height, WebpAlphaFilter filter) noexcept {
    if (filter == WebpAlphaFilter::none) {
        return;
    }
    u8* row = alpha.data;
    for (u32 x = 1; x < width; ++x) {
        row[x] = static_cast<u8>(row[x] + row[x - 1]);
    }
    for (u32 y = 1; y < height; ++y) {
        const u8* above = row;
        row += width;
        if (filter == WebpAlphaFilter::vertical) {
            u32 x = 0;
            for (; x + 16 <= width; x += 16) {
                const __m128i sum = _mm_add_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(row + x)),
                                                 _mm_loadu_si128(reinterpret_cast<const __m128i*>(above + x)));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(row + x), sum);
            }
            for (; x < width; ++x) {
                row[x] = static_cast<u8>(row[x] + above[x]);
            }
            continue;
        }
        row[0] = static_cast<u8>(row[0] + above[0]);
        if (filter == WebpAlphaFilter::horizontal) {
            for (u32 x = 1; x < width; ++x) {
                row[x] = static_cast<u8>(row[x] + row[x - 1]);
            }
        } else {
            for (u32 x = 1; x < width; ++x) {
                row[x] = static_cast<u8>(row[x] + gradient_predictor(row[x - 1], above[x], above[x - 1]));
            }
        }
    }
}

} // namespace

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
        for (u32 index = 0; index < pixel_count; ++index) {
            alpha_output[index] = payload[index + 1];
        }
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
        for (; index < pixel_count; ++index) {
            alpha_output[index] = rgba_scratch[index * 4 + 1];
        }
    }

    unfilter_alpha(alpha_output, width, height, result.filter);
    result.alpha_bytes = pixel_count;
    result.error = WebpAlphaError::none;
    return result;
}

} // namespace maicord
