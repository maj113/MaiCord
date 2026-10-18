#include "maicord/protocol/user_settings_proto.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/guild_folders.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "proto_wire.hpp"

namespace maicord {
namespace {
using namespace protocol_detail;
using E = UserSettingsProtoError;
using K = UserSettingsKind;

using Wire = ProtoSource;
// Field boundaries: [begin, p) is the whole field, [body, stop) the payload of
// a length-delimited field.
struct Field {
    u32 number, type, begin, body, stop;
};
bool next(const Wire& w, u32& p, u32 end, Field* f) noexcept {
    f->begin = p;
    if (!proto_tag(w, p, end, f->number, f->type))
        return false;
    if (f->type == 2) {
        if (!proto_length(w, p, end, f->stop))
            return false;
        f->body = p;
        p = f->stop;
        return true;
    }
    f->body = f->stop = p;
    return proto_skip(w, p, end, f->type, f->number);
}
bool valid(const Wire& w, u32 p, u32 end) noexcept {
    Field f{};
    while (p < end)
        if (!next(w, p, end, &f))
            return false;
    return true;
}
bool wrapper_kind(K kind) noexcept {
    return kind == K::boolean || kind == K::uint32 || kind == K::uint64 || kind == K::string;
}
// Merges one occurrence of a leaf into value.
bool read_leaf(const Wire& w, const Field& f, K kind, UserSettingsValue* value) noexcept {
    if (!wrapper_kind(kind)) {
        if (f.type != 0)
            return false;
        u32 p = f.begin, number = 0, type = 0;
        u64 n = 0;
        if (!proto_tag(w, p, w.size, number, type) || !proto_varint(w, p, w.size, n))
            return false;
        value->number = kind == K::plain_bool ? (n != 0) : n;
        value->present = true;
        return true;
    }
    if (f.type != 2)
        return false;
    value->present = true;
    u32 p = f.body;
    Field inner{};
    while (p < f.stop) {
        if (!next(w, p, f.stop, &inner))
            return false;
        if (inner.number != 1)
            continue;
        if (kind == K::string) {
            if (inner.type != 2 || inner.stop - inner.body > kUserSettingsTextBytes)
                return false;
            value->text_bytes = static_cast<u8>(inner.stop - inner.body);
            for (u32 i = 0; i < value->text_bytes; ++i)
                value->text[i] = static_cast<char>(w.at(inner.body + i));
        } else {
            u64 n = 0;
            u32 q = inner.begin;
            u32 number = 0, type = 0;
            if (inner.type != 0 || !proto_tag(w, q, f.stop, number, type) || !proto_varint(w, q, f.stop, n))
                return false;
            value->number = kind == K::boolean ? (n != 0) : kind == K::uint32 ? static_cast<u32>(n) : n;
        }
    }
    return true;
}
void write_leaf(ProtoWriter& out, u32 field, K kind, const UserSettingsValue& value) noexcept {
    if (!value.present)
        return;
    if (!wrapper_kind(kind)) {
        if (!value.number)
            return; // proto3 default
        out.tag(field, 0);
        out.varint(kind == K::plain_bool ? 1 : value.number);
        return;
    }
    out.tag(field, 2);
    if (kind == K::string) {
        if (!value.text_bytes) {
            out.byte(0);
            return;
        }
        out.varint(value.text_bytes + 1u + proto_varint_size(value.text_bytes));
        out.byte(10);
        out.varint(value.text_bytes);
        out.bytes(value.text, value.text_bytes);
        return;
    }
    const u64 n = kind == K::boolean ? (value.number != 0) : value.number;
    if (!n) {
        out.byte(0);
        return;
    }
    out.varint(1 + proto_varint_size(n));
    out.byte(8);
    out.varint(n);
}
bool field_valid(UserSettingsField f) noexcept {
    return f.group && f.group < 32 && f.field && f.child < 128 && f.field < 128 &&
           static_cast<u8>(f.kind) <= static_cast<u8>(K::id_member) && (f.kind != K::id_member || !f.child);
}
// Visits every ID of a repeated fixed64 field, packed (wire type 2) or not
// (wire type 1), in wire order. False for malformed input or when visit
// stops the walk.
template <class Visit>
bool visit_ids(const Wire& w, u32 number, Visit&& visit) noexcept {
    u32 p = 0;
    Field f{};
    while (p < w.size) {
        if (!next(w, p, w.size, &f))
            return false;
        if (f.number != number)
            continue;
        if (f.type == 1) {
            if (!visit(proto_fixed64(w, f.body)))
                return false;
        } else if (f.type == 2 && (f.stop - f.body) % 8 == 0) {
            for (u32 q = f.body; q < f.stop; q += 8)
                if (!visit(proto_fixed64(w, q)))
                    return false;
        } else {
            return false;
        }
    }
    return true;
}
// Concatenated contents of every occurrence of `number` in [p, end).
u32 group_bytes(const Wire& w, u32 p, u32 end, u32 number, bool* present) noexcept {
    u32 total = 0;
    Field f{};
    while (p < end && next(w, p, end, &f))
        if (f.number == number && f.type == 2) {
            total += f.stop - f.body;
            *present = true;
        }
    return total;
}
UserSettingsExtractResult extract(const Wire& w, u32 mask, Utf8View hash, Slice<u8> output) noexcept {
    UserSettingsExtractResult result{};
    if (!valid(w, 0, w.size)) {
        result.error = E::protobuf;
        return result;
    }
    ProtoWriter out{output, 0, true};
    for (u32 group = 1; group < 32; ++group) {
        if (!(mask & (1u << group)))
            continue;
        bool present = false;
        const u32 bytes = group_bytes(w, 0, w.size, group, &present);
        if (!present)
            continue;
        if (bytes > kUserSettingsProtoMaximumGroupBytes) {
            result.error = E::capacity;
            return result;
        }
        result.present_mask |= 1u << group;
        out.tag(group, 2);
        out.varint(bytes);
        u32 p = 0;
        Field f{};
        while (p < w.size && next(w, p, w.size, &f))
            if (f.number == group && f.type == 2)
                out.copy(w, f.body, f.stop);
    }
    if (hash.bytes) {
        out.tag(kUserSettingsSessionHashField, 2);
        out.varint(hash.bytes);
        out.bytes(hash.data, hash.bytes);
    }
    if (!out.ok) {
        result.error = E::capacity;
        return result;
    }
    result.bytes = out.at;
    return result;
}
bool range_valid(Slice<u8> s) noexcept {
    return byte_range_valid({s.data, s.count});
}
// Writes the tag and length of a length-delimited field into five bytes
// reserved at start and shifts its contents down behind them. Returns the end
// of the field, or ~0u.
u32 close_reserved_field(Slice<u8> output, u32 start, u32 field, u32 contents) noexcept {
    u8 prefix[5]{};
    ProtoWriter head{{prefix, 5}, 0, true};
    head.tag(field, 2);
    head.varint(contents);
    if (!head.ok)
        return ~0u;
    copy_bytes(output.data + start + head.at, output.data + start + 5, contents);
    copy_bytes(output.data + start, prefix, head.at);
    return start + head.at + contents;
}
} // namespace

UserSettingsValue user_settings_text(Utf8View text) noexcept {
    UserSettingsValue value{0, {}, 0, true};
    const u32 bytes = minimum(text.bytes, kUserSettingsTextBytes);
    if (text.data)
        copy_bytes(value.text, text.data, bytes);
    value.text_bytes = static_cast<u8>(bytes);
    return value;
}
bool user_settings_value_equal(const UserSettingsValue& a, const UserSettingsValue& b) noexcept {
    return a.present == b.present && a.number == b.number && a.text_bytes == b.text_bytes &&
           bytes_equal(a.text, b.text, a.text_bytes);
}

bool user_settings_proto_valid(ConstSlice<u8> message) noexcept {
    return byte_range_valid(message) && valid(proto_source(message), 0, message.count);
}

bool user_settings_proto_read(ConstSlice<u8> group, UserSettingsField field, UserSettingsValue* value) noexcept {
    if (!value || !field_valid(field) || field.kind == K::id_member || !byte_range_valid(group))
        return false;
    UserSettingsValue result{};
    const Wire w = proto_source(group);
    u32 p = 0;
    Field f{};
    while (p < w.size) {
        if (!next(w, p, w.size, &f))
            return false;
        if (f.number != field.field)
            continue;
        if (!field.child) {
            if (!read_leaf(w, f, field.kind, &result))
                return false;
            continue;
        }
        if (f.type != 2)
            return false;
        u32 q = f.body;
        Field inner{};
        while (q < f.stop) {
            if (!next(w, q, f.stop, &inner))
                return false;
            if (inner.number == field.child && !read_leaf(w, inner, field.kind, &result))
                return false;
        }
    }
    *value = result;
    return result.present;
}

bool user_settings_proto_contains(ConstSlice<u8> group, UserSettingsField field, u64 id) noexcept {
    if (!id || !field_valid(field) || field.kind != K::id_member || !byte_range_valid(group))
        return false;
    bool found = false;
    const bool walked = visit_ids(proto_source(group), field.field, [&](u64 n) noexcept {
        found = found || n == id;
        return true;
    });
    return walked && found;
}

u32 user_settings_proto_rewrite(ConstSlice<u8> group, UserSettingsField field, const UserSettingsValue& value,
                                Slice<u8> output) noexcept {
    if (!field_valid(field) || !byte_range_valid(group) || !range_valid(output) ||
        byte_ranges_overlap(group, {output.data, output.count}) || value.text_bytes > kUserSettingsTextBytes)
        return ~0u;
    const Wire w = proto_source(group);
    if (!valid(w, 0, w.size))
        return ~0u;
    ProtoWriter out{output, 0, true};
    u32 p = 0;
    Field f{};
    while (p < w.size) {
        next(w, p, w.size, &f);
        if (f.number != field.field)
            out.copy(w, f.begin, p);
        else if (field.child && f.type != 2)
            return ~0u;
    }
    if (field.kind == K::id_member) {
        if (!value.number)
            return ~0u;
        // One packed occurrence after the other fields; the tag and length are
        // written into five reserved bytes and the IDs shifted down after.
        const u32 start = out.at;
        out.at += 5;
        u32 count = 0;
        bool placed = false;
        const auto append = [&](u64 id) noexcept {
            if (!out.ok)
                return false;
            for (u32 i = 0; i < count; ++i) {
                u64 seen = 0;
                copy_bytes(&seen, output.data + start + 5 + i * 8, 8);
                if (seen == id)
                    return true;
            }
            u8 bytes[8];
            for (u32 i = 0; i < 8; ++i)
                bytes[i] = static_cast<u8>(id >> (i * 8));
            out.bytes(bytes, 8);
            ++count;
            return out.ok;
        };
        const bool walked = visit_ids(w, field.field, [&](u64 id) noexcept {
            if (id == value.number) {
                if (!value.present || placed)
                    return true;
                placed = true;
            }
            return append(id);
        });
        if (!walked || (value.present && !placed && !append(value.number)) || !out.ok)
            return ~0u;
        if (!count) {
            out.at = start;
            return out.at;
        }
        return close_reserved_field(output, start, field.field, count * 8);
    }
    if (!field.child) {
        write_leaf(out, field.field, field.kind, value);
        return out.ok ? out.at : ~0u;
    }
    // Merge the parent's occurrences into one sub-message, dropping the child.
    // Five reserved bytes hold the tag and length; contents are shifted down.
    const u32 start = out.at;
    out.at += 5;
    p = 0;
    bool parent = false;
    while (p < w.size) {
        next(w, p, w.size, &f);
        if (f.number != field.field)
            continue;
        parent = true;
        u32 q = f.body;
        Field inner{};
        while (q < f.stop) {
            if (!next(w, q, f.stop, &inner))
                return ~0u;
            if (inner.number != field.child)
                out.copy(w, inner.begin, q);
        }
    }
    write_leaf(out, field.child, field.kind, value);
    if (!out.ok)
        return ~0u;
    const u32 contents = out.at - start - 5;
    if (!contents && !parent) {
        out.at = start;
        return out.at;
    }
    return close_reserved_field(output, start, field.field, contents);
}

UserSettingsExtractResult user_settings_proto_extract_base64(Utf8View base64, u32 mask, Utf8View hash,
                                                             Slice<u8> output) noexcept {
    UserSettingsExtractResult result{0, 0, E::input};
    if (base64.bytes > base64_encoded_size(kUserSettingsProtoMaximumProtoBytes) || !range_valid(output) ||
        (base64.bytes && !base64.data) || hash.bytes > kUserSettingsSessionHashBytes ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(base64.data), base64.bytes}, {output.data, output.count}))
        return result;
    const auto measured = base64_decode(base64, {});
    if (!measured.ok() && measured.error != Base64Error::output_too_small) {
        result.error = E::base64;
        return result;
    }
    return extract({{}, base64, measured.bytes}, mask, hash, output);
}

