#pragma once

#include "maicord/media/deflate.hpp"

namespace maicord {

inline constexpr u32 kPngEncodedMaximumBytes = 16u * 1'024u * 1'024u;
inline constexpr u32 kPngDimensionMaximum = 2048;
inline constexpr u32 kPngInflatedMaximumBytes = kPngDimensionMaximum * (kPngDimensionMaximum * 8u + 1u);
// IDAT spans held inline; files with more chunks rebuild the full span list
// in the tail of the decode buffer (see png_decode_buffer_bytes).
inline constexpr u32 kPngIdatSpanCapacity = 64;
inline constexpr u32 kPngIdatChunkMaximum = 65'535;
inline constexpr u32 kPngPaletteCapacity = 256;

struct PngImage {
    DeflateInputSpan idat_spans[kPngIdatSpanCapacity];
    u32 palette_rgba[kPngPaletteCapacity];
    u32 width;
    u32 height;
    u32 row_bytes;
    u32 row_stride;
    u32 inflated_bytes;
    u32 compressed_bytes;
    u32 transparent_gray;
    u32 transparent_red;
    u32 transparent_green;
    u32 transparent_blue;
    u16 palette_count;
    u16 idat_span_count;
    u8 bit_depth;
    u8 color_type;
    u8 channels;
    u8 bytes_per_pixel;
    bool has_transparency;
    bool has_color_metadata;
    u8 interlace; // 0 or 1 (Adam7)
    u8 reserved;
};

enum class PngError : u8 {
    none,
    invalid_input,
    encoded_limit,
    truncated,
    signature,
    chunk_type,
    chunk_crc,
    chunk_order,
    duplicate_chunk,
    unknown_critical_chunk,
    unsupported_animation,
    unsupported_interlace,
    unsupported_format,
    dimensions,
    palette,
    transparency,
    idat_capacity,
    inflated_limit,
    inflate,
    filter,
    palette_index,
};

struct PngParseResult {
    u32 chunks;
    u32 ancillary_chunks;
    PngError error;

    constexpr bool ok() const noexcept {
        return error == PngError::none;
    }
};

struct PngDecodeResult {
    PngParseResult parse;
    ZlibInflateResult inflate;
    u32 reconstructed_bytes;
    PngError error;

    constexpr bool ok() const noexcept {
        return error == PngError::none;
    }
};

PngParseResult png_parse(ConstSlice<u8> encoded,
                         PngImage* image) noexcept;
bool png_image_configure(PngImage* image, u32 width, u32 height,
                         u8 bit_depth, u8 color_type) noexcept;
// Scanline buffer a decode needs: inflated_bytes of output, plus Adam7 pass
// storage for interlaced images, plus a span table when the file has more IDAT
// chunks than PngImage holds inline. Output is always the progressive layout.
u32 png_decode_buffer_bytes(const PngImage& image) noexcept;
inline constexpr u32 kPngDecodeBufferMaximumBytes = 2u * kPngInflatedMaximumBytes + (2u << 20);
PngDecodeResult png_decode_parsed_scanlines(
    PngImage* image, DeflateWorkspace* workspace,
    Slice<u8> scanlines) noexcept;
PngDecodeResult png_decode_scanlines(
    ConstSlice<u8> encoded, PngImage* image,
    DeflateWorkspace* workspace, Slice<u8> scanlines) noexcept;

static_assert(sizeof(PngImage) == 2'104);

} // namespace maicord
