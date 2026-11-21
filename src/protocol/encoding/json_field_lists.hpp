#pragma once

#include "maicord/core/byte_order.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/sys/intrin.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord::protocol_detail {

// One name of a fixed list of JSON member names.
template <usize N> struct MemberName {
    char text[N];
    constexpr MemberName(const char (&source)[N]) noexcept {
        for (usize i = 0; i < N; ++i) text[i] = source[i];
    }
};
// The names as the lookups below read them: the count, every length, then the characters of all names back to
// back. A lookup compares 16 lengths in one step, so the storage holds whole blocks of 16 after the count.
template <MemberName... Names> struct NameTable {
    static constexpr usize kUsed = 1 + sizeof...(Names) + (usize{} + ... + (sizeof(Names.text) - 1));
    static constexpr usize kBlocks = 1 + (sizeof...(Names) + 15) / 16 * 16;
    struct Storage { u8 bytes[kUsed > kBlocks ? kUsed : kBlocks]; };
    static constexpr Storage make() noexcept {
        Storage storage{};
        storage.bytes[0] = static_cast<u8>(sizeof...(Names));
        usize index = 1, at = 1 + sizeof...(Names);
        const auto add = [&](const auto& name) constexpr noexcept {
            storage.bytes[index++] = static_cast<u8>(sizeof(name.text) - 1);
            for (usize i = 0; i + 1 < sizeof(name.text); ++i) storage.bytes[at++] = static_cast<u8>(name.text[i]);
        };
        (add(Names), ...);
        return storage;
    }
    static constexpr Storage data = make();
};
// A load at 16 - k keeps the first k bytes of a block.
inline constexpr u8 kLowBytes[32]{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
__forceinline u32 byte_sum(__m128i bytes) noexcept {
    const __m128i sums = _mm_sad_epu8(bytes, _mm_setzero_si128());
    return static_cast<u32>(_mm_cvtsi128_si32(sums) + _mm_cvtsi128_si32(_mm_srli_si128(sums, 8)));
}
// Whether a and b, `bytes` long each, hold the same bytes: two overlapping loads of the widest word that fits,
// after eight bytes at a time for a longer name. No byte outside either name is read.
__forceinline bool same_text(const char* left, const char* right, u32 bytes) noexcept {
    const u8* a = reinterpret_cast<const u8*>(left);
    const u8* b = reinterpret_cast<const u8*>(right);
    if (bytes >= 8) {
        for (u32 at = 0; bytes - at > 8; at += 8)
            if (byte_load_le64(a + at) != byte_load_le64(b + at)) return false;
        return byte_load_le64(a + bytes - 8) == byte_load_le64(b + bytes - 8);
    }
    if (bytes >= 4)
        return ((byte_load_le32(a) ^ byte_load_le32(b)) | (byte_load_le32(a + bytes - 4) ^ byte_load_le32(b + bytes - 4))) == 0;
    if (bytes >= 2)
        return ((byte_load_le16(a) ^ byte_load_le16(b)) | (byte_load_le16(a + bytes - 2) ^ byte_load_le16(b + bytes - 2))) == 0;
    return !bytes || *a == *b;
}
// Position of the first table name equal to name, or the table's count when none is. A name the reader marked
// as holding no backslash (its has_escapes is exact) equals a listed name only byte for byte.
__declspec(noinline) inline u32 table_index(const JsonString& name, const u8* table) noexcept {
    const char* data = name.encoded.data;
    const u32 bytes = name.encoded.bytes;
    const u32 count = table[0];
    const u8* lengths = table + 1;
    const char* text = reinterpret_cast<const char*>(lengths + count);
    u32 i = 0;
    if (data && bytes - 1 < 255 && !name.has_escapes) {
        const __m128i wanted = _mm_set1_epi8(static_cast<char>(bytes));
        for (; i < count; i += 16) {
            const __m128i block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(lengths + i));
            u32 same = static_cast<u32>(_mm_movemask_epi8(_mm_cmpeq_epi8(block, wanted)));
            if (count - i < 16) same &= (1u << (count - i)) - 1;
            for (; same; same &= same - 1) {
                unsigned long k = 0;
                _BitScanForward(&k, same);
                // The name starts after the names of the earlier blocks and the first k names of this block.
                const __m128i before = _mm_loadu_si128(reinterpret_cast<const __m128i*>(kLowBytes + 16 - k));
                if (same_text(data, text + byte_sum(_mm_and_si128(block, before)), bytes)) return i + k;
            }
            text += byte_sum(block);
        }
        return count;
    }
    for (; i < count; text += lengths[i++])
        if (json_string_equals_ascii_bytes(data, bytes, text, lengths[i])) break;
    return i;
}
template <MemberName... Names> __forceinline u32 member_index(const JsonString& name) noexcept {
    return table_index(name, NameTable<Names...>::data.bytes);
}
// member_index for visitors that mostly see names outside the list: a first character that no list name
// starts with (and that is not a backslash, which may spell any character) is rejected inline, without the call.
template <MemberName... Names> __forceinline u32 skim_member_index(const JsonString& name) noexcept {
    constexpr u64 kStarts = (u64{1} << ('\\' & 63)) | (... | (u64{1} << (Names.text[0] & 63)));
    const char* data = name.encoded.data;
    if (data && name.encoded.bytes && !((kStarts >> (static_cast<u8>(data[0]) & 63)) & 1)) return sizeof...(Names);
    return table_index(name, NameTable<Names...>::data.bytes);
}

