#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/core/memory.hpp"
#include "chat_state_record_equal.hpp"

namespace maicord {
namespace {
constexpr u32 kPrefixRoles = 8, kPrefixOverwrites = 8;
constexpr u64 kPrefixBase = kPermissionViewChannel | kPermissionReadMessageHistory;
struct PrefixReserve {
    u32 calls, role_growths, overwrite_growths;
    u32 physical_roles, physical_overwrites, requested_roles, requested_overwrites;
    u8 fail_stage;
    bool watch, observed_before_mutation;
};
struct PrefixFixture {
    ChatStateStore state;
    GuildRecord guilds[4];
    ChannelRecord channels[5];
    RoleRecord roles[kPrefixRoles];
    PermissionOverwriteRecord overwrites[kPrefixOverwrites];
    u16 guild_order[4], channel_order[5];
    u8 names[2048];
    PrefixReserve reserve;
};
PrefixFixture prefix{}, prefix_before{};
u32 prefix_structurals[1024];
char prefix_payload[4096];

RoleRecord role_sentinel(u32 index) noexcept {
    RoleRecord value{};
    value.id = 0xDEAD0000ull + index; value.guild_id = 777; value.permissions = ~0ull;
    value.name_offset = 991; value.name_bytes = 17; value.position = 313; value.color = 0xABCDEF;
    value.self_assigned = value.hoist = value.managed = true; value.reserved = 3;
    return value;
}
PermissionOverwriteRecord overwrite_sentinel(u32 index) noexcept {
    return {0xBEEF0000ull + index, ~0ull, 0x123456789ABCDEF0ull, 1};
}
bool overwrite_equal(const PermissionOverwriteRecord& a, const PermissionOverwriteRecord& b) noexcept {
    return a.id == b.id && a.allow == b.allow && a.deny == b.deny && a.type == b.type;
}
bool semantic_unchanged(bool entire_banks = false) noexcept {
    const auto& a = prefix.state; const auto& b = prefix_before.state;
#define SAME(field) if (a.field != b.field) return false
    SAME(read_state); SAME(private_channels); SAME(guilds); SAME(channels); SAME(roles); SAME(overwrites);
    SAME(emojis); SAME(emoji_roles); SAME(guild_order); SAME(channel_order); SAME(names);
    SAME(guild_capacity); SAME(channel_capacity); SAME(role_capacity); SAME(overwrite_capacity);
    SAME(emoji_capacity); SAME(emoji_role_capacity); SAME(guild_count); SAME(channel_count);
    SAME(role_count); SAME(overwrite_count); SAME(emoji_count); SAME(emoji_role_count);
    SAME(permission_reserve); SAME(permission_reserve_context); SAME(role_backed_count); SAME(overwrite_backed_count);
    SAME(name_bank_bytes); SAME(name_used); SAME(name_wasted); SAME(guild_mutations); SAME(channel_mutations);
    SAME(permission_mutations); SAME(activity_mutations); SAME(emoji_mutations); SAME(compactions);
    SAME(selected_guild_id); SAME(selected_channel_id); SAME(self_user_id); SAME(self_premium_type);
    SAME(last_private_channel_id); SAME(restore_channel_id); SAME(restore_guild_id); SAME(restore_deadline_ms);
    SAME(selection_revision); SAME(navigation_intent_revision); SAME(active_name_bank); SAME(home_selected);
#undef SAME
    if (a.upload_policy.floor_bytes != b.upload_policy.floor_bytes || a.upload_policy.nitro_bytes != b.upload_policy.nitro_bytes ||
        a.upload_policy.guild_250_disabled != b.upload_policy.guild_250_disabled) return false;
    for (u32 i = 0; i < 4; ++i)
        if (!guild_equal(prefix.guilds[i], prefix_before.guilds[i]) || prefix.guild_order[i] != prefix_before.guild_order[i]) return false;
    for (u32 i = 0; i < 5; ++i)
        if (!channel_equal(prefix.channels[i], prefix_before.channels[i]) || prefix.channel_order[i] != prefix_before.channel_order[i]) return false;
    // A failed second physical commit can zero a formerly unbacked suffix.
    // Only published old backing is part of the semantic transaction snapshot.
    const u32 roles = entire_banks ? kPrefixRoles : b.role_backed_count;
    const u32 overwrites = entire_banks ? kPrefixOverwrites : b.overwrite_backed_count;
    for (u32 i = 0; i < roles; ++i) if (!role_equal(prefix.roles[i], prefix_before.roles[i])) return false;
    for (u32 i = 0; i < overwrites; ++i) if (!overwrite_equal(prefix.overwrites[i], prefix_before.overwrites[i])) return false;
    return bytes_equal(prefix.names, prefix_before.names, sizeof(prefix.names));
}
bool suffix_untouched() noexcept {
    for (u32 i = prefix.reserve.physical_roles; i < kPrefixRoles; ++i)
        if (!role_equal(prefix.roles[i], role_sentinel(i))) return false;
    for (u32 i = prefix.reserve.physical_overwrites; i < kPrefixOverwrites; ++i)
        if (!overwrite_equal(prefix.overwrites[i], overwrite_sentinel(i))) return false;
    return true;
}
bool reserve_prefix(void* context, u32 roles, u32 overwrites) noexcept {
    if (context != &prefix.reserve || roles > kPrefixRoles || overwrites > kPrefixOverwrites) return false;
    auto& reservation = *static_cast<PrefixReserve*>(context);
    ++reservation.calls; reservation.requested_roles = roles; reservation.requested_overwrites = overwrites;
    if (reservation.watch) reservation.observed_before_mutation = semantic_unchanged();
    if (reservation.fail_stage == 1) return false;
    if (roles > reservation.physical_roles) {
        for (u32 i = reservation.physical_roles; i < roles; ++i) prefix.roles[i] = {};
        reservation.physical_roles = roles; ++reservation.role_growths;
    }
    if (reservation.fail_stage == 2) return false;
    if (overwrites > reservation.physical_overwrites) {
        for (u32 i = reservation.physical_overwrites; i < overwrites; ++i) prefix.overwrites[i] = {};
        reservation.physical_overwrites = overwrites; ++reservation.overwrite_growths;
    }
    return true;
}
bool initialize(bool reserved = true, bool bind = true) noexcept {
    prefix = {};
    for (u32 i = 0; i < kPrefixRoles; ++i) prefix.roles[i] = role_sentinel(i);
    for (u32 i = 0; i < kPrefixOverwrites; ++i) prefix.overwrites[i] = overwrite_sentinel(i);
    if (!chat_state_store_initialize(&prefix.state, {prefix.guilds, 4}, {prefix.guild_order, 4},
        {prefix.channels, 5}, {prefix.channel_order, 5}, {prefix.names, sizeof(prefix.names)})) return false;
    if (!bind) return true;
    if (reserved) return chat_state_store_bind_permissions_reserved(&prefix.state,
        {prefix.roles, kPrefixRoles}, {prefix.overwrites, kPrefixOverwrites}, reserve_prefix, &prefix.reserve);
    const bool bound = chat_state_store_bind_permissions(&prefix.state,
        {prefix.roles, kPrefixRoles}, {prefix.overwrites, kPrefixOverwrites});
    if (bound) { prefix.reserve.physical_roles = kPrefixRoles; prefix.reserve.physical_overwrites = kPrefixOverwrites; }
    return bound;
}
bool seed() noexcept {
    if (!initialize()) return false;
    auto& state = prefix.state;
    if (!chat_state_store_upsert_guild(&state, {10, utf8_literal("Other"), GuildMutationKind::create, 999}).ok() ||
        !chat_state_store_upsert_guild(&state, {20, utf8_literal("Target"), GuildMutationKind::create, 999}).ok()) return false;
    const ChannelInput channels[] = {
        {100, 10, 0, utf8_literal("Other"), 0, 0, 0},
        {200, 20, 0, utf8_literal("Parent"), 0, 0, 0},
        {201, 20, 200, utf8_literal("Thread"), 1, 0, 11},
        {210, 20, 0, utf8_literal("Second"), 2, 0, 0}};
    for (const auto& channel : channels) if (!chat_state_store_upsert_channel(&state, channel).ok()) return false;
    const RoleInput roles[] = {
        {22, 20, 0, utf8_literal("Absent")}, {10, 10, kPrefixBase, utf8_literal("OtherEveryone")},
        {21, 20, kPermissionSendMessages, utf8_literal("Common"), 1, 123, true, false},
        {20, 20, kPrefixBase, utf8_literal("Everyone")}, {11, 10, 0, utf8_literal("OtherRole")}};
    for (const auto& role : roles) if (!chat_state_store_upsert_role(&state, role)) return false;
    const u64 assigned[] = {21};
    if (!chat_state_store_set_self_roles(&state, 20, 999, 42, {assigned, 1}) ||
        !chat_state_store_set_self_roles(&state, 10, 999, 42, {}) ||
        !chat_state_store_append_channel_overwrite(&state, 100, {10, 0, 0, 0}) ||
        !chat_state_store_append_channel_overwrite(&state, 200, {20, 0, 0, 0}) ||
        !chat_state_store_append_channel_overwrite(&state, 210, {21, 0, 0, 0})) return false;
    chat_state_store_recompute_guild_permissions(&state, 10);
    chat_state_store_recompute_guild_permissions(&state, 20);
    return chat_state_store_select_channel(&state, 201).ok() && state.role_count == 5 && state.overwrite_count == 3 &&
        state.role_backed_count == 5 && state.overwrite_backed_count == 3 && suffix_untouched();
}
bool route(Utf8View wire, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    GatewayEnvelope envelope{}; bool handled = false;
    *event = {}; event->kind = GatewayClientEventKind::dispatch;
    const bool valid = gateway_decode_envelope(wire, {prefix_structurals, 1024}, 32, &envelope).ok() &&
        gateway_client_route_state_dispatch(envelope, {prefix_structurals, 1024}, 32,
            {prefix_payload, sizeof(prefix_payload)}, 42, event, payload, &handled).ok() && handled;
    if (valid) event->dispatch_payload_bytes = payload->count;
    return valid;
}
ChatStateApplyResult apply(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    return chat_state_apply_gateway_event(&prefix.state, event, payload, {});
}
void watch() noexcept {
    prefix.state.restore_channel_id = 9991; prefix.state.restore_guild_id = 9992; prefix.state.restore_deadline_ms = 9993;
    prefix_before = prefix; prefix.reserve.watch = true; prefix.reserve.observed_before_mutation = false;
}
bool roles_sorted() noexcept {
    for (u32 i = 1; i < prefix.state.role_count; ++i) {
        const auto& a = prefix.roles[i - 1]; const auto& b = prefix.roles[i];
        if (a.guild_id > b.guild_id || (a.guild_id == b.guild_id && a.id >= b.id)) return false;
    }
    return true;
}
const Utf8View kCreate = utf8_literal(R"({"op":0,"t":"GUILD_CREATE","d":{"id":"20","name":"Replacement","owner_id":"999","roles":[{"id":"20","name":"Everyone","permissions":"66560"},{"id":"21","name":"Common","permissions":"274877906944"},{"id":"23","name":"NewOne","permissions":"0"},{"id":"24","name":"NewTwo","permissions":"0"}],"members":[{"user":{"id":"42"},"roles":["21"]}],"channels":[{"id":"200","type":0,"name":"Parent","permission_overwrites":[{"id":"20","type":0,"allow":"0","deny":"0"},{"id":"21","type":0,"allow":"0","deny":"0"}]},{"id":"210","type":0,"name":"Second","permission_overwrites":[{"id":"21","type":0,"allow":"0","deny":"0"}]}],"threads":[{"id":"201","type":11,"parent_id":"200","name":"Thread"}]}})");
const Utf8View kUpdate = utf8_literal(R"({"op":0,"t":"GUILD_UPDATE","d":{"id":"20","name":"Target","owner_id":"999","roles":[{"id":"20","name":"Everyone","permissions":"66560"},{"id":"21","name":"","permissions":"274877906944"},{"id":"23","name":"NewOne","permissions":"0"},{"id":"24","name":"NewTwo","permissions":"0"}]}})");
const Utf8View kChannelUpdate = utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"200","guild_id":"20","type":0,"name":"RenamedParent","permission_overwrites":[{"id":"20","type":0,"allow":"0","deny":"0"},{"id":"21","type":0,"allow":"0","deny":"0"},{"id":"42","type":1,"allow":"0","deny":"0"}]}})");
} // namespace

