#pragma once

#include "maicord/text/sfnt.hpp"
#include "maicord/text/outline.hpp"

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
// CFF2 variation programs are a separate, still unsupported provider.
SfntStatus cff_open(const SfntFace& face, CffFont* font) noexcept;
// Bounded metrics-only decode; no path buffer or allocation. Same unsimulated
// design metrics as cff_outline, including flex/composite resolution.
SfntStatus cff_glyph_metrics(const CffFont& font,u16 glyph,CffGlyphMetrics* metrics,
                            u32 work_limit=1000000) noexcept;
// Oblique emission supports the uniform positive FontMatrix used by the app.
// It preserves base charstring metrics and transforms output ink/bounds only.
SfntStatus cff_outline(const CffFont& font, u16 glyph, GlyphOutline* output,
                        u32 work_limit = 1000000, CffGlyphMetrics* metrics = nullptr,bool oblique=false) noexcept;

// Optional first-party bold prototype. Current app profiles fix CFF weights;
// keeping a separate entry lets the app linker discard its code completely.
// Uniform non-CID matrices only. Reuses output storage; no allocation.
SfntStatus cff_outline_bold(const CffFont& font,u16 glyph,GlyphOutline* output,
                          u32 work_limit=1000000,CffGlyphMetrics* metrics=nullptr,bool oblique=false) noexcept;

} // namespace maicord
