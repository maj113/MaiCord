#include "maicord/app/gateway_client.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/json_ids.hpp"

#include "gateway_dispatch_common.hpp"

#include "gateway_payload_writer.hpp"
#include "gateway_user_payload.hpp"

namespace maicord {
namespace {

struct MembershipRoot {
    u64 guild_id;
    JsonValue user;
    JsonValue stickers;
    bool guild_seen;
    bool user_seen;
    GatewayEventDecodeError error;
};

// A guild sticker list: an array, or a CLIENT_STATE_V2 {"items": [...]} set.
bool items_field(void* opaque, JsonString name, JsonValue value) noexcept {
    if (json_string_equals_ascii(name, utf8_literal("items")))
        *static_cast<JsonValue*>(opaque) = value;
    return true;
}
JsonValue sticker_array(JsonValue value, Slice<u32> scratch) noexcept {
    if (value.kind == JsonValueKind::array || value.kind != JsonValueKind::object)
        return value;
    JsonValue items{};
    const auto scan = json_scan_structurals_avx1(value.source, scratch);
    if (!scan.ok() || !json_visit_value_object(value, {scratch.data, scan.count}, 8, items_field, &items).ok())
        return {};
    return items;
}
bool stickers_field(void* opaque, JsonString name, JsonValue value) noexcept {
    if (json_string_equals_ascii(name, utf8_literal("stickers")))
        *static_cast<JsonValue*>(opaque) = value;
    return true;
}

bool visit_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& root = *static_cast<MembershipRoot*>(opaque);
    const bool guild = json_string_equals_ascii(name, utf8_literal("guild_id"));
    const bool user = !guild && json_string_equals_ascii(name, utf8_literal("user"));
    if (!guild && !user) {
        if (json_string_equals_ascii(name, utf8_literal("stickers")))
            root.stickers = value;
        return true;
    }
    bool& seen = guild ? root.guild_seen : root.user_seen;
    if (seen) {
        root.error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    seen = true;
    const bool valid = guild ? json_value_to_snowflake(value, &root.guild_id)
                             : value.kind == JsonValueKind::object;
    if (!valid) {
        root.error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    if (user)
        root.user = value;
    return true;
}

GatewayClientDispatchRouteResult failure(GatewayClientDispatchRouteError route,
                                         GatewayEventDecodeError error) noexcept {
    GatewayClientDispatchRouteResult result{};
    result.error = route;
    result.event_error = error;
    return result;
}

} // namespace

GatewayClientDispatchRouteResult gateway_client_route_guild_admin_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> payload_scratch, GatewayClientEvent* event, ConstSlice<u8>* payload, bool* handled) noexcept {
    if (!event || !payload || !handled)
        return failure(GatewayClientDispatchRouteError::invalid_input, GatewayEventDecodeError::none);
    const bool member_remove = json_string_equals_ascii(envelope.event_name, utf8_literal("GUILD_MEMBER_REMOVE"));
    const bool ban_add = json_string_equals_ascii(envelope.event_name, utf8_literal("GUILD_BAN_ADD"));
    const bool ban_remove = json_string_equals_ascii(envelope.event_name, utf8_literal("GUILD_BAN_REMOVE"));
    const bool stickers = json_string_equals_ascii(envelope.event_name, utf8_literal("GUILD_STICKERS_UPDATE"));
    *handled = member_remove || ban_add || ban_remove || stickers;
    if (!*handled)
        return {};
    if (!structural_scratch.data || !structural_scratch.count || !payload_scratch.data || !maximum_depth ||
        envelope.data.kind != JsonValueKind::object)
        return failure(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_data);
    const auto scan = json_scan_structurals_avx1(envelope.data.source, structural_scratch);
    if (!scan.ok())
        return failure(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::scan_failed);
    MembershipRoot root{};
    const auto read = json_visit_value_object(envelope.data, {structural_scratch.data, scan.count},
                                             maximum_depth, visit_root, &root);
    if (stickers) {
        // The full list travels verbatim when it fits; otherwise only the
        // guild, and Server Settings refetch it.
        if (!read.ok() || !root.guild_seen || !root.guild_id)
            return failure(GatewayClientDispatchRouteError::decode,
                           root.error != GatewayEventDecodeError::none ? root.error
                           : read.ok() ? GatewayEventDecodeError::missing_required_field
                                       : GatewayEventDecodeError::invalid_json);
        const JsonValue list = sticker_array(root.stickers, structural_scratch);
        const u32 bytes = list.kind == JsonValueKind::array && list.source.bytes <= payload_scratch.count
                              ? list.source.bytes
                              : 0;
        if (bytes)
            copy_bytes(payload_scratch.data, list.source.data, bytes);
        event->dispatch_kind = GatewayClientDispatchKind::guild_stickers_update;
        event->member_list = {};
        event->member_list.guild_id = root.guild_id;
        event->dispatch_payload_bytes = bytes;
        *payload = {reinterpret_cast<const u8*>(payload_scratch.data), bytes};
        return {};
    }
    if (!read.ok() || !root.guild_seen || !root.user_seen)
        return failure(GatewayClientDispatchRouteError::decode,
                       root.error != GatewayEventDecodeError::none ? root.error
                       : read.ok()                                  ? GatewayEventDecodeError::missing_required_field
                                                                    : GatewayEventDecodeError::invalid_json);
    // The envelope index is no longer needed; the user record rescans its object.
    GatewayPayloadWriter writer{payload_scratch, 0};
    const auto user = gateway_append_user(root.user, structural_scratch, maximum_depth, &writer);
    if (!user.ok())
        return user;
    event->dispatch_kind = member_remove ? GatewayClientDispatchKind::guild_member_remove
                           : ban_add     ? GatewayClientDispatchKind::guild_ban_add
                                         : GatewayClientDispatchKind::guild_ban_remove;
    event->member_list = {};
    event->member_list.guild_id = root.guild_id;
    event->member_list.record_count = 1;
    event->dispatch_payload_bytes = writer.used;
    *payload = {reinterpret_cast<const u8*>(payload_scratch.data), writer.used};
    return {};
}

bool gateway_client_event_guild_membership(const GatewayClientEvent& event, ConstSlice<u8> payload, u64* guild_id,
                                           u64* user_id, Utf8View* name) noexcept {
    if (!guild_id || !user_id || event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::guild_member_remove &&
         event.dispatch_kind != GatewayClientDispatchKind::guild_ban_add &&
         event.dispatch_kind != GatewayClientDispatchKind::guild_ban_remove) ||
        !event.member_list.guild_id || event.member_list.record_count != 1 ||
        event.dispatch_payload_bytes != payload.count)
        return false;
    u32 cursor = 0;
    GatewayClientUserMutation user{};
    Utf8View display{}, avatar{}, username{};
    if (!gateway_client_user_record(payload, &cursor, &user, &display, &avatar, &username) ||
        cursor != payload.count)
        return false;
    *guild_id = event.member_list.guild_id;
    *user_id = user.id;
    if (name)
        *name = display.bytes ? display : username;
    return true;
}

