#include "maicord/app/gateway/client_dispatch.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

#include "state_payload.hpp"

namespace maicord {
namespace {

// A guild create/update event whose descriptor matches its payload.
bool guild_event_payload(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    return event.kind == GatewayClientEventKind::dispatch &&
           (event.dispatch_kind == GatewayClientDispatchKind::guild_create ||
            event.dispatch_kind == GatewayClientDispatchKind::guild_update) &&
           event.dispatch_payload_bytes == payload.count;
}

inline bool read_record(ConstSlice<u8> payload, u32 offset, GatewayClientStateMutation* record) noexcept {
    if (offset > payload.count || sizeof(*record) > payload.count - offset) {
        return false;
    }
    copy_bytes(record, payload.data + offset, sizeof(*record));
    return true;
}

// visit_role (state_dispatch_records.cpp) writes each role with a nonzero id and a name that append_name decoded
// whole (UTF-8, at most 400 bytes).
bool read_guild_role(ConstSlice<u8> payload, u32 offset, GatewayClientRoleMutation* role, Utf8View* name,
                     u32* next_offset) noexcept {
    if (offset > payload.count || sizeof(*role) > payload.count - offset) {
        return false;
    }
    copy_bytes(role, payload.data + offset, sizeof(*role));
    offset += sizeof(*role);
    if (role->name_bytes > payload.count - offset) {
        return false;
    }
    *name = Utf8View{reinterpret_cast<const char*>(payload.data + offset), role->name_bytes};
    *next_offset = offset + role->name_bytes;
    return true;
}

bool guild_roles_end(const GatewayClientEvent& event, ConstSlice<u8> payload, u32* end) noexcept {
    if (gateway_client_guild_strings_bytes(event.state) > payload.count) {
        return false;
    }
    u32 cursor = gateway_client_guild_strings_bytes(event.state);
    for (u32 index = 0; index < event.state.role_count; ++index) {
        GatewayClientRoleMutation role{};
        Utf8View name{};
        if (!read_guild_role(payload, cursor, &role, &name, &cursor)) {
            return false;
        }
    }
    *end = cursor;
    return true;
}

// Where a guild create's channel records start: after its roles and the member's role ids.
bool guild_channels_begin(const GatewayClientEvent& event, ConstSlice<u8> payload, u32* begin) noexcept {
    u32 roles_end = 0;
    if (!guild_roles_end(event, payload, &roles_end)) {
        return false;
    }
    const u64 prefix = static_cast<u64>(roles_end) + static_cast<u64>(event.state.member_role_count) * 8;
    if (prefix > payload.count) {
        return false;
    }
    *begin = static_cast<u32>(prefix);
    return true;
}

} // namespace

bool gateway_client_event_as_ready_guilds(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                          Slice<u64> guild_ids, u32* guild_count) noexcept {
    if (!guild_count || event.kind != GatewayClientEventKind::ready ||
        event.dispatch_payload_bytes != payload.count || payload.count != event.state.record_count * 8u ||
        event.state.record_count > guild_ids.count || (!guild_ids.data && event.state.record_count != 0)) {
        return false;
    }
    // visit_ready_guild (state_dispatch_records.cpp) writes a guild only with its id, a nonzero snowflake.
    for (u32 index = 0; index < event.state.record_count; ++index)
        guild_ids[index] = byte_load_le64(payload.data + index * 8);
    *guild_count = event.state.record_count;
    return true;
}

// Every state name is a whole json_string_decode_utf8 output (append_name, or the recipient nick in
// private_dispatch.cpp): UTF-8.
bool gateway_client_event_state_name(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                     Utf8View* name) noexcept {
    if (!name || event.dispatch_payload_bytes != payload.count || event.state.name_bytes > payload.count ||
        (event.state.name_bytes != 0 && !payload.data)) {
        return false;
    }
    *name = Utf8View{reinterpret_cast<const char*>(payload.data), event.state.name_bytes};
    return true;
}

bool gateway_client_event_guild_icon(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                     Utf8View* icon_hash) noexcept {
    if (!icon_hash || !guild_event_payload(event, payload) ||
        (event.state.fields & gateway_client_state_field_icon) == 0 ||
        event.state.name_bytes > payload.count ||
        event.state.icon_bytes > payload.count - event.state.name_bytes ||
        ((event.state.name_bytes != 0 || event.state.icon_bytes != 0) && !payload.data)) {
        return false;
    }
    const char* begin =
        payload.data ? reinterpret_cast<const char*>(payload.data) + event.state.name_bytes : nullptr;
    *icon_hash = Utf8View{begin, event.state.icon_bytes}; // append_icon decodes the hash whole: UTF-8
    return true;
}

bool gateway_client_event_guild_banner(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                       Utf8View* banner_hash) noexcept {
    if (!banner_hash || !guild_event_payload(event, payload) || !event.state.guild_banner ||
        gateway_client_guild_strings_bytes(event.state) > payload.count ||
        (gateway_client_guild_banner_bytes(event.state) && !payload.data)) {
        return false;
    }
    const u32 offset = static_cast<u32>(event.state.name_bytes) + event.state.icon_bytes;
    const u32 bytes = gateway_client_guild_banner_bytes(event.state);
    *banner_hash = Utf8View{bytes ? reinterpret_cast<const char*>(payload.data) + offset : nullptr, bytes};
    return true; // append_icon decodes the hash whole: UTF-8
}

bool gateway_client_event_next_guild_channel(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                             u32* cursor, u32* index, GatewayClientStateMutation* channel,
                                             Utf8View* name, ConstSlice<u8>* overwrite_payload) noexcept {
    if (!cursor || !index || !channel || !name || !overwrite_payload ||
        event.kind != GatewayClientEventKind::dispatch ||
        event.dispatch_kind != GatewayClientDispatchKind::guild_create ||
        event.dispatch_payload_bytes != payload.count || event.state.name_bytes > payload.count ||
        *index >= event.state.record_count) {
        return false;
    }
    if (*index == 0 && *cursor == 0 && !guild_channels_begin(event, payload, cursor)) {
        return false;
    }
    if (*cursor < event.state.name_bytes || !read_record(payload, *cursor, channel)) {
        return false;
    }
    *cursor += sizeof(*channel);
    if ((channel->content_policy & ~1u) || (channel->content_policy && !channel->content_policy_known) ||
        channel->name_bytes > payload.count - *cursor) {
        return false;
    }
    *name = Utf8View{reinterpret_cast<const char*>(payload.data + *cursor), channel->name_bytes}; // append_name
    *cursor += channel->name_bytes;
    const u64 overwrite_bytes =
        static_cast<u64>(channel->overwrite_count) * sizeof(GatewayClientPermissionOverwriteMutation);
    if (overwrite_bytes > payload.count - *cursor) {
        return false;
    }
    *overwrite_payload = ConstSlice<u8>{payload.data + *cursor, static_cast<u32>(overwrite_bytes)};
    *cursor += static_cast<u32>(overwrite_bytes);
    ++*index;
    return true;
}

bool gateway_client_event_guild_role(const GatewayClientEvent& event, ConstSlice<u8> payload, u32 index,
                                     GatewayClientRoleMutation* role, Utf8View* name) noexcept {
    if (!role || !name || !guild_event_payload(event, payload) || index >= event.state.role_count ||
        gateway_client_guild_strings_bytes(event.state) > payload.count) {
        return false;
    }
    u32 cursor = gateway_client_guild_strings_bytes(event.state);
    for (u32 current = 0; current <= index; ++current) {
        u32 next = 0;
        if (!read_guild_role(payload, cursor, role, name, &next)) {
            return false;
        }
        cursor = next;
    }
    return true;
}

bool gateway_client_event_guild_role_offsets(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                             Slice<u32> offsets) noexcept {
    // Both callers pass their own kGuildRoleMaximum offsets, apart from the payload and the event.
    if (!guild_event_payload(event, payload) ||
        gateway_client_guild_strings_bytes(event.state) > payload.count || event.state.role_count > offsets.count)
        return false;
    u32 cursor = gateway_client_guild_strings_bytes(event.state);
    for (u32 index = 0; index < event.state.role_count; ++index) {
        GatewayClientRoleMutation role{};
        Utf8View name{};
        const u32 offset = cursor;
        if (!read_guild_role(payload, cursor, &role, &name, &cursor)) return false;
        offsets[index] = offset;
    }
    return true;
}

bool gateway_client_event_guild_role_at(const GatewayClientEvent& event, ConstSlice<u8> payload, u32 offset,
                                        GatewayClientRoleMutation* role, Utf8View* name) noexcept {
    // The offsets scan of this packet checked the bounds of the record at offset. Only the bounds are checked again.
    if (offset < gateway_client_guild_strings_bytes(event.state) || offset > payload.count ||
        sizeof(*role) > payload.count - offset)
        return false;
    copy_bytes(role, payload.data + offset, sizeof(*role));
    offset += sizeof(*role);
    if (role->name_bytes > payload.count - offset)
        return false;
    *name = Utf8View{reinterpret_cast<const char*>(payload.data + offset), role->name_bytes};
    return true;
}

bool gateway_client_event_guild_emojis_begin(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                             u32* begin) noexcept {
    if (!begin || !event.state.emojis_known) {
        return false;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::guild_emojis_update) {
        *begin = 0;
        return true;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::guild_update) {
        return guild_roles_end(event, payload, begin);
    }
    if (event.dispatch_kind != GatewayClientDispatchKind::guild_create) {
        return false;
    }
    u32 cursor = 0;
    u32 index = 0;
    while (index < event.state.record_count) {
        GatewayClientStateMutation channel{};
        Utf8View name{};
        ConstSlice<u8> overwrites{};
        if (!gateway_client_event_next_guild_channel(event, payload, &cursor, &index, &channel, &name,
                                                     &overwrites)) {
            return false;
        }
    }
    if (event.state.record_count == 0 && !guild_channels_begin(event, payload, &cursor)) {
        return false;
    }
    *begin = cursor;
    return true;
}

bool gateway_client_event_next_guild_emoji(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                           u32* cursor, u32* index, GatewayClientEmojiMutation* emoji,
                                           ConstSlice<u8>* role_payload, Utf8View* name) noexcept {
    if (!cursor || !index || !emoji || !role_payload || !name ||
        event.kind != GatewayClientEventKind::dispatch || event.dispatch_payload_bytes != payload.count ||
        !event.state.emojis_known || *index >= event.state.emoji_count ||
        (event.dispatch_kind != GatewayClientDispatchKind::guild_create &&
         event.dispatch_kind != GatewayClientDispatchKind::guild_update &&
         event.dispatch_kind != GatewayClientDispatchKind::guild_emojis_update)) {
        return false;
    }
    if (*index == 0 && *cursor == 0 && !gateway_client_event_guild_emojis_begin(event, payload, cursor)) {
        return false;
    }
    if (*cursor > payload.count || sizeof(*emoji) > payload.count - *cursor) {
        return false;
    }
    copy_bytes(emoji, payload.data + *cursor, sizeof(*emoji));
    *cursor += sizeof(*emoji);
    const u64 role_bytes = static_cast<u64>(emoji->role_count) * 8;
    // visit_emoji (state_dispatch_records.cpp) writes a nonzero id and a name that gateway_state_emoji_name_valid
    // accepts.
    if (role_bytes > payload.count - *cursor) {
        return false;
    }
    *role_payload = ConstSlice<u8>{payload.data + *cursor, static_cast<u32>(role_bytes)};
    *cursor += static_cast<u32>(role_bytes);
    if (emoji->name_bytes > payload.count - *cursor) {
        return false;
    }
    *name = Utf8View{reinterpret_cast<const char*>(payload.data + *cursor), emoji->name_bytes};
    *cursor += emoji->name_bytes;
    ++*index;
    return true;
}

bool gateway_client_event_channel_overwrites(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                             ConstSlice<u8>* overwrite_payload) noexcept {
    if (!overwrite_payload || event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::channel_create &&
         event.dispatch_kind != GatewayClientDispatchKind::channel_update) ||
        (event.state.fields & gateway_client_state_field_permission_overwrites) == 0 ||
        event.dispatch_payload_bytes != payload.count || event.state.name_bytes > payload.count) {
        return false;
    }
    const u64 bytes =
        static_cast<u64>(event.state.overwrite_count) * sizeof(GatewayClientPermissionOverwriteMutation);
    if (bytes != payload.count - event.state.name_bytes) {
        return false;
    }
    const u8* begin = payload.data;
    if (event.state.name_bytes != 0) {
        begin += event.state.name_bytes;
    }
    *overwrite_payload = ConstSlice<u8>{begin, static_cast<u32>(bytes)};
    return true;
}

bool gateway_client_permission_overwrite_at(ConstSlice<u8> overwrite_payload, u32 overwrite_count,
                                            u32 index,
                                            GatewayClientPermissionOverwriteMutation* overwrite) noexcept {
    if (!overwrite || index >= overwrite_count ||
        static_cast<u64>(overwrite_count) * sizeof(*overwrite) != overwrite_payload.count) {
        return false;
    }
    const u32 offset = index * sizeof(*overwrite);
    copy_bytes(overwrite, overwrite_payload.data + offset, sizeof(*overwrite));
    return true; // visit_overwrite writes nonzero ids and only types 0 and 1
}

} // namespace maicord
