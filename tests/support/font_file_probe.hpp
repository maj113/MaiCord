#pragma once
// Font-file facts for tests, read straight from the file (not through the engine's SFNT reader, whose output the
// tests check): a face's table, its cmap coverage (Unicode format 4 and 12 subtables) and its COLRv0 layer count.
#include "maicord/core/win32_kernel.hpp"

namespace maicord::font_file_probe {

inline u32 be16(const u8* p) noexcept {return static_cast<u32>(p[0]) << 8 | p[1];}
inline u32 be32(const u8* p) noexcept {return be16(p) << 16 | be16(p + 2);}

inline bool read_at(HANDLE file, u64 offset, void* output, u32 bytes) noexcept {
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD done = 0;
    return ReadFile(file, output, bytes, &done, &at) && done == bytes;
}

// The table directory offsets of the file's faces: one for an sfnt, each member of a collection; false otherwise.
inline bool face_offsets(HANDLE file, u32* offsets, u32 capacity, u32* count) noexcept {
    u8 header[12];
    *count = 0;
    if (!read_at(file, 0, header, 12))
        return false;
    const u32 tag = be32(header);
    if (tag == 0x74746366) { // 'ttcf'
        for (u32 f = 0; f < be32(header + 8) && *count < capacity; ++f) {
            u8 offset[4];
            if (!read_at(file, 12 + f * 4, offset, 4))
                return false;
            offsets[(*count)++] = be32(offset);
        }
        return true;
    }
    if (tag != 0x00010000 && tag != 0x4F54544F && tag != 0x74727565) // 1.0, 'OTTO', 'true'
        return false;
    offsets[(*count)++] = 0;
    return true;
}

// The file offset and length of the face's table `tag`.
inline bool table_location(HANDLE file, u32 face_offset, u32 tag, u32* offset, u32* length) noexcept {
    u8 header[12];
    if (!read_at(file, face_offset, header, 12))
        return false;
    for (u32 i = 0; i < be16(header + 4) && i < 256; ++i) {
        u8 record[16];
        if (!read_at(file, face_offset + 12 + i * 16, record, 16))
            return false;
        if (be32(record) != tag)
            continue;
        *offset = be32(record + 8);
        *length = be32(record + 12);
        return true;
    }
    return false;
}

// Reads the face's table `tag` (at most `capacity` bytes) into `output`.
inline bool table(HANDLE file, u32 face_offset, u32 tag, u8* output, u32 capacity, u32* bytes) noexcept {
    u32 offset = 0;
    return table_location(file, face_offset, tag, &offset, bytes) && *bytes <= capacity &&
        read_at(file, offset, output, *bytes);
}

// Whether a format 4 or 12 subtable at `at` of a cmap maps the scalar to a glyph other than zero.
inline bool subtable_maps(const u8* cmap, u32 bytes, u32 at, u32 code_point) noexcept {
    if (at + 4 > bytes)
        return false;
    const u8* t = cmap + at;
    if (be16(t) == 12 && at + 16 <= bytes) {
        for (u32 g = 0; g < be32(t + 12) && at + 16 + g * 12 + 12 <= bytes; ++g) {
            const u8* group = t + 16 + g * 12;
            if (code_point >= be32(group) && code_point <= be32(group + 4))
                return be32(group + 8) + (code_point - be32(group)) != 0;
        }
        return false;
    }
    if (be16(t) != 4 || code_point > 0xFFFF || at + 14 > bytes)
        return false;
    const u32 doubled = be16(t + 6);
    if (at + 16 + doubled * 4 > bytes)
        return false;
    const u8* ends = t + 14;
    const u8* starts = ends + doubled + 2;
    const u8* deltas = starts + doubled;
    const u8* ranges = deltas + doubled;
    for (u32 i = 0; i < doubled / 2; ++i) {
        if (code_point > be16(ends + 2 * i))
            continue;
        if (code_point < be16(starts + 2 * i))
            return false;
        const u32 range = be16(ranges + 2 * i);
        if (!range)
            return ((code_point + be16(deltas + 2 * i)) & 0xFFFF) != 0;
        const u64 glyph_at = static_cast<u64>(ranges + 2 * i - cmap) + range + 2 * (code_point - be16(starts + 2 * i));
        if (glyph_at + 2 > bytes)
            return false;
        const u32 glyph = be16(cmap + glyph_at);
        return glyph && ((glyph + be16(deltas + 2 * i)) & 0xFFFF) != 0;
    }
    return false;
}

// Whether any Unicode subtable (platform 0, or Windows 1 and 10) of a cmap maps the scalar.
inline bool cmap_maps(const u8* cmap, u32 bytes, u32 code_point) noexcept {
    if (bytes < 4)
        return false;
    for (u32 s = 0; s < be16(cmap + 2) && 4 + s * 8 + 8 <= bytes; ++s) {
        const u8* encoding = cmap + 4 + s * 8;
        const u32 platform = be16(encoding), id = be16(encoding + 2);
        if ((platform == 0 || (platform == 3 && (id == 1 || id == 10))) &&
            subtable_maps(cmap, bytes, be32(encoding + 4), code_point))
            return true;
    }
    return false;
}

// The COLRv0 layer count of a base glyph (zero without a record), binary-searching the base glyph records, which the
// COLR specification sorts by glyph ID.
inline u32 colr_layers(HANDLE file, u32 face_offset, u32 glyph) noexcept {
    u32 offset = 0, length = 0;
    u8 header[8];
    if (!table_location(file, face_offset, 0x434F4C52, &offset, &length) || length < 14 || // 'COLR'
        !read_at(file, offset, header, 8))
        return 0;
    const u32 count = be16(header + 2), records = be32(header + 4);
    if (records + count * 6 > length)
        return 0;
    u32 low = 0, high = count;
    while (low < high) {
        const u32 middle = (low + high) / 2;
        u8 record[6];
        if (!read_at(file, offset + records + middle * 6, record, 6))
            return 0;
        if (be16(record) == glyph)
            return be16(record + 4);
        if (be16(record) < glyph)
            low = middle + 1;
        else
            high = middle;
    }
    return 0;
}

} // namespace maicord::font_file_probe
