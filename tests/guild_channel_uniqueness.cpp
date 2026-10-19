#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/permission_bits.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr u32 kIdPrefix = sizeof(CustomEmojiInput) * kGuildEmojiMaximum / sizeof(u64);
constexpr u32 kRows = kIdPrefix + 2;
constexpr u32 kPayloadBytes = kRows * (sizeof(GatewayClientStateMutation) + 8) + 1024;
constexpr u64 kGuild = 77, kOwner = 999, kSelf = 42, kRole = 78;
constexpr u64 kPermissions = kPermissionViewChannel | kPermissionReadMessageHistory |
    kPermissionSendMessages;
static_assert(kIdPrefix == 3840 && sizeof(GatewayClientStateMutation) == 168);
struct UniquenessModel {
    ChatStateStore state;
    GuildRecord guilds[2];
    ChannelRecord channels[kRows + 1];
    RoleRecord roles[4];
    PermissionOverwriteRecord overwrites[8];
    CustomEmojiRecord emojis[4];
    u64 emoji_roles[8];
    u16 guild_order[2], channel_order[kRows + 1];
    u8 names[65536];
};
UniquenessModel actual, expected, snapshot;
GatewayClientEvent event;
u8 payload[kPayloadBytes], payload_snapshot[kPayloadBytes];
u32 channel_offsets[kRows], used, rows, order;
u64 id_base;
bool named, emoji_output;

