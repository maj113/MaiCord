#include "maicord/text/sfnt.hpp"
#include "maicord/text/base.hpp"

namespace maicord {
namespace {

inline u16 be16(const u8* p) noexcept { return byte_load_be16(p); }
inline u32 be32(const u8* p) noexcept { return byte_load_be32(p); }
inline u32 be24(const u8* p) noexcept { return (static_cast<u32>(p[0]) << 16u) | be16(p + 1); }
bool scalar_valid(u32 scalar) noexcept {
    return scalar <= 0x10ffffu && (scalar < 0xd800u || scalar > 0xdfffu);
}
bool selector_valid(u32 scalar) noexcept {
    return (scalar >= 0xfe00u && scalar <= 0xfe0fu) ||
           (scalar >= 0xe0100u && scalar <= 0xe01efu) ||
           (scalar >= 0x180bu && scalar <= 0x180du) || scalar == 0x180fu;
}

// Validate before exposing a view. Multiplications follow count/span checks.
SfntStatus subtable(ConstSlice<u8> table, u32 offset, ConstSlice<u8>* out) noexcept {
    *out = {};
    if (offset > table.count || table.count - offset < 2) return SfntStatus::truncated;
    const u8* p = table.data + offset;
    const u32 available = table.count - offset;
    const u16 format = be16(p);
    u32 size = 0, header = 0;
    if (format == 0 || format == 4 || format == 6) {
        if (available < 6) return SfntStatus::truncated;
        size = be16(p + 2); header = 6;
    } else if (format == 10 || format == 12 || format == 13) {
        if (available < 16) return SfntStatus::truncated;
        size = be32(p + 4); header = 16;
    } else if (format == 14) {
        if (available < 10) return SfntStatus::truncated;
        size = be32(p + 2); header = 10;
    } else return SfntStatus::unsupported;
    if (size < header) return SfntStatus::malformed;
    if (size > available) return SfntStatus::truncated;
    *out = {p, size};
    return SfntStatus::ok;
}

SfntStatus validate_base(ConstSlice<u8> table, u16 glyphs) noexcept {
    const u8* p = table.data;
    const u16 format = be16(p);
    if (format == 0) return table.count >= 262 ? SfntStatus::ok : SfntStatus::truncated;
    if (format == 6 || format == 10) {
        const u32 header = format == 6 ? 10u : 20u;
        if (table.count < header) return SfntStatus::truncated;
        const u32 first = format == 6 ? be16(p + 6) : be32(p + 12);
        const u32 count = format == 6 ? be16(p + 8) : be32(p + 16);
        const u32 limit = format == 6 ? 0xffffu : 0x10ffffu;
        if (first > limit || (count && count - 1 > limit - first)) return SfntStatus::malformed;
        return count <= (table.count - header) / 2u ? SfntStatus::ok : SfntStatus::truncated;
    }
    if (format == 4) {
        if (table.count < 16) return SfntStatus::truncated;
        const u32 doubled = be16(p + 6), count = doubled / 2;
        if (!count || (doubled & 1u)) return SfntStatus::malformed;
        if (count > (table.count - 16u) / 8u) return SfntStatus::truncated;
        u32 previous = 0;
        for (u32 i = 0; i < count; ++i) {
            const u32 end = be16(p + 14 + i * 2);
            const u32 start = be16(p + 16 + count * 2 + i * 2);
            if (start > end || (i && start <= previous)) return SfntStatus::malformed;
            previous = end;
            const u32 word = 16 + count * 6 + i * 2, range = be16(p + word);
            if (range) {
                if (range & 1u) return SfntStatus::malformed;
                const u32 begin = word + range;
                if (begin < 16 + count * 8) return SfntStatus::malformed;
                if (begin > table.count || end - start + 1 > (table.count - begin) / 2u)
                    return SfntStatus::truncated;
            }
        }
        return previous == 0xffffu ? SfntStatus::ok : SfntStatus::malformed;
    }
    if (format == 12 || format == 13) {
        const u32 count = be32(p + 12);
        if (count > (table.count - 16u) / 12u) return SfntStatus::truncated;
        u32 previous = 0;
        for (u32 i = 0; i < count; ++i) {
            const u8* group = p + 16 + i * 12;
            const u32 first = be32(group), last = be32(group + 4), glyph = be32(group + 8);
            if (first > last || last > 0x10ffffu || (i && first <= previous) ||
                glyph >= glyphs || (format == 12 && last - first >= glyphs - glyph))
                return SfntStatus::malformed;
            previous = last;
        }
        return SfntStatus::ok;
    }
    return SfntStatus::unsupported;
}

SfntStatus validate_uvs(ConstSlice<u8> table, u16 glyphs) noexcept {
    const u8* p = table.data;
    const u32 count = be32(p + 6);
    if (count > (table.count - 10u) / 11u) return SfntStatus::truncated;
    const u32 records_end = 10 + count * 11;
    u32 previous = 0;
    for (u32 i = 0; i < count; ++i) {
        const u8* record = p + 10 + i * 11;
        const u32 selector = be24(record);
        if (!selector_valid(selector) || (i && selector <= previous)) return SfntStatus::malformed;
        previous = selector;
        for (u32 kind = 0; kind < 2; ++kind) {
            const u32 offset = be32(record + 3 + kind * 4);
            if (!offset) continue;
            if (offset < records_end) return SfntStatus::malformed;
            if (offset > table.count || table.count - offset < 4) return SfntStatus::truncated;
            const u32 ranges = be32(p + offset), stride = kind ? 5u : 4u;
            if (ranges > (table.count - offset - 4u) / stride) return SfntStatus::truncated;
            u32 last = 0;
            for (u32 j = 0; j < ranges; ++j) {
                const u8* range = p + offset + 4 + j * stride;
                const u32 first = be24(range), end = first + (kind ? 0 : range[3]);
                if (!scalar_valid(first) || !scalar_valid(end) ||
                    (first < 0xd800u && end >= 0xd800u) || (j && first <= last) ||
                    (kind && be16(range + 3) >= glyphs)) return SfntStatus::malformed;
                last = end;
            }
        }
    }
    return SfntStatus::ok;
}

u32 base_rank(u16 platform, u16 encoding, u16 format) noexcept {
    if (!((platform == 0 && encoding != 5) ||
          (platform == 3 && (encoding == 1 || encoding == 10)))) return 0;
    // Full repertoire mappings take precedence over BMP; last-resort format 13
    // is selected only when no ordinary mapping is available.
    const u32 rank = format == 12 ? 60 : format == 10 ? 50 : format == 4 ? 40 :
                     format == 6 ? 30 : format == 0 ? 20 : format == 13 ? 10 : 0;
    return rank ? rank + (platform == 3 ? 1u : 0u) : 0;
}

SfntStatus nominal(const SfntCmap& cmap, u32 scalar, u16* out) noexcept {
    const u8* p = cmap.base.data;
    u32 glyph = 0;
    if (cmap.format == 0) {
        glyph = scalar < 256 ? p[6 + scalar] : 0;
    } else if (cmap.format == 6 || cmap.format == 10) {
        const bool short_map = cmap.format == 6;
        const u32 first = short_map ? be16(p + 6) : be32(p + 12);
        const u32 count = short_map ? be16(p + 8) : be32(p + 16);
        if (scalar >= first && scalar - first < count)
            glyph = be16(p + (short_map ? 10u : 20u) + (scalar - first) * 2u);
    } else if (cmap.format == 4) {
        if (scalar > 0xffffu) return SfntStatus::ok;
        const u32 count = be16(p + 6) / 2u;
        u32 low = 0, high = count;
        while (low < high) {
            const u32 middle = low + (high - low) / 2;
            if (be16(p + 14 + middle * 2) < scalar) low = middle + 1;
            else high = middle;
        }
        if (low == count) return SfntStatus::ok;
        const u32 first = be16(p + 16 + count * 2 + low * 2);
        if (scalar < first) return SfntStatus::ok;
        const u32 delta = be16(p + 16 + count * 4 + low * 2);
        const u32 word = 16 + count * 6 + low * 2, range = be16(p + word);
        if (!range) glyph = (scalar + delta) & 0xffffu;
        else {
            glyph = be16(p + word + range + (scalar - first) * 2);
            // A zero array entry stays .notdef even with a nonzero delta.
            if (glyph) glyph = (glyph + delta) & 0xffffu;
        }
    } else if (cmap.format == 12 || cmap.format == 13) {
        u32 low = 0, high = be32(p + 12);
        const u32 count = high;
        while (low < high) {
            const u32 middle = low + (high - low) / 2;
            if (be32(p + 20 + middle * 12) < scalar) low = middle + 1;
            else high = middle;
        }
        if (low == count) return SfntStatus::ok;
        const u8* group = p + 16 + low * 12;
        const u32 first = be32(group);
        if (scalar < first) return SfntStatus::ok;
        glyph = be32(group + 8) + (cmap.format == 12 ? scalar - first : 0);
    } else return SfntStatus::unsupported;
    if (glyph >= cmap.glyph_count) return SfntStatus::glyph_out_of_range;
    *out = static_cast<u16>(glyph);
    return SfntStatus::ok;
}

} // namespace

SfntStatus sfnt_cmap_open(const SfntFace& face, SfntCmap* cmap) noexcept {
    if (!cmap) return SfntStatus::invalid_input;
    *cmap = {};
    SfntCmap result{};
    auto status = sfnt_glyph_count(face, &result.glyph_count);
    if (status != SfntStatus::ok) return status;
    ConstSlice<u8> table{};
    status = sfnt_table(face, sfnt_tag('c', 'm', 'a', 'p'), &table);
    if (status != SfntStatus::ok) return status;
    if (table.count < 4) return SfntStatus::truncated;
    if (be16(table.data)) return SfntStatus::unsupported;
    const u32 count = be16(table.data + 2);
    if (count > (table.count - 4u) / 8u) return SfntStatus::truncated;
    u32 selected_rank = 0;
    for (u32 i = 0; i < count; ++i) {
        const u8* record = table.data + 4 + i * 8;
        const u16 platform = be16(record), encoding = be16(record + 2);
        if (platform != 0 && !(platform == 3 && (encoding == 1 || encoding == 10))) continue;
        const u32 offset = be32(record + 4);
        if (offset < 4 + count * 8) return SfntStatus::malformed;
        ConstSlice<u8> candidate{};
        status = subtable(table, offset, &candidate);
        if (status == SfntStatus::unsupported) continue;
        if (status != SfntStatus::ok) return status;
        const u16 format = be16(candidate.data);
        if (platform == 0 && encoding == 5 && format == 14) {
            if (result.variations.data) return SfntStatus::malformed;
            status = validate_uvs(candidate, result.glyph_count);
            if (status != SfntStatus::ok) return status;
            result.variations = candidate;
        } else {
            const u32 rank = base_rank(platform, encoding, format);
            if (!rank || rank <= selected_rank) continue;
            selected_rank = rank;
            result.base = candidate;
            result.format = format;
        }
    }
    if (!result.base.data) return SfntStatus::unsupported;
    status = validate_base(result.base, result.glyph_count);
    if (status != SfntStatus::ok) return status;
    *cmap = result;
    return SfntStatus::ok;
}

SfntStatus sfnt_cmap_glyph(const SfntCmap& cmap, u32 scalar, u32 selector,
                            u16* glyph) noexcept {
    if (!glyph) return SfntStatus::invalid_input;
    *glyph = 0;
    if (!cmap.base.data || !cmap.glyph_count || !scalar_valid(scalar) ||
        (selector && !selector_valid(selector))) return SfntStatus::invalid_input;
    if (!selector) return nominal(cmap, scalar, glyph);
    if (!cmap.variations.data) return SfntStatus::unsupported_sequence;
    const u8* p = cmap.variations.data;
    u32 low = 0, high = be32(p + 6);
    const u32 count = high;
    while (low < high) {
        const u32 middle = low + (high - low) / 2;
        if (be24(p + 10 + middle * 11) < selector) low = middle + 1;
        else high = middle;
    }
    if (low == count || be24(p + 10 + low * 11) != selector)
        return SfntStatus::unsupported_sequence;
    const u8* record = p + 10 + low * 11;
    // A non-default mapping takes precedence. A default range opts
    // into the nominal cmap; an absent sequence must not.
    for (u32 pass = 0; pass < 2; ++pass) {
        const bool nondefault = pass == 0;
        const u32 offset = be32(record + (nondefault ? 7u : 3u));
        if (!offset) continue;
        const u8* ranges = p + offset + 4;
        const u32 ranges_count = be32(p + offset), stride = nondefault ? 5u : 4u;
        low = 0; high = ranges_count;
        while (low < high) {
            const u32 middle = low + (high - low) / 2;
            const u8* range = ranges + middle * stride;
            const u32 end = be24(range) + (nondefault ? 0u : range[3]);
            if (end < scalar) low = middle + 1;
            else high = middle;
        }
        if (low < ranges_count) {
            const u8* range = ranges + low * stride;
            if (be24(range) <= scalar) {
                if (!nondefault) return nominal(cmap, scalar, glyph);
                *glyph = be16(range + 3);
                return SfntStatus::ok;
            }
        }
    }
    return SfntStatus::unsupported_sequence;
}

} // namespace maicord
