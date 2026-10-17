#pragma once

#include "maicord/text/sfnt.hpp"

namespace maicord {

struct CpalColor { u8 red, green, blue, alpha; };
struct CpalFont {
    ConstSlice<u8> colors, starts;
    u16 version, palette_count, entry_count;
};

SfntStatus cpal_open(const SfntFace& face, CpalFont* output) noexcept;
SfntStatus cpal_color(const CpalFont& font, u16 palette, u16 entry, CpalColor* output) noexcept;

struct ColrFont {
    CpalFont palette;
    ConstSlice<u8> table, bases_v0, layers_v0, bases_v1, layers_v1, clips;
    u32 base_list, layer_list, clip_list, variation_map, variation_store;
    u16 version, glyph_count;
};
struct ColrLegacyLayer { u16 glyph, palette; };
struct ColrLayerRange { u32 first, count; };
struct ColrClip {
    double minimum_x, minimum_y, maximum_x, maximum_y;
    u32 variation_index;
    bool present, variable;
};

// Lossless decoded table values, not a resolved Windows paint element.
// Values follow the field order in the OpenType paint format, with F2DOT14
// and Fixed converted exactly to doubles. Angles retain their encoded units;
// sweep angle bias, transforms, palette/alpha and variation resolution follow
// in the paint compiler. For composite, child[0] is source, child[1] backdrop.
struct ColrPaint {
    double values[6];
    u32 offset, child[2], color_line, first_layer, variation_index;
    u16 glyph, palette;
    u8 format, mode, layer_count, value_count;
    bool variable;
};
struct ColrColorLine {
    ConstSlice<u8> stops;
    u16 count;
    u8 extend;
    bool variable;
};
struct ColrColorStop {
    double position, alpha;
    u32 variation_index;
    u16 palette;
    bool variable;
};
struct ColrValidation {
    u32 nodes, stops, formats, maximum_depth;
    bool has_variations;
};

// Prepared views borrow the immutable font mapping and must not be fabricated
// or modified. Table access alone does not resolve variations or determine rendered pixels.
SfntStatus colr_open(const SfntFace& face, ColrFont* output) noexcept;
SfntStatus colr_base_paint(const ColrFont& font, u16 glyph, u32* offset) noexcept;
SfntStatus colr_legacy_layers(const ColrFont& font, u16 glyph, ColrLayerRange* range) noexcept;
SfntStatus colr_legacy_layer(const ColrFont& font, u32 index, ColrLegacyLayer* layer) noexcept;
SfntStatus colr_layer_paint(const ColrFont& font, u32 index, u32* offset) noexcept;
SfntStatus colr_clip(const ColrFont& font, u16 glyph, ColrClip* output) noexcept;
SfntStatus colr_paint_read(const ColrFont& font, u32 offset, ColrPaint* output) noexcept;
SfntStatus colr_color_line(const ColrFont& font, const ColrPaint& paint, ColrColorLine* output) noexcept;
SfntStatus colr_color_stop(const ColrFont& font, const ColrColorLine& line, u32 index, ColrColorStop* output) noexcept;
// Structural graph validation with caller-owned active-path storage, depth 64.
// Reused subgraphs are legal; active-path cycles and budget exhaustion are errors.
SfntStatus colr_validate_glyph(const ColrFont& font, u16 glyph, Slice<u32> active_path,
                              u32 work_limit, ColrValidation* output) noexcept;

} // namespace maicord
