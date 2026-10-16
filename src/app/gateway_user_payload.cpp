#include "gateway_user_payload.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/cdn_url.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/user_profile.hpp"

namespace maicord {
namespace {
struct LinkedRow { u64 status, type; u8 seen; };
bool linked_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& row = *static_cast<LinkedRow*>(opaque);
    const u8 bit = json_string_equals_ascii(name, utf8_literal("link_status")) ? 1 :
                   json_string_equals_ascii(name, utf8_literal("link_type")) ? 2 : 0;
    if (!bit) return true;
    if (row.seen & bit) return false;
    row.seen |= bit;
    return json_value_to_u64(value, bit == 1 ? &row.status : &row.type);
}
struct LinkedRows { JsonIndexedDocument document; u32 depth; bool parent_managed; };
bool linked_row(void* opaque, u32, JsonValue value) noexcept {
    auto& rows = *static_cast<LinkedRows*>(opaque);
    if (value.kind != JsonValueKind::object) return false;
    LinkedRow row{};
    const auto read = json_visit_indexed_object(rows.document, value, rows.depth, linked_field, &row);
    if (!read.ok() || row.seen != 3) return false;
    rows.parent_managed |= row.status == 2 && row.type == 1;
    return true;
}
struct User {
    u64 id;
    u32 premium_type;
    JsonString username, global_name, avatar;
    u16 seen;
    bool mfa_enabled;
    bool nsfw_allowed, nsfw_known;
    u8 age_verification_status;
    bool parent_managed;
    JsonIndexedDocument document;
    u32 depth;
    GatewayEventDecodeError error;
};
bool visit(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& user = *static_cast<User*>(opaque);
    const u16 field = json_string_equals_ascii(key, utf8_literal("id")) ? 1 :
        json_string_equals_ascii(key, utf8_literal("username")) ? 2 :
        json_string_equals_ascii(key, utf8_literal("global_name")) ? 4 :
        json_string_equals_ascii(key, utf8_literal("avatar")) ? 8 :
        json_string_equals_ascii(key, utf8_literal("premium_type")) ? 16 :
        json_string_equals_ascii(key, utf8_literal("mfa_enabled")) ? 32 :
        json_string_equals_ascii(key, utf8_literal("nsfw_allowed")) ? 64 :
        json_string_equals_ascii(key, utf8_literal("age_verification_status")) ? 128 :
        json_string_equals_ascii(key, utf8_literal("linked_users")) ? 256 : 0;
    if (!field) return true;
    if (user.seen & field) { user.error = GatewayEventDecodeError::duplicate_field; return false; }
    user.seen |= field;
    bool valid = false;
    if (field == 1) valid = json_value_to_snowflake(value, &user.id);
    else if (field == 16) { u64 premium = 0; valid = value.kind == JsonValueKind::null_value ||
        (json_value_to_u64(value, &premium) && premium <= 255); user.premium_type = static_cast<u32>(premium); }
    else if (field == 32) valid = json_value_to_bool(value, &user.mfa_enabled);
    else if (field == 64) {
        user.nsfw_known = value.kind != JsonValueKind::null_value;
        valid = !user.nsfw_known || json_value_to_bool(value, &user.nsfw_allowed);
    } else if (field == 128) {
        u64 status = 0;
        valid = value.kind == JsonValueKind::null_value || json_value_to_u64(value, &status);
        // Future statuses carry no adult proof until their semantics are known.
        user.age_verification_status = static_cast<u8>(status <= 4 ? status : 0);
    } else if (field == 256)
        valid = gateway_client_linked_parent(user.document, value, user.depth, &user.parent_managed);
    else if (value.kind == JsonValueKind::null_value) valid = field != 2;
    else valid = json_value_as_string(value, field == 2 ? &user.username : field == 4 ? &user.global_name : &user.avatar);
    if (!valid) user.error = GatewayEventDecodeError::invalid_user;
    return valid;
}
GatewayClientDispatchRouteResult error(GatewayClientDispatchRouteError route,
    GatewayEventDecodeError event = GatewayEventDecodeError::none) noexcept { return {event, route}; }
}

bool gateway_client_linked_parent(const JsonIndexedDocument& document, JsonValue value,
    u32 maximum_depth, bool* parent_managed) noexcept {
    if (!parent_managed || value.kind != JsonValueKind::array) return false;
    LinkedRows rows{document, maximum_depth, false};
    const auto read = json_visit_indexed_array(document, value, maximum_depth, linked_row, &rows);
    if (!read.ok()) return false;
    *parent_managed = rows.parent_managed;
    return true;
}

