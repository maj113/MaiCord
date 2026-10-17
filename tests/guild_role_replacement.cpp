#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/permission_bits.hpp"
#include "chat_state_record_equal.hpp"

namespace maicord {
namespace {
// Small fixed banks reproduce a full production bank without duplicating 8192 rows.
struct ReplacementFixture {
    ChatStateStore state;
    GuildRecord guilds[3];
    ChannelRecord channels[2];
    RoleRecord roles[8];
    PermissionOverwriteRecord overwrites[2];
    CustomEmojiRecord emojis[2];
    // Global storage deliberately fits one over-limit per-emoji restriction set.
    u64 emoji_roles[kGuildRoleMaximum + 1];
    u16 guild_order[3], channel_order[2];
    u8 names[1024];
};
ReplacementFixture fixture{}, before{};
struct ReplacementPacket {
    GatewayClientEvent event;
    u8 bytes[4096];
    u32 used;
};
ReplacementPacket packet{};
constexpr u64 base_permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
constexpr u64 old_permissions = kPermissionSendMessages;
constexpr u64 new_permissions = kPermissionSendMessagesInThreads | kPermissionAttachFiles;

bool emoji_equal(const CustomEmojiRecord& a, const CustomEmojiRecord& b) noexcept {
    return a.id == b.id && a.guild_id == b.guild_id && a.role_offset == b.role_offset &&
        a.role_count == b.role_count && a.name_bytes == b.name_bytes && a.flags == b.flags &&
        bytes_equal(a.name, b.name, sizeof(a.name));
}
bool semantic_unchanged() noexcept {
    const auto& a = fixture.state;
    const auto& b = before.state;
    // Explicit fields deliberately exclude padding and the rejection diagnostic.
#define SAME(field) if (a.field != b.field) return false
    SAME(read_state); SAME(private_channels); SAME(guilds); SAME(channels); SAME(roles); SAME(overwrites);
    SAME(emojis); SAME(emoji_roles); SAME(guild_order); SAME(channel_order); SAME(names);
    SAME(guild_capacity); SAME(channel_capacity); SAME(role_capacity); SAME(overwrite_capacity);
    SAME(emoji_capacity); SAME(emoji_role_capacity); SAME(guild_count); SAME(channel_count);
    SAME(role_count); SAME(overwrite_count); SAME(emoji_count); SAME(emoji_role_count);
    SAME(name_bank_bytes); SAME(name_used); SAME(name_wasted); SAME(guild_mutations); SAME(channel_mutations);
    SAME(permission_mutations); SAME(activity_mutations); SAME(emoji_mutations); SAME(compactions);
    SAME(selected_guild_id); SAME(selected_channel_id); SAME(self_user_id); SAME(self_premium_type);
    SAME(last_private_channel_id); SAME(restore_channel_id); SAME(restore_guild_id); SAME(restore_deadline_ms);
    SAME(selection_revision); SAME(navigation_intent_revision); SAME(active_name_bank); SAME(home_selected);
#undef SAME
    if (a.upload_policy.floor_bytes != b.upload_policy.floor_bytes ||
        a.upload_policy.nitro_bytes != b.upload_policy.nitro_bytes ||
        a.upload_policy.guild_250_disabled != b.upload_policy.guild_250_disabled) return false;
    for (u32 i = 0; i < 3; ++i)
        if (!guild_equal(fixture.guilds[i], before.guilds[i]) || fixture.guild_order[i] != before.guild_order[i]) return false;
    for (u32 i = 0; i < 2; ++i) {
        if (!channel_equal(fixture.channels[i], before.channels[i]) ||
            fixture.channel_order[i] != before.channel_order[i] ||
            !emoji_equal(fixture.emojis[i], before.emojis[i])) return false;
        const auto& x = fixture.overwrites[i]; const auto& y = before.overwrites[i];
        if (x.id != y.id || x.allow != y.allow || x.deny != y.deny || x.type != y.type) return false;
    }
    for (u32 i = 0; i < 8; ++i) if (!role_equal(fixture.roles[i], before.roles[i])) return false;
    for (u32 i = 0; i < kGuildRoleMaximum + 1; ++i)
        if (fixture.emoji_roles[i] != before.emoji_roles[i]) return false;
    return bytes_equal(fixture.names, before.names, a.name_bank_bytes * 2);
}

bool seed(u32 bank = 512, bool bind_roles = true) noexcept {
    fixture = {};
    auto& s = fixture.state;
    if (!chat_state_store_initialize(&s, {fixture.guilds, 3}, {fixture.guild_order, 3},
        {fixture.channels, 2}, {fixture.channel_order, 2}, {fixture.names, bank * 2}) ||
        (bind_roles && !chat_state_store_bind_permissions(&s, {fixture.roles, 8}, {fixture.overwrites, 2})) ||
        !chat_state_store_bind_emojis_sparse(&s, {fixture.emojis, 2},
            {fixture.emoji_roles, kGuildRoleMaximum + 1})) return false;
    const u64 guild_ids[] = {10, 20, 30};
    for (const u64 id : guild_ids) {
        if (!chat_state_store_upsert_guild(&s, {id, utf8_literal("G"), GuildMutationKind::create, 999}).ok()) return false;
    }
    if (!chat_state_store_upsert_channel(&s, {200, 20, 0, utf8_literal("P"), 0, 0, 0}).ok() ||
        !chat_state_store_upsert_channel(&s, {201, 20, 200, utf8_literal("T"), 1, 0, 11}).ok()) return false;
    const RoleInput rows[] = {
        {31, 30, 7, utf8_literal("U"), 3, 91, true, false},
        {23, 20, 0, utf8_literal("obsolete"), 5, 13},
        {11, 10, 6, utf8_literal("L"), 2, 81, false, true},
        {21, 20, old_permissions, utf8_literal("Keep"), 1, 12, true, false},
        {30, 30, 5, utf8_literal("U"), 0, 90},
        {20, 20, base_permissions, utf8_literal("E")},
        {22, 20, 0, utf8_literal("Same"), 2, 14, false, true},
        {10, 10, 4, utf8_literal("L"), 0, 80}};
    for (const auto& row : rows) if (!chat_state_store_upsert_role(&s, row)) return false;
    const u64 assigned[] = {21, 22};
    if (!chat_state_store_set_self_roles(&s, 20, 999, 42, {assigned, 2}) ||
        !chat_state_store_append_channel_overwrite(&s, 200, {20, 0, 0, 0})) return false;
    u8 restricted[8]; byte_store_le64(restricted, 21);
    const CustomEmojiInput emoji{500, utf8_literal("resident"), {restricted, 8}, 1, custom_emoji_available, 0};
    if (!chat_state_store_replace_guild_emojis(&s, 20, {&emoji, 1})) return false;
    chat_state_store_recompute_guild_permissions(&s, 20);
    return chat_state_store_select_channel(&s, 201).ok() &&
        (!bind_roles || (s.role_count == 8 && s.name_used == 26 && s.name_wasted == 0 && s.compactions == 0));
}

void begin_packet(Utf8View name = {}) noexcept {
    packet = {};
    packet.event.kind = GatewayClientEventKind::dispatch;
    packet.event.dispatch_kind = GatewayClientDispatchKind::guild_update;
    packet.event.state.id = 20;
    packet.event.state.owner_id = 999;
    packet.event.state.fields = gateway_client_state_field_id | gateway_client_state_field_owner_id;
    packet.event.state.name_bytes = static_cast<u16>(name.bytes);
    if (name.bytes) {
        packet.event.state.fields |= gateway_client_state_field_name;
        copy_bytes(packet.bytes, name.data, name.bytes);
        packet.used = name.bytes;
    }
}
void add_role(u64 id, Utf8View name = {}, u64 permissions = 0) noexcept {
    GatewayClientRoleMutation role{};
    role.id = id; role.permissions = permissions;
    role.position = static_cast<u32>(id); role.color = static_cast<u32>(id * 3);
    role.flags = 3; role.name_bytes = static_cast<u16>(name.bytes);
    copy_bytes(packet.bytes + packet.used, &role, sizeof(role)); packet.used += sizeof(role);
    copy_bytes(packet.bytes + packet.used, name.data, name.bytes); packet.used += name.bytes;
    ++packet.event.state.role_count;
}
void common_packet(Utf8View guild_name = {}) noexcept {
    begin_packet(guild_name);
    add_role(20, {}, base_permissions);
    add_role(21, {}, new_permissions);
    add_role(22, utf8_literal("Same"));
}
void add_emoji(u64 first, u64 second = 0, bool two = false) noexcept {
    GatewayClientEmojiMutation emoji{};
    emoji.id = 501; emoji.name_bytes = 3; emoji.role_count = two ? 2 : 1;
    emoji.flags = gateway_client_emoji_available;
    copy_bytes(packet.bytes + packet.used, &emoji, sizeof(emoji)); packet.used += sizeof(emoji);
    byte_store_le64(packet.bytes + packet.used, first); packet.used += 8;
    if (two) { byte_store_le64(packet.bytes + packet.used, second); packet.used += 8; }
    copy_bytes(packet.bytes + packet.used, "new", 3); packet.used += 3;
    packet.event.state.emojis_known = true;
    packet.event.state.emoji_count = 1;
    packet.event.state.emoji_role_count = emoji.role_count;
}
void add_oversized_emoji() noexcept {
    GatewayClientEmojiMutation emoji{};
    emoji.id = 501; emoji.name_bytes = 3; emoji.role_count = kGuildRoleMaximum + 1;
    emoji.flags = gateway_client_emoji_available;
    copy_bytes(packet.bytes + packet.used, &emoji, sizeof(emoji)); packet.used += sizeof(emoji);
    for (u32 i = 0; i < emoji.role_count; ++i) {
        byte_store_le64(packet.bytes + packet.used, static_cast<u64>(i) + 1); packet.used += 8;
    }
    copy_bytes(packet.bytes + packet.used, "new", 3); packet.used += 3;
    packet.event.state.emojis_known = true;
    packet.event.state.emoji_count = 1;
    packet.event.state.emoji_role_count = emoji.role_count;
}
ChatStateApplyResult apply_packet() noexcept {
    packet.event.dispatch_payload_bytes = packet.used;
    return chat_state_apply_gateway_event(&fixture.state, packet.event, {packet.bytes, packet.used}, {});
}
void reject_unchanged(void (*check)(bool, const char*) noexcept, const char* label) noexcept {
    fixture.state.restore_guild_id = 888;
    fixture.state.restore_channel_id = 889;
    fixture.state.restore_deadline_ms = 1234;
    before = fixture;
    check(!apply_packet().ok() && semantic_unchanged(), label);
}
bool sorted() noexcept {
    for (u32 i = 1; i < fixture.state.role_count; ++i) {
        const auto& a = fixture.roles[i - 1]; const auto& b = fixture.roles[i];
        if (a.guild_id > b.guild_id || (a.guild_id == b.guild_id && a.id >= b.id)) return false;
    }
    return true;
}
bool role_name(u64 id, Utf8View expected) noexcept {
    const auto* row = chat_state_store_find_role(fixture.state, 20, id);
    if (!row) return false;
    const auto name = chat_state_store_role_name(fixture.state, *row);
    return name.bytes == expected.bytes && bytes_equal(name.data, expected.data, name.bytes);
}
bool permission_selection() noexcept {
    const auto* parent = chat_state_store_find_channel(fixture.state, 200);
    const auto* thread = chat_state_store_find_channel(fixture.state, 201);
    return parent && thread && parent->permissions_resolved && thread->permissions_resolved &&
        parent->effective_permissions == (base_permissions | new_permissions) &&
        thread->effective_permissions == parent->effective_permissions &&
        chat_state_store_channel_visible(fixture.state, *parent) &&
        chat_state_store_channel_visible(fixture.state, *thread) &&
        !(parent->actions & channel_action_send_messages) &&
        (thread->actions & channel_action_send_messages) &&
        (thread->actions & channel_action_attach_files) &&
        fixture.state.selected_guild_id == 20 && fixture.state.selected_channel_id == 201;
}
u32 live_names() noexcept {
    u32 live = 0;
    for (u32 i = 0; i < fixture.state.guild_count; ++i) live += fixture.guilds[fixture.guild_order[i]].name_bytes;
    for (u32 i = 0; i < fixture.state.channel_count; ++i) live += fixture.channels[fixture.channel_order[i]].name_bytes;
    for (u32 i = 0; i < fixture.state.role_count; ++i) live += fixture.roles[i].name_bytes;
    return live;
}
bool packet_role_equal(const GatewayClientRoleMutation& a, const GatewayClientRoleMutation& b) noexcept {
    return a.id == b.id && a.permissions == b.permissions && a.position == b.position &&
        a.color == b.color && a.name_bytes == b.name_bytes && a.flags == b.flags &&
        bytes_equal(a.reserved, b.reserved, sizeof(a.reserved));
}
void test_role_offsets(void (*check)(bool, const char*) noexcept) noexcept {
    begin_packet(utf8_literal("Guild"));
    packet.event.state.icon_bytes = 32;
    packet.event.state.fields |= gateway_client_state_field_icon;
    for (u32 i = 0; i < 32; ++i) packet.bytes[packet.used++] = 'a';
    add_role(20, {}, base_permissions);
    add_role(21, utf8_literal("Keep"), new_permissions);
    add_role(22, utf8_literal("\xE7\x8C\xAB"));
    add_role(24, utf8_literal("NineBytes"));
    packet.event.dispatch_payload_bytes = packet.used;
    const ConstSlice<u8> payload{packet.bytes, packet.used};
    u32 offsets[4]{};
    bool agrees = gateway_client_event_guild_role_offsets(packet.event, payload, {offsets, 4});
    u32 expected = 37;
    for (u32 i = 0; agrees && i < 4; ++i) {
        GatewayClientRoleMutation ordinal{}, at{}; Utf8View ordinal_name{}, at_name{};
        agrees = offsets[i] == expected &&
            gateway_client_event_guild_role(packet.event, payload, i, &ordinal, &ordinal_name) &&
            gateway_client_event_guild_role_at(packet.event, payload, offsets[i], &at, &at_name) &&
            packet_role_equal(ordinal, at) && ordinal_name.bytes == at_name.bytes &&
            bytes_equal(ordinal_name.data, at_name.data, ordinal_name.bytes);
        expected += sizeof(GatewayClientRoleMutation) + ordinal.name_bytes;
    }
    check(agrees && expected == packet.used,
        "guild role offsets: update table follows guild/icon prefix and empty ASCII UTF-8 variable names exactly");
    packet.event.dispatch_kind = GatewayClientDispatchKind::guild_create;
    bool create_agrees = gateway_client_event_guild_role_offsets(packet.event, payload, {offsets, 4});
    for (u32 i = 0; create_agrees && i < 4; ++i) {
        GatewayClientRoleMutation ordinal{}, at{}; Utf8View ordinal_name{}, at_name{};
        create_agrees = gateway_client_event_guild_role(packet.event, payload, i, &ordinal, &ordinal_name) &&
            gateway_client_event_guild_role_at(packet.event, payload, offsets[i], &at, &at_name) &&
            packet_role_equal(ordinal, at) && ordinal_name.bytes == at_name.bytes &&
            bytes_equal(ordinal_name.data, at_name.data, ordinal_name.bytes);
    }
    check(create_agrees, "guild role offsets: create packet uses the same immutable role-prefix reader");
    packet.event.dispatch_kind = GatewayClientDispatchKind::guild_update;
    check(!gateway_client_event_guild_role_offsets(packet.event, payload, {offsets, 3}) &&
        !gateway_client_event_guild_role_offsets(packet.event, payload, {nullptr, 4}),
        "guild role offsets: insufficient or null transient output table rejects");
    GatewayClientRoleMutation role{}; Utf8View name{};
    check(!gateway_client_event_guild_role_at(packet.event, payload, 36, &role, &name) &&
        !gateway_client_event_guild_role_at(packet.event, payload, packet.used, &role, &name) &&
        !gateway_client_event_guild_role_at(packet.event, payload, ~0u, &role, &name),
        "guild role offsets: direct reader rejects prefix and out-of-payload offsets");

    // Malformed scans may leave a partial scratch prefix; never consume it.
    const u32 last_offset = 37 + 3 * sizeof(GatewayClientRoleMutation) + 4 + 3;
    packet.event.dispatch_payload_bytes = packet.used - 1;
    check(!gateway_client_event_guild_role_offsets(packet.event, {packet.bytes, packet.used - 1}, {offsets, 4}) &&
        !gateway_client_event_guild_role(packet.event, {packet.bytes, packet.used - 1}, 3, &role, &name),
        "guild role offsets: truncated final variable name rejects in table and ordinal readers");
    const u32 short_descriptor = last_offset + sizeof(GatewayClientRoleMutation) - 1;
    packet.event.dispatch_payload_bytes = short_descriptor;
    check(!gateway_client_event_guild_role_offsets(packet.event, {packet.bytes, short_descriptor}, {offsets, 4}) &&
        !gateway_client_event_guild_role(packet.event, {packet.bytes, short_descriptor}, 3, &role, &name),
        "guild role offsets: truncated final descriptor rejects without assuming scratch rollback");
    packet.event.dispatch_payload_bytes = packet.used;
    byte_store_le64(packet.bytes + last_offset, 0);
    check(!gateway_client_event_guild_role_offsets(packet.event, payload, {offsets, 4}) &&
        !gateway_client_event_guild_role(packet.event, payload, 3, &role, &name),
        "guild role offsets: malformed zero-ID final entry rejects in table and ordinal readers");
    byte_store_le64(packet.bytes + last_offset, 24);

    u8 saved_payload[256]; copy_bytes(saved_payload, packet.bytes, packet.used);
    check(!gateway_client_event_guild_role_offsets(packet.event, payload,
            {reinterpret_cast<u32*>(packet.bytes), 4}) && bytes_equal(saved_payload, packet.bytes, packet.used),
        "guild role offsets: used output alias with immutable payload rejects before changing source bytes");
    check(!gateway_client_event_guild_role_offsets(packet.event, payload,
            {reinterpret_cast<u32*>(&packet.event), 4}) &&
        packet.event.kind == GatewayClientEventKind::dispatch &&
        packet.event.dispatch_kind == GatewayClientDispatchKind::guild_update &&
        packet.event.dispatch_payload_bytes == packet.used && packet.event.state.id == 20 &&
        packet.event.state.role_count == 4 && packet.event.state.name_bytes == 5 && packet.event.state.icon_bytes == 32,
        "guild role offsets: used output alias with immutable event rejects before changing routing or counts");
}
} // namespace

void test_guild_role_replacement(void (*check)(bool, const char*) noexcept) noexcept {
    check(seed(), "guild replacement: eight-row mixed-guild fixed fixture initializes");
    if (fixture.state.role_count != 8) return;
    before = fixture;
    const u32 selection_revision = fixture.state.selection_revision;
    common_packet(); add_role(24, utf8_literal("New"));
    packet.event.dispatch_payload_bytes = packet.used;
    u32 old_target = 0, common_count = 0;
    for (u32 i = 0; i < fixture.state.role_count; ++i) old_target += fixture.roles[i].guild_id == 20 ? 1u : 0u;
    for (u32 i = 0; i < packet.event.state.role_count; ++i) {
        GatewayClientRoleMutation role{};
        if (gateway_client_event_guild_role(packet.event, {packet.bytes, packet.used}, i, &role) &&
            chat_state_store_find_role(fixture.state, 20, role.id)) ++common_count;
    }
    const u32 resident = fixture.state.role_count, incoming = packet.event.state.role_count;
    const u32 final_count = resident - old_target + incoming;
    check(resident == 8 && old_target == 4 && incoming == 4 && common_count == 3 &&
        resident + incoming - common_count == 9 && maximum(resident, final_count) == 8 &&
        fixture.state.role_capacity == 8 && fixture.state.name_bank_bytes == 512 &&
        fixture.state.name_used == 26 && fixture.state.name_wasted == 0 && live_names() == 26,
        "guild replacement occupancy: R8 O4 N4 C3 old-peak9 corrected-peak8 cap8 names-used26 wasted0 live26 bank512 compactions0");
    const auto result = apply_packet();
    check(result.ok() && result.guilds == 1 && result.roles == 4 && !result.selection_changed && sorted() &&
        fixture.state.role_count == 8 && !chat_state_store_find_role(fixture.state, 20, 23) &&
        chat_state_store_find_role(fixture.state, 20, 24),
        "guild replacement: full bank admits final-fitting new IDs after removing absent roles");
    const auto* common = chat_state_store_find_role(fixture.state, 20, 21);
    const auto* identical = chat_state_store_find_role(fixture.state, 20, 22);
    const auto* inserted = chat_state_store_find_role(fixture.state, 20, 24);
    check(common && identical && inserted && common->self_assigned && identical->self_assigned &&
        common->permissions == new_permissions && common->position == 21 && common->color == 63 &&
        common->hoist && common->managed && !inserted->self_assigned &&
        role_name(21, utf8_literal("Keep")) &&
        role_name(22, utf8_literal("Same")) &&
        role_equal(fixture.roles[0], before.roles[0]) && role_equal(fixture.roles[1], before.roles[1]) &&
        role_equal(fixture.roles[6], before.roles[6]) && role_equal(fixture.roles[7], before.roles[7]),
        "guild replacement: common assignment empty/identical names and unrelated sorted rows survive");
    check(permission_selection() && fixture.state.selection_revision == selection_revision &&
        fixture.state.overwrite_count == before.state.overwrite_count &&
        emoji_equal(fixture.emojis[0], before.emojis[0]) && fixture.emoji_roles[0] == 21 &&
        fixture.state.guild_mutations == before.state.guild_mutations + 1 &&
        fixture.state.permission_mutations == before.state.permission_mutations + 1 &&
        fixture.state.channel_mutations == before.state.channel_mutations + 1 &&
        fixture.state.emoji_mutations == before.state.emoji_mutations &&
        fixture.state.activity_mutations == before.state.activity_mutations,
        "guild replacement: inherited thread actions selection overwrites and omitted emoji data survive");

    check(seed(), "guild replacement: shrinking fixture initializes");
    common_packet();
    check(apply_packet().ok() && fixture.state.role_count == 7 && sorted() &&
        role_equal(fixture.roles[7], RoleRecord{}) && permission_selection(),
        "guild replacement: shrinking replacement zeroes former live tail and recomputes actions");

    check(seed(), "guild replacement: overflow fixture initializes");
    common_packet(utf8_literal("Changed")); add_role(24); add_role(25);
    reject_unchanged(check, "guild replacement: final-count overflow leaves all semantic records banks counters and restore metadata unchanged");
    check(seed(), "guild replacement: malformed fixture initializes");
    common_packet(utf8_literal("Changed")); add_role(0);
    reject_unchanged(check, "guild replacement: malformed final role rejects before publication");
    check(seed(), "guild replacement: oversized role fixture initializes");
    char long_name[401]; for (u32 i = 0; i < sizeof(long_name); ++i) long_name[i] = 'x';
    common_packet(utf8_literal("Changed")); add_role(23, {long_name, sizeof(long_name)});
    reject_unchanged(check, "guild replacement: typed 401-byte final role rejects without partial guild common-role or name writes");
    check(seed(), "guild replacement: duplicate fixture initializes");
    common_packet(utf8_literal("Changed")); add_role(21, utf8_literal("Duplicate"));
    reject_unchanged(check, "guild replacement: duplicate role IDs reject without changing count arithmetic or records");

    check(seed(), "guild replacement: oversized guild fixture initializes");
    common_packet({long_name, sizeof(long_name)}); add_role(24);
    reject_unchanged(check, "guild replacement: typed 401-byte guild name rejects before absent-role removal");
    check(seed(), "guild replacement: invalid icon fixture initializes");
    begin_packet(); packet.event.state.icon_bytes = 1;
    packet.event.state.fields |= gateway_client_state_field_icon;
    packet.bytes[packet.used++] = 'z';
    add_role(20, {}, base_permissions); add_role(21, {}, new_permissions);
    add_role(22, utf8_literal("Same")); add_role(24);
    reject_unchanged(check, "guild replacement: typed invalid guild icon rejects before any guild or role mutation");

    check(seed(), "guild replacement: emoji zero-role fixture initializes");
    common_packet(utf8_literal("Changed")); add_role(23); add_emoji(0);
    reject_unchanged(check, "guild replacement: zero emoji restriction rejects before guild role permission and emoji changes");
    check(seed(), "guild replacement: emoji duplicate-role fixture initializes");
    common_packet(utf8_literal("Changed")); add_role(23); add_emoji(21, 21, true);
    reject_unchanged(check, "guild replacement: duplicate emoji restriction rejects before any semantic mutation");
    check(seed(), "guild replacement: oversized emoji restriction fixture initializes");
    common_packet(utf8_literal("Changed")); add_role(23); add_oversized_emoji();
    check(fixture.state.emoji_role_capacity == kGuildRoleMaximum + 1,
        "guild replacement: global emoji restriction bank fits 257 IDs so per-emoji admission is isolated");
    reject_unchanged(check, "guild replacement: 257 distinct nonzero emoji restrictions reject before any semantic mutation");
    check(seed(), "guild replacement: valid emoji fixture initializes");
    common_packet(); add_role(24); add_emoji(21);
    check(apply_packet().ok() && fixture.state.emoji_count == 1 && fixture.state.emoji_role_count == 1 &&
        !chat_state_store_find_emoji(fixture.state, 500) &&
        chat_state_store_find_emoji(fixture.state, 501) && fixture.emoji_roles[0] == 21,
        "guild replacement: valid admitted emoji replacement follows role replacement");

    check(seed(32), "guild replacement: guild-first transient fixture uses 26 live bytes of 32");
    common_packet(utf8_literal("RenamedG")); add_role(24);
    reject_unchanged(check, "guild replacement: guild rename retains old name during allocation even when final names fit");
    check(seed(32), "guild replacement: later transient fixture initializes");
    begin_packet(); add_role(20, {}, base_permissions); add_role(22, utf8_literal("Same"));
    add_role(24, utf8_literal("NewSix")); add_role(21, utf8_literal("NineBytes"), new_permissions);
    reject_unchanged(check, "guild replacement: later common rename rejects before earlier allocation removal or compaction");

    check(seed(32), "guild replacement: first packet-order compaction fixture initializes");
    begin_packet(); add_role(20, {}, base_permissions); add_role(22, utf8_literal("Same"));
    add_role(24, utf8_literal("NineBytes")); add_role(21, utf8_literal("K"), new_permissions);
    check(apply_packet().ok() && sorted() && fixture.state.active_name_bank == 1 &&
        fixture.state.compactions == 1 && fixture.state.name_used == 28 && fixture.state.name_wasted == 4 &&
        live_names() == 24 && role_name(21, utf8_literal("K")) && role_name(24, utf8_literal("NineBytes")) &&
        chat_state_store_find_role(fixture.state, 20, 24)->name_offset == 50 &&
        chat_state_store_find_role(fixture.state, 20, 21)->name_offset == 59 && permission_selection(),
        "guild replacement: absent-name removal enables exact guild channel role compaction then packet-order allocation");
    check(seed(32), "guild replacement: reversed packet-order compaction fixture initializes");
    begin_packet(); add_role(20, {}, base_permissions); add_role(22, utf8_literal("Same"));
    add_role(21, utf8_literal("K"), new_permissions); add_role(24, utf8_literal("NineBytes"));
    check(apply_packet().ok() && sorted() && fixture.state.active_name_bank == 1 &&
        fixture.state.compactions == 1 && fixture.state.name_used == 24 && fixture.state.name_wasted == 0 &&
        live_names() == 24 && role_name(21, utf8_literal("K")) && role_name(24, utf8_literal("NineBytes")) &&
        chat_state_store_find_role(fixture.state, 20, 21)->name_offset == 40 &&
        chat_state_store_find_role(fixture.state, 20, 24)->name_offset == 47 && permission_selection(),
        "guild replacement: reordering allocations changes compaction offsets and wasted bytes with identical final names");

    for (u32 bank_index = 0; bank_index < 2; ++bank_index) {
        check(seed(), "guild replacement: borrowed name-bank payload fixture initializes");
        common_packet(utf8_literal("Changed")); add_role(24, utf8_literal("New"));
        const u32 offset = bank_index == 0 ? 256 : fixture.state.name_bank_bytes;
        copy_bytes(fixture.names + offset, packet.bytes, packet.used);
        packet.event.dispatch_payload_bytes = packet.used;
        before = fixture;
        const auto aliased = chat_state_apply_gateway_event(&fixture.state, packet.event,
            {fixture.names + offset, packet.used}, {});
        check(!aliased.ok() && semantic_unchanged(), bank_index == 0
            ? "guild replacement: active name-bank payload alias rejects before overwriting borrowed packet bytes"
            : "guild replacement: inactive name-bank payload alias rejects before a future compaction can overwrite it");
    }

    check(seed(), "guild replacement: route fixture initializes");
    u32 structurals[256]; char routed_bytes[1024]; GatewayEnvelope envelope{};
    GatewayClientEvent routed{}; routed.kind = GatewayClientEventKind::dispatch;
    ConstSlice<u8> payload{}; bool handled = false;
    const auto wire = utf8_literal(R"({"op":0,"t":"GUILD_UPDATE","s":1,"d":{"id":"20","name":"G","owner_id":"999","roles":[{"id":"20","name":"","permissions":"66560"},{"id":"21","name":"","permissions":"274877939712"},{"id":"22","name":"Same","permissions":"0"},{"id":"24","name":"New","permissions":"0"}]}})");
    const bool route_ok = gateway_decode_envelope(wire, {structurals, 256}, 32, &envelope).ok() &&
        gateway_client_route_state_dispatch(envelope, {structurals, 256}, 32, {routed_bytes, sizeof(routed_bytes)},
            1, &routed, &payload, &handled).ok() && handled;
    routed.dispatch_payload_bytes = payload.count;
    check(route_ok && chat_state_apply_gateway_event(&fixture.state, routed, payload, {}).ok() &&
        fixture.state.role_count == 8 && sorted() && permission_selection(),
        "guild replacement: actual JSON route and typed dispatcher share final-fitting replacement behavior");

    check(seed(512, false), "guild replacement: legacy unbound-role fixture initializes");
    common_packet(utf8_literal("Legacy")); add_role(24, utf8_literal("Ignored"));
    check(apply_packet().ok() && fixture.state.roles == nullptr && fixture.state.role_count == 0 &&
        fixture.state.name_used == 11 && chat_state_store_find_guild(fixture.state, 20) &&
        chat_state_store_channel_visible(fixture.state, *chat_state_store_find_channel(fixture.state, 201)),
        "guild replacement: legacy unbound roles remain accepted no-ops without allocating ignored role names");
    test_role_offsets(check);
}
} // namespace maicord
