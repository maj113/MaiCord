#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "../encoding/json_helpers.hpp"
#include "../encoding/json_field_lists.hpp"

// Decoding a JSON object into a struct from a table: each member name carries how its value is read (kind, flags)
// and where it goes (offset into the struct), so a decoder is a table plus the few members that need their own code
// (a hook). The table's head (ObjectSpec) says which error codes a failure reports, which members must be present
// and, for rows of an array, how large a row is and which bytes of it no two rows share.
namespace maicord::protocol_detail {

// A member's row is one info byte, then the offset of what it fills (not for a hook) and, for a text, its capacity and
// the offset of its length counter (16 bits each). Bits 0-2 of the info are the kind; the rest are flags.
enum FieldKind : u8 {
    field_snowflake,  // u64: a nonzero decimal string
    field_timestamp,  // u64 Unix milliseconds from an ISO 8601 string
    field_bool,       // bool
    field_number,     // unsigned integer into 1, 2 or 4 bytes, up to what they hold
    field_saturate,   // the same, cut to what they hold instead of refused
    field_text,       // string that must fit the buffer; its length goes to the counter
    field_label,      // string cut at a code point boundary to fit; length to the counter (a long one reads empty)
    field_hook,       // the spec's hook reads it
};
// Null is valid: it clears an id, a time or a text (and its length) and, with kFieldNullClears, a number, and leaves a
// flag as it is. The width is the member's (a text's counter) as log2 of its size, in bits 4-5. The hook sees the member
// once it is stored and may refuse it with kFieldNotify.
inline constexpr u8 kFieldNull = 8;
inline constexpr u8 kFieldNotify = 64;
inline constexpr u8 kFieldNullClears = 128;

#pragma pack(push, 1)
// The error codes a decode reports, as its error enum numbers them.
struct ErrorCodes {
    u8 duplicate; // a member given twice; 0: the later one wins
    u8 field;     // a value that does not fit; 0: it is skipped (a required member's, too, fails the object)
    u8 json;      // the reader refused the object
    u8 identity;  // an item without its required members
    u8 capacity;  // no room for another item
};
// For a table that passes over the members that do not fit: only an object that cannot be read (code 1), or a required
// member that does not fit, fails the read.
inline constexpr ErrorCodes kLenientCodes{0, 0, 1, 0, 0};

struct ObjectReader;
using FieldHook = bool (*)(ObjectReader& reader, u32 index, const JsonValue& value) noexcept;

// What a kind of object is read with: the head of its table.
struct ObjectSpec {
    FieldHook hook;
    ErrorCodes codes;
    u8 depth;
    // Members that must be present (a mask of table positions).
    u8 required;
    // As an array item: the row's size, and the key_size bytes at key_offset that no two rows share (0: none).
    u16 size;
    u16 key_offset;
    u8 key_size;
    // How a label reads: the most bytes its string decodes to before it reads empty, in 256s (0: 1024).
    u8 text;
    u8 names_bytes; // the names block: the count, then each name (length first) as member_index reads them
};
// The table: its head, the names, a hash for each name, where each name and each row starts, then the rows: one for
// each name, in order. Aligned for the hook's pointer, which the image's relocations patch.
template <usize Bytes> struct alignas(8) FieldTable : ObjectSpec {
    u8 members[Bytes];
};
#pragma pack(pop)

struct FieldDef {
    const char* name;
    u8 info;
    u16 offset, capacity, counter;
};

consteval usize field_name_length(const char* name) noexcept {
    usize n = 0;
    while (name[n]) ++n;
    return n;
}
constexpr usize field_row_bytes(u8 info) noexcept {
    const u8 kind = info & 7;
    return kind == field_hook ? 1 : kind >= field_text ? 7 : 3;
}
// A byte that tells names apart cheaply: from a name's length, first and last byte.
constexpr u8 field_name_hash(u32 bytes, u8 first, u8 last) noexcept {
    return static_cast<u8>(bytes * 7 + first + last * 3);
}
// The bytes of the names block: the count, then each name with its length first.
template <usize N> consteval usize field_names_bytes(const FieldDef (&defs)[N]) noexcept {
    usize bytes = 1;
    for (usize i = 0; i < N; ++i) bytes += 1 + field_name_length(defs[i].name);
    return bytes;
}
template <usize N> consteval usize field_table_bytes(const FieldDef (&defs)[N]) noexcept {
    usize bytes = field_names_bytes(defs) + 3 * N;
    for (usize i = 0; i < N; ++i) bytes += field_row_bytes(defs[i].info);
    return bytes;
}
template <usize Bytes, usize N>
consteval FieldTable<Bytes> make_field_table(const FieldDef (&defs)[N], ObjectSpec head) noexcept {
    FieldTable<Bytes> table{};
    static_cast<ObjectSpec&>(table) = head;
    const usize names = field_names_bytes(defs);
    // A table of at most 63 members and 255 bytes of names; a larger one fails to evaluate here.
    if (names > 255 || N > 63)
        *static_cast<volatile int*>(nullptr) = 0;
    table.names_bytes = static_cast<u8>(names);
    usize at = 0;
    const auto put16 = [&](u16 value) consteval {
        table.members[at++] = static_cast<u8>(value);
        table.members[at++] = static_cast<u8>(value >> 8);
    };
    table.members[at++] = static_cast<u8>(N);
    for (usize i = 0; i < N; ++i) {
        const usize length = field_name_length(defs[i].name);
        table.members[at++] = static_cast<u8>(length);
        for (usize k = 0; k < length; ++k) table.members[at++] = static_cast<u8>(defs[i].name[k]);
    }
    // A hash and the offset in the names block of each name, then where each row starts among the rows.
    usize name_at = 1;
    for (usize i = 0; i < N; ++i) {
        const usize length = field_name_length(defs[i].name);
        table.members[at++] = field_name_hash(static_cast<u32>(length), static_cast<u8>(defs[i].name[0]), static_cast<u8>(defs[i].name[length - 1]));
    }
    for (usize i = 0; i < N; ++i) {
        table.members[at++] = static_cast<u8>(name_at);
        name_at += 1 + field_name_length(defs[i].name);
    }
    usize row_at = 0;
    for (usize i = 0; i < N; ++i) {
        if (row_at > 255)
            *static_cast<volatile int*>(nullptr) = 0;
        table.members[at++] = static_cast<u8>(row_at);
        row_at += field_row_bytes(defs[i].info);
    }
    for (usize i = 0; i < N; ++i) {
        table.members[at++] = defs[i].info;
        const usize row = field_row_bytes(defs[i].info);
        if (row > 1) put16(defs[i].offset);
        if (row > 3) {
            put16(defs[i].capacity);
            put16(defs[i].counter);
        }
    }
    return table;
}
// A table named `name` from `defs`; the rest is the head: hook, codes, depth, and optionally required, size,
// key_offset, key_size and text.
#define MAICORD_FIELD_TABLE(name, defs, ...) \
    inline constexpr auto name = ::maicord::protocol_detail::make_field_table<::maicord::protocol_detail::field_table_bytes(defs)>( \
        defs, ::maicord::protocol_detail::ObjectSpec{__VA_ARGS__})

#define MAICORD_OFFSET(S, m) static_cast<::maicord::u16>(__builtin_offsetof(S, m))
// The width of a member of S as info bits: log2 of its size (u8 to u64) in bits 4-5.
#define MAICORD_WIDTH(S, m) \
    static_cast<::maicord::u8>((sizeof(S::m) == 1 ? 0 : sizeof(S::m) == 2 ? 1 : sizeof(S::m) == 4 ? 2 : 3) << 4)
// A member filled in place: its kind, extra flags (kFieldNull, ...), the member of S it fills.
#define MAICORD_ROW(kind, flags, S, m) \
    ::maicord::protocol_detail::kind | MAICORD_WIDTH(S, m) | (flags), MAICORD_OFFSET(S, m), 0, 0
#define MAICORD_FIELD_SNOWFLAKE(S, m) MAICORD_ROW(field_snowflake, 0, S, m)
#define MAICORD_FIELD_SNOWFLAKE_OR_NULL(S, m) MAICORD_ROW(field_snowflake, ::maicord::protocol_detail::kFieldNull, S, m)
#define MAICORD_FIELD_BOOL(S, m) MAICORD_ROW(field_bool, 0, S, m)
#define MAICORD_FIELD_BOOL_OR_NULL(S, m) MAICORD_ROW(field_bool, ::maicord::protocol_detail::kFieldNull, S, m)
#define MAICORD_FIELD_TIMESTAMP_OR_NULL(S, m) MAICORD_ROW(field_timestamp, ::maicord::protocol_detail::kFieldNull, S, m)
#define MAICORD_FIELD_TIMESTAMP(S, m) MAICORD_ROW(field_timestamp, 0, S, m)
// A number up to what its member holds.
#define MAICORD_FIELD_NUMBER(S, m) MAICORD_ROW(field_number, 0, S, m)
#define MAICORD_FIELD_NUMBER_OR_NULL(S, m) MAICORD_ROW(field_number, ::maicord::protocol_detail::kFieldNull, S, m)
// A number cut to what its member holds instead of refused.
#define MAICORD_FIELD_SATURATE(S, m) MAICORD_ROW(field_saturate, 0, S, m)
// A text into S::m, its length into S::counter (u8 or u16); or a label.
#define MAICORD_TEXT_ROW(kind, flags, S, m, counter) \
    ::maicord::protocol_detail::kind | MAICORD_WIDTH(S, counter) | (flags), MAICORD_OFFSET(S, m), \
        static_cast<::maicord::u16>(sizeof(S::m)), MAICORD_OFFSET(S, counter)
#define MAICORD_FIELD_TEXT(S, m, counter) MAICORD_TEXT_ROW(field_text, 0, S, m, counter)
#define MAICORD_FIELD_TEXT_OR_NULL(S, m, counter) MAICORD_TEXT_ROW(field_text, ::maicord::protocol_detail::kFieldNull, S, m, counter)
// The same, cut to fit (see ObjectSpec::text); null is valid.
#define MAICORD_FIELD_LABEL(S, m, counter) MAICORD_TEXT_ROW(field_label, ::maicord::protocol_detail::kFieldNull, S, m, counter)
#define MAICORD_FIELD_HOOK ::maicord::protocol_detail::field_hook, 0, 0, 0
// A hook row that may be null: null is valid without calling the hook.
#define MAICORD_FIELD_HOOK_OR_NULL ::maicord::protocol_detail::field_hook | ::maicord::protocol_detail::kFieldNull, 0, 0, 0

// What every read keeps: the document and the first error (0: none).
struct ReadState {
    const JsonIndexedDocument* document;
    u8 error;
};
// An object being read: the struct it fills and the members seen so far.
struct ObjectReader : ReadState {
    const ObjectSpec* spec;
    u8* base;
    u64 seen;
};

// Fails the reader with `code` unless it already failed.
inline bool reader_fail(ObjectReader& reader, u8 code) noexcept {
    if (!reader.error) reader.error = code;
    return false;
}

inline u32 field_u16(const u8* at) noexcept {
    return *reinterpret_cast<const u16 __unaligned*>(at);
}

// Whether a member that does not fit is passed over: the table says so (no `field` code), unless the member is required.
inline bool skipped(const ObjectSpec& spec, u32 index) noexcept {
    return !spec.codes.field && !(spec.required & (1ull << index));
}

// A string cut at a code point boundary to `capacity` bytes, in `copied`. False when it is not a string or decodes to more
// than `limit` (at most 4096) bytes, and then nothing was copied.
__declspec(noinline) inline bool clip_text(const JsonValue& value, char* output, u32 capacity, u32 limit,
                                           u32* copied) noexcept {
    JsonString string{};
    char decoded[4096];
    *copied = 0;
    if (!json_value_as_string(value, &string))
        return false;
    const auto result = json_string_decode_utf8(string, {decoded, limit});
    if (result.ok())
        *copied = utf8_copy_truncated(output, capacity, {decoded, result.bytes});
    return result.ok();
}
// The bytes clip_text copied; 0 when it copied none.
inline u32 clip_copy(const JsonValue& value, char* output, u32 capacity, u32 limit) noexcept {
    u32 copied;
    clip_text(value, output, capacity, limit, &copied);
    return copied;
}

__declspec(noinline) inline bool read_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& r = *static_cast<ObjectReader*>(opaque);
    const ObjectSpec& spec = *r.spec;
    const u8* names = reinterpret_cast<const u8*>(&spec + 1);
    const u8* const hashes = names + spec.names_bytes;
    u32 index = names[0];
    if (name.has_escapes) {
        index = member_index(name, names);
    } else if (name.encoded.bytes) {
        const char* const text = name.encoded.data;
        const u32 bytes = name.encoded.bytes;
        const u8 hash = field_name_hash(bytes, static_cast<u8>(text[0]), static_cast<u8>(text[bytes - 1]));
        for (u32 i = 0; i < names[0]; ++i) {
            if (hashes[i] == hash && name_matches(name, names + hashes[names[0] + i])) {
                index = i;
                break;
            }
        }
    }
    if (index == names[0])
        return true;
    if (spec.codes.duplicate) {
        const u64 bit = 1ull << index;
        if (r.seen & bit) {
            r.error = spec.codes.duplicate;
            return false;
        }
        r.seen |= bit;
    } else {
        r.seen |= 1ull << index;
    }
    const u8* const row = hashes + 3 * names[0] + hashes[2 * names[0] + index];
    const u32 info = row[0];
    const u32 kind = info & 7;
    const bool null = value.kind == JsonValueKind::null_value && (info & kFieldNull);
    if (kind == field_hook)
        return null || spec.hook(r, index, value) || skipped(spec, index) || reader_fail(r, spec.codes.field);
    u8* const dst = r.base + field_u16(row + 1);
    // A number, a cleared member or a text's length is stored last, in 1, 2, 4 or 8 bytes (zero unless `stored` says).
    u8* store_at = nullptr;
    u64 stored = 0;
    bool ok = true;
    if (null) {
        // Null clears an id, a time or a text, a number when the row says so, and leaves a flag; a hook is not asked.
        if ((0x63u >> kind & 1) || (info & kFieldNullClears))
            store_at = kind >= field_text ? r.base + field_u16(row + 5) : dst;
    } else {
        switch (kind) {
        case field_snowflake: ok = json_value_to_snowflake(value, reinterpret_cast<u64*>(dst)); break;
        case field_timestamp: ok = json_value_to_timestamp(value, reinterpret_cast<u64*>(dst)); break;
        case field_bool: ok = json_value_to_bool(value, reinterpret_cast<bool*>(dst)); break;
        case field_number:
        case field_saturate: {
            // What the member holds: 0xFF, 0xFFFF or 0xFFFFFFFF.
            const u64 limit = 0xFFFFFFFFu >> (32 - (8 << (info >> 4 & 3)));
            ok = json_value_to_u64(value, &stored);
            if (kind == field_saturate)
                stored = minimum<u64>(stored, limit);
            else
                ok = ok && stored <= limit;
            if (ok)
                store_at = dst;
            break;
        }
        case field_text: {
            JsonString text{};
            ok = json_value_as_string(value, &text);
            if (ok) {
                const auto decoded = json_string_decode_utf8(text, {reinterpret_cast<char*>(dst), field_u16(row + 3)});
                ok = decoded.ok();
                stored = decoded.bytes;
            }
            if (ok)
                store_at = r.base + field_u16(row + 5);
            break;
        }
        default: {
            // Cut at a code point boundary to fit; anything but a string is refused and empties it, and so is one that
            // decodes to too many bytes (it reads empty).
            const u32 limit = (spec.text ? spec.text : 4) << 8;
            u32 copied;
            ok = value.kind == JsonValueKind::string;
            clip_text(value, reinterpret_cast<char*>(dst), field_u16(row + 3), limit, &copied);
            stored = copied;
            store_at = r.base + field_u16(row + 5);
            break;
        }
        }
    }
    if (store_at) {
        switch (info & 0x30) {
        case 0x00: *store_at = static_cast<u8>(stored); break;
        case 0x10: *reinterpret_cast<u16 __unaligned*>(store_at) = static_cast<u16>(stored); break;
        case 0x20: *reinterpret_cast<u32 __unaligned*>(store_at) = static_cast<u32>(stored); break;
        default: *reinterpret_cast<u64 __unaligned*>(store_at) = stored; break;
        }
    }
    if (ok && (info & kFieldNotify))
        ok = spec.hook(r, index, value);
    return ok || skipped(spec, index) || reader_fail(r, spec.codes.field);
}