ConstSlice<u8> user_settings_proto_group(ConstSlice<u8> extracted, u32 group) noexcept {
    const Wire w = proto_source(extracted);
    u32 p = 0;
    Field f{};
    while (p < w.size && next(w, p, w.size, &f))
        if (f.number == group && f.type == 2)
            return {extracted.data + f.body, f.stop - f.body};
    return {};
}

bool user_settings_proto_partial(ConstSlice<u8> extracted) noexcept {
    const Wire w = proto_source(extracted);
    u32 p = 0;
    Field f{};
    while (p < w.size && next(w, p, w.size, &f))
        if (f.number == kUserSettingsPartialField && f.type == 0) {
            u32 q = f.begin, number = 0, type = 0;
            u64 n = 0;
            return proto_tag(w, q, w.size, number, type) && proto_varint(w, q, w.size, n) && n == 1;
        }
    return false;
}

bool user_settings_proto_patch_valid(ConstSlice<u8> partial) noexcept {
    if (!partial.count || partial.count > kUserSettingsPartialMaximumBytes || !byte_range_valid(partial))
        return false;
    const Wire w = proto_source(partial);
    u32 p = 0;
    Field f{};
    u32 fields = 0;
    bool folders = false;
    while (p < w.size) {
        if (!next(w, p, w.size, &f) || f.type != 2 || f.number < 2 || f.number > 31 ||
            f.number == user_settings_status)
            return false;
        ++fields;
        // The guild folders owner sends its group alone, as one valid message.
        if (f.number == kGuildFoldersSettingsField) {
            folders = true;
            if (!valid(w, f.body, f.stop))
                return false;
        }
    }
    return !folders || fields == 1;
}

