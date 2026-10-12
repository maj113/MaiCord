#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/trace_record.hpp"

namespace maicord {
namespace {
constexpr u32 kRows = 16;
constexpr u64 kGuild = 77, kSelf = 42, kOwner = 999;
constexpr u64 kBase = kPermissionViewChannel | kPermissionReadMessageHistory;
struct Fixture {
    ChatStateStore state;
    GuildRecord guilds[2];
    ChannelRecord channels[kRows];
    RoleRecord roles[2];
    PermissionOverwriteRecord overwrites[kRows];
    u16 guild_order[2], channel_order[kRows];
    u8 names[4096];
    u32 reserve_calls;
};
struct Packet {
    GatewayClientEvent event;
    u8 payload[8192];
    u32 used, count, mode, last_offset;
};
Fixture actual{}, before{}, expected{};
Packet packet{};

bool reserve(void* opaque, u32 roles, u32 overwrites) noexcept {
    auto& f = *static_cast<Fixture*>(opaque);
    ++f.reserve_calls;
    return roles <= 2 && overwrites <= kRows;
}
bool seed(Fixture* fixture, u64 existing_id = 500) noexcept {
    clear_bytes(fixture, sizeof(*fixture));
    auto& state = fixture->state;
    if (!chat_state_store_initialize(&state, {fixture->guilds, 2}, {fixture->guild_order, 2},
            {fixture->channels, kRows}, {fixture->channel_order, kRows}, {fixture->names, sizeof(fixture->names)}) ||
        !chat_state_store_bind_permissions_reserved(&state, {fixture->roles, 2}, {fixture->overwrites, kRows},
            reserve, fixture)) return false;
    state.self_user_id = kSelf;
    if (!chat_state_store_upsert_guild(&state, {9, utf8_literal("Other"), GuildMutationKind::create, kOwner}).ok() ||
        !chat_state_store_upsert_role(&state, {9, 9, kBase, utf8_literal("OtherRole")}) ||
        !chat_state_store_upsert_channel(&state, {existing_id, 9, 0, utf8_literal("Existing"), 0, 0, 0}).ok() ||
        !chat_state_store_append_channel_overwrite(&state, existing_id, {9, 0, 0, 0}) ||
        !chat_state_store_set_self_roles(&state, 9, kOwner, kSelf, {})) return false;
    chat_state_store_recompute_guild_permissions(&state, 9);
    if (!chat_state_store_select_channel(&state, existing_id).ok()) return false;
    fixture->reserve_calls = 0;
    return true;
}
u32 ordinal(u32 index) noexcept {
    if (packet.mode == 1) return packet.count - 1 - index;
    if (packet.mode == 2 || packet.mode == 5) {
        if (packet.mode == 5 && index + 1 == packet.count) return 4;
        return (index * 3 + 1) % packet.count;
    }
    if (packet.mode == 3 && index + 1 == packet.count) return 2;
    if (packet.mode == 4 && index + 1 == packet.count) return index - 1;
    return index;
}
GatewayClientStateMutation channel_at(u32 index) noexcept {
    GatewayClientStateMutation c{};
    c.id = 10000 + ordinal(index); c.guild_id = kGuild; c.type = 0;
    c.fields = gateway_client_state_field_id | gateway_client_state_field_guild_id | gateway_client_state_field_type |
        gateway_client_state_field_name | gateway_client_state_field_permission_overwrites | gateway_client_state_field_last_message_id;
    c.name_bytes = 4; c.overwrite_count = 1; c.position = index;
    c.last_message_id = 200000 + index; c.member_list_id = 300000 + index; c.member_list_id_known = true;
    return c;
}
void name_at(u32 index, char* name) noexcept {
    for (u32 i = 0; i < 4; ++i) name[i] = static_cast<char>('a' + index + i);
}
bool append(const void* source, u32 bytes) noexcept {
    if (bytes > sizeof(packet.payload) - packet.used) return false;
    copy_bytes(packet.payload + packet.used, source, bytes); packet.used += bytes;
    return true;
}
bool build(u32 count, u32 mode = 0, u32 malformed = 0) noexcept {
    clear_bytes(&packet, sizeof(packet)); packet.count = count; packet.mode = mode;
    auto& e = packet.event;
    e.kind = GatewayClientEventKind::dispatch; e.dispatch_kind = GatewayClientDispatchKind::guild_create;
    e.state.id = kGuild; e.state.owner_id = kOwner; e.state.name_bytes = 6;
    e.state.fields = gateway_client_state_field_id | gateway_client_state_field_name | gateway_client_state_field_owner_id;
    e.state.role_count = 1; e.state.record_count = count; e.state.overwrite_count = count;
    GatewayClientRoleMutation role{}; role.id = kGuild; role.permissions = kBase;
    if (!append("Target", 6) || !append(&role, sizeof(role))) return false;
    for (u32 i = 0; i < count; ++i) {
        auto c = channel_at(i); char name[401];
        name_at(i, name);
        const bool last = i + 1 == count;
        if (last) packet.last_offset = packet.used;
        if (last && malformed == 1) {
            c.name_bytes = 401; for (u32 n = 0; n < 401; ++n) name[n] = 'x';
        }
        if (last && malformed == 2) name[3] = static_cast<char>(0xFF);
        if (last && malformed == 4) c.fields &= ~gateway_client_state_field_permission_overwrites;
        GatewayClientPermissionOverwriteMutation overwrite{};
        overwrite.id = kGuild; overwrite.allow = kPermissionAddReactions;
        if (last && malformed == 3) overwrite.type = 2;
        if (!append(&c, sizeof(c)) || !append(name, c.name_bytes) || !append(&overwrite, sizeof(overwrite))) return false;
    }
    e.dispatch_payload_bytes = packet.used;
    return true;
}
ChatStateApplyResult apply() noexcept {
    return chat_state_apply_gateway_event(&actual.state, packet.event, {packet.payload, packet.used}, {});
}
bool direct_expected() noexcept {
    if (!seed(&expected)) return false;
    auto& state = expected.state;
    if (!chat_state_store_upsert_guild(&state, {kGuild, utf8_literal("Target"), GuildMutationKind::create, kOwner}).ok() ||
        !chat_state_store_upsert_role(&state, {kGuild, kGuild, kBase, {}})) return false;
    for (u32 i = 0; i < packet.count; ++i) {
        const auto c = channel_at(i); char name[4]; name_at(i, name);
        if (!chat_state_store_upsert_channel(&state, {c.id, kGuild, 0, {name, 4}, c.position, 0, 0,
                c.last_message_id, true, c.member_list_id, true, true}).ok() ||
            !chat_state_store_append_channel_overwrite(&state, c.id, {kGuild, kPermissionAddReactions, 0, 0})) return false;
    }
    if (!chat_state_store_set_self_roles(&state, kGuild, kOwner, kSelf, {})) return false;
    chat_state_store_recompute_guild_permissions(&state, kGuild);
    return true;
}
bool same_expected() noexcept {
    // Normalize addresses only; compare every other descriptor byte and all
    // resident records, order arrays and both name banks against direct writes.
    auto descriptor = expected.state;
    descriptor.guilds = actual.state.guilds; descriptor.channels = actual.state.channels;
    descriptor.roles = actual.state.roles; descriptor.overwrites = actual.state.overwrites;
    descriptor.guild_order = actual.state.guild_order; descriptor.channel_order = actual.state.channel_order;
    descriptor.names = actual.state.names; descriptor.permission_reserve_context = actual.state.permission_reserve_context;
    return bytes_equal(&actual.state, &descriptor, sizeof(descriptor)) &&
        bytes_equal(actual.guilds, expected.guilds, sizeof(actual.guilds)) &&
        bytes_equal(actual.channels, expected.channels, sizeof(actual.channels)) &&
        bytes_equal(actual.roles, expected.roles, sizeof(actual.roles)) &&
        bytes_equal(actual.overwrites, expected.overwrites, sizeof(actual.overwrites)) &&
        bytes_equal(actual.guild_order, expected.guild_order, sizeof(actual.guild_order)) &&
        bytes_equal(actual.channel_order, expected.channel_order, sizeof(actual.channel_order)) &&
        bytes_equal(actual.names, expected.names, sizeof(actual.names));
}
u32 first_difference(const void* left, const void* right, u32 bytes) noexcept {
    const auto* a = static_cast<const u8*>(left); const auto* b = static_cast<const u8*>(right);
    for (u32 i = 0; i < bytes; ++i) if (a[i] != b[i]) return i;
    return bytes;
}
void positive_diagnostic(const ChatStateApplyResult& result) noexcept {
    auto descriptor = expected.state;
    descriptor.guilds = actual.state.guilds; descriptor.channels = actual.state.channels;
    descriptor.roles = actual.state.roles; descriptor.overwrites = actual.state.overwrites;
    descriptor.guild_order = actual.state.guild_order; descriptor.channel_order = actual.state.channel_order;
    descriptor.names = actual.state.names; descriptor.permission_reserve_context = actual.state.permission_reserve_context;
    TraceRecord line("guild_channel_validation_failure");
    line.field("mode", packet.mode); line.field("error", static_cast<u8>(result.error));
    line.field("guilds", result.guilds); line.field("channels", result.channels);
    line.field("roles", result.roles); line.field("overwrites", result.overwrites);
    line.field("selection_changed", result.selection_changed ? 1u : 0u); line.field("reserve_calls", actual.reserve_calls);
    line.field("descriptor_difference", first_difference(&actual.state, &descriptor, sizeof(descriptor)));
    line.field("guild_difference", first_difference(actual.guilds, expected.guilds, sizeof(actual.guilds)));
    line.field("channel_difference", first_difference(actual.channels, expected.channels, sizeof(actual.channels)));
    line.field("role_difference", first_difference(actual.roles, expected.roles, sizeof(actual.roles)));
    line.field("overwrite_difference", first_difference(actual.overwrites, expected.overwrites, sizeof(actual.overwrites)));
    line.field("guild_order_difference", first_difference(actual.guild_order, expected.guild_order, sizeof(actual.guild_order)));
    line.field("channel_order_difference", first_difference(actual.channel_order, expected.channel_order, sizeof(actual.channel_order)));
    line.field("name_difference", first_difference(actual.names, expected.names, sizeof(actual.names)));
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.text, line.used, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
    const u32 offset = first_difference(&actual.state, &descriptor, sizeof(descriptor));
    if (offset != sizeof(descriptor)) {
        TraceRecord state("guild_channel_descriptor_failure"); state.field("offset", offset);
        state.field("actual_byte", reinterpret_cast<const u8*>(&actual.state)[offset]);
        state.field("expected_byte", reinterpret_cast<const u8*>(&descriptor)[offset]);
        state.field("actual_role_backed", actual.state.role_backed_count); state.field("expected_role_backed", descriptor.role_backed_count);
        state.field("actual_overwrite_backed", actual.state.overwrite_backed_count); state.field("expected_overwrite_backed", descriptor.overwrite_backed_count);
        state.field("actual_channel_mutations", actual.state.channel_mutations); state.field("expected_channel_mutations", descriptor.channel_mutations);
        state.field("actual_activity_mutations", actual.state.activity_mutations); state.field("expected_activity_mutations", descriptor.activity_mutations);
        state.field("actual_selection_revision", actual.state.selection_revision); state.field("expected_selection_revision", descriptor.selection_revision);
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), state.text, state.used, &written, nullptr);
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
    }
}
bool rejects_without_publication() noexcept {
    // Invalid packets must also preserve a pending READY restore. Successful
    // cases have no pending restore, so the unrelated selection can persist.
    actual.state.restore_channel_id = 90001;
    actual.state.restore_guild_id = 90002;
    actual.state.restore_deadline_ms = 90003;
    copy_bytes(&before, &actual, sizeof(before));
    const auto result = apply();
    return result.error == ChatStateApplyError::invalid_payload && result.store_error == ChatStateStoreError::none &&
        !result.guilds && !result.channels && !result.roles && !result.overwrites && !result.emojis && !result.selection_changed &&
        !actual.reserve_calls && bytes_equal(&actual, &before, sizeof(actual));
}
}
void test_guild_channel_validation(void (*check)(bool, const char*) noexcept) noexcept {
    const char* const labels[] = {
        "guild channel validation: ascending rows retain every field, name, count and foreign selection",
        "guild channel validation: decreasing rows retain exact packet-order semantics",
        "guild channel validation: permuted rows retain exact packet-order semantics"};
    for (u32 mode = 0; mode < 3; ++mode) {
        check(seed(&actual) && build(8, mode) && direct_expected(), "guild channel validation: ordered fixture initializes");
        const auto result = apply();
        const bool valid = result.ok() && result.guilds == 1 && result.channels == 8 && result.roles == 1 && result.overwrites == 8 &&
            !result.selection_changed && actual.state.selected_channel_id == 500 && actual.state.selected_guild_id == 9 &&
            actual.reserve_calls == 1 && same_expected();
        if (!valid) positive_diagnostic(result);
        check(valid, labels[mode]);
    }
    for (u32 mode = 3; mode < 5; ++mode) {
        check(seed(&actual) && build(8, mode), "guild channel validation: duplicate fixture initializes");
        check(rejects_without_publication(), mode == 3
            ? "guild channel validation: late nonadjacent duplicate rejects before callback and all publication"
            : "guild channel validation: adjacent duplicate rejects before callback and all publication");
    }
    check(seed(&actual) && build(8, 5), "guild channel validation: duplicate above previous ID fixture initializes");
    check(rejects_without_publication(),
        "guild channel validation: duplicate above immediately previous ID still rejects below earlier maximum");
    const char* const invalid_labels[] = {
        "guild channel validation: final increasing ID cannot bypass 401-byte name admission",
        "guild channel validation: final increasing ID cannot bypass malformed UTF-8 admission",
        "guild channel validation: final increasing ID cannot bypass malformed overwrite admission",
        "guild channel validation: final increasing ID cannot bypass omitted overwrite metadata admission"};
    for (u32 malformed = 1; malformed <= 4; ++malformed) {
        check(seed(&actual) && build(8, 0, malformed), "guild channel validation: malformed final fixture initializes");
        check(rejects_without_publication(), invalid_labels[malformed - 1]);
    }
    check(seed(&actual, 10007) && build(8), "guild channel validation: foreign maximum-ID fixture initializes");
    check(rejects_without_publication(), "guild channel validation: foreign resident conflict rejects before callback even for new maximum ID");
    check(seed(&actual) && build(0) && direct_expected(), "guild channel validation: zero-channel fixture initializes");
    const auto empty = apply();
    check(empty.ok() && empty.channels == 0 && !empty.selection_changed && actual.state.selected_channel_id == 500 &&
        actual.state.selected_guild_id == 9 && actual.state.channel_count == 1 && actual.reserve_calls == 1 && same_expected(),
        "guild channel validation: zero-channel create never reads a prior ID and preserves unrelated selected row");
}
}
