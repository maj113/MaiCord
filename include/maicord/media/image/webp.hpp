#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u32 kWebpEncodedMaximumBytes = 2u * 1024u * 1024u;
// The GPU lossy path and animation frames.
inline constexpr u32 kWebpDimensionMaximum = 512;
inline constexpr u32 kWebpPixelMaximum =
    kWebpDimensionMaximum * kWebpDimensionMaximum;
// Still lossless images and ALPH planes decode on the CPU up to the PNG
// extent; their entropy images keep the 512 px workspace bound below.
inline constexpr u32 kWebpLosslessDimensionMaximum = 2048;
inline constexpr u32 kWebpLosslessPixelMaximum = kWebpLosslessDimensionMaximum * kWebpLosslessDimensionMaximum;
inline constexpr u32 kVp8lHuffmanGroupCapacity = 256;
inline constexpr u32 kVp8lHuffmanSymbolCapacity = 65'536;
inline constexpr u32 kVp8lTransformPixelCapacity =
    (kWebpDimensionMaximum / 4) * (kWebpDimensionMaximum / 4);
inline constexpr u32 kVp8lMaximumAlphabetSize = 2'328;
inline constexpr u32 kVp8lColorCacheCapacity = 2'048;
// Root lookup tables of the Huffman codes of one entropy image: up to 2^8
// entries per code; codes past the capacity decode canonically.
inline constexpr u32 kVp8lLookupBits = 8;
inline constexpr u32 kVp8lLookupCapacity = 16'384;

enum class WebpCodec : u8 {
    none,
    vp8_lossy,
    vp8l_lossless,
};

enum class WebpError : u8 {
    none,
    invalid_input,
    encoded_limit,
    truncated,
    riff,
    chunk,
    duplicate_chunk,
    unsupported_lossy,
    unsupported_animation,
    unsupported_feature,
    dimensions,
    output_capacity,
    bitstream,
    huffman,
    resource_limit,
    distance,
    transform,
};

struct WebpImage {
    ConstSlice<u8> bitstream;
    ConstSlice<u8> alpha_bitstream;
    u32 width;
    u32 height;
    u32 encoded_bytes;
    WebpCodec codec;
    bool has_alpha;
    bool has_color_profile;
    u8 reserved;
};

struct WebpParseResult {
    u32 chunks;
    WebpError error;

    constexpr bool ok() const noexcept {
        return error == WebpError::none;
    }
};

struct Vp8lHuffmanCode {
    u32 symbol_offset;
    u32 table_offset;
    u16 counts[16];
    u16 symbol_count;
    u16 single_symbol;
    u8 table_bits; // 0: no lookup table
    u8 reserved[3];
};

struct Vp8lHuffmanGroup {
    Vp8lHuffmanCode codes[5];
};

struct Vp8lWorkspace {
    Vp8lHuffmanGroup groups[kVp8lHuffmanGroupCapacity];
    u16 symbols[kVp8lHuffmanSymbolCapacity];
    u16 entropy_codes[kVp8lTransformPixelCapacity];
    u8 predictor_modes[kVp8lTransformPixelCapacity];
    u32 color_elements[kVp8lTransformPixelCapacity];
    u32 subimage_pixels[kVp8lTransformPixelCapacity];
    u32 color_cache[kVp8lColorCacheCapacity];
    u8 code_lengths[kVp8lMaximumAlphabetSize];
    u32 symbol_count;
    u32 lookup_count;
    u16 lookup[kVp8lLookupCapacity];
};

struct WebpDecodeResult {
    WebpParseResult parse;
    u32 rgba_bytes;
    u32 huffman_symbols;
    WebpError error;

    constexpr bool ok() const noexcept {
        return error == WebpError::none;
    }
};

WebpParseResult webp_parse(ConstSlice<u8> encoded,
                           WebpImage* image) noexcept;
WebpDecodeResult webp_lossless_decode_rgba8(
    ConstSlice<u8> encoded, WebpImage* image,
    Vp8lWorkspace* workspace, Slice<u8> rgba_output) noexcept;
// Decodes one VP8L chunk payload from an already validated container frame.
// The payload begins with the VP8L signature byte and dimensions. Expected
// dimensions prevent a frame header from disagreeing with its bitstream.
WebpDecodeResult webp_lossless_decode_vp8l_frame_rgba8(
    ConstSlice<u8> vp8l_payload, u32 expected_width,
    u32 expected_height, WebpImage* image,
    Vp8lWorkspace* workspace, Slice<u8> rgba_output) noexcept;
// Decodes the headerless VP8L image stream used by an external ALPH chunk.
// Dimensions are supplied by the enclosing VP8 frame. The normal lossless
// transform/entropy bounds and exact trailing-bit policy still apply. Output is
// packed 0xAARRGGBB in aligned width*height*4-byte storage: ALPH uses only
// green, so it needs no R/B swap.
WebpDecodeResult webp_lossless_decode_image_stream_argb32(
    ConstSlice<u8> bitstream, u32 width, u32 height,
    WebpImage* image, Vp8lWorkspace* workspace,
    Slice<u8> pixel_output) noexcept;

static_assert(sizeof(WebpImage) == 48);

} // namespace maicord
