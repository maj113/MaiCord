// CBLC and CBDT: strikes, index subtable formats 1 to 5, PNG image formats 17, 18 and 19. Every offset is checked
// against its table before it is read, so a corrupt font fails with a status and never reads past a table.
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/opentype/bitmap_color.hpp"

namespace maicord {
namespace {

constexpr u32 kHeaderBytes = 8;
constexpr u32 kStrikeBytes = 48;
constexpr u32 kSubtableArrayEntryBytes = 8;
constexpr u32 kSubtableHeaderBytes = 8;
constexpr u32 kSmallMetricsBytes = 5;
constexpr u32 kBigMetricsBytes = 8;
constexpr u32 kStrikeLimit = 64;

bool span(ConstSlice<u8> table, u64 offset, u64 bytes) noexcept {
    return offset <= table.count && bytes <= table.count - offset;
}

u32 be16(ConstSlice<u8> table, u32 offset) noexcept {
    return byte_load_be16(table.data + offset);
}

u32 be32(ConstSlice<u8> table, u32 offset) noexcept {
    return byte_load_be32(table.data + offset);
}

// Where one glyph's image sits in CBDT, and the metrics its index subtable supplies for format 19.
struct Location {
    u32 offset;
    u32 length; // 0 when the subtable does not say (format 2 and 5 images have a fixed size)
    u32 image_format;
    bool has_metrics;
    u8 metrics[kBigMetricsBytes];
};

// Locates `glyph` through one index subtable that covers the range [first, last].
SfntStatus locate(const CbdtFont& font, u32 subtable, u16 first, u16 last, u16 glyph, Location* output) noexcept {
    const ConstSlice<u8> table = font.locations;
    if (!span(table, subtable, kSubtableHeaderBytes))
        return SfntStatus::truncated;
    const u32 index_format = be16(table, subtable);
    const u32 image_format = be16(table, subtable + 2);
    const u32 data_offset = be32(table, subtable + 4);
    const u32 position = static_cast<u32>(glyph) - first;
    const u32 glyph_count = static_cast<u32>(last) - first + 1;
    Location location{};
    location.image_format = image_format;
    u32 start = 0;
    u32 end = 0;
    switch (index_format) {
    case 1:
    case 3: {
        const u32 width = index_format == 1 ? 4 : 2;
        // glyph_count + 1 offsets follow the header.
        if (!span(table, subtable + kSubtableHeaderBytes, static_cast<u64>(glyph_count + 1) * width))
            return SfntStatus::truncated;
        const u32 base = subtable + kSubtableHeaderBytes;
        start = width == 4 ? be32(table, base + position * 4) : be16(table, base + position * 2);
        end = width == 4 ? be32(table, base + (position + 1) * 4) : be16(table, base + (position + 1) * 2);
        if (end < start)
            return SfntStatus::malformed;
        if (end == start)
            return SfntStatus::glyph_out_of_range;
        location.length = end - start;
        break;
    }
    case 2: {
        if (!span(table, subtable + kSubtableHeaderBytes, 4 + kBigMetricsBytes))
            return SfntStatus::truncated;
        const u32 image_size = be32(table, subtable + kSubtableHeaderBytes);
        if (image_size == 0)
            return SfntStatus::malformed;
        const u64 where = static_cast<u64>(position) * image_size;
        if (where > 0xFFFFFFFFull)
            return SfntStatus::malformed;
        start = static_cast<u32>(where);
        location.length = image_size;
        copy_bytes(location.metrics, table.data + subtable + kSubtableHeaderBytes + 4, kBigMetricsBytes);
        location.has_metrics = true;
        break;
    }
    case 4: {
        if (!span(table, subtable + kSubtableHeaderBytes, 4))
            return SfntStatus::truncated;
        const u32 count = be32(table, subtable + kSubtableHeaderBytes);
        const u32 pairs = subtable + kSubtableHeaderBytes + 4;
        // count glyph/offset pairs and a final one that only carries the end offset.
        if (count > 0xFFFF || !span(table, pairs, static_cast<u64>(count + 1) * 4))
            return SfntStatus::truncated;
        bool found = false;
        for (u32 i = 0; i < count && !found; ++i) {
            if (be16(table, pairs + i * 4) != glyph)
                continue;
            start = be16(table, pairs + i * 4 + 2);
            end = be16(table, pairs + (i + 1) * 4 + 2);
            found = true;
        }
        if (!found)
            return SfntStatus::glyph_out_of_range;
        if (end < start)
            return SfntStatus::malformed;
        if (end == start)
            return SfntStatus::glyph_out_of_range;
        location.length = end - start;
        break;
    }
    case 5: {
        const u32 fixed = kSubtableHeaderBytes + 4 + kBigMetricsBytes + 4;
        if (!span(table, subtable + kSubtableHeaderBytes, fixed - kSubtableHeaderBytes))
            return SfntStatus::truncated;
        const u32 image_size = be32(table, subtable + kSubtableHeaderBytes);
        const u32 count = be32(table, subtable + kSubtableHeaderBytes + 4 + kBigMetricsBytes);
        if (image_size == 0 || count > 0xFFFF || !span(table, subtable + fixed, static_cast<u64>(count) * 2))
            return SfntStatus::truncated;
        bool found = false;
        u32 index = 0;
        for (; index < count && !found; ++index)
            found = be16(table, subtable + fixed + index * 2) == glyph;
        if (!found)
            return SfntStatus::glyph_out_of_range;
        --index;
        const u64 where = static_cast<u64>(index) * image_size;
        if (where > 0xFFFFFFFFull)
            return SfntStatus::malformed;
        start = static_cast<u32>(where);
        location.length = image_size;
        copy_bytes(location.metrics, table.data + subtable + kSubtableHeaderBytes + 4, kBigMetricsBytes);
        location.has_metrics = true;
        break;
    }
    default:
        return SfntStatus::unsupported;
    }
    const u64 absolute = static_cast<u64>(data_offset) + start;
    if (absolute > 0xFFFFFFFFull)
        return SfntStatus::malformed;
    location.offset = static_cast<u32>(absolute);
    *output = location;
    return SfntStatus::ok;
}

} // namespace

SfntStatus cbdt_open(const SfntFace& face, CbdtFont* output) noexcept {
    if (!output)
        return SfntStatus::invalid_input;
    reset_value(output);
    CbdtFont font{};
    SfntStatus status = sfnt_table(face, sfnt_tag('C', 'B', 'L', 'C'), &font.locations);
    if (status != SfntStatus::ok)
        return status;
    status = sfnt_table(face, sfnt_tag('C', 'B', 'D', 'T'), &font.data);
    if (status != SfntStatus::ok)
        return status;
    if (!span(font.locations, 0, kHeaderBytes) || !span(font.data, 0, 4))
        return SfntStatus::truncated;
    if (be16(font.locations, 0) != 3 || be16(font.data, 0) != 3)
        return SfntStatus::unsupported;
    font.strike_count = be32(font.locations, 4);
    if (font.strike_count == 0 || font.strike_count > kStrikeLimit)
        return SfntStatus::malformed;
    if (!span(font.locations, kHeaderBytes, static_cast<u64>(font.strike_count) * kStrikeBytes))
        return SfntStatus::truncated;
    *output = font;
    return SfntStatus::ok;
}

SfntStatus cbdt_strike(const CbdtFont& font, u32 index, CbdtStrike* output) noexcept {
    if (!output)
        return SfntStatus::invalid_input;
    reset_value(output);
    if (!font.locations.data || index >= font.strike_count)
        return SfntStatus::invalid_input;
    const u32 at = kHeaderBytes + index * kStrikeBytes;
    const ConstSlice<u8> table = font.locations;
    CbdtStrike strike{};
    strike.subtable_array_offset = be32(table, at);
    strike.subtable_count = be32(table, at + 8);
    // After the offsets, sizes and colour reference: two SbitLineMetrics of 12 bytes.
    strike.ascender = static_cast<i8>(table.data[at + 16]);
    strike.descender = static_cast<i8>(table.data[at + 17]);
    strike.first_glyph = static_cast<u16>(be16(table, at + 40));
    strike.last_glyph = static_cast<u16>(be16(table, at + 42));
    strike.ppem_x = table.data[at + 44];
    strike.ppem_y = table.data[at + 45];
    strike.bit_depth = table.data[at + 46];
    if (strike.last_glyph < strike.first_glyph || strike.ppem_y == 0)
        return SfntStatus::malformed;
    if (!span(table, strike.subtable_array_offset, static_cast<u64>(strike.subtable_count) * kSubtableArrayEntryBytes))
        return SfntStatus::truncated;
    *output = strike;
    return SfntStatus::ok;
}

SfntStatus cbdt_glyph(const CbdtFont& font, u32 strike_index, u16 glyph, CbdtGlyph* output) noexcept {
    if (!output)
        return SfntStatus::invalid_input;
    reset_value(output);
    CbdtStrike strike{};
    SfntStatus status = cbdt_strike(font, strike_index, &strike);
    if (status != SfntStatus::ok)
        return status;
    if (glyph < strike.first_glyph || glyph > strike.last_glyph)
        return SfntStatus::glyph_out_of_range;
    const ConstSlice<u8> table = font.locations;
    Location location{};
    bool found = false;
    for (u32 i = 0; i < strike.subtable_count && !found; ++i) {
        const u32 entry = strike.subtable_array_offset + i * kSubtableArrayEntryBytes;
        const u16 first = static_cast<u16>(be16(table, entry));
        const u16 last = static_cast<u16>(be16(table, entry + 2));
        if (glyph < first || glyph > last)
            continue;
        const u64 subtable = static_cast<u64>(strike.subtable_array_offset) + be32(table, entry + 4);
        if (subtable > 0xFFFFFFFFull)
            return SfntStatus::malformed;
        status = locate(font, static_cast<u32>(subtable), first, last, glyph, &location);
        if (status == SfntStatus::glyph_out_of_range)
            continue;
        if (status != SfntStatus::ok)
            return status;
        found = true;
    }
    if (!found)
        return SfntStatus::glyph_out_of_range;

    const ConstSlice<u8> data = font.data;
    CbdtGlyph result{};
    u32 at = location.offset;
    const u32 format = location.image_format;
    if (format == 17 || format == 18) {
        const u32 metrics = format == 17 ? kSmallMetricsBytes : kBigMetricsBytes;
        if (!span(data, at, metrics + 4))
            return SfntStatus::truncated;
        const u8* m = data.data + at;
        result.height = m[0];
        result.width = m[1];
        result.bearing_x = static_cast<i8>(m[2]);
        result.bearing_y = static_cast<i8>(m[3]);
        result.advance = m[4];
        at += metrics;
    } else if (format == 19) {
        if (!location.has_metrics)
            return SfntStatus::unsupported;
        result.height = location.metrics[0];
        result.width = location.metrics[1];
        result.bearing_x = static_cast<i8>(location.metrics[2]);
        result.bearing_y = static_cast<i8>(location.metrics[3]);
        result.advance = location.metrics[4];
        if (!span(data, at, 4))
            return SfntStatus::truncated;
    } else {
        return SfntStatus::unsupported;
    }
    const u32 png_bytes = be32(data, at);
    at += 4;
    if (png_bytes == 0 || !span(data, at, png_bytes))
        return SfntStatus::truncated;
    result.png = {data.data + at, png_bytes};
    *output = result;
    return SfntStatus::ok;
}

SfntStatus cbdt_select_strike(const CbdtFont& font, float pixels_per_em, u16 glyph, u32* output) noexcept {
    if (!output || !(pixels_per_em > 0.0f) || !font.locations.data)
        return SfntStatus::invalid_input;
    // Strikes ordered by preference: a strike at least as large as the target comes first, the smallest of those
    // first; the ones that would be scaled up follow, the largest first.
    const u32 target = pixels_per_em > 255.0f ? 255u : static_cast<u32>(pixels_per_em + 0.999f);
    u64 previous = 0;
    bool first = true;
    for (u32 attempt = 0; attempt < font.strike_count; ++attempt) {
        u64 best_key = 0xFFFFFFFFFFFFFFFFull;
        u32 best = 0;
        for (u32 i = 0; i < font.strike_count; ++i) {
            CbdtStrike strike{};
            if (cbdt_strike(font, i, &strike) != SfntStatus::ok)
                continue;
            const u32 ppem = strike.ppem_y;
            const u32 score = ppem >= target ? ppem - target : (1u << 16) + (target - ppem);
            const u64 key = (static_cast<u64>(score) << 8) | i;
            if ((first || key > previous) && key < best_key) {
                best_key = key;
                best = i;
            }
        }
        if (best_key == 0xFFFFFFFFFFFFFFFFull)
            break;
        previous = best_key;
        first = false;
        CbdtGlyph found{};
        if (cbdt_glyph(font, best, glyph, &found) == SfntStatus::ok) {
            *output = best;
            return SfntStatus::ok;
        }
    }
    return SfntStatus::glyph_out_of_range;
}

} // namespace maicord
