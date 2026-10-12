#include "maicord/text/sfnt.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {
namespace {

inline bool span(ConstSlice<u8> bytes, u32 offset, u32 length) noexcept {
    return bytes.data && offset <= bytes.count && length <= bytes.count - offset;
}

inline ConstSlice<u8> view(ConstSlice<u8> bytes, u32 offset, u32 length) noexcept {
    return {bytes.data + offset, length};
}

inline u16 read16(ConstSlice<u8> bytes, u32 offset) noexcept {
    return byte_load_be16(bytes.data + offset);
}

inline u32 read32(ConstSlice<u8> bytes, u32 offset) noexcept {
    return byte_load_be32(bytes.data + offset);
}

} // namespace

SfntStatus sfnt_open(ConstSlice<u8> bytes, u32 face_index, SfntFace* face) noexcept {
    if (!face) return SfntStatus::invalid_input;
    *face = {};
    if (!bytes.data) return SfntStatus::invalid_input;
    if (!span(bytes, 0, 12)) return SfntStatus::truncated;
    u32 offset = 0, faces = 1;
    if (read32(bytes, 0) == sfnt_tag('t', 't', 'c', 'f')) {
        const u32 version = read32(bytes, 4);
        if (version != 0x00010000u && version != 0x00020000u)
            return SfntStatus::unsupported;
        faces = read32(bytes, 8);
        if (!faces) return SfntStatus::malformed;
        if (faces > (bytes.count - 12u) / 4u) return SfntStatus::truncated;
        if (version == 0x00020000u && !span(bytes, 12u + faces * 4u, 12))
            return SfntStatus::truncated;
        if (face_index >= faces) return SfntStatus::face_out_of_range;
        offset = read32(bytes, 12u + face_index * 4u);
        if (!span(bytes, offset, 12)) return SfntStatus::truncated;
    } else if (face_index) {
        return SfntStatus::face_out_of_range;
    }
    const u32 version = read32(bytes, offset);
    if (version != 0x00010000u && version != sfnt_tag('O', 'T', 'T', 'O') &&
        version != sfnt_tag('t', 'r', 'u', 'e')) return SfntStatus::unsupported;
    const u32 count = read16(bytes, offset + 4);
    if (!count) return SfntStatus::malformed;
    // Ignore the untrusted searchRange/entrySelector/rangeShift hints.
    if (!span(bytes, offset + 12, count * 16)) return SfntStatus::truncated;
    const auto directory = view(bytes, offset + 12, count * 16);
    u32 previous = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 at = i * 16, tag = read32(directory, at);
        if (i && tag <= previous) return SfntStatus::malformed;
        previous = tag;
        // Collection offsets are absolute from the TTC file, not the face.
        if (!span(bytes, read32(directory, at + 8), read32(directory, at + 12)))
            return SfntStatus::truncated;
    }
    *face = {bytes, directory, face_index, faces, version};
    return SfntStatus::ok;
}

SfntStatus sfnt_table(const SfntFace& face, u32 tag, ConstSlice<u8>* table) noexcept {
    if (!table) return SfntStatus::invalid_input;
    *table = {};
    if (!face.file.data || !face.directory.data || !face.directory.count ||
        face.directory.count % 16u) return SfntStatus::invalid_input;
    u32 low = 0, high = face.directory.count / 16u;
    while (low < high) {
        const u32 middle = low + (high - low) / 2;
        const u32 found = read32(face.directory, middle * 16);
        if (found < tag) low = middle + 1;
        else if (found > tag) high = middle;
        else {
            const u32 offset = read32(face.directory, middle * 16 + 8);
            const u32 length = read32(face.directory, middle * 16 + 12);
            if (!span(face.file, offset, length)) return SfntStatus::truncated;
            *table = view(face.file, offset, length);
            return SfntStatus::ok;
        }
    }
    return SfntStatus::missing_table;
}

SfntStatus sfnt_glyph_count(const SfntFace& face, u16* count) noexcept {
    if (!count) return SfntStatus::invalid_input;
    *count = 0;
    ConstSlice<u8> maxp{};
    const auto status = sfnt_table(face, sfnt_tag('m', 'a', 'x', 'p'), &maxp);
    if (status != SfntStatus::ok) return status;
    if (!span(maxp, 0, 6)) return SfntStatus::truncated;
    const u32 version = read32(maxp, 0);
    if (version != 0x00005000u && version != 0x00010000u) return SfntStatus::unsupported;
    if (version == 0x00010000u && maxp.count < 32) return SfntStatus::truncated;
    if (!read16(maxp, 4)) return SfntStatus::malformed;
    *count = read16(maxp, 4);
    return SfntStatus::ok;
}

SfntStatus sfnt_horizontal_metrics_open(const SfntFace& face,
                                         SfntHorizontalMetrics* metrics) noexcept {
    if (!metrics) return SfntStatus::invalid_input;
    *metrics = {};
    u16 glyphs = 0;
    auto status = sfnt_glyph_count(face, &glyphs);
    if (status != SfntStatus::ok) return status;
    ConstSlice<u8> head{}, hhea{}, hmtx{};
    status = sfnt_table(face, sfnt_tag('h', 'e', 'a', 'd'), &head);
    if (status != SfntStatus::ok) return status;
    status = sfnt_table(face, sfnt_tag('h', 'h', 'e', 'a'), &hhea);
    if (status != SfntStatus::ok) return status;
    status = sfnt_table(face, sfnt_tag('h', 'm', 't', 'x'), &hmtx);
    if (status != SfntStatus::ok) return status;
    if (!span(head, 0, 54) || !span(hhea, 0, 36)) return SfntStatus::truncated;
    if (read16(head, 0) != 1 || read16(hhea, 0) != 1) return SfntStatus::unsupported;
    const u16 units = read16(head, 18), longs = read16(hhea, 34);
    if (read32(head, 12) != 0x5f0f3cf5u || units < 16 || units > 16384 ||
        !longs || longs > glyphs || read16(hhea, 32) != 0) return SfntStatus::malformed;
    if (hmtx.count < static_cast<u32>(longs) * 4u + (glyphs - longs) * 2u)
        return SfntStatus::truncated;
    *metrics = {hmtx, glyphs, longs, units, static_cast<i16>(read16(hhea, 4)),
               static_cast<i16>(read16(hhea, 6)), static_cast<i16>(read16(hhea, 8))};
    return SfntStatus::ok;
}

SfntStatus sfnt_horizontal_metrics(const SfntHorizontalMetrics& metrics, u16 glyph,
                                    SfntGlyphMetrics* output) noexcept {
    if (!output) return SfntStatus::invalid_input;
    *output = {};
    if (!metrics.table.data || !metrics.long_metrics ||
        metrics.long_metrics > metrics.glyph_count) return SfntStatus::invalid_input;
    if (glyph >= metrics.glyph_count) return SfntStatus::glyph_out_of_range;
    const u32 advance = minimum<u32>(glyph, metrics.long_metrics - 1u) * 4u;
    const u32 bearing = glyph < metrics.long_metrics ? glyph * 4u + 2u
        : metrics.long_metrics * 4u + (glyph - metrics.long_metrics) * 2u;
    if (!span(metrics.table, advance, 2) || !span(metrics.table, bearing, 2))
        return SfntStatus::truncated;
    *output = {read16(metrics.table, advance), static_cast<i16>(read16(metrics.table, bearing))};
    return SfntStatus::ok;
}

} // namespace maicord