u32 user_settings_proto_write_patch_json(ConstSlice<u8> partial, u32 version, Slice<u8> output,
                                         bool required) noexcept {
    if (!user_settings_proto_patch_valid(partial) || !range_valid(output) ||
        byte_ranges_overlap(partial, {output.data, output.count}))
        return 0;
    const u32 bytes = proto_settings_patch_bytes(partial.count, version, required);
    if (output.count < bytes || !proto_write_settings_patch(partial, version, output, required))
        return 0;
    return bytes;
}

namespace {
struct Envelope {
    JsonString settings;
    bool settings_seen, out_of_date_seen, out_of_date;
};
bool envelope_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& e = *static_cast<Envelope*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("settings"))) {
        if (e.settings_seen || !json_value_as_string(value, &e.settings))
            return false;
        e.settings_seen = true;
    } else if (json_string_equals_ascii(name, utf8_literal("out_of_date"))) {
        if (e.out_of_date_seen || !json_value_to_bool(value, &e.out_of_date))
            return false;
        e.out_of_date_seen = true;
    }
    return true;
}
} // namespace

UserSettingsResponse user_settings_proto_decode_response(Utf8View body, u32 mask, Slice<char> unescape,
                                                         Slice<u8> output) noexcept {
    UserSettingsResponse result{{0, 0, E::input}, false, false};
    if (!body.data || !body.bytes)
        return result;
    u32 indexes[64]{};
    const auto scanned = json_scan_structurals_avx1(body, {indexes, 64});
    Envelope envelope{};
    if (!scanned.ok() || !json_visit_root_object(body, {indexes, scanned.count}, 8, envelope_member, &envelope).ok()) {
        result.extracted.error = E::protobuf;
        return result;
    }
    result.out_of_date = envelope.out_of_date;
    result.settings_present = envelope.settings_seen;
    if (!envelope.settings_seen)
        return result;
    Utf8View encoded = envelope.settings.encoded;
    if (envelope.settings.has_escapes) {
        const auto decoded = json_string_decode_utf8(envelope.settings, unescape);
        if (!decoded.ok()) {
            result.extracted.error = E::capacity;
            return result;
        }
        encoded = {unescape.data, decoded.bytes};
    }
    result.extracted = user_settings_proto_extract_base64(encoded, mask, {}, output);
    return result;
}

