#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord::protocol_detail {

// A fixed list of JSON member names is stored once as length-prefixed bytes,
// so each lookup site passes one pointer instead of building views on the stack.
template <usize N> struct MemberName {
    char text[N];
    constexpr MemberName(const char (&source)[N]) noexcept {
        for (usize i = 0; i < N; ++i) text[i] = source[i];
    }
};
// bytes: the name count, then each name as its length and characters.
template <MemberName... Names> struct MemberNames {
    struct Storage { u8 bytes[1 + (usize{} + ... + sizeof(Names.text))]; };
    static constexpr Storage make() noexcept {
        Storage storage{};
        storage.bytes[0] = static_cast<u8>(sizeof...(Names));
        usize at = 1;
        const auto add = [&](const auto& name) constexpr noexcept {
            storage.bytes[at++] = static_cast<u8>(sizeof(name.text) - 1);
            for (usize i = 0; i + 1 < sizeof(name.text); ++i) storage.bytes[at++] = static_cast<u8>(name.text[i]);
        };
        (add(Names), ...);
        return storage;
    }
    static constexpr Storage data = make();
};
// json_name_is plus an inline compare when the lengths agree. A name the reader marked as holding no backslash
// (its has_escapes is exact) equals a listed name only byte for byte; an escaped one still goes through the
// full comparison.
__forceinline bool name_matches(const JsonString& name, const u8* at) noexcept {
    const char* data = name.encoded.data;
    const u32 bytes = name.encoded.bytes;
    const u32 length = at[0];
    const char* text = reinterpret_cast<const char*>(at + 1);
    if (data && bytes && length) {
        if (!name.has_escapes) {
            if (bytes != length || data[0] != text[0]) return false;
            u32 k = 1;
            while (k < length && data[k] == text[k]) ++k;
            return k == length;
        }
        if (data[0] != text[0]) {
            if (data[0] != '\\') return false;
        } else if (bytes == length) {
            u32 k = 1;
            while (k < length && data[k] == text[k]) ++k;
            if (k == length) return true;
        }
    }
    return json_string_equals_ascii_bytes(data, bytes, text, length);
}

// Position of the first list name equal to name, or the list's count when none is.
__declspec(noinline) inline u32 member_index(const JsonString& name, const u8* list) noexcept {
    const u8* at = list + 1;
    u32 i = 0;
    for (; i < list[0]; ++i, at += 1 + at[0])
        if (name_matches(name, at)) break;
    return i;
}
template <MemberName... Names> __forceinline u32 member_index(const JsonString& name) noexcept {
    return member_index(name, MemberNames<Names...>::data.bytes);
}
// member_index for visitors that mostly see names outside the list: a first character that no list name
// starts with (and that is not a backslash, which may spell any character) is rejected inline, without the call.
template <MemberName... Names> __forceinline u32 skim_member_index(const JsonString& name) noexcept {
    constexpr u64 kStarts = (u64{1} << ('\\' & 63)) | (... | (u64{1} << (Names.text[0] & 63)));
    const char* data = name.encoded.data;
    if (data && name.encoded.bytes && !((kStarts >> (static_cast<u8>(data[0]) & 63)) & 1)) return sizeof...(Names);
    return member_index(name, MemberNames<Names...>::data.bytes);
}

// 1 << position of the name, or 0 when it is absent or its position is not below limit.
template <MemberName... Names>
__forceinline u32 member_bit(const JsonString& name, u32 limit = sizeof...(Names)) noexcept {
    const u32 i = member_index<Names...>(name);
    return i < limit ? 1u << i : 0;
}

