#pragma once
// Reads a cluster map back for tests: the first physical glyph index per source cluster and, optionally, one byte per
// glyph marking that first occurrence (not DirectWrite isClusterStart, which is separate shaping policy). Glyph records
// begin with SfntClusterGlyph; extra metadata is ignored. Cluster membership may interleave in glyph order (a
// decomposed ligature followed by its component marks); every source cluster must have a glyph, else invalid_input.
// Outputs are valid only on success.
#include "maicord/core/byte_order.hpp"
#include "maicord/text/shaping/clusters.hpp"

namespace maicord {

inline SfntStatus sfnt_cluster_map_export(const SfntClusterMap& map, SfntGlyphView glyphs, Slice<u32> characters,
                                          Slice<u8> starts = {}, u64 work = 1000000) noexcept {
    constexpr u32 absent = 0xffffffffu;
    if (!sfnt_glyph_view_valid(glyphs) || glyphs.record_size < sizeof(SfntClusterGlyph) ||
        (characters.count && !characters.data) || (starts.count && !starts.data))
        return SfntStatus::invalid_input;
    const u32 n = sfnt_glyph_count(glyphs);
    if (characters.count < map.text_count || (starts.count && starts.count < n))
        return SfntStatus::output_too_small;
    const u64 required = static_cast<u64>(map.text_count) * 2 + static_cast<u64>(n) * (map.levels * 2 + 3);
    if (required > work)
        return SfntStatus::resource_limit;
    for (u32 i = 0; i < map.text_count; ++i)
        characters.data[i] = absent;
    for (u32 i = 0; i < n; ++i) {
        const auto* record = reinterpret_cast<const u8*>(&sfnt_glyph_at(glyphs, i));
        const u32 source = byte_load_le32(record + 4);
        if (source >= map.text_count)
            return SfntStatus::invalid_input;
        const u32 root = sfnt_cluster_map_start(map, source);
        if (root == absent)
            return SfntStatus::invalid_input;
        const bool first = characters.data[root] == absent;
        if (first)
            characters.data[root] = i;
        if (starts.count)
            starts.data[i] = first ? 1u : 0u;
    }
    u32 glyph = absent;
    for (u32 i = 0; i < map.text_count; ++i) {
        if (sfnt_cluster_map_starts(map, i))
            glyph = characters.data[i];
        if (glyph == absent)
            return SfntStatus::invalid_input;
        characters.data[i] = glyph;
    }
    return SfntStatus::ok;
}

} // namespace maicord
