#pragma once
// Position flags for tests that drive GPOS directly: projects the current source-cluster transitions and the glyph
// classes into natural-horizontal flags (text/opentype/gpos_execute.hpp). Interleaved membership can start the same
// source cluster more than once in glyph order. Records begin with SfntClusterGlyph; extra metadata is ignored. Zero
// advance follows mark classification. Outputs are valid only on success.
#include "maicord/core/byte_order.hpp"
#include "maicord/text/opentype/gpos_execute.hpp"

namespace maicord {

inline SfntStatus sfnt_position_flags_from_clusters(const SfntClusterMap& clusters, SfntGlyphView glyphs,
                                                    Slice<u8> output) noexcept {
    if (!sfnt_glyph_view_valid(glyphs) || glyphs.record_size < sizeof(SfntClusterGlyph) ||
        (output.count && !output.data))
        return SfntStatus::invalid_input;
    const u32 count = sfnt_glyph_count(glyphs);
    if (output.count < count)
        return SfntStatus::output_too_small;
    u32 first = 0xffffffffu, end = 0, previous = 0xffffffffu;
    for (u32 i = 0; i < count; ++i) {
        const auto& glyph = sfnt_glyph_at(glyphs, i);
        const u32 source = byte_load_le32(reinterpret_cast<const u8*>(&glyph) + 4);
        if (source >= clusters.text_count || static_cast<u8>(glyph.glyph_class) > 4)
            return SfntStatus::invalid_input;
        if (source < first || source >= end) {
            const auto status = sfnt_cluster_map_find(clusters, source, &first, &end);
            if (status != SfntStatus::ok)
                return status;
        }
        const bool zero_advance = glyph.glyph_class == SfntGlyphClass::mark;
        output.data[i] = static_cast<u8>((first != previous ? sfnt_position_cluster_start : 0) |
                                         (zero_advance ? sfnt_position_zero_advance : 0));
        previous = first;
    }
    return SfntStatus::ok;
}

} // namespace maicord