void test_permission_prefix(void (*check)(bool, const char*) noexcept) noexcept {
    auto& state = prefix.state;
    check(initialize(true, false), "permission prefix: unbound fixture initializes with suffix sentinels");
    prefix_before = prefix;
    check(!chat_state_store_bind_permissions_reserved(&state, {nullptr, 8}, {prefix.overwrites, 8}, reserve_prefix, &prefix.reserve) &&
        !chat_state_store_bind_permissions_reserved(&state, {prefix.roles, 8}, {prefix.overwrites, 0}, reserve_prefix, &prefix.reserve) &&
        !chat_state_store_bind_permissions_reserved(&state, {prefix.roles, 65536}, {prefix.overwrites, 8}, reserve_prefix, &prefix.reserve) &&
        !chat_state_store_bind_permissions_reserved(&state, {prefix.roles, 8}, {prefix.overwrites, 8}, nullptr, &prefix.reserve) &&
        semantic_unchanged(true) && prefix.reserve.calls == 0,
        "permission prefix: invalid reserved bindings publish neither bank callback metadata nor writes");
    check(chat_state_store_bind_permissions_reserved(&state, {prefix.roles, 8}, {prefix.overwrites, 8}, reserve_prefix, &prefix.reserve) &&
        state.roles == prefix.roles && state.overwrites == prefix.overwrites && state.role_backed_count == 0 &&
        state.overwrite_backed_count == 0 && state.permission_reserve == reserve_prefix &&
        state.permission_reserve_context == &prefix.reserve && suffix_untouched(),
        "permission prefix: nonnull zero-backed banks bind without touching any reserved slot");
    prefix_before = prefix;
    check(!chat_state_store_bind_permissions_reserved(&state, {prefix.roles, 8}, {prefix.overwrites, 8}, reserve_prefix, &prefix.reserve) &&
        semantic_unchanged(true), "permission prefix: rebinding rejection preserves installed ownership");
    check(chat_state_store_upsert_guild(&state, {20, utf8_literal("Empty"), GuildMutationKind::create, 999}).ok() &&
        chat_state_store_upsert_channel(&state, {200, 20, 0, utf8_literal("Parent"), 0, 0, 0}).ok() &&
        chat_state_store_upsert_channel(&state, {201, 20, 200, utf8_literal("Thread"), 1, 0, 11}).ok(),
        "permission prefix: unresolved parent and thread fixture publishes without permission backing");
    chat_state_store_recompute_guild_permissions(&state, 20);
    const auto* parent = chat_state_store_find_channel(state, 200);
    const auto* thread = chat_state_store_find_channel(state, 201);
    check(parent && thread && !chat_state_store_channel_visible(state, *parent) &&
        !chat_state_store_channel_visible(state, *thread) && parent->actions == 0 && thread->actions == 0 &&
        !parent->permissions_resolved && !thread->permissions_resolved && state.selected_channel_id == 0 &&
        prefix.reserve.calls == 0 && suffix_untouched(),
        "permission prefix: nonnull zero-backed banks remain fail-closed for parent thread actions and selection");
    prefix_before = prefix;
    check(chat_state_store_prepare_permissions(&state, 0, 0) &&
        !chat_state_store_prepare_permissions(&state, 9, 0) &&
        !chat_state_store_prepare_permissions(&state, 0, 9) && semantic_unchanged(true) && prefix.reserve.calls == 0,
        "permission prefix: zero admission is a no-op and either logical cap rejects before callback");

    prefix.reserve.fail_stage = 1; watch();
    check(!chat_state_store_upsert_role(&state, {20, 20, kPrefixBase, utf8_literal("NoAllocatedName")}) &&
        prefix.reserve.calls == 1 && prefix.reserve.requested_roles == 1 && prefix.reserve.requested_overwrites == 0 &&
        prefix.reserve.observed_before_mutation && semantic_unchanged() && suffix_untouched(),
        "permission prefix: direct role failure calls admission before allocating names or publishing rows");
    prefix.reserve.fail_stage = 0; prefix.reserve.watch = false;
    check(chat_state_store_upsert_role(&state, {20, 20, kPrefixBase, utf8_literal("Everyone")}) &&
        state.role_count == 1 && state.role_backed_count == 1 && prefix.reserve.physical_roles == 1 && suffix_untouched(),
        "permission prefix: first direct role admits exactly one zeroed writable record");
    const u32 calls = prefix.reserve.calls;
    check(chat_state_store_upsert_role(&state, {20, 20, kPrefixBase, utf8_literal("Renamed")}) &&
        chat_state_store_prepare_permissions(&state, 1, 0) &&
        chat_state_store_prepare_permissions(&state, 0, 0) && prefix.reserve.calls == calls && state.role_backed_count == 1,
        "permission prefix: existing role update and already-backed/zero admission do not call reserve");
    prefix.reserve.fail_stage = 2; watch();
    check(!chat_state_store_append_channel_overwrite(&state, 200, {20, 0, 0, 0}) &&
        prefix.reserve.calls == calls + 1 && prefix.reserve.observed_before_mutation &&
        semantic_unchanged() && state.overwrite_count == 0 && suffix_untouched(),
        "permission prefix: direct overwrite failure preserves owner ranges rows names and revisions");

    check(seed(), "permission prefix: mixed-guild live reserved prefixes seed");
    prefix.reserve.fail_stage = 1; watch();
    const u32 first_calls = prefix.reserve.calls;
    check(!chat_state_store_prepare_permissions(&state, 6, 4) && prefix.reserve.calls == first_calls + 1 &&
        prefix.reserve.physical_roles == 5 && prefix.reserve.physical_overwrites == 3 &&
        prefix.reserve.observed_before_mutation && semantic_unchanged() && suffix_untouched(),
        "permission prefix: first-bank failure preserves both physical prefixes and all model semantics");
    prefix.reserve.fail_stage = 2; watch();
    const u32 prior_role_growths = prefix.reserve.role_growths;
    check(!chat_state_store_prepare_permissions(&state, 6, 4) && prefix.reserve.physical_roles == 6 &&
        prefix.reserve.physical_overwrites == 3 && prefix.reserve.role_growths == prior_role_growths + 1 &&
        prefix.reserve.observed_before_mutation && semantic_unchanged() && state.role_backed_count == 5 &&
        state.overwrite_backed_count == 3 && suffix_untouched(),
        "permission prefix: second-bank failure retains physical role growth without publishing backed metadata or mutations");
    prefix.reserve.fail_stage = 0; prefix.reserve.watch = false;
    const u32 after_failed_role_growths = prefix.reserve.role_growths;
    check(chat_state_store_prepare_permissions(&state, 6, 4) && state.role_backed_count == 6 && state.overwrite_backed_count == 4 &&
        state.role_count == 5 && state.overwrite_count == 3 && prefix.reserve.role_growths == after_failed_role_growths &&
        prefix.reserve.physical_overwrites == 4 && suffix_untouched(),
        "permission prefix: retry reuses retained physical first bank and publishes both guaranteed prefixes together");

    check(seed(), "permission prefix: whole-guild dispatch failure fixture seeds");
    GatewayClientEvent event{}; ConstSlice<u8> payload{};
    bool routed = route(kCreate, &event, &payload);
    check(routed, "permission prefix: actual guild-create role member channel thread payload routes");
    if (!routed) return;
    prefix.reserve.fail_stage = 2; watch();
    const u32 create_calls = prefix.reserve.calls;
    check(!apply(event, payload).ok() && prefix.reserve.calls == create_calls + 1 &&
        prefix.reserve.requested_roles == 6 && prefix.reserve.requested_overwrites == 4 &&
        prefix.reserve.physical_roles == 6 && prefix.reserve.physical_overwrites == 3 &&
        prefix.reserve.observed_before_mutation && semantic_unchanged() && suffix_untouched(),
        "permission prefix: combined guild-create failure occurs once before metadata old-channel removal roles names or selection");
    prefix.reserve.fail_stage = 0; prefix.reserve.watch = false;
    const u32 create_role_growths = prefix.reserve.role_growths;
    check(apply(event, payload).ok() && prefix.reserve.calls == create_calls + 2 &&
        prefix.reserve.role_growths == create_role_growths && state.role_count == 6 && state.overwrite_count == 4 &&
        state.role_backed_count == 6 && state.overwrite_backed_count == 4 && roles_sorted() && suffix_untouched(),
        "permission prefix: guild-create retry uses retained backing and per-record writes issue no extra reserve calls");

    check(seed(), "permission prefix: malformed guild role fixture seeds");
    routed = route(kCreate, &event, &payload); check(routed, "permission prefix: malformed-role source packet routes");
    if (!routed) return;
    u32 role_offsets[4]{};
    const bool offsets = gateway_client_event_guild_role_offsets(event, payload, {role_offsets, 4});
    if (offsets) {
        GatewayClientRoleMutation invalid{};
        copy_bytes(&invalid, payload.data + role_offsets[3], sizeof(invalid)); invalid.id = 0;
        copy_bytes(const_cast<u8*>(payload.data) + role_offsets[3], &invalid, sizeof(invalid));
    }
    watch(); const u32 malformed_role_calls = prefix.reserve.calls;
    check(offsets && !apply(event, payload).ok() && prefix.reserve.calls == malformed_role_calls && semantic_unchanged(true),
        "permission prefix: malformed final typed guild role rejects before reserve or any publication");
    check(seed(), "permission prefix: malformed guild overwrite fixture seeds");
    routed = route(kCreate, &event, &payload); check(routed, "permission prefix: malformed-overwrite source packet routes");
    if (!routed) return;
    u32 cursor = 0, index = 0; bool changed = false;
    while (index < event.state.record_count) {
        GatewayClientStateMutation channel{}; Utf8View name{}; ConstSlice<u8> overwrite_bytes{};
        if (!gateway_client_event_next_guild_channel(event, payload, &cursor, &index, &channel, &name, &overwrite_bytes)) break;
        if (channel.id == 210 && overwrite_bytes.count == sizeof(GatewayClientPermissionOverwriteMutation)) {
            GatewayClientPermissionOverwriteMutation invalid{};
            copy_bytes(&invalid, overwrite_bytes.data, sizeof(invalid)); invalid.id = 0;
            copy_bytes(const_cast<u8*>(overwrite_bytes.data), &invalid, sizeof(invalid)); changed = true;
        }
    }
    watch(); const u32 malformed_overwrite_calls = prefix.reserve.calls;
    check(changed && !apply(event, payload).ok() && prefix.reserve.calls == malformed_overwrite_calls && semantic_unchanged(true),
        "permission prefix: invalid last ordinary-channel overwrite rejects before whole-event reserve");

    check(seed(), "permission prefix: duplicate typed channel fixture seeds");
    routed = route(kCreate, &event, &payload); check(routed, "permission prefix: duplicate-channel source routes");
    if (!routed) return;
    cursor = index = 0; changed = false;
    while (index < event.state.record_count) {
        GatewayClientStateMutation channel{}; Utf8View name{}; ConstSlice<u8> overwrites{};
        if (!gateway_client_event_next_guild_channel(event, payload, &cursor, &index, &channel, &name, &overwrites)) break;
        if (channel.id == 210) {
            const u32 offset = static_cast<u32>(reinterpret_cast<const u8*>(name.data) - payload.data) - sizeof(channel);
            channel.id = 200; copy_bytes(const_cast<u8*>(payload.data) + offset, &channel, sizeof(channel)); changed = true;
        }
    }
    watch(); const u32 duplicate_channel_calls = prefix.reserve.calls;
    check(changed && !apply(event, payload).ok() && prefix.reserve.calls == duplicate_channel_calls && semantic_unchanged(true),
        "permission prefix: duplicate typed channel IDs reject before reserve removal or name publication");

    check(seed(), "permission prefix: 401-byte final channel fixture seeds");
    routed = route(kCreate, &event, &payload); check(routed, "permission prefix: oversized-channel source routes");
    if (!routed) return;
    cursor = index = 0; changed = false;
    while (index < event.state.record_count) {
        GatewayClientStateMutation channel{}; Utf8View name{}; ConstSlice<u8> overwrites{};
        if (!gateway_client_event_next_guild_channel(event, payload, &cursor, &index, &channel, &name, &overwrites)) break;
        if (index == event.state.record_count && channel.id == 201) {
            const u32 name_offset = static_cast<u32>(reinterpret_cast<const u8*>(name.data) - payload.data);
            const u32 record_offset = name_offset - sizeof(channel);
            if (name_offset + name.bytes == payload.count && name_offset + 401 <= sizeof(prefix_payload)) {
                channel.name_bytes = 401;
                copy_bytes(const_cast<u8*>(payload.data) + record_offset, &channel, sizeof(channel));
                for (u32 i = 0; i < 401; ++i) prefix_payload[name_offset + i] = 'x';
                payload.count = name_offset + 401; event.dispatch_payload_bytes = payload.count; changed = true;
            }
        }
    }
    watch(); const u32 oversized_channel_calls = prefix.reserve.calls;
    check(changed && !apply(event, payload).ok() && prefix.reserve.calls == oversized_channel_calls && semantic_unchanged(true),
        "permission prefix: complete valid-UTF8 401-byte final typed channel name rejects before reserve or earlier mutations");

    check(seed(), "permission prefix: duplicate typed self-role fixture seeds");
    routed = route(kCreate, &event, &payload); check(routed, "permission prefix: duplicate-self-role source routes");
    if (!routed) return;
    u32 self_offsets[4]{}; GatewayClientRoleMutation final_role{}; Utf8View final_name{};
    changed = gateway_client_event_guild_role_offsets(event, payload, {self_offsets, 4}) &&
        gateway_client_event_guild_role_at(event, payload, self_offsets[3], &final_role, &final_name);
    if (changed) {
        const u32 self_begin = self_offsets[3] + sizeof(final_role) + final_name.bytes;
        const u32 self_end = self_begin + 8;
        changed = event.state.member_role_count == 1 && self_end <= payload.count && payload.count + 8 <= sizeof(prefix_payload);
        if (changed) {
            copy_bytes(prefix_payload + self_end + 8, prefix_payload + self_end, payload.count - self_end);
            copy_bytes(prefix_payload + self_end, prefix_payload + self_begin, 8);
            ++event.state.member_role_count; payload.count += 8; event.dispatch_payload_bytes = payload.count;
        }
    }
    watch(); const u32 duplicate_self_calls = prefix.reserve.calls;
    check(changed && !apply(event, payload).ok() && prefix.reserve.calls == duplicate_self_calls && semantic_unchanged(true),
        "permission prefix: duplicate valid packet self-role IDs reject before reserve or member assignment publication");

    check(seed(), "permission prefix: valid zero-channel guild-create fixture seeds");
    routed = route(utf8_literal(R"({"op":0,"t":"GUILD_CREATE","d":{"id":"20","name":"Zero channels","owner_id":"999","roles":[{"id":"20","name":"Everyone","permissions":"66560"},{"id":"21","name":"Common","permissions":"274877906944"}],"members":[{"user":{"id":"42"},"roles":["21"]}],"channels":[]}})"), &event, &payload);
    check(routed && event.state.record_count == 0 && event.state.member_role_count == 1,
        "permission prefix: zero-channel role/self-role payload routes with nonzero validated prefix");
    const u32 zero_channel_calls = prefix.reserve.calls;
    prefix.reserve.watch = false;
    check(routed && apply(event, payload).ok() && state.channel_count == 1 && state.role_count == 4 &&
        state.overwrite_count == 1 && state.role_backed_count == 5 && state.overwrite_backed_count == 3 &&
        chat_state_store_find_channel(state, 100) && !chat_state_store_find_channel(state, 200) &&
        role_equal(prefix.roles[4], {}) && overwrite_equal(prefix.overwrites[1], {}) &&
        overwrite_equal(prefix.overwrites[2], {}) && prefix.reserve.calls == zero_channel_calls && suffix_untouched(),
        "permission prefix: valid zero-channel guild create replaces target state and validates complete role/self prefix without growth");

    check(seed(), "permission prefix: retained-role guild-update fixture seeds");
    routed = route(kUpdate, &event, &payload); check(routed, "permission prefix: actual guild-update replacement routes");
    if (!routed) return;
    const auto old_common = *chat_state_store_find_role(state, 20, 21);
    const auto old_other = *chat_state_store_find_role(state, 10, 11);
    watch(); const u32 update_calls = prefix.reserve.calls;
    prefix.reserve.fail_stage = 1;
    check(!apply(event, payload).ok() && prefix.reserve.calls == update_calls + 1 &&
        prefix.reserve.requested_roles == 6 && prefix.reserve.requested_overwrites == 3 &&
        prefix.reserve.observed_before_mutation && semantic_unchanged(true),
        "permission prefix: guild-update failed growth precedes guild rename and absent-role removal");
    // Rejection audited the pending missing restore. Explicit navigation clears
    // it before testing retention of the current thread across a successful update.
    check(chat_state_store_select_channel(&state, 201).ok() && state.restore_channel_id == 0 &&
        state.restore_guild_id == 0 && state.restore_deadline_ms == 0,
        "permission prefix: explicit thread navigation clears audited pending restore before successful replacement");
    prefix.reserve.fail_stage = 0; prefix.reserve.watch = false;
    check(apply(event, payload).ok() && prefix.reserve.calls == update_calls + 2 && state.role_count == 6 &&
        state.role_backed_count == 6 && state.overwrite_backed_count == 3 && roles_sorted() && suffix_untouched(),
        "permission prefix: guild-update final prefix grows once without transient append occupancy");
    const auto* common = chat_state_store_find_role(state, 20, 21);
    const auto* new_role = chat_state_store_find_role(state, 20, 23);
    const auto* other_role = chat_state_store_find_role(state, 10, 11);
    parent = chat_state_store_find_channel(state, 200); thread = chat_state_store_find_channel(state, 201);
    check(common && new_role && common->self_assigned && !new_role->self_assigned,
        "permission prefix: successful replacement retains common assignment and does not assign new IDs");
    const Utf8View common_name = common ? chat_state_store_role_name(state, *common) : Utf8View{};
    check(common && common->name_bytes == old_common.name_bytes && common->name_offset == old_common.name_offset &&
        common_name.bytes == 6 && bytes_equal(common_name.data, "Common", 6),
        "permission prefix: empty common-role name preserves resident bytes and offset without compaction");
    check(!chat_state_store_find_role(state, 20, 22),
        "permission prefix: successful replacement removes the absent target role");
    check(other_role && role_equal(*other_role, old_other),
        "permission prefix: successful replacement preserves every unrelated role field");
    check(parent && thread && parent->permissions_resolved && thread->permissions_resolved &&
        parent->effective_permissions == (kPrefixBase | kPermissionSendMessagesInThreads) &&
        thread->effective_permissions == parent->effective_permissions,
        "permission prefix: common permission update resolves parent and inherited thread permissions");
    check(parent && thread && !(parent->actions & channel_action_send_messages) &&
        (thread->actions & channel_action_send_messages),
        "permission prefix: ordinary send action clears while inherited thread send action becomes available");
    check(state.selected_channel_id == 201 && state.selected_guild_id == 20,
        "permission prefix: successful replacement retains explicitly selected eligible thread");

    check(seed(), "permission prefix: individual role dispatch fixture seeds");
    routed = route(utf8_literal(R"({"op":0,"t":"GUILD_ROLE_CREATE","d":{"guild_id":"20","role":{"id":"23","name":"NewRole","permissions":"0"}}})"), &event, &payload);
    check(routed, "permission prefix: actual new-role event routes");
    if (!routed) return;
    watch(); const u32 role_calls = prefix.reserve.calls;
    check(apply(event, payload).ok() && prefix.reserve.calls == role_calls + 1 && prefix.reserve.observed_before_mutation &&
        state.role_count == 6 && state.role_backed_count == 6 && suffix_untouched(),
        "permission prefix: individual new-role event admits once before name and sorted record mutation");
    routed = route(utf8_literal(R"({"op":0,"t":"GUILD_ROLE_UPDATE","d":{"guild_id":"20","role":{"id":"21","name":"","permissions":"274877906944"}}})"), &event, &payload);
    prefix.reserve.watch = false;
    const u32 existing_role_calls = prefix.reserve.calls;
    check(routed && apply(event, payload).ok() && prefix.reserve.calls == existing_role_calls,
        "permission prefix: existing-role dispatch update stays inside high-water prefix without callback");

    check(seed(), "permission prefix: channel-create admission fixture seeds");
    routed = route(utf8_literal(R"({"op":0,"t":"CHANNEL_CREATE","d":{"id":"300","guild_id":"10","type":0,"name":"NewChannel","permission_overwrites":[{"id":"10","type":0,"allow":"0","deny":"0"}]}})"), &event, &payload);
    check(routed, "permission prefix: actual new-channel event routes");
    if (!routed) return;
    watch(); const u32 new_channel_calls = prefix.reserve.calls;
    check(apply(event, payload).ok() && prefix.reserve.calls == new_channel_calls + 1 &&
        prefix.reserve.requested_roles == 5 && prefix.reserve.requested_overwrites == 4 &&
        prefix.reserve.observed_before_mutation && state.channel_count == 5 && state.overwrite_count == 4 &&
        state.overwrite_backed_count == 4 && chat_state_store_find_channel(state, 300) && suffix_untouched(),
        "permission prefix: new channel admits complete overwrite prefix once before slot name or range publication");

    check(seed(), "permission prefix: mixed-range channel dispatch fixture seeds");
    routed = route(kChannelUpdate, &event, &payload); check(routed, "permission prefix: growing channel-overwrite packet routes");
    if (!routed) return;
    prefix.reserve.fail_stage = 2; watch(); const u32 channel_calls = prefix.reserve.calls;
    check(!apply(event, payload).ok() && prefix.reserve.calls == channel_calls + 1 &&
        prefix.reserve.requested_roles == 5 && prefix.reserve.requested_overwrites == 5 &&
        prefix.reserve.observed_before_mutation && semantic_unchanged(true),
        "permission prefix: channel-overwrite admission failure preserves renamed channel old ranges and all resident owners");
    prefix.reserve.fail_stage = 0; prefix.reserve.watch = false;
    check(apply(event, payload).ok() && prefix.reserve.calls == channel_calls + 2 && state.overwrite_count == 5 &&
        state.overwrite_backed_count == 5 && state.role_backed_count == 5 && suffix_untouched(),
        "permission prefix: complete channel overwrite replacement admits once and all appends reuse backing");
    const auto* other_channel = chat_state_store_find_channel(state, 100);
    const auto* second_channel = chat_state_store_find_channel(state, 210);
    parent = chat_state_store_find_channel(state, 200);
    check(other_channel && second_channel && parent && other_channel->overwrite_offset == 0 &&
        other_channel->overwrite_count == 1 && second_channel->overwrite_offset == 1 && second_channel->overwrite_count == 1 &&
        parent->overwrite_offset == 2 && parent->overwrite_count == 3 && prefix.overwrites[0].id == 10 &&
        prefix.overwrites[1].id == 21 && prefix.overwrites[2].id == 20 && prefix.overwrites[3].id == 21 && prefix.overwrites[4].id == 42,
        "permission prefix: replacing interior channel range preserves mixed-guild owner offsets and packet order");
    const u32 range_calls = prefix.reserve.calls;
    chat_state_store_clear_channel_overwrites(&state, 200);
    check(state.overwrite_count == 2 && overwrite_equal(prefix.overwrites[2], {}) &&
        overwrite_equal(prefix.overwrites[3], {}) && overwrite_equal(prefix.overwrites[4], {}) &&
        chat_state_store_append_channel_overwrite(&state, 200, {20, 0, 0, 0}) &&
        chat_state_store_append_channel_overwrite(&state, 200, {42, 0, 0, 1}) &&
        chat_state_store_remove_channel(&state, 100) && state.overwrite_count == 3 &&
        chat_state_store_find_channel(state, 210)->overwrite_offset == 0 &&
        chat_state_store_find_channel(state, 200)->overwrite_offset == 1 &&
        overwrite_equal(prefix.overwrites[3], {}) && overwrite_equal(prefix.overwrites[4], {}) &&
        prefix.reserve.calls == range_calls && state.overwrite_backed_count == 5 && suffix_untouched(),
        "permission prefix: clear append and unrelated-channel removal compact live ranges zero tails and reuse high-water backing");
    routed = route(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"200","guild_id":"20","type":0,"name":"Omitted overwrites"}})"), &event, &payload);
    check(routed && apply(event, payload).ok() && prefix.reserve.calls == range_calls &&
        state.overwrite_count == 3 && chat_state_store_find_channel(state, 200)->overwrite_count == 2,
        "permission prefix: omitted overwrites preserve existing channel slice without reserve");

    check(seed(), "permission prefix: malformed channel-overwrite fixture seeds");
    routed = route(kChannelUpdate, &event, &payload); check(routed, "permission prefix: malformed channel source routes");
    if (!routed) return;
    ConstSlice<u8> channel_overwrites{};
    changed = gateway_client_event_channel_overwrites(event, payload, &channel_overwrites);
    if (changed) {
        GatewayClientPermissionOverwriteMutation invalid{};
        const u32 offset = 2 * sizeof(invalid);
        copy_bytes(&invalid, channel_overwrites.data + offset, sizeof(invalid)); invalid.id = 0;
        copy_bytes(const_cast<u8*>(channel_overwrites.data) + offset, &invalid, sizeof(invalid));
    }
    watch(); const u32 bad_channel_calls = prefix.reserve.calls;
    check(changed && !apply(event, payload).ok() && prefix.reserve.calls == bad_channel_calls && semantic_unchanged(true),
        "permission prefix: invalid final channel overwrite rejects before reserve rename or old-range clearing");

    check(seed(), "permission prefix: full logical bank fixture seeds");
    check(chat_state_store_prepare_permissions(&state, 8, 8) &&
        chat_state_store_upsert_role(&state, {23, 20, 0, {}}) &&
        chat_state_store_upsert_role(&state, {24, 20, 0, {}}) &&
        chat_state_store_upsert_role(&state, {25, 20, 0, {}}),
        "permission prefix: admitted capacity fills without extra role callbacks");
    for (u32 i = 0; i < 5; ++i) check(chat_state_store_append_channel_overwrite(&state, 200, {50ull + i, 0, 0, 0}),
        "permission prefix: overwrite logical capacity fills inside admitted prefix");
    watch(); const u32 cap_calls = prefix.reserve.calls;
    check(!chat_state_store_upsert_role(&state, {26, 20, 0, utf8_literal("OverflowName")}) &&
        !chat_state_store_append_channel_overwrite(&state, 200, {99, 0, 0, 0}) &&
        state.role_count == 8 && state.overwrite_count == 8 && prefix.reserve.calls == cap_calls && semantic_unchanged(true),
        "permission prefix: direct full-bank writers reject before callback names shifts or owner mutation");

    check(initialize(false), "permission prefix: fixed binder compatibility fixture initializes");
    bool fixed = state.role_backed_count == 8 && state.overwrite_backed_count == 8 &&
        state.permission_reserve == nullptr && state.permission_reserve_context == nullptr;
    for (u32 i = 0; i < 8; ++i) fixed = fixed && role_equal(prefix.roles[i], {}) && overwrite_equal(prefix.overwrites[i], {});
    check(fixed && chat_state_store_prepare_permissions(&state, 8, 8) && prefix.reserve.calls == 0,
        "permission prefix: fixed binder zeroes complete banks and marks full capacity backed without callback");
    check(initialize(true, false) && chat_state_store_bind_permissions_reserved(&state, {prefix.roles, 8},
        {prefix.overwrites, 8}, reserve_prefix, nullptr) && state.permission_reserve_context == nullptr &&
        chat_state_store_prepare_permissions(&state, 0, 0) && suffix_untouched(),
        "permission prefix: reserved binder accepts an opaque null context and zero admission never invokes it");
    check(initialize(true, false) && chat_state_store_upsert_guild(&state,
        {20, utf8_literal("Legacy"), GuildMutationKind::create, 999}).ok() &&
        chat_state_store_upsert_channel(&state, {200, 20, 0, utf8_literal("Legacy"), 0, 0, 0}).ok() &&
        chat_state_store_upsert_role(&state, {20, 20, kPrefixBase, utf8_literal("Ignored")}) && state.roles == nullptr &&
        state.overwrites == nullptr && state.role_count == 0 &&
        chat_state_store_channel_visible(state, *chat_state_store_find_channel(state, 200)) && suffix_untouched(),
        "permission prefix: existing unbound fixed caller keeps accepted no-op role and visibility compatibility");
}
} // namespace maicord
