#pragma once

#include "maicord/media/webp.hpp"

namespace maicord {

enum class WebpAlphaCompression : u8 {
    raw,
    lossless,
};

enum class WebpAlphaFilter : u8 {
    none,
    horizontal,
    vertical,
    gradient,
};

enum class WebpAlphaPreprocessing : u8 {
    none,
    level_reduction,
};

enum class WebpAlphaError : u8 {
    none,
    invalid_input,
    dimensions,
    reserved_bits,
    unsupported_preprocessing,
    unsupported_compression,
    data_size,
    output_capacity,
    scratch_capacity,
    lossless_decode,
};

struct WebpAlphaDecodeResult {
    WebpDecodeResult lossless;
    u32 alpha_bytes;
    WebpAlphaCompression compression;
    WebpAlphaFilter filter;
    WebpAlphaPreprocessing preprocessing;
    WebpAlphaError error;

    constexpr bool ok() const noexcept {
        return error == WebpAlphaError::none;
    }
};

// Decodes one ALPH chunk payload, including its one-byte control header.
// For compressed input, rgba_scratch must be aligned and hold width*height*4
// bytes. alpha_output always receives width*height unfiltered bytes.
WebpAlphaDecodeResult webp_alpha_decode(
    ConstSlice<u8> payload, u32 width, u32 height,
    Vp8lWorkspace* workspace, Slice<u8> rgba_scratch,
    Slice<u8> alpha_output) noexcept;
// Reverses one row's prediction filter in place. `above` is the already
// unfiltered row above, or null for the first row. Not for filter none.
void webp_alpha_unfilter_row(WebpAlphaFilter filter, u8* row, const u8* above, u32 width) noexcept;

} // namespace maicord
