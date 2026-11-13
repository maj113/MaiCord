#pragma once
// JPEG decoding for tests: the product's parse and coefficient decode in one call (the client runs them as separate
// steps, src/app/media/cdn_media_pipeline.cpp), and a block read back from the coefficient workspace as the GPU reads
// it (src/render/shaders/jpeg.hlsl).
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/image/jpeg.hpp"

namespace maicord {

inline JpegDecodeResult jpeg_decode_coefficients(ConstSlice<u8> encoded, JpegImage* image,
                                                 JpegWorkspace* workspace) noexcept {
    JpegDecodeResult result{};
    result.parse = jpeg_parse(encoded, image, workspace);
    if (!result.parse.ok()) {
        result.error = result.parse.error;
        return result;
    }
    const JpegParseResult parse = result.parse;
    result = jpeg_decode_parsed_coefficients(encoded, *image, &workspace->coefficients);
    result.parse = parse;
    return result;
}

// Block `block` of a decode of `blocks` blocks in raster order, its unstored rows zero.
inline JpegCoefficientBlock jpeg_coefficient_block(const JpegCoefficientWorkspace& coefficients, u32 blocks,
                                                   u32 block) noexcept {
    JpegCoefficientBlock result{};
    const u8* const stored = coefficients.bytes + jpeg_coefficient_row_offset(blocks);
    if (coefficients.whole_blocks) {
        copy_bytes(&result, stored + block * sizeof(JpegCoefficientBlock), static_cast<u32>(sizeof(result)));
        return result;
    }
    const u32 word = byte_load_le32(coefficients.bytes + block * 4u);
    const u32 rows = word & 15u;
    copy_bytes(&result, stored + (word >> 8) * 16u, (rows < 8u ? rows : 8u) * 16u);
    return result;
}

} // namespace maicord