void user_settings_store_reset(UserSettingsStore* store) noexcept {
    if (!store)
        return;
    const u64 revision = store->revision + 1;
    clear_bytes(store->dirty, sizeof(store->dirty));
    clear_bytes(store->inflight, sizeof(store->inflight));
    clear_bytes(store->session_hash, sizeof(store->session_hash));
    store->server_bytes = store->present_mask = store->data_version = 0;
    store->dirty_count = store->inflight_count = store->offline_data_version = 0;
    store->session_hash_bytes = 0;
    store->due_ms = 0;
    store->loaded = store->live = store->inflight_active = store->refetch = store->offline_pinned = false;
    store->revision = revision;
}

ConstSlice<u8> user_settings_store_server_group(const UserSettingsStore& store, u32 group) noexcept {
    return user_settings_proto_group({store.server, store.server_bytes}, group);
}

bool user_settings_store_apply(UserSettingsStore* store, ConstSlice<u8> extracted, bool partial) noexcept {
    if (!store || !user_settings_proto_valid(extracted) || extracted.count > kUserSettingsProtoArenaBytes)
        return false;
    ProtoWriter out{{store->scratch, kUserSettingsProtoArenaBytes}, 0, true};
    u32 present = 0;
    for (u32 group = 1; group < 32; ++group) {
        if (!(kUserSettingsStoredGroups & (1u << group)))
            continue;
        bool incoming = false;
        group_bytes(proto_source(extracted), 0, extracted.count, group, &incoming);
        const ConstSlice<u8> source = incoming ? user_settings_proto_group(extracted, group)
                                      : partial ? user_settings_store_server_group(*store, group)
                                                : ConstSlice<u8>{};
        const bool kept = incoming || (partial && (store->present_mask & (1u << group)));
        if (!kept)
            continue;
        present |= 1u << group;
        out.tag(group, 2);
        out.varint(source.count);
        out.bytes(source.data, source.count);
    }
    if (!out.ok)
        return false;
    copy_bytes(store->server, store->scratch, out.at);
    store->server_bytes = out.at;
    store->present_mask = present;
    UserSettingsValue version{};
    store->data_version =
        user_settings_proto_read(user_settings_store_server_group(*store, user_settings_versions),
                                 {user_settings_versions, 3, 0, K::enumeration}, &version)
            ? static_cast<u32>(version.number)
            : 0;
    const auto hash = user_settings_proto_group(extracted, kUserSettingsSessionHashField);
    if (hash.count && hash.count <= kUserSettingsSessionHashBytes) {
        copy_bytes(store->session_hash, hash.data, hash.count);
        store->session_hash_bytes = static_cast<u8>(hash.count);
    }
    if (!partial)
        store->loaded = true;
    ++store->revision;
    return true;
}

