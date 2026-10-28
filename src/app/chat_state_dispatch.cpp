#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/app/user_store_dispatch.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/sys/crt.hpp"

namespace maicord {
namespace {

struct GuildEmojiScratch {
    CustomEmojiInput values[kGuildEmojiMaximum];
};
inline constexpr u32 kGuildChannelIdCapacity = sizeof(GuildEmojiScratch) / sizeof(u64);
struct GuildChannelIdScratch {
    u64 values[kGuildChannelIdCapacity];
};
union GuildTransientScratch {
    GuildEmojiScratch emojis;
    GuildChannelIdScratch channel_ids;
};
struct GuildStateScratch {
    GuildTransientScratch transient;
    u32 role_offsets[kGuildRoleMaximum];
    u64 self_roles[kGuildRoleMaximum];
};
struct PrivateStateScratch {
    u64 ids[kPrivateRecipientMaximum];
    PrivateNicknameInput nicks[kPrivateRecipientMaximum];
};
struct MemberRoleScratch {
    u64 ids[kGuildRoleMaximum];
};
static_assert(is_trivial_v<GuildStateScratch> && is_trivial_v<PrivateStateScratch> &&
              is_trivial_v<MemberRoleScratch>);
static_assert(is_trivial_v<GuildEmojiScratch> && is_trivial_v<GuildChannelIdScratch> &&
              is_trivial_v<GuildTransientScratch>);
static_assert(sizeof(GuildChannelIdScratch) <= sizeof(GuildEmojiScratch) &&
              alignof(GuildChannelIdScratch) <= alignof(GuildEmojiScratch) &&
              sizeof(GuildTransientScratch) == sizeof(GuildEmojiScratch) &&
              alignof(GuildTransientScratch) == alignof(GuildEmojiScratch));
static_assert(sizeof(GuildStateScratch) == 33'792 && sizeof(PrivateStateScratch) == 8'192 &&
              sizeof(MemberRoleScratch) == 2'048);
static_assert(alignof(GuildStateScratch) <= 16 && alignof(PrivateStateScratch) <= 16 &&
              alignof(MemberRoleScratch) <= 16);
static_assert(kGuildRoleMaximum <= kPrivateRecipientMaximum);

ChatStateApplyResult apply_error(ChatStateApplyError error) noexcept {
    ChatStateApplyResult result{};
    result.error = error;
    return result;
}

ChatStateApplyResult store_error(const ChatStateStoreResult& mutation) noexcept {
    ChatStateApplyResult result{};
    result.store_error = mutation.error;
    result.error = ChatStateApplyError::capacity;
    return result;
}

// A full table is reported by name so a rejection's log detail says which
// one (channels, roles, overwrites, emojis) ran out.
ChatStateApplyResult table_full(ChatStateStoreError table) noexcept {
    return store_error(ChatStateStoreResult{0, table, false, false});
}

ChatStateApplyResult admission_error(ChatStateStoreError admission,
                                     ChatStateApplyError otherwise) noexcept {
    return admission == ChatStateStoreError::invalid_input ? apply_error(otherwise) : table_full(admission);
}

GuildSettings guild_settings(const GatewayClientStateMutation& state) noexcept {
    GuildSettings settings{};
    settings.known = static_cast<u16>(state.guild_settings_known & guild_settings_all);
    settings.system_channel_id = state.system_channel_id;
    settings.afk_channel_id = state.afk_channel_id;
    settings.afk_timeout_seconds = state.afk_timeout;
    settings.self_communication_disabled_until = state.self_communication_disabled_until;
    settings.mfa_level = state.mfa_level;
    settings.verification_level = state.verification_level;
    settings.default_message_notifications = state.default_message_notifications;
    settings.explicit_content_filter = state.explicit_content_filter;
    settings.self_member = state.self_member;
    return settings;
}

RoleInput packed_role_input(u64 guild_id, const GatewayClientRoleMutation& role, Utf8View name) noexcept {
    return RoleInput{role.id,
                     guild_id,
                     role.permissions,
                     name,
                     role.position,
                     role.color,
                     (role.flags & gateway_client_role_hoist) != 0,
                     (role.flags & gateway_client_role_managed) != 0,
                     (role.flags & gateway_client_role_mentionable) != 0,
                     (role.flags & gateway_client_role_has_icon) != 0,
                     (role.flags & gateway_client_role_in_prompt) != 0,
                     role.secondary_color,
                     role.tertiary_color,
                     (role.flags & gateway_client_role_linked) != 0};
}

GuildInput guild_input(const GatewayClientEvent& event, ConstSlice<u8> payload, Utf8View name,
                              Utf8View icon_hash,
                              GuildMutationKind kind,
                              const GuildSettings* settings = nullptr) noexcept {
    const GatewayClientStateMutation& state = event.state;
    GuildInput input{state.id,
                             name,
                             kind,
                             state.owner_id,
                             icon_hash,
                             (state.fields & gateway_client_state_field_icon) != 0,
                             state.upload_features, state.upload_features_known,
                             static_cast<u8>((state.content_policy & kGatewayStateContentGuildLevelMarker) ?
                                 state.content_policy & ~kGatewayStateContentGuildLevelMarker : 0u),
                             state.content_policy_known && (state.content_policy & kGatewayStateContentGuildLevelMarker) != 0,
                             settings && settings->known ? settings : nullptr};
    // Presentation only: a malformed banner leaves the stored one unchanged.
    input.banner_known = gateway_client_event_guild_banner(event, payload, &input.banner_hash);
    return input;
}

ChannelInput channel_input(const GatewayClientStateMutation& state, Utf8View name) noexcept {
    return ChannelInput{state.id,       state.guild_id, state.parent_id, name,
                               state.position, state.flags,    state.type,
                               state.last_message_id,
                               (state.fields & gateway_client_state_field_last_message_id) != 0,
                               state.member_list_id, state.member_list_id_known,
                               (state.fields & gateway_client_state_field_name) != 0,
                               state.thread_archived != 0, state.thread_archived_known != 0,
                               state.content_policy, state.content_policy_known != 0,
                               state.thread_locked != 0, state.thread_locked_known != 0,
                               state.thread_self_joined != 0, state.thread_self_joined_known != 0,
                               state.thread_archived_known && !state.thread_archived,
                               state.thread_archived_known != 0,
                               static_cast<u16>(state.rate_limit_per_user ? state.rate_limit_per_user - 1 : 0),
                               state.rate_limit_per_user != 0,
                               // Aliases a role's colour, so only a joined thread carries it.
                               state.thread_self_joined && channel_is_thread(state.type)
                                   ? state.thread_join_seconds
                                   : 0u};
}

bool validate_channel_state(const GatewayClientStateMutation& state, bool allow_new_thread = false) noexcept {
    const bool private_channel = channel_is_private(state.type);
    const u16 required = gateway_client_state_field_id | gateway_client_state_field_type |
        (private_channel ? 0 : gateway_client_state_field_guild_id);
    return (state.fields & required) == required && state.id != 0 &&
        !(state.channel_event_flags & ~gateway_client_channel_new_thread) &&
        (allow_new_thread || !state.channel_event_flags) &&
        (!state.channel_event_flags || channel_is_thread(state.type)) &&
        !state.guild_banner && state.rate_limit_per_user <= 21601 &&
        !(state.content_policy & ~kChannelContentAgeRestricted) && (!state.content_policy || state.content_policy_known) &&
        (!state.thread_archived || state.thread_archived_known) &&
        (!state.thread_locked || state.thread_locked_known) &&
        (!state.thread_self_joined || state.thread_self_joined_known) &&
        (!state.thread_archived_known || channel_is_thread(state.type)) &&
        (!(state.thread_locked_known || state.thread_self_joined_known) || channel_is_thread(state.type)) &&
        (private_channel ? state.guild_id == 0 && state.parent_id == 0 &&
            !(state.fields & gateway_client_state_field_permission_overwrites) : state.guild_id != 0);
}

bool read_u64(ConstSlice<u8> payload, u32 offset, u64* value) noexcept {
    if (!value || offset > payload.count || 8 > payload.count - offset) {
        return false;
    }
    const u64 decoded = byte_load_le64(payload.data + offset);
    *value = decoded;
    return decoded != 0;
}

bool payload_disjoint(ConstSlice<u8> payload, const void* base, u64 bytes) noexcept {
    if (payload.count == 0 || !base || bytes == 0) return true;
    const usize source = reinterpret_cast<usize>(payload.data);
    const usize target = reinterpret_cast<usize>(base);
    if (payload.count > ~usize{0} - source || bytes > ~usize{0} - target) return false;
    return source < target ? target - source >= payload.count : source - target >= bytes;
}

bool state_payload_disjoint(const ChatStateStore& store, ConstSlice<u8> payload) noexcept {
    // Publication reparses borrowed role/emoji data; none may live in backing
    // changed by sorted shifts, name compaction, guild metadata or recompute.
    return payload_disjoint(payload, &store, sizeof(store)) &&
        payload_disjoint(payload, store.names, static_cast<u64>(store.name_bank_bytes) * 2) &&
        payload_disjoint(payload, store.guilds, static_cast<u64>(store.guild_capacity) * sizeof(GuildRecord)) &&
        payload_disjoint(payload, store.channels, static_cast<u64>(store.channel_capacity) * sizeof(ChannelRecord)) &&
        payload_disjoint(payload, store.roles, static_cast<u64>(store.role_capacity) * sizeof(RoleRecord)) &&
        payload_disjoint(payload, store.overwrites, static_cast<u64>(store.overwrite_capacity) * sizeof(PermissionOverwriteRecord)) &&
        payload_disjoint(payload, store.guild_order, static_cast<u64>(store.guild_capacity) * sizeof(u16)) &&
        payload_disjoint(payload, store.channel_order, static_cast<u64>(store.channel_capacity) * sizeof(u16)) &&
        payload_disjoint(payload, store.emojis, static_cast<u64>(store.emoji_capacity) * sizeof(CustomEmojiRecord)) &&
        payload_disjoint(payload, store.emoji_roles, static_cast<u64>(store.emoji_role_capacity) * sizeof(u64));
}

struct GuildRoleInputContext {
    const GatewayClientEvent* event;
    ConstSlice<u8> payload;
    ConstSlice<u32> offsets;
};

struct GuildChannelInputContext {
    const GatewayClientEvent* event;
    ConstSlice<u8> payload;
    mutable u32 cursor;
    mutable u32 index;
};

bool read_guild_channel_input(const void* opaque, u32 index, ChannelInput* output) noexcept {
    const auto& context = *static_cast<const GuildChannelInputContext*>(opaque);
    if (index != context.index) return false;
    GatewayClientStateMutation channel{};
    Utf8View name{};
    ConstSlice<u8> overwrites{};
    if (!gateway_client_event_next_guild_channel(*context.event, context.payload,
        &context.cursor, &context.index, &channel, &name, &overwrites)) return false;
    *output = channel_input(channel, name);
    return true;
}

bool read_guild_role_input(const void* opaque, u32 index, RoleInput* output) noexcept {
    const auto& context = *static_cast<const GuildRoleInputContext*>(opaque);
    GatewayClientRoleMutation role{};
    Utf8View name{};
    if (index >= context.offsets.count || !gateway_client_event_guild_role_at(
        *context.event, context.payload, context.offsets[index], &role, &name)) return false;
    *output = packed_role_input(context.event->state.id, role, name);
    return true;
}

bool guild_roles_valid(const GatewayClientEvent& event, ConstSlice<u8> payload,
                       ConstSlice<u32> offsets) noexcept {
    if (event.state.role_count == 0 || event.state.role_count > kGuildRoleMaximum) {
        return false;
    }
    bool everyone = false;
    if (offsets.count < event.state.role_count) return false;
    for (u32 index = 0; index < event.state.role_count; ++index) {
        GatewayClientRoleMutation role{};
        Utf8View name{};
        if (!gateway_client_event_guild_role_at(event, payload, offsets[index], &role, &name) || name.bytes > 400) {
            return false;
        }
        for (u32 previous = 0; previous < index; ++previous) {
            GatewayClientRoleMutation other{};
            Utf8View other_name{};
            if (!gateway_client_event_guild_role_at(event, payload, offsets[previous], &other, &other_name) ||
                other.id == role.id) return false;
        }
        everyone = everyone || role.id == event.state.id;
    }
    return everyone;
}

bool guild_roles_payload_bytes(const GatewayClientEvent& event, ConstSlice<u8> payload,
                               u32* bytes, ConstSlice<u32> offsets) noexcept {
    u64 total = gateway_client_guild_strings_bytes(event.state);
    for (u32 index = 0; index < event.state.role_count; ++index) {
        GatewayClientRoleMutation role{};
        Utf8View name{};
        if (index >= offsets.count ||
            !gateway_client_event_guild_role_at(event, payload, offsets[index], &role, &name)) {
            return false;
        }
        total += sizeof(role) + name.bytes;
        if (total > 0xFFFFFFFFull) {
            return false;
        }
    }
    *bytes = static_cast<u32>(total);
    return true;
}

bool guild_has_packet_role(const GatewayClientEvent& event, ConstSlice<u8> payload,
                           u64 role_id, ConstSlice<u32> offsets) noexcept {
    for (u32 index = 0; index < offsets.count; ++index) {
        GatewayClientRoleMutation role{};
        Utf8View name{};
        if (gateway_client_event_guild_role_at(event, payload, offsets[index], &role, &name) && role.id == role_id) {
            return true;
        }
    }
    return false;
}

bool guild_self_roles(const GatewayClientEvent& event, ConstSlice<u8> payload,
                      Slice<u64> roles, ConstSlice<u32> offsets, u32 roles_end) noexcept {
    if (event.state.member_role_count > roles.count || roles_end > payload.count ||
        event.state.member_role_count > (payload.count - roles_end) / 8u) {
        return false;
    }
    for (u32 index = 0; index < event.state.member_role_count; ++index) {
        if (!read_u64(payload, roles_end + index * 8u, &roles[index]) ||
            !guild_has_packet_role(event, payload, roles[index], offsets)) {
            return false;
        }
        for (u32 previous = 0; previous < index; ++previous)
            if (roles[previous] == roles[index]) return false;
    }
    return true;
}

bool member_roles(const GatewayClientEvent& event, ConstSlice<u8> payload,
                  Slice<u64> roles) noexcept {
    if (event.dispatch_kind != GatewayClientDispatchKind::guild_member_update ||
        event.state.name_bytes != 0 || event.state.member_role_count > roles.count ||
        event.state.record_count != event.state.member_role_count ||
        payload.count != event.state.member_role_count * 8u) {
        return false;
    }
    for (u32 index = 0; index < event.state.member_role_count; ++index) {
        if (!read_u64(payload, index * 8, &roles[index])) {
            return false;
        }
        for (u32 previous = 0; previous < index; ++previous) {
            if (roles[previous] == roles[index]) {
                return false;
            }
        }
    }
    return true;
}

bool overwrite_payload_valid(ConstSlice<u8> payload, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index) {
        GatewayClientPermissionOverwriteMutation overwrite{};
        if (!gateway_client_permission_overwrite_at(payload, count, index, &overwrite)) {
            return false;
        }
    }
    return true;
}

static_assert(static_cast<u8>(gateway_client_emoji_animated) == custom_emoji_animated &&
              static_cast<u8>(gateway_client_emoji_available) == custom_emoji_available &&
              static_cast<u8>(gateway_client_emoji_managed) == custom_emoji_managed &&
              static_cast<u8>(gateway_client_emoji_require_colons) == custom_emoji_require_colons);

bool guild_emojis_payload(
    const GatewayClientEvent& event, ConstSlice<u8> payload,
    Slice<CustomEmojiInput> emojis, u32* count) noexcept {
    if (!event.state.emojis_known ||
        event.state.emoji_count > emojis.count ||
        event.state.emoji_count > kGuildEmojiMaximum ||
        (!emojis.data && event.state.emoji_count != 0)) {
        return false;
    }
    u32 cursor = 0;
    u32 index = 0;
    u32 role_count = 0;
    if (!gateway_client_event_guild_emojis_begin(event, payload, &cursor)) {
        return false;
    }
    while (index < event.state.emoji_count) {
        GatewayClientEmojiMutation emoji{};
        ConstSlice<u8> roles{};
        Utf8View name{};
        if (!gateway_client_event_next_guild_emoji(
                event, payload, &cursor, &index, &emoji, &roles, &name) ||
            emoji.role_count > kGuildRoleMaximum ||
            (emoji.flags & static_cast<u8>(
                 ~(gateway_client_emoji_animated |
                   gateway_client_emoji_available |
                   gateway_client_emoji_managed |
                   gateway_client_emoji_require_colons))) != 0 ||
            role_count > ~0u - emoji.role_count) {
            return false;
        }
        role_count += emoji.role_count;
        for (u32 role = 0; role < emoji.role_count; ++role) {
            u64 id = 0;
            if (!read_u64(roles, role * 8u, &id)) return false;
            for (u32 previous = 0; previous < role; ++previous) {
                u64 other = 0;
                if (!read_u64(roles, previous * 8u, &other) || other == id) return false;
            }
        }
        // The wire flag bits are the store's (validated against the known mask above).
        emojis[index - 1] = CustomEmojiInput{emoji.id, name, roles, emoji.role_count, emoji.flags, 0};
        for (u32 previous = 0; previous + 1 < index; ++previous) {
            if (emojis[previous].id == emoji.id) {
                return false;
            }
        }
    }
    if (cursor != payload.count ||
        role_count != event.state.emoji_role_count) {
        return false;
    }
    *count = index;
    return true;
}

bool apply_overwrites(ChatStateStore* store, u64 channel_id, ConstSlice<u8> payload,
                      u32 count) noexcept {
    chat_state_store_clear_channel_overwrites(store, channel_id);
    for (u32 index = 0; index < count; ++index) {
        GatewayClientPermissionOverwriteMutation overwrite{};
        if (!gateway_client_permission_overwrite_at(payload, count, index, &overwrite) ||
            !chat_state_store_append_channel_overwrite(
                store, channel_id,
                PermissionOverwriteInput{overwrite.id, overwrite.allow, overwrite.deny,
                                                overwrite.type})) {
            return false;
        }
    }
    return true;
}

u32 resident_guild_channels(const ChatStateStore& store, u64 guild_id) noexcept {
    u32 count = 0;
    for (u32 order = 0; order < store.channel_count; ++order) {
        count += store.channels[store.channel_order[order]].guild_id == guild_id ? 1u : 0u;
    }
    return count;
}

u32 resident_guild_roles(const ChatStateStore& store, u64 guild_id) noexcept {
    u32 count = 0;
    for (u32 index = 0; index < store.role_count; ++index) {
        count += store.roles[index].guild_id == guild_id ? 1u : 0u;
    }
    return count;
}

u32 resident_guild_overwrites(const ChatStateStore& store, u64 guild_id) noexcept {
    u32 count = 0;
    for (u32 order = 0; order < store.channel_count; ++order) {
        const auto& channel = store.channels[store.channel_order[order]];
        if (channel.guild_id == guild_id) count += channel.overwrite_count;
    }
    return count;
}

} // namespace

bool gateway_client_dispatch_is_state(GatewayClientDispatchKind kind) noexcept {
    return kind == GatewayClientDispatchKind::guild_create ||
           kind == GatewayClientDispatchKind::guild_update ||
           kind == GatewayClientDispatchKind::guild_delete ||
           kind == GatewayClientDispatchKind::channel_create ||
           kind == GatewayClientDispatchKind::channel_update ||
           kind == GatewayClientDispatchKind::channel_delete ||
           kind == GatewayClientDispatchKind::guild_role_create ||
           kind == GatewayClientDispatchKind::guild_role_update ||
           kind == GatewayClientDispatchKind::guild_role_delete ||
           kind == GatewayClientDispatchKind::guild_member_update ||
           kind == GatewayClientDispatchKind::guild_emojis_update ||
           kind == GatewayClientDispatchKind::channel_recipient_add || kind == GatewayClientDispatchKind::channel_recipient_remove ||
           kind == GatewayClientDispatchKind::thread_self_update || kind == GatewayClientDispatchKind::thread_list_sync;
}

namespace {
struct ThreadSyncInputs {
    ConstSlice<u8> payload;
    ConstSlice<u32> offsets;
};
bool thread_sync_record(ConstSlice<u8> payload, u32 offset, GatewayClientStateMutation* channel,
                        Utf8View* name, u32* next) noexcept {
    if (offset > payload.count || sizeof(*channel) > payload.count - offset) return false;
    copy_bytes(channel, payload.data + offset, sizeof(*channel));
    offset += sizeof(*channel);
    if (!validate_channel_state(*channel) || !channel_is_thread(channel->type) || !channel->parent_id ||
        channel->parent_id == channel->id ||
        channel->overwrite_count || (channel->fields & gateway_client_state_field_permission_overwrites) ||
        channel->name_bytes > payload.count - offset) return false;
    *name = {reinterpret_cast<const char*>(payload.data + offset), channel->name_bytes};
    if (!utf8_validate(*name).valid || name->bytes > 400) return false;
    *next = offset + channel->name_bytes;
    return true;
}
bool thread_sync_input(const void* opaque, u32 index, ChannelInput* out) noexcept {
    const auto& inputs = *static_cast<const ThreadSyncInputs*>(opaque);
    GatewayClientStateMutation state{}; Utf8View name{}; u32 next{};
    if (index >= inputs.offsets.count || !thread_sync_record(inputs.payload, inputs.offsets[index], &state, &name, &next)) return false;
    *out = channel_input(state, name);
    // LIST_SYNC itself proves activity, even when metadata was omitted.
    out->thread_listed_active = true;
    out->thread_active_known = true;
    return true;
}
bool id_in(ConstSlice<u8> payload, u32 count, u64 id) noexcept {
    for (u32 i = 0; i < count; ++i) if (byte_load_le64(payload.data + i * 8) == id) return true;
    return false;
}
// Joined-thread entries of THREAD_LIST_SYNC: id, then join seconds.
bool joined_in(ConstSlice<u8> payload, u32 begin, u32 count, u64 id, u32* join_seconds) noexcept {
    for (u32 i = 0; i < count; ++i) {
        const u32 at = begin + i * kGatewayThreadSyncMemberBytes;
        if (byte_load_le64(payload.data + at) != id) continue;
        *join_seconds = byte_load_le32(payload.data + at + 8);
        return true;
    }
    return false;
}
}

ChatStateApplyResult chat_state_apply_thread_authority(
    const GatewayClientEvent& input_event, ConstSlice<u8> input_payload, ChatStateStore* store) noexcept {
    if (!store || input_event.kind != GatewayClientEventKind::dispatch ||
        input_event.dispatch_payload_bytes != input_payload.count || (!input_payload.data && input_payload.count))
        return apply_error(ChatStateApplyError::invalid_input);
    GatewayClientEvent normalized;
    ConstSlice<u8> payload = input_payload;
    if (input_event.notification_payload_bytes && !gateway_client_notification_trim(input_event, input_payload, &normalized, &payload))
        return apply_error(ChatStateApplyError::invalid_payload);
    const auto& event = input_event.notification_payload_bytes ? normalized : input_event;
    ChatStateApplyResult result{};
    if (event.dispatch_kind == GatewayClientDispatchKind::thread_self_update) {
        const auto& proof = event.thread_self;
        if (payload.count || !proof.guild_id || !proof.thread_id || !proof.self_user_id || proof.self_user_id != store->self_user_id ||
            proof.authority_flags != (gateway_thread_self_known | gateway_thread_self_joined))
            return apply_error(ChatStateApplyError::invalid_payload);
        for (u8 value : proof.reserved) if (value) return apply_error(ChatStateApplyError::invalid_payload);
        const auto* channel = chat_state_store_find_channel(*store, proof.thread_id);
        if (!channel) return result;
        if (channel->guild_id != proof.guild_id || !channel_is_thread(channel->type)) return apply_error(ChatStateApplyError::invalid_payload);
        const auto* guild = chat_state_store_find_guild(*store, proof.guild_id);
        if (!guild || guild->unavailable) return result;
        if (!chat_state_store_set_thread_self_membership(store, proof.thread_id, proof.self_user_id, true))
            return apply_error(ChatStateApplyError::invalid_payload);
        chat_state_store_set_thread_join_time(store, proof.thread_id, proof.self_user_id, proof.join_seconds);
        result.channels = 1;
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::thread_members_update ||
        event.dispatch_kind == GatewayClientDispatchKind::thread_member_list_update) {
        const auto& proof = event.thread_members;
        if (!proof.guild_id || !proof.thread_id || proof.authority_reserved ||
            (proof.authority_flags & ~(gateway_thread_self_known | gateway_thread_self_joined)) ||
            (proof.authority_flags && (!(proof.authority_flags & gateway_thread_self_known) || proof.authority_self_id != store->self_user_id)) ||
            (!proof.authority_flags && proof.authority_self_id) ||
            proof.snapshot != (event.dispatch_kind == GatewayClientDispatchKind::thread_member_list_update))
            return apply_error(ChatStateApplyError::invalid_payload);
        u32 cursor{}, index{}; u8 actual{}; u8 phase{};
        while (index < proof.record_count) {
            GatewayClientMemberListMutation row{}; ConstSlice<u8> roles{}, global{}, guild{};
            Utf8View nickname{}, name{}, avatar{}, username{};
            if (!gateway_client_event_thread_member_record(event, payload, &cursor, &index, &row, &roles,
                &nickname, &name, &avatar, &username, &global, &guild)) return apply_error(ChatStateApplyError::invalid_payload);
            const u8 next_phase = row.operation == GatewayClientMemberListOperation::update ? 2 : row.operation == GatewayClientMemberListOperation::remove ? 1 : 0;
            if (next_phase < phase) return apply_error(ChatStateApplyError::invalid_payload);
            phase = next_phase;
            if (proof.authority_self_id && row.user_id == proof.authority_self_id && row.operation != GatewayClientMemberListOperation::update) {
                if (actual) return apply_error(ChatStateApplyError::invalid_payload);
                actual = gateway_thread_self_known;
                if (row.operation != GatewayClientMemberListOperation::remove) actual |= gateway_thread_self_joined;
            }
        }
        if (cursor != payload.count || actual != proof.authority_flags) return apply_error(ChatStateApplyError::invalid_payload);
        const auto* channel = chat_state_store_find_channel(*store, proof.thread_id);
        if (!channel) return result;
        if (channel->guild_id != proof.guild_id || !channel_is_thread(channel->type)) return apply_error(ChatStateApplyError::invalid_payload);
        const auto* guild = chat_state_store_find_guild(*store, proof.guild_id);
        if (!guild || guild->unavailable) return result;
        if (proof.incomplete) {
            chat_state_store_set_thread_self_membership(store, proof.thread_id, store->self_user_id, false, false);
            result.channels = 1;
        } else if (actual) {
            if (!chat_state_store_set_thread_self_membership(store, proof.thread_id, store->self_user_id,
                                                               (actual & gateway_thread_self_joined) != 0))
                return apply_error(ChatStateApplyError::invalid_payload);
            if (actual & gateway_thread_self_joined)
                chat_state_store_set_thread_join_time(store, proof.thread_id, store->self_user_id,
                                                      proof.authority_join_seconds);
            result.channels = 1;
        }
        return result;
    }
    if (event.dispatch_kind != GatewayClientDispatchKind::thread_list_sync) return apply_error(ChatStateApplyError::unsupported_event);
    const auto& sync = event.thread_sync;
    if (!sync.guild_id || !sync.self_user_id || sync.self_user_id != store->self_user_id ||
        sync.parent_count > 4096 || sync.thread_count > 4096 ||
        (sync.scope != GatewayClientThreadSyncScope::guild && sync.scope != GatewayClientThreadSyncScope::parents) ||
        (sync.scope == GatewayClientThreadSyncScope::guild && sync.parent_count) ||
        (sync.flags & ~gateway_client_thread_sync_members_known) || sync.reserved[0] || sync.reserved[1] ||
        (!(sync.flags & gateway_client_thread_sync_members_known) && sync.member_count) ||
        !state_payload_disjoint(*store, payload) ||
        !state_payload_disjoint(*store, {reinterpret_cast<const u8*>(&event), sizeof(event)}))
        return apply_error(ChatStateApplyError::invalid_payload);
    const u64 parent_bytes = u64{sync.parent_count} * 8;
    if (parent_bytes > payload.count) return apply_error(ChatStateApplyError::invalid_payload);
    for (u32 i = 0; i < sync.parent_count; ++i) {
        const u64 id = byte_load_le64(payload.data + i * 8);
        const auto* parent = chat_state_store_find_channel(*store, id);
        if (!id || id_in(payload, i, id) ||
            (parent && (parent->guild_id != sync.guild_id || !channel_is_thread_parent(parent->type))))
            return apply_error(ChatStateApplyError::invalid_payload);
    }
    u32 offsets[4096]; u32 cursor = static_cast<u32>(parent_bytes);
    for (u32 i = 0; i < sync.thread_count; ++i) {
        GatewayClientStateMutation state{}; Utf8View name{};
        offsets[i] = cursor;
        if (!thread_sync_record(payload, cursor, &state, &name, &cursor) || state.guild_id != sync.guild_id ||
            (sync.scope == GatewayClientThreadSyncScope::parents && !id_in(payload, sync.parent_count, state.parent_id)))
            return apply_error(ChatStateApplyError::invalid_payload);
        const auto* parent = chat_state_store_find_channel(*store, state.parent_id);
        const auto* resident = chat_state_store_find_channel(*store, state.id);
        if (parent && (parent->guild_id != sync.guild_id || !channel_is_thread_parent(parent->type)))
            return apply_error(ChatStateApplyError::invalid_payload);
        if (resident && !channel_is_thread(resident->type))
            return apply_error(ChatStateApplyError::invalid_payload);
    }
    const u32 members_begin = cursor;
    if (u64{sync.member_count} * kGatewayThreadSyncMemberBytes != payload.count - cursor) return apply_error(ChatStateApplyError::invalid_payload);
    for (u32 i = 0; i < sync.member_count; ++i) {
        const u64 id = byte_load_le64(payload.data + members_begin + i * kGatewayThreadSyncMemberBytes); bool found = false;
        u32 earlier = 0;
        if (!id || joined_in(payload, members_begin, i, id, &earlier)) return apply_error(ChatStateApplyError::invalid_payload);
        for (u32 r = 0; r < sync.thread_count; ++r) if (byte_load_le64(payload.data + offsets[r]) == id) found = true;
        if (!found) return apply_error(ChatStateApplyError::invalid_payload);
    }
    const auto* guild = chat_state_store_find_guild(*store, sync.guild_id);
    if (!guild || guild->unavailable) return result;
    ThreadSyncInputs inputs{payload, {offsets, sync.thread_count}};
    const auto admission = chat_state_store_preflight_channel_batch(*store, sync.thread_count, thread_sync_input, &inputs);
    if (admission != ChatStateStoreError::none) return apply_error(admission == ChatStateStoreError::invalid_input ? ChatStateApplyError::invalid_payload : ChatStateApplyError::capacity);
    for (u32 i = 0; i < sync.thread_count; ++i) {
        ChannelInput input{};
        if (!thread_sync_input(&inputs, i, &input)) return apply_error(ChatStateApplyError::invalid_payload);
        u32 join_seconds = 0;
        const bool listed_joined = joined_in(payload, members_begin, sync.member_count, input.id, &join_seconds);
        if (sync.flags & gateway_client_thread_sync_members_known) {
            input.thread_self_joined = input.thread_self_joined || listed_joined;
            input.thread_self_joined_known = true;
        }
        if (listed_joined && join_seconds)
            input.thread_join_seconds = join_seconds;
        const auto applied = chat_state_store_upsert_channel(store, input);
        if (!applied.ok()) return store_error(applied);
    }
    // Complete inventory retires cached missing rows without fabricating archive
    // metadata or removing history. Membership negatives retain their stricter
    // archive guard because absence does not prove leaving an archived thread.
    for (u32 i = 0; i < store->channel_count; ++i) {
        const auto& channel = store->channels[store->channel_order[i]];
        if (channel.guild_id != sync.guild_id ||
            (sync.scope == GatewayClientThreadSyncScope::parents &&
             !id_in(payload, sync.parent_count, channel_is_thread(channel.type) ? channel.parent_id : channel.id)))
            continue;
        if (channel_is_thread_parent(channel.type)) {
            chat_state_store_set_thread_listing_synced(store, channel.id, sync.self_user_id);
            continue;
        }
        if (!channel_is_thread(channel.type))
            continue;
        bool listed = false;
        for (u32 r = 0; r < sync.thread_count; ++r) if (byte_load_le64(payload.data + offsets[r]) == channel.id) listed = true;
        if (!listed) {
            chat_state_store_set_thread_active(store, channel.id, sync.self_user_id, false);
            if (channel.thread_archived_known && !channel.thread_archived)
                chat_state_store_set_thread_self_membership(store, channel.id, sync.self_user_id, false);
        }
    }
    chat_state_store_recompute_guild_permissions(store, sync.guild_id);
    result.channels = sync.thread_count;
    return result;
}

ChatStateApplyResult chat_state_apply_gateway_event(ChatStateStore* store,
                                                          const GatewayClientEvent& input_event,
                                                          ConstSlice<u8> input_payload,
                                                          Slice<u64> guild_id_scratch, UserStore* users) noexcept {
    if (!store || (!input_payload.data && input_payload.count != 0) || input_event.dispatch_payload_bytes != input_payload.count) {
        return apply_error(ChatStateApplyError::invalid_input);
    }
    // Pump-normalized events borrow their descriptor and body. Direct routed
    // events validate metadata before ordinary state mutation and copy only
    // the descriptor whose payload length must change.
    GatewayClientEvent normalized_event;
    ConstSlice<u8> payload=input_payload;
    if (input_event.notification_payload_bytes &&
        !gateway_client_notification_trim(input_event,input_payload,&normalized_event,&payload))
        return apply_error(ChatStateApplyError::invalid_payload);
    const GatewayClientEvent& event=input_event.notification_payload_bytes?normalized_event:input_event;
    if (event.dispatch_kind == GatewayClientDispatchKind::thread_self_update || event.dispatch_kind == GatewayClientDispatchKind::thread_list_sync)
        return chat_state_apply_thread_authority(event, payload, store);
    const u32 selection_revision = store->selection_revision;
    ChatStateApplyResult result{};

    if (event.kind == GatewayClientEventKind::ready) {
        if ((event.state.account_content_flags &
             ~(account_content_teen_defaults | account_content_age_confirmation | account_content_parent_managed)) || event.state.id == 0 ||
            (store->self_user_id != 0 && store->self_user_id != event.state.id)) {
            return apply_error(ChatStateApplyError::invalid_payload);
        }
        u32 guild_count = 0;
        if (!gateway_client_event_as_ready_guilds(event, payload, guild_id_scratch, &guild_count)) {
            return apply_error(ChatStateApplyError::invalid_payload);
        }
        for (u32 first = 0; first < guild_count; ++first) {
            for (u32 second = first + 1; second < guild_count; ++second) {
                if (guild_id_scratch[first] == guild_id_scratch[second]) {
                    return apply_error(ChatStateApplyError::invalid_payload);
                }
            }
        }
        if (guild_count > store->guild_capacity) {
            return apply_error(ChatStateApplyError::capacity);
        }
        // All READY guild IDs are validated before its partial read-state carry
        // can reserve backing or any old channel is removed.
        if (store->read_state && !read_state_prepare_ready(store,
            event.state.read_state_partial, store->read_state->version))
            return apply_error(ChatStateApplyError::capacity);
        store->self_user_id = event.state.id;
        store->self_premium_type = 0;
        store->content_policy = {AccountContentEligibility::unknown, AccountAgeVerification::unknown,
                                 event.state.account_content_flags};
        store->upload_policy = event.state.upload_policy;
        const u64 restore_channel = store->restore_channel_id ? store->restore_channel_id : store->selected_channel_id;
        const u64 restore_guild = store->restore_channel_id ? store->restore_guild_id : store->selected_guild_id;
        store->restore_channel_id = store->restore_guild_id = store->restore_deadline_ms = 0;
        result.channels = chat_state_store_clear_private_channels(store);
        chat_state_store_clear_guilds(store);
        read_state_finish_ready(store);
        store->restore_channel_id = restore_channel;
        store->restore_guild_id = restore_guild;
        store->restore_deadline_ms = restore_channel ? deadline_after(event.timestamp_ms, 10'000) : 0;
        for (u32 index = 0; index < guild_count; ++index) {
            const ChatStateStoreResult applied = chat_state_store_upsert_guild(
                store, GuildInput{guild_id_scratch[index], {},
                                         GuildMutationKind::unavailable, 0});
            if (!applied.ok()) {
                return store_error(applied);
            }
        }
        result.guilds = guild_count;
        result.selection_changed = selection_revision != store->selection_revision;
        return result;
    }

    if (event.kind != GatewayClientEventKind::dispatch ||
        !gateway_client_dispatch_is_state(event.dispatch_kind)) {
        return apply_error(ChatStateApplyError::unsupported_event);
    }

    const bool recipient_event = event.dispatch_kind == GatewayClientDispatchKind::channel_recipient_add ||
                                 event.dispatch_kind == GatewayClientDispatchKind::channel_recipient_remove;
    const bool guild_event = event.dispatch_kind == GatewayClientDispatchKind::guild_emojis_update ||
                             event.dispatch_kind == GatewayClientDispatchKind::guild_create ||
                             event.dispatch_kind == GatewayClientDispatchKind::guild_update;
    const bool member_event = event.dispatch_kind == GatewayClientDispatchKind::guild_member_update;
    const bool no_scratch_event = event.dispatch_kind == GatewayClientDispatchKind::guild_delete ||
                                  event.dispatch_kind == GatewayClientDispatchKind::guild_role_create ||
                                  event.dispatch_kind == GatewayClientDispatchKind::guild_role_update ||
                                  event.dispatch_kind == GatewayClientDispatchKind::guild_role_delete ||
                                  event.dispatch_kind == GatewayClientDispatchKind::channel_delete;
    // Mirror the branches below. Only the final private-channel branch needs
    // private scratch after the explicitly handled event groups.
    const usize scratch_bytes = guild_event ? sizeof(GuildStateScratch)
        : member_event ? sizeof(MemberRoleScratch)
        : recipient_event || (!no_scratch_event && channel_is_private(event.state.type))
            ? sizeof(PrivateStateScratch) : 0;
    void* const scratch = scratch_bytes ? _alloca(scratch_bytes) : nullptr;
    if (recipient_event) {
        u32 cursor = 0;
        GatewayClientUserMutation user{};
        Utf8View display{}, avatar{};
        if (!event.state.id || !gateway_client_user_record(payload, &cursor, &user, &display, &avatar) ||
            event.state.name_bytes != payload.count - cursor || event.state.name_bytes > 128 ||
            (event.state.name_bytes && !(event.state.fields & gateway_client_state_field_name)))
            return apply_error(ChatStateApplyError::invalid_payload);
        const Utf8View nickname{reinterpret_cast<const char*>(payload.data + cursor), event.state.name_bytes};
        if (!utf8_validate(nickname).valid) return apply_error(ChatStateApplyError::invalid_payload);
        const auto* channel = chat_state_store_find_channel(*store, event.state.id);
        if (!channel || !channel_is_private(channel->type) ||
            (event.dispatch_kind == GatewayClientDispatchKind::channel_recipient_add && user.id == store->self_user_id)) {
            if (!user_store_apply_gateway_snapshots(users, {payload.data, cursor}, 1)) return apply_error(ChatStateApplyError::capacity);
            return result;
        }
        auto* private_store = store->private_channels;
        const auto* metadata = private_store ? private_channel_store_find(*private_store, channel->id) : nullptr;
        if (!metadata) return apply_error(ChatStateApplyError::invalid_payload);
        const bool add = event.dispatch_kind == GatewayClientDispatchKind::channel_recipient_add;
        const auto prior = private_channel_store_recipients(*private_store, *metadata);
        if (prior.count > kPrivateRecipientMaximum) return apply_error(ChatStateApplyError::invalid_payload);
        auto& private_scratch = *::new (scratch) PrivateStateScratch;
        auto& ids = private_scratch.ids;
        u32 count = 0;
        bool found = false;
        for (u32 i = 0; i < prior.count; ++i) {
            found = found || prior[i] == user.id;
            if (prior[i] != user.id || add) ids[count++] = prior[i];
        }
        if (add && !found) {
            if (count == kPrivateRecipientMaximum) return apply_error(ChatStateApplyError::capacity);
            ids[count++] = user.id;
        }
        if (add) heap_sort(ids, count, [](u64 a, u64 b) noexcept { return a < b; });
        const PrivateChannelInput input{channel->id, 0, {ids, count}, {}, true, false, false};
        auto& nicks = private_scratch.nicks;
        u32 nick_count = 0;
        const bool update_nick = add && (event.state.fields & gateway_client_state_field_name);
        if (update_nick) {
            if (!private_channel_store_nicknames(*private_store, channel->id, {nicks, kPrivateRecipientMaximum}, &nick_count))
                return apply_error(ChatStateApplyError::capacity);
            u32 retained = 0;
            for (u32 i = 0; i < nick_count; ++i) if (nicks[i].user_id != user.id) nicks[retained++] = nicks[i];
            nick_count = retained;
            if (nickname.bytes) {
                if (nick_count == kPrivateRecipientMaximum) return apply_error(ChatStateApplyError::capacity);
                nicks[nick_count++] = {user.id, nickname};
            }
            if (!private_channel_store_prepare_nicknames(private_store, channel->id, {nicks, nick_count}))
                return apply_error(ChatStateApplyError::capacity);
        }
        if (!private_channel_store_prepare(private_store, input) ||
            !user_store_apply_gateway_snapshots(users, {payload.data, cursor}, 1)) return apply_error(ChatStateApplyError::capacity);
        const u32 revision = private_store->revision;
        if (!private_channel_store_replace(private_store, input) ||
            (update_nick && !private_channel_store_replace_nicknames(private_store, channel->id, {nicks, nick_count})))
            return apply_error(ChatStateApplyError::capacity);
        if (private_store->revision != revision) {
            chat_state_store_touch_private_channel(store, channel->id);
            result.channels = 1;
        }
        return result;
    }
    if (guild_event) {
        // Start only the selected trivial member. Every decoder writes its used
        // prefix, and synchronous consumers finish before this lifetime ends.
        // Default construction leaves every unused scalar untouched.
        auto& guild_scratch = *::new (scratch) GuildStateScratch;
        if (event.dispatch_kind == GatewayClientDispatchKind::guild_emojis_update) {
            auto& emojis = (::new (static_cast<void*>(&guild_scratch.transient.emojis)) GuildEmojiScratch)->values;
            u32 emoji_count = 0;
            if (event.state.guild_id == 0 || !event.state.emojis_known ||
                !chat_state_store_find_guild(*store, event.state.guild_id) ||
                !guild_emojis_payload(
                    event, payload,
                    Slice<CustomEmojiInput>{emojis, kGuildEmojiMaximum},
                    &emoji_count)) {
                return apply_error(ChatStateApplyError::invalid_payload);
            }
            const auto emoji_admission = chat_state_store_guild_emoji_admission(
                *store, event.state.guild_id, emoji_count, event.state.emoji_role_count);
            if (emoji_admission != ChatStateStoreError::none)
                return admission_error(emoji_admission, ChatStateApplyError::invalid_payload);
            if (!chat_state_store_replace_guild_emojis(
                    store, event.state.guild_id,
                    ConstSlice<CustomEmojiInput>{emojis, emoji_count})) {
                return apply_error(ChatStateApplyError::capacity);
            }
            result.emojis = emoji_count;
        } else if (event.dispatch_kind == GatewayClientDispatchKind::guild_create) {
            Utf8View name{};
            Utf8View icon_hash{};
            if (!state_payload_disjoint(*store, payload) ||
                !state_payload_disjoint(*store, {reinterpret_cast<const u8*>(&event), sizeof(event)}) ||
                !gateway_client_event_state_name(event, payload, &name) || name.bytes > 400 || event.state.id == 0) {
                return apply_error(ChatStateApplyError::invalid_payload);
            }
            if ((event.state.fields & gateway_client_state_field_icon) != 0 &&
                !gateway_client_event_guild_icon(event, payload, &icon_hash)) {
                return apply_error(ChatStateApplyError::invalid_payload);
            }
            if (event.state.unavailable) {
                const ChatStateStoreResult applied = chat_state_store_upsert_guild(
                    store, guild_input(event, payload, name, icon_hash,
                                       GuildMutationKind::unavailable));
                if (!applied.ok()) {
                    return store_error(applied);
                }
                result.guilds = 1;
            } else {
                auto& role_offsets = guild_scratch.role_offsets;
                const ConstSlice<u32> offsets{role_offsets, event.state.role_count};
                if (store->self_user_id == 0 || event.state.owner_id == 0 ||
                    !gateway_client_event_guild_role_offsets(event, payload, {role_offsets, kGuildRoleMaximum}) ||
                    !guild_roles_valid(event, payload, offsets)) {
                    return apply_error(ChatStateApplyError::invalid_payload);
                }
                u32 roles_end = 0;
                if (!guild_roles_payload_bytes(event, payload, &roles_end, offsets) ||
                    (!(event.state.member_role_flags & gateway_client_member_roles_known) &&
                     event.state.member_role_count) ||
                    event.state.member_role_count > kGuildRoleMaximum || roles_end > payload.count ||
                    event.state.member_role_count > (payload.count - roles_end) / 8u)
                    return apply_error(ChatStateApplyError::invalid_payload);
                const u32 channels_begin = roles_end + event.state.member_role_count * 8u;
                auto& self_roles = guild_scratch.self_roles;
                if (!guild_self_roles(event, payload,
                                      Slice<u64>{self_roles, kGuildRoleMaximum}, offsets, roles_end)) {
                    return apply_error(ChatStateApplyError::invalid_payload);
                }
                u32 cursor = channels_begin;
                u32 index = 0;
                u32 overwrite_count = 0;
                u64 largest_prior_id = 0;
                auto& channel_ids = *::new (static_cast<void*>(&guild_scratch.transient.channel_ids)) GuildChannelIdScratch;
                bool ids_increasing = true;
                for (; index < event.state.record_count;) {
                    GatewayClientStateMutation channel{};
                    Utf8View channel_name{};
                    ConstSlice<u8> overwrites{};
                    if (!gateway_client_event_next_guild_channel(event, payload, &cursor, &index, &channel,
                                                                 &channel_name, &overwrites) ||
                        !validate_channel_state(channel) || channel.guild_id != event.state.id ||
                        channel_name.bytes > 400 ||
                        (!(channel.fields & gateway_client_state_field_permission_overwrites) && channel.overwrite_count != 0) ||
                        (!channel_is_thread(channel.type) &&
                         (channel.fields & gateway_client_state_field_permission_overwrites) == 0) ||
                        !overwrite_payload_valid(overwrites, channel.overwrite_count)) {
                        return apply_error(ChatStateApplyError::invalid_payload);
                    }
                    const auto* resident = chat_state_store_find_channel(*store, channel.id);
                    if (resident && resident->guild_id != event.state.id)
                        return apply_error(ChatStateApplyError::invalid_payload);
                    // Admission precedes every ID write. This transient prefix
                    // borrows the later emoji lifetime without changing packet
                    // order or the public typed-payload capacity. Larger callers
                    // keep the exact prior-record check beyond the scratch bound.
                    if (index <= kGuildChannelIdCapacity) {
                        if (index > 1 && channel.id <= channel_ids.values[index - 2]) ids_increasing = false;
                        channel_ids.values[index - 1] = channel.id;
                    } else if (channel.id <= largest_prior_id) {
                        u32 prior_cursor = channels_begin, prior_index = 0;
                        while (prior_index + 1 < index) {
                            GatewayClientStateMutation prior{}; Utf8View prior_name{}; ConstSlice<u8> prior_overwrites{};
                            if (!gateway_client_event_next_guild_channel(event, payload, &prior_cursor, &prior_index,
                                &prior, &prior_name, &prior_overwrites) || prior.id == channel.id)
                                return apply_error(ChatStateApplyError::invalid_payload);
                        }
                    }
                    if (channel.id > largest_prior_id) largest_prior_id = channel.id;
                    overwrite_count += channel.overwrite_count;
                }
                const u32 id_count = minimum(index, kGuildChannelIdCapacity);
                if (!ids_increasing) {
                    heap_sort(channel_ids.values, id_count, [](u64 a, u64 b) noexcept { return a < b; });
                    for (u32 id = 1; id < id_count; ++id)
                        if (channel_ids.values[id - 1] == channel_ids.values[id])
                            return apply_error(ChatStateApplyError::invalid_payload);
                }
                channel_ids.~GuildChannelIdScratch();
                auto& emojis = (::new (static_cast<void*>(&guild_scratch.transient.emojis)) GuildEmojiScratch)->values;
                u32 emoji_count = 0;
                if (overwrite_count != event.state.overwrite_count ||
                    (event.state.emojis_known
                         ? !guild_emojis_payload(
                               event, payload,
                               Slice<CustomEmojiInput>{
                                   emojis, kGuildEmojiMaximum},
                               &emoji_count)
                         : (event.state.emoji_count != 0 ||
                            event.state.emoji_role_count != 0 ||
                            cursor != payload.count))) {
                    return apply_error(ChatStateApplyError::invalid_payload);
                }
                const u32 resident_channels = resident_guild_channels(*store, event.state.id);
                if (event.state.record_count >
                    store->channel_capacity - (store->channel_count - resident_channels))
                    return table_full(ChatStateStoreError::channel_capacity);
                const auto permission_admission = chat_state_store_guild_permission_admission(
                    *store, event.state.id, event.state.role_count, event.state.overwrite_count);
                if (permission_admission != ChatStateStoreError::none)
                    return admission_error(permission_admission, ChatStateApplyError::capacity);
                if (event.state.emojis_known) {
                    const auto emoji_admission = chat_state_store_guild_emoji_admission(
                        *store, event.state.id, emoji_count, event.state.emoji_role_count);
                    if (emoji_admission != ChatStateStoreError::none)
                        return admission_error(emoji_admission, ChatStateApplyError::capacity);
                }
                const GuildSettings settings = guild_settings(event.state);
                const auto input =
                    guild_input(event, payload, name, icon_hash, GuildMutationKind::create, &settings);
                const GuildRoleInputContext role_input{&event, payload, offsets};
                const GuildChannelInputContext channel_input_context{&event, payload, channels_begin, 0};
                const auto admission = chat_state_store_preflight_guild_create(*store, input,
                    event.state.role_count, read_guild_role_input, &role_input,
                    event.state.record_count, read_guild_channel_input, &channel_input_context);
                if (admission == ChatStateStoreError::invalid_input)
                    return apply_error(ChatStateApplyError::invalid_payload);
                if (admission != ChatStateStoreError::none)
                    return table_full(admission);
                if (!chat_state_store_prepare_permissions(store,
                    store->role_count - resident_guild_roles(*store, event.state.id) + event.state.role_count,
                    store->overwrite_count - resident_guild_overwrites(*store, event.state.id) + event.state.overwrite_count))
                    return apply_error(ChatStateApplyError::capacity);
                const ChatStateStoreResult guild = chat_state_store_upsert_guild(store, input);
                if (!guild.ok()) {
                    return store_error(guild);
                }
                // The selected channel is briefly absent while the guild is
                // rebuilt; choose a default only once it is complete.
                struct SelectionHold {
                    ChatStateStore* store;
                    ~SelectionHold() {
                        store->selection_held = false;
                        chat_state_store_select_default(store);
                    }
                } const hold{store};
                store->selection_held = true;
                chat_state_store_remove_guild_channels(store, event.state.id);
                chat_state_store_remove_guild_roles(store, event.state.id);
                for (u32 role_index = 0; role_index < event.state.role_count; ++role_index) {
                    GatewayClientRoleMutation role{};
                    Utf8View role_name{};
                    if (!gateway_client_event_guild_role_at(event, payload, offsets[role_index],
                                                            &role, &role_name) ||
                        !chat_state_store_upsert_role(store, packed_role_input(event.state.id, role, role_name))) {
                        return apply_error(ChatStateApplyError::capacity);
                    }
                }
                cursor = channels_begin;
                index = 0;
                while (index < event.state.record_count) {
                    GatewayClientStateMutation channel{};
                    Utf8View channel_name{};
                    ConstSlice<u8> overwrites{};
                    if (!gateway_client_event_next_guild_channel(event, payload, &cursor, &index, &channel,
                                                                 &channel_name, &overwrites)) {
                        return apply_error(ChatStateApplyError::invalid_payload);
                    }
                    const ChatStateStoreResult applied =
                        chat_state_store_upsert_channel(store, channel_input(channel, channel_name));
                    if (!applied.ok()) {
                        return store_error(applied);
                    }
                    if ((channel.fields & gateway_client_state_field_permission_overwrites) != 0 &&
                        !apply_overwrites(store, channel.id, overwrites, channel.overwrite_count)) {
                        return apply_error(ChatStateApplyError::capacity);
                    }
                }
                if ((event.state.member_role_flags & gateway_client_member_roles_known) &&
                    !chat_state_store_set_self_roles(
                        store, event.state.id, event.state.owner_id, store->self_user_id,
                        ConstSlice<u64>{self_roles, event.state.member_role_count})) {
                    return apply_error(ChatStateApplyError::invalid_payload);
                }
                chat_state_store_recompute_guild_permissions(store, event.state.id);
                if (event.state.emojis_known &&
                    !chat_state_store_replace_guild_emojis(
                        store, event.state.id,
                        ConstSlice<CustomEmojiInput>{emojis, emoji_count})) {
                    return apply_error(ChatStateApplyError::capacity);
                }
                result.guilds = 1;
                result.channels = event.state.record_count;
                result.roles = event.state.role_count;
                result.overwrites = event.state.overwrite_count;
                result.emojis = emoji_count;
            }
        } else if (event.dispatch_kind == GatewayClientDispatchKind::guild_update) {
            auto& emojis = (::new (static_cast<void*>(&guild_scratch.transient.emojis)) GuildEmojiScratch)->values;
            Utf8View name{};
            Utf8View icon_hash{};
            u32 roles_bytes = 0;
            auto& role_offsets = guild_scratch.role_offsets;
            const ConstSlice<u32> offsets{role_offsets, event.state.role_count};
            u32 emoji_count = 0;
            if (!state_payload_disjoint(*store, payload) ||
                !state_payload_disjoint(*store, {reinterpret_cast<const u8*>(&event), sizeof(event)}) ||
                !gateway_client_event_state_name(event, payload, &name) || event.state.id == 0 ||
                event.state.owner_id == 0 ||
                ((event.state.fields & gateway_client_state_field_icon) != 0 &&
                 !gateway_client_event_guild_icon(event, payload, &icon_hash)) ||
                !gateway_client_event_guild_role_offsets(event, payload, {role_offsets, kGuildRoleMaximum}) ||
                !guild_roles_payload_bytes(event, payload, &roles_bytes, offsets) ||
                (event.state.emojis_known
                     ? !guild_emojis_payload(
                           event, payload,
                           Slice<CustomEmojiInput>{emojis,
                                                    kGuildEmojiMaximum},
                           &emoji_count)
                     : (event.state.emoji_count != 0 ||
                        event.state.emoji_role_count != 0 ||
                        roles_bytes != payload.count)) ||
                !guild_roles_valid(event, payload, offsets)) {
                return apply_error(ChatStateApplyError::invalid_payload);
            }
            const u32 old_roles = resident_guild_roles(*store, event.state.id);
            if (store->roles && event.state.role_count >
                                    store->role_capacity - (store->role_count - old_roles))
                return table_full(ChatStateStoreError::role_capacity);
            if (event.state.emojis_known) {
                const auto emoji_admission = chat_state_store_guild_emoji_admission(
                    *store, event.state.id, emoji_count, event.state.emoji_role_count);
                if (emoji_admission != ChatStateStoreError::none)
                    return admission_error(emoji_admission, ChatStateApplyError::capacity);
            }
            const GuildSettings settings = guild_settings(event.state);
            const auto guild =
                guild_input(event, payload, name, icon_hash, GuildMutationKind::update, &settings);
            const GuildRoleInputContext role_input{&event, payload, offsets};
            const auto admission = chat_state_store_preflight_guild_role_update(
                *store, guild, event.state.role_count, read_guild_role_input, &role_input);
            if (admission == ChatStateStoreError::invalid_input) return apply_error(ChatStateApplyError::invalid_payload);
            if (admission != ChatStateStoreError::none)
                return table_full(admission);
            if (!chat_state_store_prepare_permissions(store,
                store->role_count - old_roles + event.state.role_count, store->overwrite_count))
                return apply_error(ChatStateApplyError::capacity);
            const ChatStateStoreResult applied = chat_state_store_upsert_guild(store, guild);
            if (!applied.ok()) {
                return store_error(applied);
            }
            for (u32 index = store->role_count; index != 0; --index) {
                const RoleRecord role = store->roles[index - 1];
                bool retained = false;
                if (role.guild_id == event.state.id) for (u32 incoming = 0; incoming < offsets.count; ++incoming) {
                    RoleInput input{};
                    if (!read_guild_role_input(&role_input, incoming, &input)) return apply_error(ChatStateApplyError::invalid_payload);
                    if (input.id == role.id) { retained = true; break; }
                }
                if (role.guild_id == event.state.id && !retained) {
                    chat_state_store_remove_role(store, event.state.id, role.id);
                }
            }
            for (u32 index = 0; index < event.state.role_count; ++index) {
                GatewayClientRoleMutation role{};
                Utf8View role_name{};
                if (!gateway_client_event_guild_role_at(event, payload, offsets[index], &role, &role_name) ||
                    !chat_state_store_upsert_role(store, packed_role_input(event.state.id, role, role_name))) {
                    return apply_error(ChatStateApplyError::capacity);
                }
            }
            chat_state_store_recompute_guild_permissions(store, event.state.id);
            if (event.state.emojis_known &&
                !chat_state_store_replace_guild_emojis(
                    store, event.state.id,
                    ConstSlice<CustomEmojiInput>{emojis, emoji_count})) {
                return apply_error(ChatStateApplyError::capacity);
            }
            result.guilds = 1;
            result.roles = event.state.role_count;
            result.emojis = emoji_count;
        }
    } else if (event.dispatch_kind == GatewayClientDispatchKind::guild_delete) {
        const bool unavailable =
            (event.state.fields & gateway_client_state_field_unavailable) != 0 && event.state.unavailable;
        if (event.state.id == 0 || !chat_state_store_remove_guild(store, event.state.id, unavailable)) {
            return apply_error(ChatStateApplyError::invalid_payload);
        }
        result.guilds = 1;
    } else if (event.dispatch_kind == GatewayClientDispatchKind::guild_role_create ||
               event.dispatch_kind == GatewayClientDispatchKind::guild_role_update) {
        constexpr u16 required = gateway_client_state_field_id |
                                 gateway_client_state_field_guild_id |
                                 gateway_client_state_field_permissions;
        Utf8View role_name{};
        if ((event.state.fields & required) != required || event.state.id == 0 ||
            event.state.guild_id == 0 ||
            !gateway_client_event_state_name(event, payload, &role_name) ||
            payload.count != role_name.bytes ||
            !chat_state_store_find_guild(*store, event.state.guild_id) ||
            !chat_state_store_upsert_role(
                store, RoleInput{event.state.id, event.state.guild_id,
                                 event.state.permissions, role_name,
                                 event.state.position, event.state.color,
                                 event.state.hoist,
                                 event.state.managed,
                                 (event.state.role_flags & gateway_client_role_mentionable) != 0,
                                 (event.state.role_flags & gateway_client_role_has_icon) != 0,
                                 (event.state.role_flags & gateway_client_role_in_prompt) != 0,
                                 event.state.secondary_color, event.state.tertiary_color,
                                 (event.state.role_flags & gateway_client_role_linked) != 0})) {
            return apply_error(ChatStateApplyError::capacity);
        }
        chat_state_store_recompute_guild_permissions(store, event.state.guild_id);
        result.roles = 1;
    } else if (event.dispatch_kind == GatewayClientDispatchKind::guild_role_delete) {
        if (event.state.id == 0 || event.state.guild_id == 0 || payload.count != 0 ||
            !chat_state_store_remove_role(store, event.state.guild_id, event.state.id)) {
            return apply_error(ChatStateApplyError::invalid_payload);
        }
        chat_state_store_recompute_guild_permissions(store, event.state.guild_id);
        result.roles = 1;
    } else if (member_event) {
        const GuildRecord* guild =
            chat_state_store_find_guild(*store, event.state.guild_id);
        auto& member_scratch = *::new (scratch) MemberRoleScratch;
        auto& roles = member_scratch.ids;
        if (!guild || event.state.id == 0 || event.state.id != store->self_user_id ||
            !member_roles(event, payload, Slice<u64>{roles, kGuildRoleMaximum}) ||
            (!(event.state.member_role_flags & gateway_client_member_roles_known) &&
             event.state.member_role_count) ||
            ((event.state.member_role_flags & gateway_client_member_roles_known) &&
             !chat_state_store_set_self_roles(store, event.state.guild_id, guild->owner_id, event.state.id,
                                              ConstSlice<u64>{roles, event.state.member_role_count}))) {
            return apply_error(ChatStateApplyError::invalid_payload);
        }
        if (event.state.guild_settings_known & guild_settings_self_timeout)
            chat_state_store_set_self_timeout(store, event.state.guild_id,
                                                 event.state.self_communication_disabled_until);
        if (event.state.guild_settings_known & (guild_settings_self_member | guild_settings_self_quarantine)) {
            GuildSettings member{};
            member.known = static_cast<u16>(event.state.guild_settings_known &
                                            (guild_settings_self_member | guild_settings_self_quarantine));
            member.self_member = event.state.self_member;
            chat_state_store_set_guild_settings(store, event.state.guild_id, member);
        }
        chat_state_store_recompute_guild_permissions(store, event.state.guild_id);
        result.roles = event.state.member_role_count;
    } else if (event.dispatch_kind == GatewayClientDispatchKind::channel_delete) {
        const ChannelRecord* channel =
            chat_state_store_find_channel(*store, event.state.id);
        if (!channel && validate_channel_state(event.state) && channel_is_private(event.state.type)) return result;
        if (!validate_channel_state(event.state) || !channel || channel->guild_id != event.state.guild_id ||
            channel->type != event.state.type) {
            return apply_error(ChatStateApplyError::invalid_payload);
        }
        const u64 guild_id = channel->guild_id;
        if (!chat_state_store_remove_channel(store, event.state.id)) {
            return apply_error(ChatStateApplyError::invalid_payload);
        }
        chat_state_store_recompute_guild_permissions(store, guild_id);
        result.channels = 1;
    } else {
        Utf8View name{};
        ConstSlice<u8> overwrites{};
        const bool has_overwrites =
            (event.state.fields & gateway_client_state_field_permission_overwrites) != 0;
        const ChannelRecord* resident =
            chat_state_store_find_channel(*store, event.state.id);
        if (!state_payload_disjoint(*store, payload) ||
            !state_payload_disjoint(*store, {reinterpret_cast<const u8*>(&event), sizeof(event)}) ||
            !validate_channel_state(event.state, event.dispatch_kind == GatewayClientDispatchKind::channel_create) ||
            !gateway_client_event_state_name(event, payload, &name) || name.bytes > 400 ||
            (resident && resident->guild_id != event.state.guild_id) ||
            (!has_overwrites && event.state.overwrite_count != 0) ||
            (!has_overwrites && !channel_is_thread(event.state.type) && !channel_is_private(event.state.type) &&
             (event.dispatch_kind == GatewayClientDispatchKind::channel_create || !resident)) ||
            (has_overwrites &&
             (!gateway_client_event_channel_overwrites(event, payload, &overwrites) ||
              !overwrite_payload_valid(overwrites, event.state.overwrite_count) ||
              !chat_state_store_can_replace_channel_overwrites(
                  *store, event.state.id, event.state.overwrite_count)))) {
            return apply_error(ChatStateApplyError::invalid_payload);
        }
        auto input = channel_input(event.state, name);
        if (resident && !(event.state.fields & gateway_client_state_field_parent_id)) input.parent_id = resident->parent_id;
        const auto admission = chat_state_store_preflight_channel(*store, input);
        if (admission == ChatStateStoreError::invalid_input)
            return apply_error(ChatStateApplyError::invalid_payload);
        if (admission != ChatStateStoreError::none)
            return table_full(admission);
        if (has_overwrites && !chat_state_store_prepare_permissions(store, store->role_count,
            store->overwrite_count - (resident ? resident->overwrite_count : 0u) + event.state.overwrite_count))
            return apply_error(ChatStateApplyError::capacity);
        PrivateChannelInput private_input{};
        PrivateNicknameInput* private_nicks = nullptr;
        const bool private_channel = channel_is_private(event.state.type);
        if (private_channel) {
            auto& private_scratch = *::new (scratch) PrivateStateScratch;
            auto& recipients = private_scratch.ids;
            private_nicks = private_scratch.nicks;
            if ((!event.state.recipients_known && event.state.recipient_count) ||
                event.state.recipient_count > kPrivateRecipientMaximum ||
                event.state.icon_bytes > payload.count - name.bytes ||
                (event.state.icon_bytes && !(event.state.fields & gateway_client_state_field_icon)))
                return apply_error(ChatStateApplyError::invalid_payload);
            private_input = {event.state.id, event.state.owner_id, {recipients, event.state.recipient_count},
                {payload.data ? reinterpret_cast<const char*>(payload.data + name.bytes) : nullptr, event.state.icon_bytes},
                event.state.recipients_known, (event.state.fields & gateway_client_state_field_owner_id) != 0,
                (event.state.fields & gateway_client_state_field_icon) != 0};
            u32 cursor = name.bytes + event.state.icon_bytes;
            const u32 user_offset = cursor;
            for (u32 i = 0; i < event.state.recipient_count; ++i) {
                GatewayClientUserMutation user{};
                Utf8View display{}, avatar{};
                if (!gateway_client_user_record(payload, &cursor, &user, &display, &avatar))
                    return apply_error(ChatStateApplyError::invalid_payload);
                recipients[i] = user.id;
            }
            const u32 user_end = cursor;
            if ((!event.state.private_nicknames_known && event.state.record_count) || event.state.record_count > kPrivateRecipientMaximum)
                return apply_error(ChatStateApplyError::invalid_payload);
            for (u32 i = 0; i < event.state.record_count; ++i) {
                GatewayClientUserMutation nick{};
                Utf8View name_value{}, avatar_value{};
                if (!gateway_client_user_record(payload, &cursor, &nick, &name_value, &avatar_value) || nick.flags != 1)
                    return apply_error(ChatStateApplyError::invalid_payload);
                private_nicks[i] = {nick.id, name_value};
            }
            // Optional trailer: request/safety flags, then the warning id.
            const u8 private_flags = cursor < payload.count ? payload.data[cursor++] : 0;
            private_input.request_known = (private_flags & gateway_client_private_request_known) != 0;
            private_input.request_flags =
                static_cast<u8>(((private_flags & gateway_client_private_request_pending) ? private_request_pending : 0) |
                                ((private_flags & gateway_client_private_request_spam) ? private_request_spam : 0));
            private_input.safety_known = (private_flags & gateway_client_private_safety_known) != 0;
            if (const u8 safety = gateway_client_private_safety_type(private_flags)) {
                if (safety > 4 || payload.count - cursor < 8 || !private_input.safety_known)
                    return apply_error(ChatStateApplyError::invalid_payload);
                private_input.safety_warning = static_cast<PrivateSafetyWarning>(safety);
                private_input.safety_warning_id = byte_load_le64(payload.data + cursor);
                cursor += 8;
            }
            if (cursor != payload.count ||
                ((private_input.recipients_known || private_input.owner_known || private_input.icon_known ||
                  private_input.request_known || private_input.safety_known ||
                  event.state.private_nicknames_known) && !store->private_channels) ||
                (store->private_channels && !private_channel_store_prepare(store->private_channels, private_input)))
                return apply_error(ChatStateApplyError::invalid_payload);
            if (event.state.private_nicknames_known && !private_channel_store_prepare_nicknames(store->private_channels,
                    event.state.id, {private_nicks, event.state.record_count})) return apply_error(ChatStateApplyError::capacity);
            if (!user_store_apply_gateway_snapshots(users,
                    {payload.data ? payload.data + user_offset : nullptr, user_end - user_offset}, event.state.recipient_count))
                return apply_error(ChatStateApplyError::capacity);
        }
        const ChatStateStoreResult applied =
            chat_state_store_upsert_channel(store, input);
        if (!applied.ok()) {
            return store_error(applied);
        }
        if (private_channel && store->private_channels && !private_channel_store_replace(store->private_channels, private_input))
            return apply_error(ChatStateApplyError::capacity);
        if (private_channel && event.state.private_nicknames_known && !private_channel_store_replace_nicknames(
                store->private_channels, event.state.id, {private_nicks, event.state.record_count}))
            return apply_error(ChatStateApplyError::capacity);
        if (private_channel) private_channel_store_touch_sort(store->private_channels, event.state.id);
        if (has_overwrites &&
            !apply_overwrites(store, event.state.id, overwrites, event.state.overwrite_count)) {
            return apply_error(ChatStateApplyError::capacity);
        }
        chat_state_store_recompute_guild_permissions(store, event.state.guild_id);
        if (event.state.channel_event_flags & gateway_client_channel_new_thread)
            chat_state_store_note_thread(store, input.parent_id, input.id, event.state.owner_id);
        result.channels = 1;
        result.overwrites = event.state.overwrite_count;
    }

    result.selection_changed = selection_revision != store->selection_revision;
    return result;
}

bool guild_detail_store_apply_gateway_event(GuildDetailStore* store, const ChatStateStore& state,
                                            const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    if (!store || !store->records)
        return false;
    if (event.kind == GatewayClientEventKind::ready) {
        const u32 revision = store->revision;
        guild_detail_store_clear(store);
        return revision != store->revision;
    }
    if (event.kind != GatewayClientEventKind::dispatch)
        return false;
    if (event.dispatch_kind == GatewayClientDispatchKind::guild_delete)
        return !(event.state.unavailable && (event.state.fields & gateway_client_state_field_unavailable)) &&
               guild_detail_store_remove(store, event.state.id);
    GatewayGuildDetails details{};
    Utf8View description{}, vanity{};
    ConstSlice<u8> names{};
    if (!gateway_client_event_guild_details(event, payload, &details, &description, &vanity, &names))
        return false;
    const GuildRecord* guild = chat_state_store_find_guild(state, event.state.id);
    constexpr u64 manage = kPermissionManageGuild | kPermissionAdministrator;
    GuildDetailInput input{};
    input.guild_id = event.state.id;
    input.rules_channel_id = details.rules_channel_id;
    input.public_updates_channel_id = details.public_updates_channel_id;
    input.safety_alerts_channel_id = details.safety_alerts_channel_id;
    input.invites_disabled_until_ms = details.invites_disabled_until_ms;
    input.dms_disabled_until_ms = details.dms_disabled_until_ms;
    input.raid_detected_at_ms = details.raid_detected_at_ms;
    input.dm_spam_detected_at_ms = details.dm_spam_detected_at_ms;
    input.description = description;
    input.vanity_url_code = vanity;
    input.splash = {details.splash, details.splash_bytes <= sizeof(details.splash) ? details.splash_bytes : u8{0}};
    input.feature_names = names;
    input.system_channel_flags = details.system_channel_flags;
    input.premium_subscription_count = details.premium_subscription_count;
    input.member_count = details.member_count;
    input.feature_count = details.feature_count;
    input.known = details.known;
    input.premium_tier = details.premium_tier;
    input.lockdown_duration_hours = details.lockdown_duration_hours;
    input.premium_progress_bar_enabled = details.premium_progress_bar_enabled;
    input.keep_feature_names = !guild || !guild->permissions_resolved || (guild->base_permissions & manage);
    return guild_detail_store_apply(store, input);
}

} // namespace maicord