bool gateway_client_publish_guild_stickers(JsonValue guild, u64 guild_id, Slice<u32> scratch, Slice<char> payload,
                                           u64 sequence, u64 now_ms, GatewayRelationshipPublish publish,
                                           void* context) noexcept {
    if (!publish || !guild_id || !scratch.data || scratch.count < 64 || !payload.data ||
        payload.count < kGatewayClientDispatchPayloadCapacity || guild.kind != JsonValueKind::object)
        return true;
    JsonValue found{};
    const auto scan = json_scan_structurals_avx1(guild.source, scratch);
    if (!scan.ok() || !json_visit_value_object(guild, {scratch.data, scan.count}, 32, stickers_field, &found).ok())
        return true;
    const JsonValue list = sticker_array(found, scratch);
    if (list.kind != JsonValueKind::array || !list.source.bytes ||
        list.source.bytes > kGatewayClientDispatchPayloadCapacity)
        return true;
    copy_bytes(payload.data, list.source.data, list.source.bytes);
    GatewayClientEvent event =
        gateway_dispatch_event(GatewayClientDispatchKind::guild_stickers_update, sequence, now_ms);
    event.member_list.guild_id = guild_id;
    event.dispatch_payload_bytes = list.source.bytes;
    return publish(context, event, {reinterpret_cast<const u8*>(payload.data), list.source.bytes});
}

} // namespace maicord