namespace {
// Edits address a field; id_member edits address one ID of it (`key`).
bool edit_matches(const UserSettingsEdit& edit, UserSettingsField field, u64 key) noexcept {
    return user_settings_field_equal(edit.field, field) && (field.kind != K::id_member || edit.value.number == key);
}
const UserSettingsEdit* find(const UserSettingsEdit* edits, u32 count, UserSettingsField field, u64 key) noexcept {
    for (u32 i = count; i--;)
        if (edit_matches(edits[i], field, key))
            return &edits[i];
    return nullptr;
}
bool base_value(const UserSettingsStore& store, UserSettingsField field, u64 key, UserSettingsValue* value) noexcept {
    if (const auto* edit = find(store.inflight, store.inflight_count, field, key)) {
        *value = edit->value;
        return value->present;
    }
    const auto group = user_settings_store_server_group(store, field.group);
    if (field.kind == K::id_member) {
        *value = user_settings_member(key, user_settings_proto_contains(group, field, key));
        return value->present;
    }
    *value = {};
    return user_settings_proto_read(group, field, value);
}
} // namespace

bool user_settings_store_read(const UserSettingsStore& store, UserSettingsField field,
                              UserSettingsValue* value) noexcept {
    if (!value || !field_valid(field) || field.kind == K::id_member)
        return false;
    if (const auto* edit = find(store.dirty, store.dirty_count, field, 0)) {
        *value = edit->value;
        return value->present;
    }
    return base_value(store, field, 0, value);
}