// Long lists (a dozen names or more) scanned for names that mostly are not in them: the count, then every
// length, every first character, and the characters of all names back to back. The scan reads
// fixed-stride bytes instead of following each name's length to the next one.
template <MemberName... Names> struct WideMemberNames {
    struct Storage { u8 bytes[1 + 2 * sizeof...(Names) + (usize{} + ... + (sizeof(Names.text) - 1))]; };
    static constexpr Storage make() noexcept {
        constexpr usize count = sizeof...(Names);
        Storage storage{};
        storage.bytes[0] = static_cast<u8>(count);
        usize index = 0;
        usize at = 1 + 2 * count;
        const auto add = [&](const auto& name) constexpr noexcept {
            storage.bytes[1 + index] = static_cast<u8>(sizeof(name.text) - 1);
            storage.bytes[1 + count + index] = static_cast<u8>(name.text[0]);
            for (usize i = 0; i + 1 < sizeof(name.text); ++i) storage.bytes[at++] = static_cast<u8>(name.text[i]);
            ++index;
        };
        (add(Names), ...);
        return storage;
    }
    static constexpr Storage data = make();
};
// The same result as member_index over a MemberNames of the same names.
__declspec(noinline) inline u32 wide_member_index(const JsonString& name, const u8* list) noexcept {
    const char* data = name.encoded.data;
    const u32 bytes = name.encoded.bytes;
    const u32 count = list[0];
    const u8* lengths = list + 1;
    const u8* firsts = lengths + count;
    const char* text = reinterpret_cast<const char*>(firsts + count);
    u32 i = 0;
    if (data && bytes && data[0] != '\\') {
        const u8 first = static_cast<u8>(data[0]);
        for (; i < count; ++i) {
            const u32 length = lengths[i];
            if (firsts[i] == first && length <= bytes) {
                // Equal lengths can only match byte for byte: an escape is always longer than what it stands for.
                if (length == bytes) {
                    u32 k = 1;
                    while (k < length && data[k] == text[k]) ++k;
                    if (k == length) return i;
                } else if (name.has_escapes && json_string_equals_ascii_bytes(data, bytes, text, length)) {
                    return i;
                }
            }
            text += length;
        }
        return count;
    }
    for (; i < count; ++i) {
        const u32 length = lengths[i];
        if (json_string_equals_ascii_bytes(data, bytes, text, length)) break;
        text += length;
    }
    return i;
}
template <MemberName... Names> __forceinline u32 wide_member_index(const JsonString& name) noexcept {
    return wide_member_index(name, WideMemberNames<Names...>::data.bytes);
}
// One named member of an object: absent until seen.
struct FieldValue { JsonValue value{}; bool seen{}; };
// Left uninitialized: every lookup clears its fields first, and each set is only read after one.
template <MemberName... Names> struct FieldSet {
    union { FieldValue fields[sizeof...(Names)]; };
    FieldSet() noexcept {}
    FieldValue& operator[](usize i) noexcept { return fields[i]; }
    const FieldValue& operator[](usize i) const noexcept { return fields[i]; }
    FieldValue* begin() noexcept { return fields; }
    FieldValue* end() noexcept { return fields + sizeof...(Names); }
};
struct FieldVisit { FieldValue* fields; const u8* names; u32 count; };
__declspec(noinline) inline bool field_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& f = *static_cast<FieldVisit*>(opaque);
    const u8* at = f.names;
    for (u32 i = 0; i < f.count; ++i, at += 1 + at[0])
        if (name_matches(name, at)) {
            if (f.fields[i].seen) return false;
            f.fields[i].seen = true;
            f.fields[i].value = value;
            break;
        }
    return true;
}
// names points at the first length-prefixed name, after the list's count.
__declspec(noinline) inline bool fields_n(const JsonIndexedDocument& doc, const JsonValue& value, u32 depth,
                                          FieldValue* out, u32 count, const u8* names) noexcept {
    for (u32 i = 0; i < count; ++i) out[i] = FieldValue{};
    FieldVisit list{out, names, count};
    return json_visit_indexed_object(doc, value, depth, field_member, &list).ok();
}
// `list` is a MemberNames: the count, then the length-prefixed names.
template <u32 Depth>
__declspec(noinline) inline bool fields_list(const JsonIndexedDocument& doc, const JsonValue& value, FieldValue* out,
                                             const u8* list) noexcept {
    return fields_n(doc, value, Depth, out, list[0], list + 1);
}
template <u32 Depth, MemberName... Names>
__forceinline bool fields_at(const JsonIndexedDocument& doc, const JsonValue& value, FieldSet<Names...>& set) noexcept {
    return fields_list<Depth>(doc, value, set.fields, MemberNames<Names...>::data.bytes);
}
// The document root, which must be an object.
template <u32 Depth>
__declspec(noinline) inline bool fields_root(const JsonIndexedDocument& doc, FieldValue* out, const u8* list) noexcept {
    return fields_n(doc, {doc.input, 0, JsonValueKind::object}, Depth, out, list[0], list + 1);
}
template <u32 Depth, MemberName... Names>
__forceinline bool fields_at(const JsonIndexedDocument& doc, FieldSet<Names...>& set) noexcept {
    return fields_root<Depth>(doc, set.fields, MemberNames<Names...>::data.bytes);
}

} // namespace maicord::protocol_detail