// Reads the members of the object `value` into reader.base. The first error's code, or 0.
__declspec(noinline) inline u8 read_object(ObjectReader& r, const JsonValue& value) noexcept {
    if (json_visit_indexed_object(*r.document, value, r.spec->depth, read_member, &r).ok())
        return 0;
    return r.error ? r.error : r.spec->codes.json;
}

// An array of objects read into rows of spec.size bytes (zeroed first).
struct ItemReader : ReadState {
    const ObjectSpec* spec;
    u8* rows;
    u32 capacity, count;
};

// One element: a full array, a non-object, a failed read, a missing required member and a repeated key each fail.
__declspec(noinline) inline bool read_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ItemReader*>(opaque);
    const ObjectSpec& spec = *p.spec;
    const auto fail = [&](u8 code) noexcept {
        if (!p.error) p.error = code;
        return false;
    };
    if (p.count >= p.capacity)
        return fail(spec.codes.capacity);
    if (value.kind != JsonValueKind::object)
        return fail(spec.codes.field);
    u8* row = p.rows + p.count * spec.size;
    clear_bytes(row, spec.size);
    ObjectReader r{{p.document, 0}, &spec, row, 0};
    if (const u8 result = read_object(r, value))
        return fail(result);
    if ((r.seen & spec.required) != spec.required)
        return fail(spec.codes.identity);
    for (u32 i = 0; spec.key_size && i < p.count; ++i)
        if (bytes_equal(p.rows + i * spec.size + spec.key_offset, row + spec.key_offset, spec.key_size))
            return fail(spec.codes.duplicate);
    ++p.count;
    return true;
}

} // namespace maicord::protocol_detail