bool user_settings_store_contains(const UserSettingsStore& store, UserSettingsField field, u64 id) noexcept {
    if (!id || !field_valid(field) || field.kind != K::id_member)
        return false;
    if (const auto* edit = find(store.dirty, store.dirty_count, field, id))
        return edit->value.present;
    UserSettingsValue value{};
    return base_value(store, field, id, &value);
}

bool user_settings_store_set(UserSettingsStore* store, UserSettingsField field, const UserSettingsValue& value,
                             u64 now_ms) noexcept {
    if (!store || !store->loaded || !field_valid(field) || !(kUserSettingsStoredGroups & (1u << field.group)) ||
        field.group == user_settings_versions || value.text_bytes > kUserSettingsTextBytes ||
        (field.kind == K::id_member && (!value.number || value.text_bytes)))
        return false;
    const u64 key = field.kind == K::id_member ? value.number : 0;
    u32 index = store->dirty_count;
    for (u32 i = 0; i < store->dirty_count; ++i)
        if (edit_matches(store->dirty[i], field, key))
            index = i;
    UserSettingsValue base{};
    base_value(*store, field, key, &base);
    const bool redundant = user_settings_value_equal(base, value) ||
                           (!base.present && value.present && !value.number && !value.text_bytes &&
                            (field.kind == K::enumeration || field.kind == K::plain_bool));
    if (redundant) {
        if (index == store->dirty_count)
            return true;
        for (u32 i = index + 1; i < store->dirty_count; ++i)
            store->dirty[i - 1] = store->dirty[i];
        store->dirty[--store->dirty_count] = {};
        if (!store->dirty_count)
            store->due_ms = 0;
        ++store->revision;
        return true;
    }
    if (index == store->dirty_count) {
        if (store->dirty_count == kUserSettingsEditCapacity)
            return false;
        ++store->dirty_count;
    }
    store->dirty[index] = {field, value};
    if (!store->due_ms)
        store->due_ms = now_ms + kUserSettingsBatchMilliseconds;
    if (!store->live && !store->offline_pinned) {
        store->offline_data_version = store->data_version;
        store->offline_pinned = true;
    }
    ++store->revision;
    return true;
}

