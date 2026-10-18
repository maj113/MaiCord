#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// Message animations are not emoji-sized. Bound compressed input, canvas work
// and metadata separately; do not allocate one decoded canvas per frame.
inline constexpr u32 kGifEncodedMaximumBytes = 16u * 1024u * 1024u;
inline constexpr u32 kGifDimensionMaximum = 2048;
inline constexpr u32 kGifPixelMaximum =
    kGifDimensionMaximum * kGifDimensionMaximum;
inline constexpr u32 kGifFrameCapacity = 2048;
inline constexpr u32 kGifLzwDictionaryCapacity = 4096;
inline constexpr u64 kGifShortFrameThresholdUs = 10'000;
inline constexpr u64 kGifShortFrameReplacementUs = 100'000;

enum class GifDispose : u8 {
    none,
    background,
    previous,
};

enum GifFrameFlags : u8 {
    gif_frame_interlaced = 1u << 0,
    gif_frame_local_palette = 1u << 1,
    gif_frame_transparent = 1u << 2,
};

struct GifFrame {
    // data_offset addresses the first image-data sub-block size byte. data_end
    // is one past the terminating zero-size sub-block.
    u32 data_offset;
    u32 data_end;
    u32 palette_offset;
    u32 compressed_bytes;
    u32 width;
    u32 height;
    u32 x_offset;
    u32 y_offset;
    u16 palette_count;
    u16 delay_cs;
    u8 minimum_code_size;
    u8 transparent_index;
    GifDispose dispose;
    u8 flags;
};

// LZW state lives on the decoding thread's stack (24 KiB per frame decode).
struct GifWorkspace {
    GifFrame frames[kGifFrameCapacity];
};

struct GifImage {
    GifFrame* frames;
    u32 frame_capacity;
    u32 frame_count;
    u32 play_count;
    u32 canvas_width;
    u32 canvas_height;
    u32 global_palette_offset;
    u32 background_rgba;
    u16 global_palette_count;
    u8 background_index;
    bool has_global_palette;
};

enum class GifError : u8 {
    none,
    invalid_input,
    encoded_limit,
    truncated,
    signature,
    version,
    dimensions,
    block,
    block_order,
    reserved_bits,
    palette,
    frame_limit,
    frame_rectangle,
    graphic_control,
    application_extension,
    duplicate_loop_extension,
    image_data,
    output_capacity,
    lzw_code_size,
    lzw_missing_clear,
    lzw_invalid_code,
    lzw_dictionary,
    lzw_pixel_count,
    lzw_missing_end,
};

struct GifParseResult {
    u32 blocks;
    u32 extensions;
    u32 data_subblocks;
    GifError error;

    constexpr bool ok() const noexcept {
        return error == GifError::none;
    }
};

struct GifPalette {
    u32 rgba[256];
    u32 background_rgba;
    u16 count;
    u8 transparent_index;
    bool has_transparency;
};

struct GifDecodeResult {
    GifParseResult parse;
    GifFrame frame;
    GifPalette palette;
    u64 duration_us;
    u32 frame_index;
    u32 pixels;
    GifError error;

    constexpr bool ok() const noexcept {
        return error == GifError::none;
    }
};

GifParseResult gif_parse(ConstSlice<u8> encoded, GifImage* image,
                         GifWorkspace* workspace) noexcept;
GifDecodeResult gif_decode_parsed_frame(
    ConstSlice<u8> encoded, const GifImage& image, u32 frame_index,
    GifWorkspace* workspace, Slice<u8> indices) noexcept;
GifDecodeResult gif_decode_frame(
    ConstSlice<u8> encoded, u32 frame_index, GifImage* image,
    GifWorkspace* workspace, Slice<u8> indices) noexcept;

static_assert(sizeof(GifFrame) == 40);
static_assert(sizeof(GifWorkspace) == 81'920);
static_assert(sizeof(GifImage) == 40);

} // namespace maicord
