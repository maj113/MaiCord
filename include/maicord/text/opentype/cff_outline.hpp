#pragma once

#include "maicord/text/opentype/sfnt.hpp"
#include "maicord/text/opentype/outline.hpp"
#if !defined(_WIN32)
#include "maicord/text/opentype/variation_store.hpp"
#endif

namespace maicord {

struct CffIndex {
    ConstSlice<u8> offsets;
    ConstSlice<u8> objects;
    u32 count;
    u8 offset_size;
};

struct CffFont {
    ConstSlice<u8> table;
    CffIndex charstrings;
    CffIndex global_subrs;
    CffIndex local_subrs;
    CffIndex font_dicts;
    ConstSlice<u8> fd_select;
    double matrix[6];
    double nominal_width;
    u32 charset;
    u32 random_seed;
    u16 glyph_count;
    u16 units_per_em;
    bool cid;
    // The Top DICT names its own FontMatrix. Without one a Font DICT matrix applies alone (Unifont).
    bool top_matrix;
#if !defined(_WIN32)
    // CFF2 (Linux only: GDI does not select CFF2 faces): the charstrings carry no widths and no endchar, and
    // blend takes its deltas from `variations`. Noto Sans CJK on NixOS and Fedora is a CFF2 collection.
    bool cff2;
    ItemVariationStore variations;
#endif
};

// CFF1 / Type 2, without raster hinting. Views borrow the immutable font map.
// Font handles must come from cff_open, remain unmodified and outlive decoding.
// On Linux, cff_open also reads a CFF2 table when the face has no CFF table.
SfntStatus cff_open(const SfntFace& face, CffFont* font) noexcept;
// Oblique emission supports only a uniform positive FontMatrix. It transforms the ink and bounds only: hmtx gives
// the advance.
SfntStatus cff_outline(const CffFont& font, u16 glyph, GlyphOutline* output,
                        u32 work_limit = 1000000, bool oblique = false) noexcept;
#if !defined(_WIN32)
// A CFF2 glyph at the normalized axis coordinates (post-avar, in fvar order; empty for the default instance).
// cff_outline draws a CFF2 glyph at the default instance.
SfntStatus cff2_outline(const CffFont& font, u16 glyph, GlyphOutline* output, ConstSlice<double> coordinates,
                        u32 work_limit = 1000000, bool oblique = false) noexcept;
#endif
// DirectWrite's bold simulation of a CFF or CFF2 face, on an outline cff_outline or cff2_outline drew (after its oblique):
// a square pen of 1310/65536 em. Each edge of the point polygons, cubic control points included, moves right and up by
// the pen corner its outward normal selects. Each point moves to the meeting of its two edges, at most one pen width on
// each axis. The bounds follow the moved points. The advance widening is the caller's.
void cff_bold(GlyphOutline* outline) noexcept;
// Whether DirectWrite widens the bold advance of the glyph: its control box has width and height. A glyph that draws
// only a horizontal or vertical line keeps its advance. The coordinates are a CFF2 face's instance, as for
// cff2_outline (Windows ignores them). Most glyphs answer after their first three path commands.
SfntStatus cff_glyph_ink(const CffFont& font, u16 glyph, ConstSlice<double> coordinates, bool* ink,
                         u32 work_limit = 1000000) noexcept;

} // namespace maicord
