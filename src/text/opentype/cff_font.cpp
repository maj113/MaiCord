#include "cff_internal.hpp"
#include "maicord/core/memory.hpp"
#include "sfnt_read.hpp"

namespace maicord {
namespace cff_detail {
namespace {

using namespace sfnt_read;

bool unsigned_integer(double value, u32* result) noexcept {
    if (!(value >= 0 && value <= 4294967295.0)) return false;
    *result = static_cast<u32>(value);
    return static_cast<double>(*result) == value;
}

SfntStatus real_number(ConstSlice<u8> data, u32* at, double* value) noexcept {
    double mantissa = 0;
    i32 fractional = 0, exponent = 0;
    bool negative = false, fraction = false, exp = false, exp_negative = false;
    bool digits = false, exp_digits = false;
    u32 nibbles = 0;
    while (*at < data.count) {
        const u8 byte = data.data[(*at)++];
        for (u32 part = 0; part < 2; ++part) {
            const u8 nibble = part ? byte & 15u : byte >> 4u;
            if (++nibbles > 128) return SfntStatus::malformed;
            if (nibble <= 9) {
                if (exp) {
                    if (exponent > 100) return SfntStatus::malformed;
                    exponent = exponent * 10 + nibble; exp_digits = true;
                } else {
                    mantissa = mantissa * 10 + nibble; digits = true;
                    if (fraction) ++fractional;
                }
            } else if (nibble == 10 && !fraction && !exp) fraction = true;
            else if ((nibble == 11 || nibble == 12) && !exp && digits) {
                exp = true; exp_negative = nibble == 12;
            } else if (nibble == 14 && !digits && !negative && !fraction && !exp) negative = true;
            else if (nibble == 15) {
                if (!digits || (exp && !exp_digits)) return SfntStatus::malformed;
                const i32 power = (exp_negative ? -exponent : exponent) - fractional;
                if (power < -300 || power > 300) return SfntStatus::malformed;
                for (i32 i = 0; i < power; ++i) mantissa *= 10;
                for (i32 i = 0; i > power; --i) mantissa /= 10;
                *value = negative ? -mantissa : mantissa;
                return *value >= -1.e100 && *value <= 1.e100 ? SfntStatus::ok : SfntStatus::malformed;
            } else return SfntStatus::malformed;
        }
    }
    return SfntStatus::truncated;
}

// The offset array and objects of an INDEX whose count was read; `at` is just past the count.
SfntStatus index_entries(ConstSlice<u8> data, u32* at, u32 count, CffIndex* index) noexcept {
    if (!count) return SfntStatus::ok;
    if (!span(data, *at, 1)) return SfntStatus::truncated;
    const u8 size = data.data[(*at)++];
    if (!size || size > 4) return SfntStatus::malformed;
    const u32 offset_bytes = (count + 1u) * size;
    if (!span(data, *at, offset_bytes)) return SfntStatus::truncated;
    const u8* offsets = data.data + *at; *at += offset_bytes;
    u32 previous = packed(offsets, size);
    if (previous != 1) return SfntStatus::malformed;
    for (u32 i = 1; i <= count; ++i) {
        const u32 next = packed(offsets + i * size, size);
        if (next < previous) return SfntStatus::malformed;
        previous = next;
    }
    const u32 bytes = previous - 1;
    if (!span(data, *at, bytes)) return SfntStatus::truncated;
    *index = {{offsets, offset_bytes}, {data.data + *at, bytes}, count, size};
    *at += bytes;
    return SfntStatus::ok;
}

#if !defined(_WIN32)
// A CFF2 INDEX: a 32-bit count. Glyph and subroutine numbers are 16-bit, so a larger count is refused.
SfntStatus index2_open(ConstSlice<u8> data, u32* at, CffIndex* index) noexcept {
    reset_value(index);
    if (!span(data, *at, 4)) return SfntStatus::truncated;
    const u32 count = byte_load_be32(data.data + *at); *at += 4;
    if (count > 0x10000u) return SfntStatus::unsupported;
    return index_entries(data, at, count, index);
}

// CFF2 tables start with major version 2; cff_open read the first byte of every table it accepted.
bool font_is_cff2(ConstSlice<u8> table) noexcept { return table.data[0] == 2; }
#endif

} // namespace

SfntStatus index_open(ConstSlice<u8> data, u32* at, CffIndex* index) noexcept {
    reset_value(index);
    if (!span(data, *at, 2)) return SfntStatus::truncated;
    const u32 count = byte_load_be16(data.data + *at); *at += 2;
    return index_entries(data, at, count, index);
}

ConstSlice<u8> index_object(const CffIndex& index, u32 object) noexcept {
    if (object >= index.count) return {};
    const u8* p = index.offsets.data + object * index.offset_size;
    const u32 first = packed(p, index.offset_size) - 1;
    const u32 last = packed(p + index.offset_size, index.offset_size) - 1;
    return {index.objects.data + first, last - first};
}

SfntStatus dictionary(ConstSlice<u8> bytes, Dictionary* out) noexcept {
#if defined(_WIN32)
    constexpr u32 limit = 48;
    double stack[limit]{};
#else
    // CFF2 allows 513 operands: a Private DICT blends each hint value with a delta for each region. Only the values
    // below `count` are read, so the array is not cleared.
    constexpr u32 limit = 513;
    double stack[limit];
#endif
    u32 count = 0, at = 0;
    while (at < bytes.count) {
        const u8 byte = bytes.data[at++];
        if (byte >= 32 || byte == 28 || byte == 29 || byte == 30) {
            if (count == limit) return SfntStatus::malformed;
            double value = 0;
            if (byte >= 32 && byte <= 246) value = static_cast<i32>(byte) - 139;
            else if (byte >= 247 && byte <= 254) {
                if (!span(bytes, at, 1)) return SfntStatus::truncated;
                const u8 next = bytes.data[at++];
                value = byte <= 250 ? (byte - 247) * 256 + next + 108 : -(byte - 251) * 256 - next - 108;
            } else if (byte == 28 || byte == 29) {
                const u32 size = byte == 28 ? 2u : 4u;
                if (!span(bytes, at, size)) return SfntStatus::truncated;
                value = byte == 28 ? static_cast<i16>(byte_load_be16(bytes.data + at))
                                   : static_cast<i32>(byte_load_be32(bytes.data + at));
                at += size;
            } else if (byte == 30) {
                const auto status = real_number(bytes, &at, &value);
                if (status != SfntStatus::ok) return status;
            } else return SfntStatus::malformed;
            stack[count++] = value;
            continue;
        }
        u32 op = byte;
        if (op == 12) {
            if (at == bytes.count) return SfntStatus::truncated;
            op = 0x100u + bytes.data[at++];
        }
        u32* target = nullptr;
        switch (op) {
        case 15: target = &out->charset; break;
        case 17: target = &out->charstrings; break;
        case 19: target = &out->subrs; break;
        case 20: case 21:
            if (count != 1 || !(stack[0] >= -1.e12 && stack[0] <= 1.e12)) return SfntStatus::malformed;
            if (op == 20) out->default_width = stack[0]; else out->nominal_width = stack[0];
            break;
        case 0x124: target = &out->fd_array; break;
        case 0x125: target = &out->fd_select; break;
        case 0x113:
            if (count != 1 || stack[0] < -2147483648.0 || stack[0] > 2147483647.0)
                return SfntStatus::malformed;
            out->random_seed = static_cast<u32>(static_cast<i32>(stack[0])); break;
        case 18:
            if (count != 2 || !unsigned_integer(stack[0], &out->private_size) ||
                !unsigned_integer(stack[1], &out->private_offset)) return SfntStatus::malformed;
            break;
        case 0x106:
            if (count != 1 || stack[0] != 2) return SfntStatus::unsupported;
            break;
        case 0x107:
            if (count != 6) return SfntStatus::malformed;
            for (u32 i = 0; i < 6; ++i) {
                if (!(stack[i] >= -1.e6 && stack[i] <= 1.e6)) return SfntStatus::malformed;
                out->matrix[i] = stack[i];
            }
            out->has_matrix = true; break;
        case 0x11e:
            if (count != 3) return SfntStatus::malformed;
            out->cid = true; break;
#if !defined(_WIN32)
        case 22: target = &out->vsindex; break;
        case 24: target = &out->variation_store; break;
#endif
        default: break; // Font names/hint preferences do not change unhinted paths.
        }
        if (target && (count != 1 || !unsigned_integer(stack[0], target))) return SfntStatus::malformed;
        count = 0;
    }
    return count ? SfntStatus::malformed : SfntStatus::ok;
}

#if defined(_WIN32)
SfntStatus private_subrs(ConstSlice<u8> table, const Dictionary& dict, CffIndex* subrs, u32* seed,
                           double* default_width, double* nominal_width) noexcept {
#else
SfntStatus private_subrs(ConstSlice<u8> table, const Dictionary& dict, CffIndex* subrs, u32* seed,
                           double* default_width, double* nominal_width, u32* vsindex) noexcept {
#endif
    reset_value(subrs); *seed = 0;
    if (!span(table, dict.private_offset, dict.private_size)) return SfntStatus::truncated;
    Dictionary priv{};
    const auto status = dictionary({table.data + dict.private_offset, dict.private_size}, &priv);
    if (status != SfntStatus::ok) return status;
    *seed = priv.random_seed;
    *default_width = priv.default_width; *nominal_width = priv.nominal_width;
#if !defined(_WIN32)
    *vsindex = priv.vsindex;
#endif
    if (!priv.subrs) return SfntStatus::ok;
    if (priv.subrs > table.count - dict.private_offset) return SfntStatus::truncated;
    u32 at = dict.private_offset + priv.subrs;
#if !defined(_WIN32)
    if (font_is_cff2(table)) return index2_open(table, &at, subrs);
#endif
    return index_open(table, &at, subrs);
}

#if defined(_WIN32)
SfntStatus select_dict(const CffFont& font, u16 glyph, CffIndex* subrs, double* matrix, u32* seed,
                         double* default_width, double* nominal_width) noexcept {
#else
SfntStatus select_dict(const CffFont& font, u16 glyph, CffIndex* subrs, double* matrix, u32* seed,
                         double* default_width, double* nominal_width, u32* vsindex) noexcept {
    *vsindex = 0;
#endif
    *subrs = font.local_subrs; *seed = font.random_seed;
    *default_width = font.default_width; *nominal_width = font.nominal_width;
    for (u32 i = 0; i < 6; ++i) matrix[i] = font.matrix[i];
    if (!font.cid) return SfntStatus::ok;
    const auto select = font.fd_select;
    u32 fd = 0;
#if !defined(_WIN32)
    // A CFF2 font with one Font DICT may have no FDSelect. Format 4 has 32-bit glyph numbers and 16-bit FD numbers.
    if (!select.data) fd = 0;
    else if (select.data[0] == 4) {
        const u32 count = byte_load_be32(select.data + 1);
        u32 low = 0, high = count;
        while (low + 1 < high) {
            const u32 mid = low + (high - low) / 2;
            if (byte_load_be32(select.data + 5 + mid * 6) <= glyph) low = mid;
            else high = mid;
        }
        fd = byte_load_be16(select.data + 5 + low * 6 + 4);
    } else
#endif
    if (select.data[0] == 0) fd = select.data[1u + glyph];
    else {
        const u32 count = byte_load_be16(select.data + 1);
        u32 low = 0, high = count;
        while (low + 1 < high) {
            const u32 mid = low + (high - low) / 2;
            if (byte_load_be16(select.data + 3 + mid * 3) <= glyph) low = mid;
            else high = mid;
        }
        fd = select.data[3 + low * 3 + 2];
    }
    Dictionary dict{};
    auto status = dictionary(index_object(font.font_dicts, fd), &dict);
    if (status != SfntStatus::ok) return status;
#if defined(_WIN32)
    status = private_subrs(font.table, dict, subrs, seed, default_width, nominal_width);
#else
    status = private_subrs(font.table, dict, subrs, seed, default_width, nominal_width, vsindex);
#endif
    if (status != SfntStatus::ok) return status;
    if (dict.has_matrix) {
        // Font DICT transform, followed by the Top DICT transform.
        const auto* a = font.matrix; const auto* b = dict.matrix;
        matrix[0] = a[0]*b[0]+a[2]*b[1]; matrix[1] = a[1]*b[0]+a[3]*b[1];
        matrix[2] = a[0]*b[2]+a[2]*b[3]; matrix[3] = a[1]*b[2]+a[3]*b[3];
        matrix[4] = a[0]*b[4]+a[2]*b[5]+a[4]; matrix[5] = a[1]*b[4]+a[3]*b[5]+a[5];
    }
    return SfntStatus::ok;
}

namespace {

// The FDSelect at `offset`, checked against the glyph and Font DICT counts.
SfntStatus fd_select_open(ConstSlice<u8> table, u32 offset, u32 glyph_count, u32 fd_count,
                          ConstSlice<u8>* output) noexcept {
    if (offset >= table.count) return SfntStatus::truncated;
    const ConstSlice<u8> select{table.data + offset, table.count - offset};
    if (select.data[0] == 0) {
        if (select.count < 1u + glyph_count) return SfntStatus::truncated;
        for (u32 i = 0; i < glyph_count; ++i)
            if (select.data[1 + i] >= fd_count) return SfntStatus::malformed;
    } else if (select.data[0] == 3) {
        if (select.count < 5) return SfntStatus::truncated;
        const u32 count = byte_load_be16(select.data + 1);
        if (!count || count > (select.count - 5u) / 3u) return SfntStatus::truncated;
        u32 previous = 0;
        for (u32 i = 0; i < count; ++i) {
            const u32 first = byte_load_be16(select.data + 3 + i * 3);
            if ((!i && first) || (i && first <= previous) || first >= glyph_count ||
                select.data[5 + i * 3] >= fd_count) return SfntStatus::malformed;
            previous = first;
        }
        if (byte_load_be16(select.data + 3 + count * 3) != glyph_count) return SfntStatus::malformed;
#if !defined(_WIN32)
    } else if (select.data[0] == 4) {
        if (select.count < 9) return SfntStatus::truncated;
        const u32 count = byte_load_be32(select.data + 1);
        if (!count || count > (select.count - 9u) / 6u) return SfntStatus::truncated;
        u32 previous = 0;
        for (u32 i = 0; i < count; ++i) {
            const u32 first = byte_load_be32(select.data + 5 + i * 6);
            if ((!i && first) || (i && first <= previous) || first >= glyph_count ||
                byte_load_be16(select.data + 9 + i * 6) >= fd_count) return SfntStatus::malformed;
            previous = first;
        }
        if (byte_load_be32(select.data + 5 + count * 6) != glyph_count) return SfntStatus::malformed;
#endif
    } else return SfntStatus::unsupported;
    *output = select;
    return SfntStatus::ok;
}

} // namespace

#if !defined(_WIN32)
// CFF2: a header with the Top DICT length, then the Top DICT itself (not an INDEX), the global subroutines and,
// through the Top DICT, the charstrings, the Font DICTs, the FDSelect and the variation store. Every glyph reads
// its local subroutines and its default vsindex from the Private DICT of its Font DICT. Once per face: compiled for
// size.
__attribute__((minsize))
SfntStatus cff2_open(const SfntFace& face, CffFont* font) noexcept {
    CffFont result{};
    result.cff2 = true; result.cid = true;
    auto status = sfnt_table(face, sfnt_tag('C','F','F','2'), &result.table);
    if (status != SfntStatus::ok) return status;
    const auto table = result.table;
    if (table.count < 5) return SfntStatus::truncated;
    if (table.data[0] != 2) return SfntStatus::unsupported;
    const u32 header = table.data[2], top_size = byte_load_be16(table.data + 3);
    if (header < 5) return SfntStatus::malformed;
    if (!span(table, header, top_size)) return SfntStatus::truncated;
    SfntHorizontalMetrics metrics{};
    status = sfnt_horizontal_metrics_open(face, &metrics);
    if (status != SfntStatus::ok) return status;
    result.glyph_count = metrics.glyph_count; result.units_per_em = metrics.units_per_em;
    Dictionary dict{};
    status = dictionary({table.data + header, top_size}, &dict);
    if (status != SfntStatus::ok) return status;
    u32 at = header + top_size;
    status = index2_open(table, &at, &result.global_subrs); if (status != SfntStatus::ok) return status;
    if (!dict.charstrings || !dict.fd_array) return SfntStatus::malformed;
    at = dict.charstrings;
    status = index2_open(table, &at, &result.charstrings); if (status != SfntStatus::ok) return status;
    if (result.charstrings.count != result.glyph_count) return SfntStatus::malformed;
    at = dict.fd_array;
    status = index2_open(table, &at, &result.font_dicts); if (status != SfntStatus::ok) return status;
    if (!result.font_dicts.count || result.font_dicts.count > 256) return SfntStatus::malformed;
    if (dict.fd_select) {
        status = fd_select_open(table, dict.fd_select, result.glyph_count, result.font_dicts.count, &result.fd_select);
        if (status != SfntStatus::ok) return status;
    } else if (result.font_dicts.count != 1) return SfntStatus::malformed;
    // The store follows a 16-bit length.
    if (dict.variation_store) {
        if (!span(table, dict.variation_store, 2)) return SfntStatus::truncated;
        status = item_variation_store_open(table, dict.variation_store + 2, &result.variations);
        if (status != SfntStatus::ok) return status;
    }
    for (u32 i = 0; i < 6; ++i) result.matrix[i] = dict.matrix[i];
    copy_value(font, result);
    return SfntStatus::ok;
}
#endif

} // namespace cff_detail

// Once per face, before any outline: compiled for size.
__attribute__((minsize))
SfntStatus cff_open(const SfntFace& face, CffFont* font) noexcept {
    if (!font) return SfntStatus::invalid_input;
    reset_value(font);
    CffFont result{};
    auto status = sfnt_table(face, sfnt_tag('C','F','F',' '), &result.table);
#if !defined(_WIN32)
    if (status == SfntStatus::missing_table) return cff_detail::cff2_open(face, font);
#endif
    if (status != SfntStatus::ok) return status;
    const auto table = result.table;
    if (table.count < 4) return SfntStatus::truncated;
    if (table.data[0] != 1) return SfntStatus::unsupported;
    if (table.data[2] < 4 || table.data[3] < 1 || table.data[3] > 4) return SfntStatus::malformed;
    SfntHorizontalMetrics metrics{};
    status = sfnt_horizontal_metrics_open(face, &metrics);
    if (status != SfntStatus::ok) return status;
    result.glyph_count = metrics.glyph_count; result.units_per_em = metrics.units_per_em;
    u32 at = table.data[2];
    CffIndex names{}, tops{}, strings{};
    status = cff_detail::index_open(table, &at, &names); if (status != SfntStatus::ok) return status;
    status = cff_detail::index_open(table, &at, &tops); if (status != SfntStatus::ok) return status;
    if (names.count != 1 || tops.count != 1) return SfntStatus::unsupported;
    status = cff_detail::index_open(table, &at, &strings); if (status != SfntStatus::ok) return status;
    status = cff_detail::index_open(table, &at, &result.global_subrs); if (status != SfntStatus::ok) return status;
    cff_detail::Dictionary dict{};
    status = cff_detail::dictionary(cff_detail::index_object(tops, 0), &dict);
    if (status != SfntStatus::ok) return status;
    if (!dict.charstrings) return SfntStatus::malformed;
    at = dict.charstrings;
    status = cff_detail::index_open(table, &at, &result.charstrings);
    if (status != SfntStatus::ok) return status;
    if (result.charstrings.count != result.glyph_count) return SfntStatus::malformed;
    for (u32 i = 0; i < 6; ++i) result.matrix[i] = dict.matrix[i];
    result.charset = dict.charset; result.cid = dict.cid;
#if defined(_WIN32)
    status = cff_detail::private_subrs(table, dict, &result.local_subrs, &result.random_seed,
                                      &result.default_width, &result.nominal_width);
#else
    u32 vsindex = 0;
    status = cff_detail::private_subrs(table, dict, &result.local_subrs, &result.random_seed,
                                      &result.default_width, &result.nominal_width, &vsindex);
#endif
    if (status != SfntStatus::ok) return status;
    if (result.cid) {
        if (!dict.fd_array || !dict.fd_select) return SfntStatus::malformed;
        at = dict.fd_array;
        status = cff_detail::index_open(table, &at, &result.font_dicts);
        if (status != SfntStatus::ok) return status;
        if (!result.font_dicts.count || result.font_dicts.count > 256) return SfntStatus::malformed;
        status = cff_detail::fd_select_open(table, dict.fd_select, result.glyph_count, result.font_dicts.count, &result.fd_select);
        if (status != SfntStatus::ok) return status;
    }
    copy_value(font, result);
    return SfntStatus::ok;
}

} // namespace maicord
