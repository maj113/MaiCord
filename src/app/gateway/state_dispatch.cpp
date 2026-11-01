#include "state_dispatch_internal.hpp"
#include "dispatch_common.hpp"
#include "maicord/protocol/media/upload_allowance.hpp"
#include "maicord/protocol/messages/thread_actions.hpp"

namespace maicord {
namespace {

// Out of line: a private channel's recipient, nickname and safety-warning contexts would otherwise join the
// frame of every channel event.
__declspec(noinline) GatewayClientDispatchRouteResult route_private_channel_parts(
    const StateObjectContext& object, Slice<u32> structural_scratch, u32 maximum_depth, PayloadWriter& writer,
    GatewayClientEvent* event) noexcept {
    u32 object_scratch[512];
    if ((object.mutation.fields & gateway_client_state_field_icon) && object.icon.encoded.bytes) {
        if (!append_icon(&writer, object.icon, &event->state.icon_bytes))
            return route_error(GatewayClientDispatchRouteError::payload_capacity);
    }
    if (object.recipient_ids_seen || object.recipients_seen) {
        const JsonValue recipients = object.recipient_ids_seen ? object.recipient_ids : object.recipients;
        const auto scan = json_scan_structurals_avx1(recipients.source, structural_scratch);
        if (!scan.ok())
            return decode_route(GatewayEventDecodeError::scan_failed);
        RecipientContext context{&writer, {object_scratch, 512}, maximum_depth, 0, object.recipient_ids_seen, {}};
        const auto read = json_visit_value_array(recipients, {structural_scratch.data, scan.count}, maximum_depth,
                                                 append_recipient, &context);
        if (!context.result.ok())
            return context.result;
        if (!read.ok())
            return decode_route(GatewayEventDecodeError::invalid_json);
        event->state.recipient_count = context.count;
        event->state.recipients_known = true;
    }
    if (object.nicks_seen) {
        NicknameContext context{&writer, nullptr, maximum_depth, 0, {}};
        if (object.nicks.kind == JsonValueKind::array) {
            const auto scan = json_scan_structurals_avx1(object.nicks.source, structural_scratch);
            if (!scan.ok())
                return decode_route(GatewayEventDecodeError::scan_failed);
            const JsonIndexedDocument document{object.nicks.source, {structural_scratch.data, scan.count}};
            context.document = &document;
            const auto read = json_visit_indexed_array(document, object.nicks, maximum_depth, append_nickname,
                                                       &context);
            if (!context.result.ok())
                return context.result;
            if (!read.ok())
                return decode_route(GatewayEventDecodeError::invalid_json);
        }
        event->state.record_count = context.count;
        event->state.private_nicknames_known = true;
    }
    u8 flags = 0;
    if (object.message_request_seen || object.spam_seen)
        flags |= gateway_client_private_request_known |
                 (object.message_request ? gateway_client_private_request_pending : 0) |
                 (object.spam ? gateway_client_private_request_spam : 0);
    SafetyWarningContext warning{};
    if (object.safety_warnings_seen) {
        flags |= gateway_client_private_safety_known;
        if (object.safety_warnings.kind == JsonValueKind::array) {
            const auto scan = json_scan_structurals_avx1(object.safety_warnings.source, structural_scratch);
            const JsonIndexedDocument document{object.safety_warnings.source,
                                               {structural_scratch.data, scan.ok() ? scan.count : 0}};
            warning.document = &document;
            if (scan.ok())
                json_visit_indexed_array(document, {object.safety_warnings.source, 0, JsonValueKind::array},
                                         maximum_depth, safety_warning_item, &warning);
        }
        if (warning.id)
            flags |= static_cast<u8>(warning.type << kGatewayClientPrivateSafetyShift);
    }
    if ((flags && !gateway_payload_append(&writer, reinterpret_cast<const char*>(&flags), 1)) ||
        (warning.id && !gateway_payload_append_u64(&writer, warning.id)))
        return route_error(GatewayClientDispatchRouteError::payload_capacity);
    return route_error(GatewayClientDispatchRouteError::none);
}

GatewayClientDispatchRouteResult route_channel_object(const StateObjectContext& object,
                                                      Slice<u32> structural_scratch, u32 maximum_depth,
                                                      PayloadWriter& writer, GatewayClientEvent* event,
                                                      bool channel_create, bool channel_update,
                                                      bool channel_delete) noexcept {
    u32 object_scratch[512];
    const bool private_channel = object.mutation.type == 1 || object.mutation.type == 3;
    const u16 required = gateway_client_state_field_id | gateway_client_state_field_type |
                         (private_channel ? 0 : gateway_client_state_field_guild_id);
    if ((object.mutation.fields & required) != required) {
        return decode_route(GatewayEventDecodeError::missing_required_field);
    }
    if (private_channel && (object.mutation.guild_id != 0 || object.mutation.parent_id != 0 ||
                            object.permission_overwrites_seen))
        return decode_route(GatewayEventDecodeError::invalid_data);
    event->dispatch_kind = channel_create ? GatewayClientDispatchKind::channel_create
                                          : (channel_update ? GatewayClientDispatchKind::channel_update
                                                            : GatewayClientDispatchKind::channel_delete);
    if ((object.mutation.fields & gateway_client_state_field_name) != 0 && !object.name_null &&
        !append_name(&writer, object.name, &event->state.name_bytes)) {
        return route_error(GatewayClientDispatchRouteError::payload_capacity);
    }
    if (private_channel && !channel_delete) {
        const auto parts = route_private_channel_parts(object, structural_scratch, maximum_depth, writer, event);
        if (!parts.ok())
            return parts;
    }
    if (!channel_delete && object.permission_overwrites_seen) {
        const GatewayEventDecodeResult appended = append_overwrite_array(
            object.permission_overwrites, structural_scratch, Slice<u32>{object_scratch, 512},
            maximum_depth, &writer, &event->state.overwrite_count);
        if (!appended.ok())
            return append_failure(appended.error);
    }
    return route_error(GatewayClientDispatchRouteError::none);
}

} // namespace

GatewayClientDispatchRouteResult gateway_client_route_channel_create(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> storage,
    u64 self_user_id, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    if (!event || !payload)
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    StateObjectContext object;
    const auto decoded = decode_state_object(data, structural_scratch, maximum_depth, &object, true);
    if (!decoded.ok())
        return decode_route(decoded.error);
    if (!hydrate_self_member(&object, self_user_id))
        return decode_route(GatewayEventDecodeError::invalid_data);
    PayloadWriter writer{storage, 0};
    event->state = object.mutation;
    auto routed =
        route_channel_object(object, structural_scratch, maximum_depth, writer, event, true, false, false);
    routed.payload_overflow = !routed.ok() && writer.overflow;
    if (routed.ok())
        *payload = {reinterpret_cast<const u8*>(writer.output.data), writer.used};
    return routed;
}

GatewayClientDispatchRouteResult gateway_client_route_private_channel(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> storage,
    GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    if (!event || !payload)
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    StateObjectContext object;
    const auto decoded = decode_state_object(data, structural_scratch, maximum_depth, &object, true);
    if (!decoded.ok())
        return decode_route(decoded.error);
    if (object.mutation.type != 1 && object.mutation.type != 3)
        return decode_route(GatewayEventDecodeError::invalid_data);
    reset_value(event);
    event->kind = GatewayClientEventKind::dispatch;
    event->state = object.mutation;
    PayloadWriter writer{storage, 0};
    const auto routed =
        route_channel_object(object, structural_scratch, maximum_depth, writer, event, true, false, false);
    if (!routed.ok())
        return routed;
    *payload = {reinterpret_cast<const u8*>(storage.data), writer.used};
    event->dispatch_payload_bytes = writer.used;
    return routed;
}

namespace {

// Out of line: the 4 KB of emoji role index would otherwise cost every state event a stack probe.
__declspec(noinline) GatewayEventDecodeResult append_emoji_array_with_roles(
    JsonValue emojis, Slice<u32> structural_scratch, Slice<u32> object_scratch, u32 maximum_depth,
    PayloadWriter* writer, u32* emoji_count, u32* role_count) noexcept {
    u32 role_scratch[1'024];
    return append_emoji_array(emojis, structural_scratch, object_scratch, Slice<u32>{role_scratch, 1'024},
                              maximum_depth, writer, emoji_count, role_count);
}

// The branches of route_state_dispatch that visit nested arrays or decode a second object, out of line so
// their index and context locals do not join the frame of every state event.
__declspec(noinline) GatewayClientDispatchRouteResult route_guild_emojis(
    StateObjectContext& object, Slice<u32> structural_scratch, u32 maximum_depth, PayloadWriter& writer,
    GatewayClientEvent* event) noexcept {
    u32 object_scratch[512];
    if ((object.mutation.fields & gateway_client_state_field_guild_id) == 0 || !object.emojis_seen) {
        return decode_route(GatewayEventDecodeError::missing_required_field);
    }
    const GatewayEventDecodeResult appended =
        append_emoji_array_with_roles(object.emojis, structural_scratch, Slice<u32>{object_scratch, 512},
                                      maximum_depth, &writer, &event->state.emoji_count,
                                      &event->state.emoji_role_count);
    if (!appended.ok())
        return append_failure(appended.error);
    event->state.emojis_known = true;
    event->dispatch_kind = GatewayClientDispatchKind::guild_emojis_update;
    return route_error(GatewayClientDispatchRouteError::none);
}

__declspec(noinline) GatewayClientDispatchRouteResult route_role_object(
    StateObjectContext& object, bool role_create, bool role_delete, u32 maximum_depth,
    PayloadWriter& writer, GatewayClientEvent* event) noexcept {
    u32 object_scratch[512];
    if ((object.mutation.fields & gateway_client_state_field_guild_id) == 0) {
        return decode_route(GatewayEventDecodeError::missing_required_field);
    }
    if (role_delete) {
        if ((object.mutation.fields & gateway_client_state_field_id) == 0) {
            return decode_route(GatewayEventDecodeError::missing_required_field);
        }
        event->dispatch_kind = GatewayClientDispatchKind::guild_role_delete;
        return route_error(GatewayClientDispatchRouteError::none);
    }
    if (!object.role_seen) {
        return decode_route(GatewayEventDecodeError::missing_required_field);
    }
    StateObjectContext role;
    const GatewayEventDecodeResult role_decoded =
        decode_state_object(object.role, Slice<u32>{object_scratch, 512}, maximum_depth, &role);
    constexpr u16 required = gateway_client_state_field_id | gateway_client_state_field_permissions;
    if (!role_decoded.ok() || (role.mutation.fields & required) != required) {
        return decode_route(role_decoded.ok() ? GatewayEventDecodeError::missing_required_field
                                              : role_decoded.error);
    }
    event->state = role.mutation;
    event->state.role_flags = role_flags(role);
    event->state.guild_id = object.mutation.guild_id;
    event->state.fields |= gateway_client_state_field_guild_id;
    if ((role.mutation.fields & gateway_client_state_field_name) != 0 &&
        !append_name(&writer, role.name, &event->state.name_bytes)) {
        return route_error(GatewayClientDispatchRouteError::payload_capacity);
    }
    event->dispatch_kind = role_create ? GatewayClientDispatchKind::guild_role_create
                                       : GatewayClientDispatchKind::guild_role_update;
    return route_error(GatewayClientDispatchRouteError::none);
}

__declspec(noinline) GatewayClientDispatchRouteResult route_member_update(
    const GatewayEnvelope& envelope, StateObjectContext& object, Slice<u32> structural_scratch, u32 maximum_depth,
    u64 self_user_id, PayloadWriter& writer, bool* handled, GatewayClientEvent* event) noexcept {
    u32 object_scratch[512];
    if ((object.mutation.fields & gateway_client_state_field_guild_id) == 0 || self_user_id == 0) {
        return decode_route(GatewayEventDecodeError::missing_required_field);
    }
    MemberIdentity identity{};
    if (!member_identity(envelope.data, {object_scratch, 512}, maximum_depth, &identity) ||
        (!identity.user_id_seen && !identity.user_seen) ||
        (identity.user_id_seen && identity.user_seen && identity.user_id != identity.nested_id)) {
        return decode_route(GatewayEventDecodeError::invalid_data);
    }
    const u64 member_id = identity.user_id_seen ? identity.user_id : identity.nested_id;
    if (member_id != self_user_id) {
        event->dispatch_kind = GatewayClientDispatchKind::unknown;
        // Non-self metadata belongs to the bounded member hydration route,
        // while the self route above retains its permission-role authority.
        *handled = false;
        return route_error(GatewayClientDispatchRouteError::none);
    }
    event->state.member_role_flags = 0;
    member_state_from_flags(&event->state);
    event->state.guild_settings_known &=
        static_cast<u16>(guild_settings_self_timeout | guild_settings_self_member | guild_settings_self_quarantine);
    u32 role_count = 0;
    const GatewayEventDecodeResult roles =
        !object.roles_seen ? GatewayEventDecodeResult{}
                           : append_snowflake_array(object.roles, structural_scratch, maximum_depth, &writer,
                                                    &role_count);
    if (!roles.ok())
        return append_failure(roles.error);
    if (object.roles_seen)
        event->state.member_role_flags = gateway_client_member_roles_known;
    event->state.id = member_id;
    event->state.fields |= gateway_client_state_field_id;
    event->state.member_role_count = role_count;
    event->state.record_count = role_count;
    event->dispatch_kind = GatewayClientDispatchKind::guild_member_update;
    return route_error(GatewayClientDispatchRouteError::none);
}

__declspec(noinline) GatewayClientDispatchRouteResult route_guild_object(
    StateObjectContext& object, Slice<u32> structural_scratch, u32 maximum_depth, u64 self_user_id,
    JsonValue ready_members, bool ready_guild, bool guild_create, bool guild_update,
    bool largest, PayloadWriter& writer, GatewayClientEvent* event) noexcept {
    u32 object_scratch[512];
    event->state.member_role_flags = 0;
    if (ready_guild && ready_members.source.data) {
        if (ready_members.kind != JsonValueKind::array)
            return decode_route(GatewayEventDecodeError::invalid_data);
        object.members = ready_members;
        object.members_seen = true;
    }
    if ((object.mutation.fields & gateway_client_state_field_id) == 0) {
        return route_detail(GatewayClientDispatchRouteError::decode,
                            GatewayEventDecodeError::missing_required_field, 1);
    }
    event->dispatch_kind = guild_create ? GatewayClientDispatchKind::guild_create
                                        : (guild_update ? GatewayClientDispatchKind::guild_update
                                                        : GatewayClientDispatchKind::guild_delete);
    const bool unavailable = object.mutation.unavailable &&
                             (object.mutation.fields & gateway_client_state_field_unavailable) != 0;
    event->state.guild_settings_known &=
        static_cast<u16>(~(guild_settings_self_timeout | guild_settings_self_member |
                           guild_settings_self_quarantine));
    event->state.self_member = 0;
    constexpr u16 guild_required = gateway_client_state_field_name | gateway_client_state_field_owner_id;
    if (((guild_create && !unavailable) || guild_update) &&
        ((object.mutation.fields & guild_required) != guild_required || !object.roles_seen)) {
        return route_detail(
            GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::missing_required_field,
            2 | (static_cast<u32>(object.mutation.fields) << 8) | (object.roles_seen ? 1u << 24 : 0));
    }
    // A live GUILD_CREATE for a server joined elsewhere may carry no member
    // list, or one without the current member (a preview); both still apply.
    if (guild_create && !unavailable && (!object.channels_seen || self_user_id == 0)) {
        return route_detail(
            GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::missing_required_field,
            3 | (object.channels_seen ? 1u << 8 : 0) | (object.members_seen ? 1u << 9 : 0) |
                (object.threads_seen ? 1u << 10 : 0));
    }
    if ((object.mutation.fields & gateway_client_state_field_name) != 0 &&
        !append_name(&writer, object.name, &event->state.name_bytes)) {
        return route_error(GatewayClientDispatchRouteError::payload_capacity);
    }
    if ((object.mutation.fields & gateway_client_state_field_icon) != 0 && object.icon.encoded.bytes != 0 &&
        !append_icon(&writer, object.icon, &event->state.icon_bytes)) {
        return route_error(GatewayClientDispatchRouteError::payload_capacity);
    }
    // The banner follows the icon: guild_banner is 1 + its bytes (1: null).
    if (object.banner_seen && (guild_create || guild_update)) {
        u8 bytes = 0;
        const u32 mark = writer.used;
        if (object.banner.encoded.bytes != 0 && !append_icon(&writer, object.banner, &bytes)) {
            if (writer.overflow)
                return route_error(GatewayClientDispatchRouteError::payload_capacity);
            writer.used = mark; // An oversized or undecodable hash is dropped.
        } else {
            event->state.guild_banner = static_cast<u8>(1u + bytes);
        }
    }
    event->state.guild_details_bytes = 0;
    if ((guild_create && !unavailable) || guild_update) {
        if (!gateway_guild_details_append(object.details, object.features, object.features_seen,
                                          structural_scratch, maximum_depth, &writer,
                                          &event->state.guild_details_bytes))
            return route_error(GatewayClientDispatchRouteError::payload_capacity);
        GatewayEventDecodeResult appended =
            append_role_array(object.roles, structural_scratch, Slice<u32>{object_scratch, 512}, maximum_depth,
                              &writer, &event->state.role_count);
        if (!appended.ok())
            return append_failure(appended.error, 4);
        bool self_found = false;
        if (guild_create && object.members_seen) {
            appended = append_self_member_roles(object.members, structural_scratch,
                                                Slice<u32>{object_scratch, 512}, maximum_depth, self_user_id,
                                                &writer, &event->state.member_role_count, &event->state,
                                                &self_found);
            if (!appended.ok())
                return append_failure(appended.error, 5);
        }
        // A guild without a join time reads as previewed (lurked).
        if (guild_create && !ready_guild && !self_found && object.joined_at_seen && !object.joined) {
            event->state.guild_settings_known =
                static_cast<u16>(event->state.guild_settings_known | guild_settings_self_member);
            event->state.self_member = static_cast<u8>(event->state.self_member | guild_self_member_lurking);
        }
    }
    if (guild_create && !unavailable) {
        u32 record_count = 0, skipped_channels = 0;
        u32 overwrite_count = 0;
        GatewayEventDecodeResult appended = append_channel_array(
            object.channels, structural_scratch, Slice<u32>{object_scratch, 512}, maximum_depth,
            object.mutation.id, &writer, &record_count, &overwrite_count, self_user_id, &skipped_channels);
        if (!appended.ok()) {
            auto failed = append_failure(appended.error, 6);
            failed.scan_error = appended.scan_error;
            failed.read_error = appended.read_error;
            failed.error_offset = appended.error_offset;
            return failed;
        }
        if (object.threads_seen) {
            // Active threads are supplementary (thread lists refetch them).
            // When they do not fit or cannot be read, keep the guild
            // without them rather than losing the whole guild.
            const u32 mark = writer.used, records_mark = record_count, overwrites_mark = overwrite_count;
            appended = append_channel_array(
                object.threads, structural_scratch, Slice<u32>{object_scratch, 512}, maximum_depth,
                object.mutation.id, &writer, &record_count, &overwrite_count, self_user_id, &skipped_channels);
            if (!appended.ok()) {
                if (appended.error == GatewayEventDecodeError::scan_failed &&
                    appended.scan_error == JsonScanError::output_overflow) {
                    auto failed = route_detail(GatewayClientDispatchRouteError::decode, appended.error, 7);
                    failed.scan_error = appended.scan_error;
                    failed.error_offset = appended.error_offset;
                    return failed;
                }
                if (writer.overflow && !largest)
                    return route_detail(GatewayClientDispatchRouteError::payload_capacity, appended.error, 7);
                writer.used = mark;
                writer.overflow = false;
                record_count = records_mark;
                overwrite_count = overwrites_mark;
            }
        }
        event->state.record_count = record_count;
        event->state.overwrite_count = overwrite_count;
    }
    if (((guild_create && !unavailable) || guild_update) && object.emojis_seen) {
        const u32 mark = writer.used;
        const GatewayEventDecodeResult appended =
            append_emoji_array_with_roles(object.emojis, structural_scratch, Slice<u32>{object_scratch, 512},
                                          maximum_depth, &writer, &event->state.emoji_count,
                                          &event->state.emoji_role_count);
        if (!appended.ok() && appended.error == GatewayEventDecodeError::scan_failed &&
            appended.scan_error == JsonScanError::output_overflow) {
            auto failed = route_detail(GatewayClientDispatchRouteError::decode, appended.error, 8);
            failed.scan_error = appended.scan_error;
            return failed;
        }
        if (!appended.ok() && writer.overflow && !largest)
            return route_detail(GatewayClientDispatchRouteError::payload_capacity, appended.error, 9);
        // Custom emoji are supplementary. A list beyond the native bound
        // or an unreadable entry leaves the emoji set unknown (a later
        // GUILD_EMOJIS_UPDATE supplies it) and keeps the guild.
        if (appended.ok()) {
            event->state.emojis_known = true;
        } else {
            writer.used = mark;
            writer.overflow = false;
            event->state.emoji_count = event->state.emoji_role_count = 0;
        }
    }
    return route_error(GatewayClientDispatchRouteError::none);
}

GatewayClientDispatchRouteResult route_state_dispatch(const GatewayEnvelope& envelope,
                                                      Slice<u32> structural_scratch, u32 maximum_depth,
                                                      Slice<char> payload_scratch, u64 self_user_id,
                                                      GatewayClientEvent* event, ConstSlice<u8>* payload,
                                                      bool* handled, bool* overflowed,
                                                      JsonValue ready_members = {}) noexcept {
    if (!event || !payload || !handled) {
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    }
    const u32 event_index =
        skim_member_index<"READY_GUILD", "THREAD_LIST_SYNC", "THREAD_MEMBER_UPDATE", "GUILD_CREATE", "GUILD_UPDATE",
                          "GUILD_DELETE", "THREAD_CREATE", "CHANNEL_CREATE", "CHANNEL_UPDATE", "THREAD_UPDATE",
                          "CHANNEL_DELETE", "THREAD_DELETE", "GUILD_ROLE_CREATE", "GUILD_ROLE_UPDATE",
                          "GUILD_ROLE_DELETE", "GUILD_MEMBER_UPDATE", "GUILD_EMOJIS_UPDATE">(envelope.event_name);
    const bool ready_guild = event_index == 0;
    if (event_index == 1) {
        *handled = true;
        return route_thread_sync(envelope, structural_scratch, maximum_depth, payload_scratch, self_user_id, event, payload);
    }
    if (event_index == 2) {
        *handled = true;
        StateObjectContext member;
        const auto decoded = decode_state_object(envelope.data, structural_scratch, maximum_depth, &member);
        ThreadMemberSnapshot receipt{};
        if (!decoded.ok() || !member.mutation.id || !member.mutation.guild_id ||
            !thread_member_decode(envelope.data.source, member.mutation.id, self_user_id, &receipt).ok())
            return decode_route(GatewayEventDecodeError::invalid_data);
        event->dispatch_kind = GatewayClientDispatchKind::thread_self_update;
        event->thread_self = {member.mutation.guild_id, receipt.thread_id, self_user_id,
                             gateway_thread_self_known | gateway_thread_self_joined, {},
                             thread_join_seconds_from_ms(receipt.join_timestamp_ms)};
        reset_value(payload); event->dispatch_payload_bytes = 0;
        return {};
    }
    const bool guild_create = ready_guild || event_index == 3;
    const bool guild_update = event_index == 4;
    const bool guild_delete = event_index == 5;
    const bool thread_create = event_index == 6;
    const bool channel_create = event_index == 7 || thread_create;
    const bool channel_update = event_index == 8 || event_index == 9;
    const bool channel_delete = event_index == 10 || event_index == 11;
    const bool role_create = event_index == 12;
    const bool role_update = event_index == 13;
    const bool role_delete = event_index == 14;
    const bool member_update = event_index == 15;
    const bool guild_emojis_update = event_index == 16;
    *handled = guild_create || guild_update || guild_delete || channel_create || channel_update ||
               channel_delete || role_create || role_update || role_delete || member_update ||
               guild_emojis_update;
    if (!*handled) {
        return route_error(GatewayClientDispatchRouteError::none);
    }

    StateObjectContext object;
    const GatewayEventDecodeResult decoded =
        decode_state_object(envelope.data, structural_scratch, maximum_depth, &object,
                            channel_create || channel_update || channel_delete);
    if (!decoded.ok()) {
        auto result = decode_route(decoded.error);
        // READY can grow its nested index and retry this record. Keep the
        // established error contract of the other state/receipt routes.
        if (ready_guild) {
            result.scan_error = decoded.scan_error;
            result.read_error = decoded.read_error;
            result.error_offset = decoded.error_offset;
        }
        return result;
    }
    if ((channel_create || channel_update) && !hydrate_self_member(&object, self_user_id))
        return decode_route(GatewayEventDecodeError::invalid_data);
    if ((guild_create || guild_update) && object.properties_seen) {
        StateObjectContext properties;
        const GatewayEventDecodeResult property_decoded =
            decode_state_object(object.properties, structural_scratch, maximum_depth, &properties);
        if (!property_decoded.ok() || !merge_guild_properties(&object, properties)) {
            return route_detail(
                GatewayClientDispatchRouteError::decode,
                property_decoded.ok() ? GatewayEventDecodeError::invalid_data : property_decoded.error, 8);
        }
    }
    PayloadWriter writer{payload_scratch, 0};
    // Every return below reports whether the scratch filled up, so a caller
    // with larger storage can route the same record again.
    struct OverflowReport {
        const PayloadWriter& writer;
        bool* output;
        ~OverflowReport() { *output = writer.overflow; }
    } overflow_report{writer, overflowed};
    // Supplementary parts are dropped on overflow only at the largest record
    // capacity; below it the whole record is routed again with more room.
    const bool largest = payload_scratch.count >= kGatewayClientGuildRecordCapacity;
    // The envelope scan is complete, so its structural storage can be reused
    // by nested arrays. The branches that visit nested arrays keep their own
    // object index (route_guild_object and the helpers above).
    event->state = object.mutation;

    if (channel_create || channel_update || channel_delete)
        event->state.channel_event_flags = thread_create && object.newly_created
            ? gateway_client_channel_new_thread : 0;

    GatewayClientDispatchRouteResult routed = route_error(GatewayClientDispatchRouteError::none);
    if (guild_emojis_update)
        routed = route_guild_emojis(object, structural_scratch, maximum_depth, writer, event);
    else if (role_create || role_update || role_delete)
        routed = route_role_object(object, role_create, role_delete, maximum_depth, writer, event);
    else if (member_update)
        routed = route_member_update(envelope, object, structural_scratch, maximum_depth, self_user_id, writer,
                                     handled, event);
    else if (guild_create || guild_update || guild_delete)
        routed = route_guild_object(object, structural_scratch, maximum_depth, self_user_id, ready_members,
                                    ready_guild, guild_create, guild_update, largest, writer, event);
    else
        routed = route_channel_object(object, structural_scratch, maximum_depth, writer, event, channel_create,
                                      channel_update, channel_delete);
    if (!routed.ok())
        return routed;
    *payload = ConstSlice<u8>{reinterpret_cast<const u8*>(writer.output.data), writer.used};
    return route_error(GatewayClientDispatchRouteError::none);
}

} // namespace

GatewayClientDispatchRouteResult gateway_client_route_state_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> payload_scratch, u64 self_user_id, GatewayClientEvent* event, ConstSlice<u8>* payload,
    bool* handled) noexcept {
    bool overflowed = false;
    auto routed = route_state_dispatch(envelope, structural_scratch, maximum_depth, payload_scratch, self_user_id,
                                       event, payload, handled, &overflowed);
    routed.payload_overflow = !routed.ok() && overflowed;
    return routed;
}

GatewayClientDispatchRouteResult gateway_client_route_ready_guild(const GatewayEnvelope& envelope,
                                                                  JsonValue members, Slice<u32> scratch,
                                                                  Slice<char> storage, u64 self_user_id,
                                                                  GatewayClientLargeRecord large,
                                                                  GatewayClientEvent* event,
                                                                  ConstSlice<u8>* payload) noexcept {
    if (!event || !payload || !storage.data || !storage.count || !self_user_id)
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    u32 capacity = storage.count;
    for (;;) {
        reset_value(event);
        reset_value(payload);
        event->kind = GatewayClientEventKind::dispatch;
        event->sequence = envelope.sequence;
        event->has_sequence = envelope.has_sequence;
        bool handled = false, overflowed = false;
        auto routed = route_state_dispatch(envelope, scratch, 64, storage, self_user_id, event, payload,
                                           &handled, &overflowed, members);
        routed.payload_overflow = !routed.ok() && overflowed;
        event->dispatch_payload_bytes = payload->count;
        if (routed.ok()) {
            gateway_client_append_notification_sidecar(envelope, scratch, storage, self_user_id, event,
                                                       payload);
            return routed;
        }
        if (!overflowed || !large.acquire || capacity >= kGatewayClientGuildRecordCapacity)
            return routed;
        capacity = grown_record_capacity(capacity);
        if (!large.acquire(large.context, capacity, &storage) || !storage.data || storage.count < capacity)
            return routed;
    }
}

GatewayClientDispatchRouteResult gateway_client_route_self_members(JsonValue members, u64 guild_id,
                                                                   u64 self_user_id, Slice<u32> scratch,
                                                                   Slice<char> storage,
                                                                   GatewayClientEvent* event,
                                                                   ConstSlice<u8>* payload) noexcept {
    if (!event || !payload || !guild_id || !self_user_id || members.kind != JsonValueKind::array)
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    reset_value(event);
    reset_value(payload);
    u32 object_scratch[512];
    PayloadWriter writer{storage, 0};
    bool found = false;
    const auto decoded =
        append_self_member_roles(members, scratch, {object_scratch, 512}, 64, self_user_id, &writer,
                                 &event->state.member_role_count, &event->state, &found);
    if (!decoded.ok() || event->state.member_role_count > kGatewayGuildRoleMaximum)
        return decode_route(decoded.ok() ? GatewayEventDecodeError::invalid_data : decoded.error);
    if (!found)
        return {};
    event->kind = GatewayClientEventKind::dispatch;
    event->dispatch_kind = GatewayClientDispatchKind::guild_member_update;
    event->state.id = self_user_id;
    event->state.guild_id = guild_id;
    event->state.fields = gateway_client_state_field_id | gateway_client_state_field_guild_id;
    event->state.record_count = event->state.member_role_count;
    event->dispatch_payload_bytes = writer.used;
    *payload = {reinterpret_cast<const u8*>(storage.data), writer.used};
    return {};
}

GatewayClientDispatchRouteResult gateway_client_route_dispatch_large(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> content_scratch, Slice<u64> bulk_ids, u64 self_user_id, GatewayClientLargeRecord large,
    GatewayClientEvent* event, ConstSlice<u8>* payload, GatewayMessageDecodeScratch* message_scratch) noexcept {
    auto routed = gateway_client_route_dispatch(envelope, structural_scratch, maximum_depth, content_scratch,
                                                bulk_ids, self_user_id, event, payload, message_scratch);
    u32 capacity = content_scratch.count;
    while (!routed.ok() && routed.payload_overflow && large.acquire &&
           capacity < kGatewayClientGuildRecordCapacity) {
        // Grow geometrically: an oversized guild routes at most four more
        // times, and only the pages its record touches are ever committed.
        capacity = grown_record_capacity(capacity);
        Slice<char> storage{};
        if (!large.acquire(large.context, capacity, &storage) || !storage.data || storage.count < capacity)
            return routed;
        reset_value(event);
        reset_value(payload);
        routed = gateway_client_route_dispatch(envelope, structural_scratch, maximum_depth,
                                               {storage.data, capacity}, bulk_ids, self_user_id, event, payload,
                                               message_scratch);
    }
    return routed;
}

bool gateway_client_route_guild_placeholder(JsonValue guild, Slice<u32> structural_scratch,
                                            u32 maximum_depth, GatewayClientEvent* event) noexcept {
    if (!event || guild.kind != JsonValueKind::object || !guild.source.data || !structural_scratch.data ||
        !structural_scratch.count || !maximum_depth)
        return false;
    const JsonScanResult scan = json_scan_structurals_avx1(guild.source, structural_scratch);
    if (!scan.ok())
        return false;
    struct Identity {
        u64 id;
        bool seen;
    } identity{};
    const JsonReadResult read = json_visit_value_object(
        guild, ConstSlice<u32>{structural_scratch.data, scan.count}, maximum_depth,
        +[](void* opaque, JsonString name, JsonValue value) noexcept {
            auto& found = *static_cast<Identity*>(opaque);
            if (!json_name_is(name, utf8_literal("id")))
                return true;
            if (found.seen)
                return false;
            found.seen = true;
            return json_value_to_snowflake(value, &found.id);
        },
        &identity);
    if (!read.ok() || !identity.seen || !identity.id)
        return false;
    reset_value<GatewayClientEvent>(event);
    event->kind = GatewayClientEventKind::dispatch;
    event->dispatch_kind = GatewayClientDispatchKind::guild_create;
    event->state.id = identity.id;
    event->state.unavailable = true;
    event->state.fields = static_cast<u16>(gateway_client_state_field_id | gateway_client_state_field_unavailable);
    return true;
}

namespace {

struct RegionalContentPolicy {
    u8 seen, flags;
};
bool regional_content_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& policy = *static_cast<RegionalContentPolicy*>(opaque);
    const u32 bit = member_bit<"teen_by_default_settings", "age_gated_features">(name);
    if (!bit) return true;
    if (policy.seen & bit) return false;
    policy.seen = static_cast<u8>(policy.seen | bit);
    u64 flags = 0;
    if (!json_value_to_u64(value, &flags) || flags > 0xffffffffu) return false;
    if (bit == 1 && (flags & 1)) policy.flags |= account_content_teen_defaults;
    if (bit == 2 && (flags & 2)) policy.flags |= account_content_age_confirmation;
    return true;
}
u8 ready_content_region(JsonValue value, Slice<u32> scratch, u32 depth) noexcept {
    if (!value.source.data || value.kind != JsonValueKind::object) return 0;
    const auto scan = json_scan_structurals_avx1(value.source, scratch);
    if (!scan.ok()) return 0;
    RegionalContentPolicy policy{};
    const auto read = json_visit_value_object(value, {scratch.data, scan.count}, depth,
                                               regional_content_field, &policy);
    return read.ok() ? policy.flags : u8{0};
}
bool ready_content_parent(JsonValue value, Slice<u32> scratch, u32 depth) noexcept {
    if (!value.source.data || value.kind != JsonValueKind::array) return false;
    const auto scan = json_scan_structurals_avx1(value.source, scratch);
    if (!scan.ok()) return false;
    bool parent = false;
    return gateway_client_linked_parent({value.source, {scratch.data, scan.count}}, value, depth, &parent) && parent;
}

} // namespace

GatewayClientDispatchRouteResult gateway_client_route_ready(
    const GatewayReady& ready, Slice<u32> structural_scratch, Slice<u32> object_scratch, u32 maximum_depth,
    Slice<char> payload_scratch, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    if (!event || !payload || ready.user_id == 0 || ready.guilds.kind != JsonValueKind::array ||
        !ready.guilds.source.data || !structural_scratch.data || structural_scratch.count == 0 ||
        !object_scratch.data || object_scratch.count == 0 || maximum_depth == 0 || !payload_scratch.data) {
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    }
    GatewayClientReadStateEvent read_state{};
    if (ready.read_state.source.data) {
        const auto decoded = gateway_client_read_state_metadata(ready.read_state, structural_scratch,
                                                                maximum_depth, &read_state);
        if (!decoded.ok())
            return decoded;
    }
    const JsonScanResult scan = json_scan_structurals_avx1(ready.guilds.source, structural_scratch);
    if (!scan.ok()) {
        GatewayClientDispatchRouteResult result =
            route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::scan_failed);
        result.scan_error = scan.error;
        result.error_offset = scan.error_offset;
        return result;
    }
    PayloadWriter writer{payload_scratch, 0};
    ReadyContext context{&writer, object_scratch, maximum_depth};
    const JsonReadResult read =
        json_visit_value_array(ready.guilds, ConstSlice<u32>{structural_scratch.data, scan.count},
                              maximum_depth, visit_ready_guild, &context);
    if (!read.ok()) {
        GatewayClientDispatchRouteResult result = route_error(
            context.error == GatewayEventDecodeError::invalid_data
                ? GatewayClientDispatchRouteError::payload_capacity
                : GatewayClientDispatchRouteError::decode,
            context.error != GatewayEventDecodeError::none ? context.error
                                                           : GatewayEventDecodeError::invalid_json);
        result.read_error = read.error;
        if (context.scan_error != JsonScanError::none) {
            result.scan_error = context.scan_error;
        }
        if (context.read_error != JsonReadError::none) {
            result.read_error = context.read_error;
        }
        result.error_offset = context.error_offset != 0 ? context.error_offset : read.error_offset;
        return result;
    }
    reset_value<GatewayClientEvent>(event);
    event->kind = GatewayClientEventKind::ready;
    event->state.id = ready.user_id;
    event->state.read_state_partial = read_state.partial;
    if (!upload_policy_decode(ready.apex_experiments, ready.user_id, structural_scratch,
                              &event->state.upload_policy))
        return decode_route(GatewayEventDecodeError::invalid_data);
    event->state.upload_policy_known = true;
    event->state.account_content_flags = ready_content_region(ready.regional_feature_config,
                                                               structural_scratch, maximum_depth);
    if (ready_content_parent(ready.linked_users, structural_scratch, maximum_depth))
        event->state.account_content_flags |= account_content_parent_managed;
    event->state.fields = gateway_client_state_field_id;
    event->state.record_count = context.count;
    event->dispatch_payload_bytes = writer.used;
    *payload = ConstSlice<u8>{reinterpret_cast<const u8*>(payload_scratch.data), writer.used};
    return route_error(GatewayClientDispatchRouteError::none);
}

} // namespace maicord