// 1 << position of the name, or 0 when it is absent or its position is not below limit.
template <MemberName... Names>
__forceinline u32 member_bit(const JsonString& name, u32 limit = sizeof...(Names)) noexcept {
    const u32 i = member_index<Names...>(name);
    return i < limit ? 1u << i : 0;
}

// One named member of an object: absent until seen.
struct FieldValue { JsonValue value; bool seen; };
// Left uninitialized: every lookup clears its fields first, and each set is only read after one.
template <MemberName... Names> struct FieldSet {
    union { FieldValue fields[sizeof...(Names)]; };
    FieldSet() noexcept {}
    FieldValue& operator[](usize i) noexcept { return fields[i]; }
    const FieldValue& operator[](usize i) const noexcept { return fields[i]; }
    FieldValue* begin() noexcept { return fields; }
    FieldValue* end() noexcept { return fields + sizeof...(Names); }
};
// One member whose name the caller holds as text. A name the reader marked as holding no backslash equals it only
// byte for byte.
struct NamedVisit { FieldValue* field; const char* text; u32 bytes; };
__declspec(noinline) inline bool named_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& f = *static_cast<NamedVisit*>(opaque);
    const char* data = name.encoded.data;
    if (!data || name.has_escapes) {
        if (!json_string_equals_ascii_bytes(data, name.encoded.bytes, f.text, f.bytes)) return true;
    } else if (name.encoded.bytes != f.bytes || !same_text(data, f.text, f.bytes)) {
        return true;
    }
    if (f.field->seen) return false;
    *f.field = {value, true};
    return true;
}
// Fills out from the member of the object that has this name. A repeated member of this name fails.
__declspec(noinline) inline bool field_named(const JsonIndexedDocument& doc, const JsonValue& value, u32 depth,
                                             const char* text, u32 bytes, FieldValue* out) noexcept {
    *out = FieldValue{};
    NamedVisit visit{out, text, bytes};
    return json_visit_indexed_object(doc, value, depth, named_member, &visit).ok();
}
struct TableVisit { FieldValue* fields; const u8* table; };
__declspec(noinline) inline bool table_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& f = *static_cast<TableVisit*>(opaque);
    const u32 i = table_index(name, f.table);
    if (i == f.table[0]) return true;
    if (f.fields[i].seen) return false;
    f.fields[i] = {value, true};
    return true;
}
// Fills out from the object's members that `table` (a NameTable) names. A repeated named member fails.
__declspec(noinline) inline bool fields_table(const JsonIndexedDocument& doc, const JsonValue& value, u32 depth,
                                              FieldValue* out, const u8* table) noexcept {
    for (u32 i = 0; i < table[0]; ++i) out[i] = FieldValue{};
    TableVisit visit{out, table};
    return json_visit_indexed_object(doc, value, depth, table_member, &visit).ok();
}
template <u32 Depth>
__declspec(noinline) inline bool fields_list(const JsonIndexedDocument& doc, const JsonValue& value, FieldValue* out,
                                             const u8* table) noexcept {
    return fields_table(doc, value, Depth, out, table);
}
template <u32 Depth, MemberName... Names>
__forceinline bool fields_at(const JsonIndexedDocument& doc, const JsonValue& value, FieldSet<Names...>& set) noexcept {
    return fields_list<Depth>(doc, value, set.fields, NameTable<Names...>::data.bytes);
}
// The document root, which must be an object.
template <u32 Depth>
__declspec(noinline) inline bool fields_root(const JsonIndexedDocument& doc, FieldValue* out, const u8* table) noexcept {
    return fields_table(doc, {doc.input, 0, JsonValueKind::object}, Depth, out, table);
}
template <u32 Depth, MemberName... Names>
__forceinline bool fields_at(const JsonIndexedDocument& doc, FieldSet<Names...>& set) noexcept {
    return fields_root<Depth>(doc, set.fields, NameTable<Names...>::data.bytes);
}

} // namespace maicord::protocol_detail