bool reset(UniquenessModel* model, bool resident = false) noexcept {
    clear_bytes(model, sizeof(*model));
    auto& state = model->state;
    if (!chat_state_store_initialize(&state, {model->guilds, 2}, {model->guild_order, 2},
        {model->channels, kRows + 1}, {model->channel_order, kRows + 1}, {model->names, sizeof(model->names)}) ||
        !chat_state_store_bind_permissions(&state, {model->roles, 4}, {model->overwrites, 8}) ||
        !chat_state_store_bind_emojis_sparse(&state, {model->emojis, 4}, {model->emoji_roles, 8})) return false;
    state.self_user_id = kSelf;
    if (!chat_state_store_upsert_guild(&state, {99, utf8_literal("Stay"), GuildMutationKind::create, kOwner}).ok() ||
        !chat_state_store_upsert_channel(&state, {5000, 99, 0, utf8_literal("stable"), 0, 0, 0}).ok()) return false;
    if (resident && (!chat_state_store_upsert_guild(&state,
        {kGuild, utf8_literal("Old"), GuildMutationKind::create, kOwner}).ok() ||
        !chat_state_store_upsert_channel(&state, {100001, kGuild, 0, utf8_literal("retire"), 0, 0, 0}).ok() ||
        !chat_state_store_upsert_role(&state, {kGuild, kGuild, kPermissions, utf8_literal("O")}))) return false;
    return true;
}
u32 ordinal(u32 index) noexcept {
    if (order == 1) return rows - 1 - index;
    if (order == 2) return (index + rows / 3) % rows;
    return index;
}
GatewayClientStateMutation wire_channel(u32 index) noexcept {
    GatewayClientStateMutation value{};
    value.id = id_base + ordinal(index);
    value.guild_id = kGuild;
    value.position = index;
    value.type = 0;
    value.fields = gateway_client_state_field_id | gateway_client_state_field_type |
        gateway_client_state_field_guild_id | gateway_client_state_field_name |
        gateway_client_state_field_permission_overwrites | gateway_client_state_field_last_message_id;
    value.last_message_id = 1000000 + index;
    value.name_bytes = named ? 8 : 0;
    value.member_list_id_known = named;
    value.member_list_id = named ? 2000000 + index : 0;
    return value;
}
void channel_name(u32 index, char* text) noexcept {
    for (u32 i = 0; i < 8; ++i) text[i] = static_cast<char>('a' + (index + 3 * i) % 26);
}
bool append(const void* data, u32 bytes) noexcept {
    if (bytes > sizeof(payload) - used) return false;
    copy_bytes(payload + used, data, bytes); used += bytes; return true;
}
bool append_emojis() noexcept {
    for (u32 i = 0; i < 2; ++i) {
        GatewayClientEmojiMutation emoji{};
        emoji.id = 900 + i; emoji.name_bytes = 5; emoji.role_count = 1;
        emoji.flags = static_cast<u8>(gateway_client_emoji_available | (i ? gateway_client_emoji_animated : 0));
        u8 role_bytes[8]; byte_store_le64(role_bytes, kRole);
        if (!append(&emoji, sizeof(emoji)) || !append(role_bytes, 8) ||
            !append(i ? "waves" : "spark", 5)) return false;
    }
    return true;
}
bool build(u32 count, u32 sequence, bool names, bool emojis, bool high_ids = false) noexcept {
    if (count > kRows) return false;
    event = {}; used = 0; rows = count; order = sequence; named = names; emoji_output = emojis;
    id_base = high_ids ? ~u64{0} - count - 1 : 100000;
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::guild_create;
    event.state.id = kGuild; event.state.owner_id = kOwner;
    event.state.fields = gateway_client_state_field_id | gateway_client_state_field_name | gateway_client_state_field_owner_id;
    event.state.name_bytes = 5; event.state.role_count = 2; event.state.member_role_count = 1;
    event.state.member_role_flags = gateway_client_member_roles_known;
    event.state.record_count = count; event.state.emojis_known = emojis;
    event.state.emoji_count = emojis ? 2 : 0; event.state.emoji_role_count = emojis ? 2 : 0;
    if (!append("Bench", 5)) return false;
    for (u32 i = 0; i < 2; ++i) {
        GatewayClientRoleMutation role{};
        role.id = i ? kRole : kGuild; role.permissions = kPermissions;
        role.name_bytes = 1; role.position = i;
        if (!append(&role, sizeof(role)) || !append(i ? "R" : "E", 1)) return false;
    }
    u8 self_role[8]; byte_store_le64(self_role, kRole);
    if (!append(self_role, 8)) return false;
    for (u32 i = 0; i < rows; ++i) {
        const auto channel = wire_channel(i); char name[8]; channel_name(i, name);
        channel_offsets[i] = used;
        if (!append(&channel, sizeof(channel)) || !append(name, channel.name_bytes)) return false;
    }
    if (emojis && !append_emojis()) return false;
    event.dispatch_payload_bytes = used;
    return true;
}
bool expected_model(bool resident) noexcept {
    if (!reset(&expected, resident)) return false;
    auto& state = expected.state;
    if (!chat_state_store_upsert_guild(&state, {kGuild, utf8_literal("Bench"),
        GuildMutationKind::create, kOwner}).ok()) return false;
    chat_state_store_remove_guild_channels(&state, kGuild);
    chat_state_store_remove_guild_roles(&state, kGuild);
    if (!chat_state_store_upsert_role(&state, {kGuild, kGuild, kPermissions, utf8_literal("E"), 0}) ||
        !chat_state_store_upsert_role(&state, {kRole, kGuild, kPermissions, utf8_literal("R"), 1})) return false;
    for (u32 i = 0; i < rows; ++i) {
        const auto value = wire_channel(i); char name[8]; channel_name(i, name);
        if (!chat_state_store_upsert_channel(&state, {value.id, kGuild, 0, {name, value.name_bytes},
            value.position, 0, 0, value.last_message_id, true, value.member_list_id,
            value.member_list_id_known, true}).ok()) return false;
    }
    const u64 self_role = kRole;
    if (!chat_state_store_set_self_roles(&state, kGuild, kOwner, kSelf, {&self_role, 1})) return false;
    chat_state_store_recompute_guild_permissions(&state, kGuild);
    if (emoji_output) {
        u8 role_bytes[8]; byte_store_le64(role_bytes, kRole);
        const CustomEmojiInput emojis[] = {{900, utf8_literal("spark"), {role_bytes, 8}, 1, custom_emoji_available, 0},
            {901, utf8_literal("waves"), {role_bytes, 8}, 1, custom_emoji_available | custom_emoji_animated, 0}};
        if (!chat_state_store_replace_guild_emojis(&state, kGuild, {emojis, 2})) return false;
    }
    return true;
}
bool same_model() noexcept {
    ChatStateStore normalized;
    copy_bytes(&normalized, &expected.state, sizeof(normalized));
    normalized.guilds = actual.state.guilds; normalized.channels = actual.state.channels;
    normalized.roles = actual.state.roles; normalized.overwrites = actual.state.overwrites;
    normalized.emojis = actual.state.emojis; normalized.emoji_roles = actual.state.emoji_roles;
    normalized.guild_order = actual.state.guild_order; normalized.channel_order = actual.state.channel_order;
    normalized.names = actual.state.names;
    return bytes_equal(&actual.state, &normalized, sizeof(normalized)) &&
        bytes_equal(actual.guilds, expected.guilds, sizeof(actual.guilds)) &&
        bytes_equal(actual.channels, expected.channels, sizeof(actual.channels)) &&
        bytes_equal(actual.roles, expected.roles, sizeof(actual.roles)) &&
        bytes_equal(actual.overwrites, expected.overwrites, sizeof(actual.overwrites)) &&
        bytes_equal(actual.emojis, expected.emojis, sizeof(actual.emojis)) &&
        bytes_equal(actual.emoji_roles, expected.emoji_roles, sizeof(actual.emoji_roles)) &&
        bytes_equal(actual.guild_order, expected.guild_order, sizeof(actual.guild_order)) &&
        bytes_equal(actual.channel_order, expected.channel_order, sizeof(actual.channel_order)) &&
        bytes_equal(actual.names, expected.names, sizeof(actual.names));
}
ChatStateApplyResult apply() noexcept {
    return chat_state_apply_gateway_event(&actual.state, event, {payload, used}, {});
}
bool success_case(u32 count, u32 sequence, bool emojis, bool resident = false, bool high = false) noexcept {
    if (!build(count, sequence, true, emojis, high) || !reset(&actual, resident) || !expected_model(resident)) return false;
    copy_bytes(payload_snapshot, payload, used);
    const u32 selection_revision = actual.state.selection_revision;
    const auto result = apply();
    return result.ok() && result.guilds == 1 && result.roles == 2 && result.channels == count &&
        result.emojis == (emojis ? 2u : 0u) && result.overwrites == 0 &&
        result.selection_changed == (actual.state.selection_revision != selection_revision) && same_model() &&
        bytes_equal(payload_snapshot, payload, used);
}
ChatStateApplyResult last_rejection{};
bool rejection(ChatStateApplyError error = ChatStateApplyError::invalid_payload) noexcept {
    copy_bytes(&snapshot, &actual, sizeof(actual));
    copy_bytes(payload_snapshot, payload, used);
    const auto result = apply();
    last_rejection = result;
    return result.error == error && !result.guilds && !result.channels && !result.roles && !result.overwrites &&
        !result.emojis && !result.selection_changed && bytes_equal(&actual, &snapshot, sizeof(actual)) &&
        bytes_equal(payload_snapshot, payload, used);
}
void replace_channel(u32 index, const GatewayClientStateMutation& value) noexcept {
    copy_bytes(payload + channel_offsets[index], &value, sizeof(value));
}
bool duplicate_case(u32 count, u32 duplicate, u32 prior, u32 sequence = 0) noexcept {
    if (!build(count, sequence, true, true) || !reset(&actual, true)) return false;
    auto value = wire_channel(duplicate); value.id = wire_channel(prior).id;
    replace_channel(duplicate, value);
    return rejection();
}
bool branch_emojis(bool update) noexcept {
    if (!reset(&actual, true)) return false;
    if (update) {
        if (!build(0, 0, false, true)) return false;
        event.dispatch_kind = GatewayClientDispatchKind::guild_update;
        // GUILD_UPDATE roles and emoji payload omit the GUILD_CREATE self-role tuple.
        const u32 tuple = 5 + 2 * (sizeof(GatewayClientRoleMutation) + 1);
        copy_bytes(payload + tuple, payload + tuple + 8, used - tuple - 8);
        used -= 8;
        event.state.member_role_count = 0;
        event.state.member_role_flags = 0;
        event.dispatch_payload_bytes = used;
    } else {
        event = {}; used = 0;
        event.kind = GatewayClientEventKind::dispatch; event.dispatch_kind = GatewayClientDispatchKind::guild_emojis_update;
        event.state.guild_id = kGuild; event.state.emojis_known = true;
        event.state.emoji_count = 2; event.state.emoji_role_count = 2;
        if (!append_emojis()) return false;
        event.dispatch_payload_bytes = used;
    }
    const auto result = apply();
    const auto* emoji = chat_state_store_find_emoji(actual.state, 901);
    return result.ok() && result.emojis == 2 && emoji && emoji->guild_id == kGuild &&
        emoji->role_count == 1 && actual.state.emoji_roles[emoji->role_offset] == kRole &&
        bytes_equal(emoji->name, "waves", 5) &&
        (!update || (result.roles == 2 && chat_state_store_find_role(actual.state, kGuild, kRole)));
}
}

