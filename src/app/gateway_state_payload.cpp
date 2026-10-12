#include "maicord/app/gateway_client.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

#include "gateway_state_payload.hpp"

namespace maicord {
namespace {

bool read_record(ConstSlice<u8> payload, u32 offset, GatewayClientStateMutation* record) noexcept {
    if (!record || offset > payload.count || sizeof(*record) > payload.count - offset) {
        return false;
    }
    copy_bytes(record, payload.data + offset, sizeof(*record));
    return true;
}

bool offset_output_disjoint(Slice<u32> output, u32 used, const void* input, u64 input_bytes) noexcept {
    if (used == 0 || input_bytes == 0 || !input) return true;
    const usize target = reinterpret_cast<usize>(output.data);
    const usize source = reinterpret_cast<usize>(input);
    const u64 target_bytes = static_cast<u64>(used) * sizeof(u32);
    if (target_bytes > ~usize{0} - target || input_bytes > ~usize{0} - source) return false;
    return target < source ? source - target >= target_bytes : target - source >= input_bytes;
}

bool read_guild_role(ConstSlice<u8> payload, u32 offset, GatewayClientRoleMutation* role, Utf8View* name,
                     u32* next_offset) noexcept {
    if (!role || !name || !next_offset || offset > payload.count ||
        sizeof(*role) > payload.count - offset) {
        return false;
    }
    copy_bytes(role, payload.data + offset, sizeof(*role));
    offset += sizeof(*role);
    if (role->id == 0 || role->name_bytes > payload.count - offset) {
        return false;
    }
    *name = Utf8View{reinterpret_cast<const char*>(payload.data + offset), role->name_bytes};
    if (!utf8_validate(*name).valid) {
        return false;
    }
    *next_offset = offset + role->name_bytes;
    return true;
}

bool guild_roles_end(const GatewayClientEvent& event, ConstSlice<u8> payload, u32* end) noexcept {
    if (!end || gateway_client_guild_strings_bytes(event.state) > payload.count) {
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

} // namespace

bool gateway_client_event_as_ready_guilds(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                          Slice<u64> guild_ids, u32* guild_count) noexcept {
    if (!guild_count || event.kind != GatewayClientEventKind::ready ||
        event.dispatch_payload_bytes != payload.count || payload.count != event.state.record_count * 8u ||
        event.state.record_count > guild_ids.count || (!guild_ids.data && event.state.record_count != 0)) {
        return false;
    }
    for (u32 index = 0; index < event.state.record_count; ++index) {
        const u64 id = byte_load_le64(payload.data + index * 8);
        if (id == 0) {
            return false;
        }
        guild_ids[index] = id;
    }
    *guild_count = event.state.record_count;
    return true;
}

bool gateway_client_event_state_name(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                     Utf8View* name) noexcept {
    if (!name || event.dispatch_payload_bytes != payload.count || event.state.name_bytes > payload.count ||
        (event.state.name_bytes != 0 && !payload.data)) {
        return false;
    }
    *name = Utf8View{reinterpret_cast<const char*>(payload.data), event.state.name_bytes};
    return utf8_validate(*name).valid;
}

bool gateway_client_event_guild_icon(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                     Utf8View* icon_hash) noexcept {
    if (!icon_hash || event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::guild_create &&
         event.dispatch_kind != GatewayClientDispatchKind::guild_update) ||
        event.dispatch_payload_bytes != payload.count ||
        (event.state.fields & gateway_client_state_field_icon) == 0 ||
        event.state.name_bytes > payload.count ||
        event.state.icon_bytes > payload.count - event.state.name_bytes ||
        ((event.state.name_bytes != 0 || event.state.icon_bytes != 0) && !payload.data)) {
        return false;
    }
    const char* begin =
        payload.data ? reinterpret_cast<const char*>(payload.data) + event.state.name_bytes : nullptr;
    *icon_hash = Utf8View{begin, event.state.icon_bytes};
    return utf8_validate(*icon_hash).valid;
}

bool gateway_client_event_guild_banner(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                       Utf8View* banner_hash) noexcept {
    if (!banner_hash || event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::guild_create &&
         event.dispatch_kind != GatewayClientDispatchKind::guild_update) ||
        event.dispatch_payload_bytes != payload.count || !event.state.guild_banner ||
        gateway_client_guild_strings_bytes(event.state) > payload.count ||
        (gateway_client_guild_banner_bytes(event.state) && !payload.data)) {
        return false;
    }
    const u32 offset = static_cast<u32>(event.state.name_bytes) + event.state.icon_bytes;
    const u32 bytes = gateway_client_guild_banner_bytes(event.state);
    *banner_hash = Utf8View{bytes ? reinterpret_cast<const char*>(payload.data) + offset : nullptr, bytes};
    return utf8_validate(*banner_hash).valid;
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
    if (*index == 0 && *cursor == 0) {
        u32 roles_end = 0;
        if (!guild_roles_end(event, payload, &roles_end)) {
            return false;
        }
        const u64 prefix =
            static_cast<u64>(roles_end) + static_cast<u64>(event.state.member_role_count) * 8;
        if (prefix > payload.count) {
            return false;
        }
        *cursor = static_cast<u32>(prefix);
    }
    if (*cursor < event.state.name_bytes || !read_record(payload, *cursor, channel)) {
        return false;
    }
    *cursor += sizeof(*channel);
    if ((channel->content_policy & ~1u) || (channel->content_policy && !channel->content_policy_known) ||
        channel->name_bytes > payload.count - *cursor) {
        return false;
    }
    *name = Utf8View{reinterpret_cast<const char*>(payload.data + *cursor), channel->name_bytes};
    if (!utf8_validate(*name).valid) {
        return false;
    }
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
    if (!role || !name || event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::guild_create &&
         event.dispatch_kind != GatewayClientDispatchKind::guild_update) ||
        event.dispatch_payload_bytes != payload.count || index >= event.state.role_count) {
        return false;
    }
    if (gateway_client_guild_strings_bytes(event.state) > payload.count) {
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
    if (event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::guild_create &&
         event.dispatch_kind != GatewayClientDispatchKind::guild_update) ||
        event.dispatch_payload_bytes != payload.count ||
        gateway_client_guild_strings_bytes(event.state) > payload.count || event.state.role_count > offsets.count ||
        (!offsets.data && event.state.role_count != 0) || (!payload.data && payload.count != 0) ||
        !offset_output_disjoint(offsets, event.state.role_count, payload.data, payload.count) ||
        !offset_output_disjoint(offsets, event.state.role_count, &event, sizeof(event))) return false;
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
    if (!role || !name || event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::guild_create &&
         event.dispatch_kind != GatewayClientDispatchKind::guild_update) ||
        event.dispatch_payload_bytes != payload.count ||
        gateway_client_guild_strings_bytes(event.state) > payload.count ||
        offset < gateway_client_guild_strings_bytes(event.state) ||
        (!payload.data && payload.count != 0)) return false;
    u32 next = 0;
    return read_guild_role(payload, offset, role, name, &next);
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
    if (event.state.record_count == 0) {
        u32 roles_end = 0;
        if (!guild_roles_end(event, payload, &roles_end)) {
            return false;
        }
        const u64 prefix =
            static_cast<u64>(roles_end) + static_cast<u64>(event.state.member_role_count) * 8;
        if (prefix > payload.count) {
            return false;
        }
        cursor = static_cast<u32>(prefix);
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
    if (emoji->id == 0 || emoji->name_bytes == 0 || emoji->name_bytes > 32 ||
        role_bytes > payload.count - *cursor) {
        return false;
    }
    *role_payload = ConstSlice<u8>{payload.data + *cursor, static_cast<u32>(role_bytes)};
    *cursor += static_cast<u32>(role_bytes);
    if (emoji->name_bytes > payload.count - *cursor) {
        return false;
    }
    *name = Utf8View{reinterpret_cast<const char*>(payload.data + *cursor), emoji->name_bytes};
    if (!gateway_state_emoji_name_valid(*name)) {
        return false;
    }
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
    return overwrite->id != 0 && overwrite->type <= 1;
}

} // namespace maicord