GatewayClientDispatchRouteResult gateway_append_user(JsonValue value, Slice<u32> scratch,
    u32 maximum_depth, GatewayPayloadWriter* writer, bool id_only) noexcept {
    if (!writer || !writer->output.data) return error(GatewayClientDispatchRouteError::invalid_input);
    User decoded{};
    u32 structural_count = 0;
    if (id_only) {
        if (!json_value_to_snowflake(value, &decoded.id))
            return error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_user);
    } else {
        if (value.kind != JsonValueKind::object) return error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_user);
        const auto scan = json_scan_structurals_avx1(value.source, scratch);
        if (!scan.ok()) {
            auto failed = error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::scan_failed);
            failed.scan_error = scan.error;
            failed.error_offset = scan.error_offset;
            return failed;
        }
        structural_count = scan.count;
        decoded.document = {value.source, {scratch.data, scan.count}};
        decoded.depth = maximum_depth;
        const auto read = json_visit_value_object(value, {scratch.data, scan.count}, maximum_depth, visit, &decoded);
        if (!read.ok() || !decoded.id) return error(GatewayClientDispatchRouteError::decode,
            decoded.error != GatewayEventDecodeError::none ? decoded.error : GatewayEventDecodeError::invalid_user);
    }
    UserAppearance appearance;
    u8 appearance_data[1024]; u32 appearance_bytes = 0;
    if (!id_only) {
        if (!user_appearance_decode({value.source, {scratch.data, structural_count}},
            {value.source, 0, JsonValueKind::object}, decoded.id, 0, &appearance)) return error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_user);
        appearance_bytes = user_appearance_write(appearance, {appearance_data, sizeof(appearance_data)});
        if (appearance.fields && !appearance_bytes) return error(GatewayClientDispatchRouteError::payload_capacity);
    }
    char name[128], avatar[34], username[128];
    u32 name_bytes = 0, avatar_bytes = 0;
    const auto username_result = json_string_decode_utf8(decoded.username, {username, 128});
    if (decoded.username.encoded.bytes && !username_result.ok())
        return error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_user);
    const u32 username_bytes = decoded.username.encoded.bytes ? username_result.bytes : 0;
    const char* display = username;
    name_bytes = username_bytes;
    if (decoded.global_name.encoded.bytes) {
        const auto result = json_string_decode_utf8(decoded.global_name, {name, 128});
        if (!result.ok() || !result.bytes) return error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_user);
        name_bytes = result.bytes;
        display = name;
    }
    if (decoded.avatar.encoded.bytes) {
        const auto result = json_string_decode_utf8(decoded.avatar, {avatar, 34});
        if (!result.ok() || !guild_icon_hash_valid({avatar, result.bytes}))
            return error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_user);
        avatar_bytes = result.bytes;
    }
    GatewayClientUserMutation user{decoded.id, static_cast<u16>(name_bytes), static_cast<u8>(avatar_bytes),
        static_cast<u8>((name_bytes ? 1 : 0) | ((decoded.seen & 8) ? 2 : 0) | ((decoded.seen & 16) ? 4 : 0) |
                        ((decoded.seen & 32) ? 8 : 0) | (decoded.mfa_enabled ? 16 : 0) |
                        ((decoded.seen & 64) ? 32 : 0) | (decoded.nsfw_known ? 64 : 0) |
                        (decoded.nsfw_allowed ? 128 : 0)), decoded.premium_type,
                        static_cast<u16>(username_bytes), static_cast<u16>(appearance_bytes),
                        decoded.age_verification_status, static_cast<u8>(((decoded.seen & 128) ? 1 : 0) |
                            ((decoded.seen & 256) ? 2 : 0) | (decoded.parent_managed ? 4 : 0)), 0};
    const u32 bytes = sizeof(user) + name_bytes + avatar_bytes + username_bytes + appearance_bytes;
    if (writer->used > writer->output.count || bytes > writer->output.count - writer->used)
        return error(GatewayClientDispatchRouteError::payload_capacity);
    gateway_payload_append(writer, &user, sizeof(user));
    gateway_payload_append(writer, display, name_bytes);
    gateway_payload_append(writer, avatar, avatar_bytes);
    gateway_payload_append(writer, username, username_bytes);
    gateway_payload_append(writer, appearance_data, appearance_bytes);
    return {};
}

bool gateway_client_user_record(ConstSlice<u8> payload, u32* cursor,
    GatewayClientUserMutation* user, Utf8View* name, Utf8View* avatar, Utf8View* username, ConstSlice<u8>* appearance) noexcept {
    if (!cursor || !user || !name || !avatar || !payload.data || *cursor > payload.count ||
        sizeof(*user) > payload.count - *cursor) return false;
    copy_bytes(user, payload.data + *cursor, sizeof(*user));
    const u32 first = *cursor + sizeof(*user);
    const u32 bytes = static_cast<u32>(user->name_bytes) + user->avatar_bytes + user->username_bytes + user->appearance_bytes;
    if (!user->id || user->premium_type > 255 || (!(user->flags & 4) && user->premium_type) ||
        ((user->flags & 16) && !(user->flags & 8)) || ((user->flags & 64) && !(user->flags & 32)) ||
        ((user->flags & 128) && !(user->flags & 64)) || user->reserved || (user->content_fields & ~7u) ||
        ((user->content_fields & 4) && !(user->content_fields & 2)) ||
        user->age_verification_status > 4 || (!(user->content_fields & 1) && user->age_verification_status) ||
        user->name_bytes > 128 || user->avatar_bytes > 34 ||
        bytes > payload.count - first || ((user->flags & 1) != 0) != (user->name_bytes != 0) ||
        (user->avatar_bytes && !(user->flags & 2)) || user->username_bytes > 128) return false;
    *name = {reinterpret_cast<const char*>(payload.data + first), user->name_bytes};
    *avatar = {name->data + user->name_bytes, user->avatar_bytes};
    const Utf8View login_name{avatar->data + avatar->bytes, user->username_bytes};
    if (username) *username = login_name;
    if (!utf8_validate(login_name).valid || !utf8_validate(*name).valid || (avatar->bytes && !guild_icon_hash_valid(*avatar))) return false;
    const ConstSlice<u8> extra{reinterpret_cast<const u8*>(login_name.data + login_name.bytes), user->appearance_bytes};
    UserAppearance decoded{};
    if (!user_appearance_read(extra, user->id, 0, &decoded)) return false;
    if (appearance) *appearance = extra;
    *cursor = first + bytes;
    return true;
}
} // namespace maicord
