#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/text/opentype/sfnt.hpp"

// Colour bitmap glyphs: the CBLC (locations) and CBDT (data) tables, the emoji format of Noto Color Emoji as
// Debian, Ubuntu and nixpkgs ship it (Fedora 43 and later ship COLRv1 instead, which color_font.hpp reads). The
// Linux text path uses it; the Windows build does not compile it (Segoe UI Emoji is COLR).
//
// A font holds strikes, one per size, each with PNG images for the glyphs it covers: image formats 17 (PNG with
// small metrics), 18 (PNG with big metrics) and 19 (PNG, metrics in the index) through index subtables 1 to 5.
// A strike is chosen by the size the glyph is drawn at, and its bitmap is scaled to that size (bitmap_color_glyph_build,
// which uses the in-house PNG decoder) into the premultiplied pixels the colour glyph path of the renderer places in
// its atlas.

namespace maicord {

using i8 = signed char;
struct CbdtFont {
    ConstSlice<u8> locations; // CBLC
    ConstSlice<u8> data;      // CBDT
    u32 strike_count;
};

// One strike (a BitmapSize record of CBLC).
struct CbdtStrike {
    u16 first_glyph;
    u16 last_glyph;
    u8 ppem_x;
    u8 ppem_y;
    u8 bit_depth;
    i8 ascender; // the strike's horizontal line metrics, in its own pixels
    i8 descender;
    u32 subtable_count;
    u32 subtable_array_offset;
};

// A glyph's image in a strike: the PNG bytes and the metrics of the bitmap in the strike's pixels.
struct CbdtGlyph {
    ConstSlice<u8> png;
    u8 width;
    u8 height;
    i8 bearing_x; // from the origin to the left edge
    i8 bearing_y; // from the baseline up to the top edge
    u8 advance;
};

// Opens CBLC and CBDT (version 3.x, at least one strike). missing_table when the face has no colour bitmaps.
SfntStatus cbdt_open(const SfntFace& face, CbdtFont* output) noexcept;
SfntStatus cbdt_strike(const CbdtFont& font, u32 index, CbdtStrike* output) noexcept;
// The image of `glyph` in strike `strike`; glyph_out_of_range when that strike has none for it, unsupported for an
// image format other than 17, 18 and 19.
SfntStatus cbdt_glyph(const CbdtFont& font, u32 strike, u16 glyph, CbdtGlyph* output) noexcept;
// The strike to draw `glyph` with at `pixels_per_em`: the smallest strike that is at least that size (it is scaled
// down), else the largest (scaled up), skipping the strikes that lack the glyph. glyph_out_of_range when none has it.
SfntStatus cbdt_select_strike(const CbdtFont& font, float pixels_per_em, u16 glyph, u32* strike) noexcept;

// A glyph drawn at a size: premultiplied RGBA8 pixels (red in the low byte), the tile's top-left corner relative to
// the glyph origin in device pixels (y grows down, so `top` is negative above the baseline) and the advance.
struct BitmapColorGlyph {
    u32* pixels;
    u32 width;
    u32 height;
    i32 left;
    i32 top;
    float advance;
};

// The largest tile bitmap_color_glyph_build produces on a side; a bigger glyph (a huge size) is refused.
inline constexpr u32 kBitmapColorGlyphExtentMaximum = 1024;

// Chooses the strike, decodes its PNG and scales it by box averaging (bilinear when enlarging) to `pixels_per_em`.
// The pixels, and the scratch of the decode, come from `arena`, which the caller rewinds when the tile is consumed.
// unsupported for an image the PNG decoder cannot read or a glyph beyond the tile limit, malformed for corrupt data.
SfntStatus bitmap_color_glyph_build(const CbdtFont& font, u16 glyph, float pixels_per_em, Arena* arena,
                                    BitmapColorGlyph* output) noexcept;

} // namespace maicord
