#pragma once

#include "maicord/media/apng_frame.hpp"
#include "maicord/media/png.hpp"

namespace maicord {

inline constexpr u32 kApngFrameCapacity = 256;
inline constexpr u32 kApngDataSpanCapacity = 512;
inline constexpr u32 kApngFrameDataSpanCapacity = 64;
// A retained parse can use the unused tail. Larger span tables use the full array.
inline constexpr u32 kApngRetainedMetadataBytes = 64;
inline constexpr u32 kApngRetainedSpanCapacity = kApngDataSpanCapacity - kApngRetainedMetadataBytes / 8;
// Valid encoded offsets are at most kPngEncodedMaximumBytes.
inline constexpr u32 kApngRetainedMarker = 0xffffffffu;
inline constexpr u64 kApngCompatibilityFrameFloorUs = 10'000;
inline constexpr u64 kApngFrameDurationMaximumUs =
    24ull * 60ull * 60ull * 1'000'000ull;

struct ApngDataSpan {
    u32 offset;
    u32 bytes;
};

struct ApngWorkspace {
    ApngFrame frames[kApngFrameCapacity];
    ApngDataSpan spans[kApngDataSpanCapacity];
};

struct ApngImage {
    PngImage canvas;
    ApngFrame* frames;
    ApngDataSpan* spans;
    u32 frame_capacity;
    u32 span_capacity;
    u32 frame_count;
    u32 span_count;
    u32 play_count;
    u32 default_first_span;
    u32 default_compressed_bytes;
    u16 default_span_count;
    bool default_in_animation;
    u8 reserved;
};

enum class ApngError : u8 {
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
    missing_animation,
    unsupported_interlace,
    unsupported_format,
    dimensions,
    palette,
    transparency,
    frame_limit,
    span_limit,
    frame_control,
    frame_sequence,
    frame_data,
    inflate,
};

struct ApngParseResult {
    u32 chunks;
    u32 sequence_chunks;
    ApngError error;

    constexpr bool ok() const noexcept {
        return error == ApngError::none;
    }
};

struct ApngDecodeResult {
    ApngParseResult parse;
    PngDecodeResult png;
    ApngFrame frame;
    u64 duration_us;
    u32 frame_index;
    ApngError error;

    constexpr bool ok() const noexcept {
        return error == ApngError::none;
    }
};

ApngParseResult apng_parse(ConstSlice<u8> encoded, ApngImage* image,
                           ApngWorkspace* workspace) noexcept;
ApngDecodeResult apng_decode_parsed_frame(
    ConstSlice<u8> encoded, const ApngImage& image, u32 frame_index,
    DeflateWorkspace* deflate_workspace, PngImage* frame_image,
    Slice<u8> scanlines) noexcept;
PngDecodeResult apng_decode_parsed_default(
    ConstSlice<u8> encoded, const ApngImage& image,
    DeflateWorkspace* deflate_workspace, PngImage* default_image,
    Slice<u8> scanlines) noexcept;

static_assert(sizeof(ApngDataSpan) == 8);
static_assert(sizeof(ApngWorkspace) == 12'288);
static_assert(sizeof(ApngImage) == 2'152);

} // namespace maicord