void test_guild_channel_uniqueness(void (*check)(bool, const char*) noexcept) noexcept {
    const u32 counts[] = {0, 1, 64, kIdPrefix - 1, kIdPrefix, kIdPrefix + 1, kRows};
    for (u32 count : counts) for (u32 sequence = 0; sequence < 3; ++sequence)
        check(success_case(count, sequence, true), "guild uniqueness accepts exact/scratch-exceeding bounds and preserves all packet-order output");
    check(success_case(73, 2, true, true), "guild uniqueness same-guild replacement preserves retired-slot generations and original mutation order");
    check(success_case(64, 1, false, false, true), "guild uniqueness compares full-width maximum-range u64 IDs");
    check(duplicate_case(64, 1, 0) && duplicate_case(64, 32, 0) && duplicate_case(64, 63, 0),
        "guild uniqueness rejects adjacent/middle/late retained-prefix duplicates atomically");
    check(duplicate_case(kIdPrefix, kIdPrefix - 1, 0, 1), "guild uniqueness rejects duplicate at exact retained-prefix boundary");
    check(duplicate_case(kRows, kIdPrefix, 0) && duplicate_case(kRows, kIdPrefix + 1, kIdPrefix),
        "guild uniqueness fallback rejects prefix-to-overflow and overflow-to-overflow collisions atomically");
    check(duplicate_case(kRows, kIdPrefix - 1, 0), "guild uniqueness finds retained-prefix duplicate even with overflow rows following");
    check(branch_emojis(false) && branch_emojis(true), "guild uniqueness all emoji member lifetimes retain nonempty names and restricted roles");

    bool valid = build(64, 2, true, true) && reset(&actual, true);
    if (valid) { auto value = wire_channel(63); value.guild_id = 88; replace_channel(63, value); valid = rejection(); }
    check(valid, "guild uniqueness malformed late row preserves complete prior model");
    valid = build(64, 2, true, true) && reset(&actual, true);
    if (valid) { auto value = wire_channel(20); value.fields &= static_cast<u16>(~gateway_client_state_field_type); replace_channel(20, value); valid = rejection(); }
    check(valid, "guild uniqueness missing typed field rejects without scratch publication");
    valid = build(64, 2, true, true) && reset(&actual, true);
    if (valid) { auto value = wire_channel(20); value.id = 0; replace_channel(20, value); valid = rejection(); }
    check(valid, "guild uniqueness zero ID is rejected before entering the transient ID prefix");
    valid = build(64, 2, true, true) && reset(&actual, true);
    if (valid) {
        auto value = wire_channel(20); value.overwrite_count = 1; replace_channel(20, value);
        const u32 offset = channel_offsets[20] + sizeof(value) + value.name_bytes;
        GatewayClientPermissionOverwriteMutation overwrite{}; overwrite.id = kGuild; overwrite.type = 2;
        copy_bytes(payload + offset + sizeof(overwrite), payload + offset, used - offset);
        copy_bytes(payload + offset, &overwrite, sizeof(overwrite));
        used += sizeof(overwrite); event.state.overwrite_count = 1; event.dispatch_payload_bytes = used;
        valid = rejection();
    }
    check(valid, "guild uniqueness invalid typed overwrite is rejected before an ID scratch write");
    valid = build(64, 2, true, true) && reset(&actual, true);
    if (valid) { event.state.overwrite_count = 1; valid = rejection(); }
    check(valid, "guild uniqueness overwrite total mismatch remains atomic before emoji/model adoption");
    valid = build(64, 2, true, true) && reset(&actual, true);
    if (valid) { --used; event.dispatch_payload_bytes = used; valid = rejection(); }
    check(valid, "guild uniqueness truncated emoji payload after sort remains atomic");
    valid = build(64, 2, true, true) && reset(&actual, true);
    if (valid) { event.state.record_count = ~u32{0}; valid = rejection(); }
    check(valid, "guild uniqueness exaggerated public record_count rejects without an out-of-bounds ID write");
    valid = build(64, 2, true, true) && reset(&actual, true);
    if (valid) { auto value = wire_channel(20); value.id = 5000; replace_channel(20, value); valid = rejection(); }
    check(valid, "guild uniqueness resident cross-guild collision still rejects atomically");
    valid = build(64, 2, true, true) && reset(&actual, true);
    if (valid) { actual.state.channel_capacity = 32; valid = rejection(ChatStateApplyError::capacity); }
    check(valid && last_rejection.store_error == ChatStateStoreError::channel_capacity,
          "guild uniqueness capacity failure after sorting preserves complete prior model and names the channel table");
}
namespace {
u32 uniqueness_failures;
void standalone_check(bool value, const char* label) noexcept {
    test_check(&uniqueness_failures, value, label);
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_guild_channel_uniqueness_entry() noexcept {
    maicord::test_guild_channel_uniqueness(maicord::standalone_check);
    ExitProcess(maicord::uniqueness_failures ? 1 : 0);
}
