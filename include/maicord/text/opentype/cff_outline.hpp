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
    double default_width, nominal_width;
    u32 charset;
    u32 random_seed;
    u16 glyph_count;
    u16 units_per_em;
    bool cid;
#if !defined(_WIN32)
    // CFF2 (Linux only: GDI does not select CFF2 faces): the charstrings carry no widths and no endchar, and
    // blend takes its deltas from `variations`. Noto Sans CJK on NixOS and Fedora is a CFF2 collection.
    bool cff2;
    ItemVariationStore variations;
#endif
};

// Resolved charstring advance and control box in head design units, Y up.
// Keep these separate from raw hmtx and from the normalized float path bounds.
struct CffGlyphMetrics {
    double advance_x, advance_y;
    double minimum_x, minimum_y, maximum_x, maximum_y;
    bool has_bounds;
};

// CFF1 / Type 2, without raster hinting. Views borrow the immutable font map.
// Font handles must come from cff_open, remain unmodified and outlive decoding.
// On Linux, cff_open also reads a CFF2 table when the face has no CFF table.
SfntStatus cff_open(const SfntFace& face, CffFont* font) noexcept;
// Oblique emission supports only a uniform positive FontMatrix.
// It preserves base charstring metrics and transforms output ink/bounds only.
SfntStatus cff_outline(const CffFont& font, u16 glyph, GlyphOutline* output,
                        u32 work_limit = 1000000, CffGlyphMetrics* metrics = nullptr,bool oblique=false) noexcept;
#if !defined(_WIN32)
// A CFF2 glyph at the normalized axis coordinates (post-avar, in fvar order; empty for the default instance).
// cff_outline draws a CFF2 glyph at the default instance.
SfntStatus cff2_outline(const CffFont& font, u16 glyph, GlyphOutline* output, ConstSlice<double> coordinates,
                        u32 work_limit = 1000000, bool oblique = false) noexcept;
#endif

} // namespace maicord
