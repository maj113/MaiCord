#pragma once

#include "maicord/media/image/gif.hpp"
#include "maicord/media/image/png.hpp"

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
// output holds width * height * 4 bytes; sums holds width * 5 entries. output, sums and every
// pushed source row must be disjoint.
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

// Area-average fit for media the CDN cannot size (profile effect layers): a canvas reduced, aspect kept, until
// it fits a box. Every output texel averages exactly the source area it covers (fractional at its edges, in
// 1/256 texel steps), colour weighted by alpha, so fine detail does not alias at any ratio. An animation frame
// is a region of its canvas: it lands on the fitted canvas's cells, keeping its offset.
struct MediaFit {
    u32 source_width, source_height; // the canvas
    u32 width, height;               // the fitted canvas
};
// False when the canvas already fits the box (nothing to do) or a side is zero.
bool media_fit_plan(u32 source_width, u32 source_height, u32 box_width, u32 box_height, MediaFit* fit) noexcept;
struct MediaFitRegion {
    u32 x, y, width, height;
};
// The fitted cells a source region of the canvas touches.
MediaFitRegion media_fit_region(const MediaFit& fit, u32 x, u32 y, u32 width, u32 height) noexcept;
struct MediaFitSink {
    MediaFit fit;
    MediaFitRegion source, region;
    u8* output;
    u32 stride, lead;
    u64* sums;     // two fitted rows: weighted alpha, then alpha-weighted R, G, B per cell
    u32* columns;  // per source column: its cell (region-relative) << 9 | its weight in that cell (1..256)
    u32 row;       // source rows pushed
    u32 cell_row;  // the fitted row sums[0] holds
};
// Scratch media_fit_begin needs for a source region `width` wide fitted to `cells` columns.
constexpr u64 media_fit_scratch_bytes(u32 width, u32 cells) noexcept {
    return u64{cells} * 8u * sizeof(u64) + u64{width} * sizeof(u32);
}
// output: region.height rows of `stride` bytes, each written as straight RGBA8 from byte `lead` (1 leaves a
// PNG filter byte, which the caller clears). The source region must lie in the canvas.
bool media_fit_begin(MediaFitSink* sink, const MediaFit& fit, MediaFitRegion source, u8* output, u32 stride,
                     u32 lead, Slice<u8> scratch) noexcept;
// The next source row of the region: source.width straight RGBA8 texels. The last one writes every cell left.
bool media_fit_push_row(MediaFitSink* sink, const u8* rgba) noexcept;

} // namespace maicord
