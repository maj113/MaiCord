#include "maicord/protocol/relationships.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "json_helpers.hpp"
#include "maicord/protocol/rest_route.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {
namespace {
using protocol_detail::decode_text;
RelationshipsDecodeResult failure(RelationshipsDecodeError error) noexcept { return {error, 0, 0, false}; }
struct Parse {
    const JsonIndexedDocument* document;
    RelationshipMutation mutation;
    JsonValue user;
    u32 seen, user_seen;
    bool nickname_null;
    RelationshipsDecodeError error;
};
bool mark(Parse& parse, u32* seen, u32 bit) noexcept {
    if (*seen & bit) { parse.error = RelationshipsDecodeError::duplicate; return false; }
    *seen |= bit;
    return true;
}
bool visit_user(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(opaque);
    auto& user = parse.mutation.user;
    bool valid = true;
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (!mark(parse, &parse.user_seen, 1)) return false;
        valid = json_value_to_snowflake(value, &user.id) && user.id;
    } else if (json_string_equals_ascii(name, utf8_literal("username"))) {
        if (!mark(parse, &parse.user_seen, 2)) return false;
        valid = decode_text(value, user.username, sizeof(user.username), &user.username_bytes, false) && user.username_bytes;
    } else if (json_string_equals_ascii(name, utf8_literal("global_name"))) {
        if (!mark(parse, &parse.user_seen, 4)) return false;
        valid = decode_text(value, user.display_name, sizeof(user.display_name), &user.display_name_bytes, true);
    } else if (json_string_equals_ascii(name, utf8_literal("avatar"))) {
        if (!mark(parse, &parse.user_seen, 8)) return false;
        user.avatar_seen = true;
        u16 bytes = 0;
        valid = decode_text(value, user.avatar, sizeof(user.avatar), &bytes, true);
        if (valid && bytes) {
            const u32 prefix = bytes == 34 && user.avatar[0] == 'a' && user.avatar[1] == '_' ? 2u : 0u;
            valid = bytes == 32 || prefix == 2;
            for (u32 index = prefix; valid && index < bytes; ++index)
                valid = (user.avatar[index] >= '0' && user.avatar[index] <= '9') ||
                    (user.avatar[index] >= 'a' && user.avatar[index] <= 'f');
        }
        user.avatar_bytes = static_cast<u8>(bytes);
    }
    if (!valid) parse.error = RelationshipsDecodeError::record;
    return valid;
}
bool visit_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(opaque);
    auto& record = parse.mutation.record;
    bool valid = true;
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (!mark(parse, &parse.seen, 1)) return false;
        valid = json_value_to_snowflake(value, &record.user_id) && record.user_id;
    } else if (json_string_equals_ascii(name, utf8_literal("type"))) {
        if (!mark(parse, &parse.seen, 2)) return false;
        u64 type = 0;
        valid = json_value_to_u64(value, &type) && type <= static_cast<u8>(RelationshipType::suggestion);
        if (valid) record.type = static_cast<RelationshipType>(type);
    } else if (json_string_equals_ascii(name, utf8_literal("nickname"))) {
        if (!mark(parse, &parse.seen, 4)) return false;
        parse.nickname_null = value.kind == JsonValueKind::null_value;
        valid = decode_text(value, record.nickname, sizeof(record.nickname), &record.nickname_bytes, true);
    } else if (json_string_equals_ascii(name, utf8_literal("is_spam_request"))) {
        if (!mark(parse, &parse.seen, 8)) return false;
        valid = json_value_to_bool(value, &record.spam);
    } else if (json_string_equals_ascii(name, utf8_literal("user_ignored"))) {
        if (!mark(parse, &parse.seen, 16)) return false;
        valid = json_value_to_bool(value, &record.ignored);
    } else if (json_string_equals_ascii(name, utf8_literal("is_stranger_request"))) {
        if (!mark(parse, &parse.seen, 32)) return false;
        valid = json_value_to_bool(value, &record.stranger);
        record.stranger_known = valid;
    } else if (json_string_equals_ascii(name, utf8_literal("user"))) {
        if (!mark(parse, &parse.seen, 64)) return false;
        valid = value.kind == JsonValueKind::object || value.kind == JsonValueKind::null_value;
        parse.user = value;
    }
    if (!valid) parse.error = RelationshipsDecodeError::record;
    return valid;
}
RelationshipsDecodeError decode_record(const JsonIndexedDocument& document, JsonValue value,
    RelationshipEvent event, RelationshipMutation* output) noexcept {
    if (value.kind != JsonValueKind::object) return RelationshipsDecodeError::record;
    Parse parse{};
    parse.document = &document;
    parse.mutation.record.presence = RelationshipPresence::unknown;
    const auto read = json_visit_indexed_object(document, value, 64, visit_member, &parse);
    if (!read.ok()) return parse.error == RelationshipsDecodeError::none ? RelationshipsDecodeError::json : parse.error;
    if (!(parse.seen & 1) || (event != RelationshipEvent::remove && !(parse.seen & 2))) return RelationshipsDecodeError::record;
    if ((parse.seen & 64) && parse.user.kind == JsonValueKind::object) {
        const auto user_read = json_visit_indexed_object(document, parse.user, 64, visit_user, &parse);
        if (!user_read.ok()) return parse.error == RelationshipsDecodeError::none ? RelationshipsDecodeError::json : parse.error;
        if (!(parse.user_seen & 1) || parse.mutation.user.id != parse.mutation.record.user_id) return RelationshipsDecodeError::record;
        if (!parse.mutation.user.display_name_bytes && parse.mutation.user.username_bytes) {
            copy_bytes(parse.mutation.user.display_name, parse.mutation.user.username, parse.mutation.user.username_bytes);
            parse.mutation.user.display_name_bytes = parse.mutation.user.username_bytes;
        }
    }
    parse.mutation.preserve_nickname = event == RelationshipEvent::add &&
        (!(parse.seen & 4) || parse.nickname_null);
    copy_value(output, parse.mutation);
    return RelationshipsDecodeError::none;
}
bool scan_document(Utf8View input, Slice<u32> scratch, JsonIndexedDocument* document) noexcept {
    const auto scan = json_scan_structurals_avx1(input, scratch);
    if (!scan.ok()) return false;
    *document = {input, {scratch.data, scan.count}};
    return true;
}
bool valid_input(Utf8View input, Slice<u32> scratch) noexcept {
    return input.bytes && input.bytes <= kRelationshipsMaximumBytes && scratch.count &&
        scratch.count <= ~u32{0} / sizeof(u32) &&
        byte_range_valid({reinterpret_cast<const u8*>(input.data), input.bytes}) &&
        byte_range_valid({reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))}) &&
        !byte_ranges_overlap({reinterpret_cast<const u8*>(input.data), input.bytes},
            {reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))});
}
struct Snapshot {
    const JsonIndexedDocument* document;
    RelationshipStore* store;
    Slice<RelationshipRecord> records;
    Slice<RelationshipUser> users;
    u32 count, user_count;
    RelationshipsDecodeError error;
};
bool visit_snapshot(void* opaque, u32, JsonValue value) noexcept {
    auto& snapshot = *static_cast<Snapshot*>(opaque);
    if (snapshot.count >= snapshot.records.count) { snapshot.error = RelationshipsDecodeError::capacity; return false; }
    RelationshipMutation decoded{};
    snapshot.error = decode_record(*snapshot.document, value, RelationshipEvent::update, &decoded);
    if (snapshot.error != RelationshipsDecodeError::none) return false;
    if (const auto* old = relationship_store_find(*snapshot.store, decoded.record.user_id)) decoded.record.presence = old->presence;
    snapshot.records[snapshot.count++] = decoded.record;
    if (decoded.user.id && snapshot.users.count) {
        if (snapshot.user_count == snapshot.users.count) { snapshot.error = RelationshipsDecodeError::capacity; return false; }
        copy_value(&snapshot.users[snapshot.user_count++], decoded.user);
    }
    return true;
}
} // namespace