bool user_settings_store_due(const UserSettingsStore& store, u64 now_ms) noexcept {
    return store.loaded && store.live && store.dirty_count && !store.inflight_active && store.due_ms &&
           now_ms >= store.due_ms;
}

u32 user_settings_store_begin(UserSettingsStore* store, Slice<u8> partial, u32* version, bool* required) noexcept {
    if (!store || !store->dirty_count || store->inflight_active || !range_valid(partial) || !version)
        return 0;
    u32 groups = 0;
    for (u32 i = 0; i < store->dirty_count; ++i)
        groups |= 1u << store->dirty[i].field.group;
    constexpr u32 half = kUserSettingsProtoArenaBytes / 2;
    ProtoWriter out{partial, 0, true};
    for (u32 group = 2; group < 32; ++group) {
        if (!(groups & (1u << group)))
            continue;
        ConstSlice<u8> current = user_settings_store_server_group(*store, group);
        u32 which = 0;
        for (u32 i = 0; i < store->dirty_count; ++i) {
            if (store->dirty[i].field.group != group)
                continue;
            u8* target = store->scratch + which * half;
            const u32 bytes = user_settings_proto_rewrite(current, store->dirty[i].field, store->dirty[i].value,
                                                          {target, half});
            if (bytes == ~0u)
                return 0;
            current = {target, bytes};
            which ^= 1;
        }
        out.tag(group, 2);
        out.varint(current.count);
        out.bytes(current.data, current.count);
    }
    if (!out.ok || !out.at)
        return 0;
    copy_bytes(store->inflight, store->dirty, store->dirty_count * static_cast<u32>(sizeof(UserSettingsEdit)));
    store->inflight_count = store->dirty_count;
    clear_bytes(store->dirty, sizeof(store->dirty));
    store->dirty_count = 0;
    store->due_ms = 0;
    store->inflight_active = true;
    *version = store->offline_data_version;
    if (required)
        *required = store->offline_pinned;
    store->offline_data_version = 0;
    store->offline_pinned = false;
    return out.at;
}

UserSettingsCompletion user_settings_store_complete(UserSettingsStore* store, UserSettingsOutcome outcome,
                                                    ConstSlice<u8> response, u64 retry_at_ms) noexcept {
    UserSettingsCompletion result{};
    if (!store || !store->inflight_active)
        return result;
    if (outcome == UserSettingsOutcome::rate_limited) {
        // Pending edits are newer; in-flight ones they replace are dropped.
        UserSettingsEdit merged[kUserSettingsEditCapacity]{};
        u32 count = 0;
        for (u32 i = 0; i < store->inflight_count; ++i)
            if (!find(store->dirty, store->dirty_count, store->inflight[i].field, store->inflight[i].value.number))
                merged[count++] = store->inflight[i];
        for (u32 i = 0; i < store->dirty_count && count < kUserSettingsEditCapacity; ++i)
            merged[count++] = store->dirty[i];
        copy_bytes(store->dirty, merged, sizeof(merged));
        store->dirty_count = count;
        store->due_ms = retry_at_ms ? retry_at_ms : 1;
        result.retry = true;
    } else {
        const bool snapshot = (outcome == UserSettingsOutcome::saved || outcome == UserSettingsOutcome::out_of_date) &&
                              response.count && user_settings_store_apply(store, response, false);
        result.rolled_back = outcome != UserSettingsOutcome::saved || !snapshot;
        store->refetch = outcome == UserSettingsOutcome::invalid_data ||
                         (outcome == UserSettingsOutcome::saved && !snapshot);
        if (store->dirty_count && !store->due_ms)
            store->due_ms = 1;
    }
    clear_bytes(store->inflight, sizeof(store->inflight));
    store->inflight_count = 0;
    store->inflight_active = false;
    ++store->revision;
    return result;
}

} // namespace maicord
