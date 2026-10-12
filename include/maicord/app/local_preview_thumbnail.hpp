#pragma once

#include "maicord/media/gif.hpp"
#include "maicord/media/png.hpp"

namespace maicord {

// Bounded straight-RGBA thumbnails for composer upload previews. Sources are
// streamed row by row through an alpha-weighted box filter, so no full-size
// RGBA copy is ever made of large local images.
inline constexpr u32 kLocalPreviewThumbnailExtent = 400;

// Integer box factor that fits both sides into extent.
constexpr u32 local_thumbnail_scale(u32 width, u32 height, u32 extent) noexcept {
    const u32 longest = width > height ? width : height;
    return extent && longest > extent ? (longest + extent - 1) / extent : 1;
}
constexpr u32 local_thumbnail_extent(u32 source, u32 scale) noexcept {
    return scale ? (source + scale - 1) / scale : 0;
}

struct LocalThumbnailSink {
    u8* output;
    u32* sums; // Per output column: alpha-weighted R, G, B, alpha and samples.
    u32 source_width, source_height;
    u32 scale, width, height;
    u32 row;
};
// output holds width * height * 4 bytes; sums holds width * 5 entries.
bool local_thumbnail_begin(LocalThumbnailSink* sink, u32 source_width, u32 source_height, u32 extent,
                           Slice<u8> output, Slice<u32> sums) noexcept;
// One straight RGBA8 source row; the final source row flushes the last block.
bool local_thumbnail_push_row(LocalThumbnailSink* sink, const u8* rgba) noexcept;
constexpr bool local_thumbnail_complete(const LocalThumbnailSink& sink) noexcept {
    return sink.output && sink.row == sink.source_height;
}

// Expands one reconstructed PNG row (after its filter byte) to RGBA8,
// including palette, tRNS keys and 16-bit samples.
bool local_thumbnail_png_row(const PngImage& image, const u8* row, u8* rgba) noexcept;
// Composites a decoded GIF frame (palette indices) onto a transparent canvas row.
bool local_thumbnail_gif_row(const GifFrame& frame, const GifPalette& palette, ConstSlice<u8> indices, u32 canvas_width,
                             u32 y, u8* rgba) noexcept;
// EXIF orientation 1-8 applied to a small RGBA image; scratch holds a copy.
bool local_thumbnail_orient(Slice<u8> pixels, u32* width, u32* height, u8 orientation, Slice<u8> scratch) noexcept;

} // namespace maicord