RestMethod relationship_method(RelationshipOperation operation) noexcept {
    switch (operation) {
    case RelationshipOperation::request: return RestMethod::post;
    case RelationshipOperation::accept: case RelationshipOperation::block:
    case RelationshipOperation::accept_stranger: case RelationshipOperation::ignore: return RestMethod::put;
    case RelationshipOperation::remove: case RelationshipOperation::unignore:
    case RelationshipOperation::clear_incoming: case RelationshipOperation::clear_hidden: return RestMethod::delete_method;
    default: return RestMethod::get;
    }
}
RelationshipWriteResult write_relationship_target(RelationshipOperation operation, u64 user_id, Slice<u8> output) noexcept {
    const bool individual = operation == RelationshipOperation::accept || operation == RelationshipOperation::remove ||
        operation == RelationshipOperation::block || operation == RelationshipOperation::accept_stranger ||
        operation == RelationshipOperation::ignore || operation == RelationshipOperation::unignore;
    if (operation > RelationshipOperation::clear_hidden || individual != (user_id != 0) ||
        !byte_range_valid({output.data, output.count})) return {0, RelationshipWriteError::invalid_input};
    char temporary[96]{};
    RouteWriter writer{};
    route_writer_initialize(&writer, {reinterpret_cast<u8*>(temporary), sizeof(temporary)});
    route_append(&writer, utf8_literal("/api/v10/users/@me/relationships"));
    if (individual) { route_append_byte(&writer, '/'); route_append_u64(&writer, user_id); }
    if (operation == RelationshipOperation::ignore || operation == RelationshipOperation::unignore)
        route_append(&writer, utf8_literal("/ignore"));
    if (operation == RelationshipOperation::clear_incoming || operation == RelationshipOperation::clear_hidden)
        route_append(&writer, utf8_literal("?relationship_type=3"));
    if (!writer.valid || writer.bytes > output.count) return {0, RelationshipWriteError::output_too_small};
    copy_bytes(output.data, temporary, writer.bytes);
    return {writer.bytes, RelationshipWriteError::none};
}
RelationshipWriteResult write_relationship_body(RelationshipOperation operation, Utf8View username, Slice<u8> output) noexcept {
    if (operation > RelationshipOperation::clear_hidden || !byte_range_valid({output.data, output.count}) ||
        !byte_range_valid({reinterpret_cast<const u8*>(username.data), username.bytes}) ||
        (operation != RelationshipOperation::request && username.bytes)) return {0, RelationshipWriteError::invalid_input};
    char temporary[128]{};
    RouteWriter writer{};
    route_writer_initialize(&writer, {reinterpret_cast<u8*>(temporary), sizeof(temporary)});
    if (operation == RelationshipOperation::request) {
        if (username.bytes < 2 || username.bytes > kRelationshipUsernameMaximumBytes) return {0, RelationshipWriteError::invalid_input};
        for (u32 i = 0; i < username.bytes; ++i) {
            const char c = username.data[i];
            if (!(ascii_is_alnum(c) || c == '_' || c == '.'))
                return {0, RelationshipWriteError::invalid_input};
        }
        route_append(&writer, utf8_literal("{\"username\":\""));
        route_append(&writer, username);
        route_append(&writer, utf8_literal("\",\"discriminator\":null}"));
    } else if (operation == RelationshipOperation::accept) route_append(&writer, utf8_literal("{}"));
    else if (operation == RelationshipOperation::block) route_append(&writer, utf8_literal("{\"type\":2}"));
    else if (operation == RelationshipOperation::accept_stranger)
        route_append(&writer, utf8_literal("{\"confirm_stranger_request\":true}"));
    else if (operation == RelationshipOperation::clear_hidden)
        route_append(&writer, utf8_literal("{\"filters\":[1,2]}"));
    if (!writer.valid || writer.bytes > output.count) return {0, RelationshipWriteError::output_too_small};
    copy_bytes(output.data, temporary, writer.bytes);
    return {writer.bytes, RelationshipWriteError::none};
}
RelationshipsDecodeResult relationship_decode_record(Utf8View input, Slice<u32> scratch,
    RelationshipEvent event, RelationshipMutation* output) noexcept {
    if (event > RelationshipEvent::update || !valid_input(input, scratch)) return failure(RelationshipsDecodeError::invalid_input);
    const ConstSlice<u8> destination{reinterpret_cast<const u8*>(output), sizeof(*output)};
    if (!byte_range_valid(destination) || byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(input.data), input.bytes}) ||
        byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))}))
        return failure(RelationshipsDecodeError::invalid_input);
    JsonIndexedDocument document{};
    if (!scan_document(input, scratch, &document)) return failure(RelationshipsDecodeError::scan);
    RelationshipMutation candidate{};
    const auto error = decode_record(document, {input, 0, JsonValueKind::object}, event, &candidate);
    if (error != RelationshipsDecodeError::none) return failure(error);
    *output = candidate;
    return {RelationshipsDecodeError::none, 1, candidate.user.id ? 1u : 0u, false};
}
RelationshipStoreResult relationship_apply_decoded(RelationshipStore* store,
    RelationshipEvent event, const RelationshipMutation& mutation) noexcept {
    if (event > RelationshipEvent::update) return {RelationshipStoreError::invalid_input, false};
    if (event == RelationshipEvent::remove) return relationship_store_remove(store, mutation.record.user_id);
    auto record = mutation.record;
    if (const auto* old = relationship_store_find(*store, record.user_id)) {
        record.presence = old->presence;
        if (mutation.preserve_nickname) {
            record.nickname_bytes = old->nickname_bytes;
            copy_bytes(record.nickname, old->nickname, sizeof(record.nickname));
        }
    }
    return relationship_store_upsert(store, record);
}
RelationshipsDecodeResult relationships_replace(RelationshipStore* store, Utf8View input,
    Slice<u32> scratch, Slice<RelationshipRecord> candidate, Slice<RelationshipUser> users,
    RelationshipsAdmission admission) noexcept {
    if (!store->records.data || !store->records.count || store->records.count > kRelationshipMaximumRecords ||
        store->count > store->records.count || !valid_input(input, scratch) || !candidate.data || !candidate.count ||
        candidate.count > kRelationshipMaximumRecords || users.count > kRelationshipMaximumRecords)
        return failure(RelationshipsDecodeError::invalid_input);
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(input.data), input.bytes},
        {reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))},
        {reinterpret_cast<const u8*>(candidate.data), candidate.count * static_cast<u32>(sizeof(*candidate.data))},
        {reinterpret_cast<const u8*>(users.data), users.count * static_cast<u32>(sizeof(*users.data))},
        {reinterpret_cast<const u8*>(store), sizeof(*store)},
        {reinterpret_cast<const u8*>(store->records.data), store->records.count * static_cast<u32>(sizeof(*store->records.data))}};
    for (u32 i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
        if (!byte_range_valid(ranges[i])) return failure(RelationshipsDecodeError::invalid_input);
        for (u32 j = 0; j < i; ++j) if (byte_ranges_overlap(ranges[i], ranges[j])) return failure(RelationshipsDecodeError::invalid_input);
    }
    JsonIndexedDocument document{};
    if (!scan_document(input, scratch, &document)) return failure(RelationshipsDecodeError::scan);
    struct Count { u32 count, capacity; bool exhausted; } count{0, candidate.count};
    const auto preflight = json_visit_root_array(input, document.structurals, 64,
        +[](void* opaque, u32, JsonValue) noexcept {
            auto& count = *static_cast<Count*>(opaque);
            if (count.count == count.capacity) { count.exhausted = true; return false; }
            ++count.count;
            return true;
        }, &count);
    if (!preflight.ok()) return failure(count.exhausted ? RelationshipsDecodeError::capacity : RelationshipsDecodeError::json);
    if (count.count > store->records.count) return failure(RelationshipsDecodeError::capacity);
    if (admission.admit && !admission.admit(admission.context, count.count, false))
        return failure(RelationshipsDecodeError::capacity);
    Snapshot snapshot{&document, store, candidate, users};
    const auto read = json_visit_root_array(input, document.structurals, 64, visit_snapshot, &snapshot);
    if (!read.ok()) return failure(snapshot.error == RelationshipsDecodeError::none ? RelationshipsDecodeError::json : snapshot.error);
    heap_sort(candidate.data, snapshot.count, [](const auto& a, const auto& b) noexcept { return a.user_id < b.user_id; });
    for (u32 i = 1; i < snapshot.count; ++i) if (candidate[i - 1].user_id == candidate[i].user_id) return failure(RelationshipsDecodeError::duplicate);
    // Apply the store's own validity checks before the owner commits its
    // destination; presence may be inherited from the previous store.
    for (u32 i = 0; i < snapshot.count; ++i) {
        const auto& record = candidate[i];
        if (!record.user_id || record.type > RelationshipType::suggestion ||
            record.presence > RelationshipPresence::unknown || record.nickname_bytes > kRelationshipNicknameMaximumBytes ||
            !utf8_validate({record.nickname, record.nickname_bytes}).valid)
            return failure(RelationshipsDecodeError::store);
    }
    if (admission.admit && !admission.admit(admission.context,
            snapshot.count > store->count ? snapshot.count : store->count, true))
        return failure(RelationshipsDecodeError::capacity);
    const auto applied = relationship_store_replace(store, {candidate.data, snapshot.count});
    if (!applied.ok()) return failure(applied.error == RelationshipStoreError::capacity ? RelationshipsDecodeError::capacity : RelationshipsDecodeError::store);
    return {RelationshipsDecodeError::none, snapshot.count, snapshot.user_count, applied.changed};
}
RelationshipUserApplyResult relationship_users_apply(UserStore* users, ConstSlice<RelationshipUser> snapshots) noexcept {
    if (snapshots.count > kRelationshipMaximumRecords ||
        !byte_range_valid({reinterpret_cast<const u8*>(snapshots.data), snapshots.count * static_cast<u32>(sizeof(*snapshots.data))}))
        return {UserStoreError::invalid_input, 0};
    for (u32 i = 0; i < snapshots.count; ++i) {
        const auto& snapshot = snapshots[i];
        if (!snapshot.id || snapshot.username_bytes > sizeof(snapshot.username) || snapshot.display_name_bytes > sizeof(snapshot.display_name) ||
            snapshot.avatar_bytes > sizeof(snapshot.avatar)) return {UserStoreError::invalid_input, i};
        const auto* old = user_store_find(*users, snapshot.id);
        const Utf8View name = snapshot.display_name_bytes ? Utf8View{snapshot.display_name, snapshot.display_name_bytes} :
            old ? user_store_name(*users, *old) : Utf8View{};
        const auto upserted = user_store_upsert(users, snapshot.id, name, {snapshot.username, snapshot.username_bytes});
        if (!upserted.ok()) return {upserted.error, i};
        if (snapshot.avatar_seen && !user_store_set_avatar(users, snapshot.id, {snapshot.avatar, snapshot.avatar_bytes}))
            return {UserStoreError::invalid_input, i};
    }
    return {UserStoreError::none, snapshots.count};
}
RelationshipUserApplyResult relationships_hydrate(UserStore* users, Utf8View input, Slice<u32> scratch) noexcept {
    if (!valid_input(input, scratch)) return {UserStoreError::invalid_input, 0};
    JsonIndexedDocument document{};
    if (!scan_document(input, scratch, &document)) return {UserStoreError::invalid_input, 0};
    struct Hydrate { const JsonIndexedDocument* document; UserStore* users; RelationshipUserApplyResult result; } context{&document, users, {}};
    const auto read = json_visit_root_array(input, document.structurals, 64, +[](void* opaque, u32, JsonValue value) noexcept {
        auto& context = *static_cast<Hydrate*>(opaque);
        RelationshipMutation decoded{};
        if (decode_record(*context.document, value, RelationshipEvent::update, &decoded) != RelationshipsDecodeError::none) {
            context.result.error = UserStoreError::invalid_input;
            return false;
        }
        if (!decoded.user.id) return true;
        const auto applied = relationship_users_apply(context.users, {&decoded.user, 1});
        if (!applied.ok()) { context.result.error = applied.error; return false; }
        ++context.result.applied;
        return true;
    }, &context);
    if (!read.ok() && context.result.error == UserStoreError::none) context.result.error = UserStoreError::invalid_input;
    return context.result;
}
} // namespace maicord
