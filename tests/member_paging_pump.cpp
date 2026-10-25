#include "maicord/app/message_event_pump.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/presence_coalescer.hpp"
#include "maicord/model/member_list.hpp"
#include "maicord/model/role_policy.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/model/guild_member_store.hpp"

namespace maicord {
namespace {
struct Fixture {
    GuildMemberStore members;
    GuildMemberRecord records[8];
    u32 member_order[8];
    UserStore users;
    UserRecord user_records[16];
    u32 user_order[16];
    u8 names[4096];
    ChatStateStore state;
    GuildRecord guild;
    ChannelRecord channel;
    RoleRecord role_records[4];
    PermissionOverwriteRecord overwrites[1];
    u16 guild_order[1], channel_order[1];
    u8 state_names[1024];
    MemberListView projection;
    MemberListRow rows[16];
    u32 member_scratch[8];
    MessageTimeline timeline;
    MessageEventPump pump;
    char payload[16 * 1024];
    u32 structurals[4096];
};
Fixture fixture{};

bool initialize() noexcept {
    auto& f = fixture;
    if (!guild_member_store_initialize(&f.members, {f.records, 8}, {f.member_order, 8}) ||
        !user_store_initialize(&f.users, {f.user_records, 16}, {f.user_order, 16},
                                       {f.names, sizeof(f.names)}) ||
        !chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1}, {&f.channel, 1},
                                        {f.channel_order, 1}, {f.state_names, sizeof(f.state_names)}) ||
        !chat_state_store_bind_permissions(&f.state, {f.role_records, 4}, {f.overwrites, 1}) ||
        !member_list_view_initialize(&f.projection, {f.rows, 16}, {f.member_scratch, 8}))
        return false;
    f.guild.id = 7;
    f.guild.occupied = true;
    f.state.guild_count = 1;
    f.guild_order[0] = 0;
    f.channel.id = 80;
    f.channel.guild_id = 7;
    f.channel.member_list_id = 70;
    f.channel.occupied = true;
    f.state.channel_count = 1;
    f.channel_order[0] = 0;
    f.state.selected_guild_id = 7;
    f.state.selected_channel_id = 80;
    f.pump = {};
    f.pump.timeline = &f.timeline;
    f.pump.members = &f.members;
    f.pump.users = &f.users;
    f.pump.state = &f.state;
    return true;
}
bool route(Utf8View source, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    auto& f = fixture;
    GatewayEnvelope envelope{};
    bool handled = false;
    if (!gateway_decode_envelope(source, {f.structurals, 4096}, 32, &envelope).ok() ||
        !gateway_client_route_member_list_dispatch(envelope, {f.structurals, 4096}, 32,
                                                   {f.payload, sizeof(f.payload)}, event, payload, &handled)
             .ok() ||
        !handled)
        return false;
    event->kind = GatewayClientEventKind::dispatch;
    event->sequence = envelope.sequence;
    event->dispatch_payload_bytes = payload->count;
    return true;
}
bool apply(Utf8View source) noexcept {
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    return route(source, &event, &payload) &&
           message_event_pump_consume(&fixture.pump, event, payload).ok();
}
bool project() noexcept {
    auto& f = fixture;
    return member_list_view_rebuild(&f.projection, f.members, f.users, f.state, 7, 70).ok();
}
u64 user(u32 index) noexcept {
    const auto* row = member_list_view_row(fixture.projection, index);
    return row && row->kind == MemberListRowKind::member ? row->user_id : 0;
}
u64 append_hash(u64 hash, const void* data, u32 count) noexcept {
    const auto* bytes = static_cast<const u8*>(data);
    for (u32 i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}
u64 state_hash() noexcept {
    auto& f = fixture;
    u64 hash = 14695981039346656037ull;
    hash = append_hash(hash, &f.members, sizeof(f.members));
    hash = append_hash(hash, f.records, sizeof(f.records));
    hash = append_hash(hash, f.member_order, sizeof(f.member_order));
    hash = append_hash(hash, &f.users, sizeof(f.users));
    hash = append_hash(hash, f.user_records, sizeof(f.user_records));
    hash = append_hash(hash, f.user_order, sizeof(f.user_order));
    return append_hash(hash, f.names, sizeof(f.names));
}

bool label(u32 index, Utf8View expected) noexcept {
    const auto& f = fixture;
    const auto* row = member_list_view_row(f.projection, index);
    return row && utf8_equal(member_list_row_label(*row, f.members, f.users, f.state), expected);
}

struct RoleRefreshProbe {
    u32 calls;
    u64 guild;
    bool applied;
};
void test_self_quarantine(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = fixture;
    const bool initialized = initialize();
    check(initialized, "member quarantine: fixture initializes");
    if (!initialized)
        return;
    f.state.self_user_id = 20;
    f.guild.owner_id = 1;
    const PermissionSet allowed = kPermissionViewChannel | kPermissionReadMessageHistory | kPermissionChangeNickname;
    const bool permissions = chat_state_store_upsert_role(
                                 &f.state, {7, 7, allowed | kPermissionSendMessages | kPermissionManageMessages, {}, 0}) &&
                             chat_state_store_set_self_roles(&f.state, 7, 1, 20, {});
    const bool synced = apply(utf8_literal(
        R"({"op":0,"s":1,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"SYNC","range":[0,0],"items":[{"member":{"user":{"id":"20","username":"Self"},"roles":[],"flags":128}}]}]}})"));
    check(permissions && synced && guild_self_quarantined(f.guild.settings) &&
              f.channel.effective_permissions == allowed && !(f.channel.actions & channel_action_send_messages),
          "member quarantine: list SYNC updates self permissions and removes write actions");
    check(apply(utf8_literal(
                    R"({"op":0,"s":2,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"UPDATE","index":0,"item":{"member":{"user":{"id":"20","username":"Self"},"presence":{"status":"idle"}}}}]}})")) &&
              guild_self_quarantined(f.guild.settings) && f.channel.effective_permissions == allowed,
          "member quarantine: sparse in-place presence updates preserve self quarantine");
    check(apply(utf8_literal(
                    R"({"op":0,"s":3,"t":"GUILD_MEMBERS_CHUNK","d":{"guild_id":"7","chunk_index":0,"chunk_count":1,"members":[{"user":{"id":"20","username":"Self"},"roles":[],"flags":0}]}})")) &&
              !guild_self_quarantined(f.guild.settings) && (f.channel.actions & channel_action_send_messages),
          "member quarantine: a chunk's explicit clear restores current-account write permissions");
    check(apply(utf8_literal(
                    R"({"op":0,"s":4,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"UPDATE","index":0,"item":{"member":{"user":{"id":"20","username":"Self"},"roles":[],"flags":1024}}}]}})")) &&
              guild_self_quarantined(f.guild.settings) && f.channel.effective_permissions == allowed,
          "member quarantine: an in-place guild-tag observation disables writes again");
    check(apply(utf8_literal(
                    R"({"op":0,"s":5,"t":"GUILD_MEMBERS_CHUNK","d":{"guild_id":"7","chunk_index":0,"chunk_count":1,"members":[{"user":{"id":"21","username":"Other"},"roles":[],"flags":0}]}})")) &&
              guild_self_quarantined(f.guild.settings),
          "member quarantine: another member's flags never release current-account restrictions");
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    const bool routed = route(utf8_literal(
                                  R"({"op":0,"s":6,"t":"GUILD_MEMBERS_CHUNK","d":{"guild_id":"7","chunk_index":0,"chunk_count":1,"members":[{"user":{"id":"20","username":"Self"},"roles":[],"flags":0}]}})"),
                              &event, &payload);
    const u64 before = state_hash();
    if (routed) {
        GatewayClientMemberListMutation mutation{};
        copy_bytes(&mutation, payload.data, sizeof(mutation));
        mutation.hydration_flags = gateway_member_quarantined;
        copy_bytes(f.payload, &mutation, sizeof(mutation));
    }
    check(routed && !message_event_pump_consume(&f.pump, event, payload).ok() && state_hash() == before &&
              guild_self_quarantined(f.guild.settings) && f.channel.effective_permissions == allowed,
          "member quarantine: forged value without observation marker rejects before changing cached state");
    user_appearance_release(&f.users.appearance);
}
void role_refresh(void* context, u64 guild) noexcept {
    auto& probe = *static_cast<RoleRefreshProbe*>(context);
    ++probe.calls;
    probe.guild = guild;
    const auto* member = guild_member_store_find(fixture.members, guild, 20);
    probe.applied = member && !member->roles_known && !member->role_color;
}
void test_role_hydration(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = fixture;
    const bool initialized = initialize() &&
                             chat_state_store_upsert_role(&f.state, {17, 7, 0, {}, 5, 0x9922CC, true}) &&
                             chat_state_store_upsert_role(&f.state, {18, 7, 0, {}, 9, 0, true}) &&
                             chat_state_store_upsert_role(&f.state, {19, 7, 0, {}, 5, 0xFF8800, true});
    check(initialized, "member roles: real bounded pump role fixture initializes");
    if (!initialized)
        return;
    const auto full = utf8_literal(
        R"({"op":0,"s":1,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"SYNC","range":[0,0],"items":[{"member":{"user":{"id":"20","username":"Colored"},"roles":["19","18","17"]}}]}]}})");
    const bool hydrated = apply(full);
    const auto* member = guild_member_store_find(f.members, 7, 20);
    const u64 assigned[] = {19, 18, 17};
    const auto* effective = role_member_display(f.state, 7, {assigned, 3}).color;
    check(hydrated && member && member->roles_known && member->role_count == 3 &&
              member->color_role_id == 17 && member->role_color == 0x9922CC &&
              member->group_role_id == 18 && effective && effective->id == 17,
          "member roles: higher uncolored role keeps lower colored role and lower-ID color tie wins "
          "regardless of wire order");
    const auto sparse = utf8_literal(
        R"({"op":0,"s":2,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"UPDATE","index":0,"item":{"member":{"user":{"id":"20","username":"Renamed"},"presence":{"status":"idle"}}}}]}})");
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    const bool routed = route(sparse, &event, &payload);
    GatewayClientMemberListMutation mutation{};
    Utf8View nickname{}, display{}, avatar{};
    ConstSlice<u8> roles{};
    u32 offset = 0, index = 0;
    check(routed &&
              gateway_client_event_member_list_record(event, payload, &offset, &index, &mutation, &roles,
                                                      &nickname, &display, &avatar) &&
              mutation.hydration_flags == gateway_member_roles_omitted && !mutation.role_count,
          "member roles: actual producer/reader preserves omitted role presence without growing binary "
          "records");
    const bool preserved = routed && message_event_pump_consume(&f.pump, event, payload).ok();
    member = guild_member_store_find(f.members, 7, 20);
    check(preserved && member && member->roles_known && member->role_count == 3 &&
              member->role_color == 0x9922CC && member->status == GuildMemberStatus::idle,
          "member roles: sparse identity/presence update preserves known role assignments and color");
    const auto populated = utf8_literal(
        R"({"op":0,"s":3,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"UPDATE","index":0,"item":{"member":{"user":{"id":"20","username":"Bad"},"roles":["17"]}}}]}})");
    const bool bad_routed = route(populated, &event, &payload);
    const u64 before = state_hash();
    if (bad_routed) {
        copy_bytes(&mutation, payload.data, sizeof(mutation));
        mutation.hydration_flags = gateway_member_roles_omitted;
        copy_bytes(f.payload, &mutation, sizeof(mutation));
    }
    check(bad_routed && !message_event_pump_consume(&f.pump, event, payload).ok() && state_hash() == before,
          "member roles: omitted marker with populated role payload rejects atomically");
    RoleRefreshProbe refresh{};
    message_event_pump_set_member_roles_refresh(&f.pump, role_refresh, &refresh);
    GatewayClientEvent role{};
    role.kind = GatewayClientEventKind::dispatch;
    role.dispatch_kind = GatewayClientDispatchKind::guild_role_update;
    role.state.id = 17;
    role.state.guild_id = 7;
    role.state.fields = gateway_client_state_field_id | gateway_client_state_field_guild_id |
                        gateway_client_state_field_permissions;
    role.state.position = 5;
    role.state.color = 0x7755DD;
    role.state.hoist = true;
    const bool recolored = message_event_pump_consume(&f.pump, role, {}).ok();
    member = guild_member_store_find(f.members, 7, 20);
    check(recolored && member && member->roles_known && member->role_color == 0x7755DD && !refresh.calls,
          "member roles: committed nonzero color-only winner update recomputes cached color without "
          "hydration");
    role.state.color = 0;
    const bool invalidated = message_event_pump_consume(&f.pump, role, {}).ok();
    member = guild_member_store_find(f.members, 7, 20);
    check(invalidated && member && !member->roles_known && !member->role_color && refresh.calls == 1 &&
              refresh.guild == 7 && refresh.applied,
          "member roles: color fallback ambiguity retires authority and emits exact guild refresh only "
          "after model commit");
    check(apply(sparse) && !guild_member_store_find(f.members, 7, 20)->roles_known && apply(full) &&
              guild_member_store_find(f.members, 7, 20)->role_color == 0xFF8800,
          "member roles: omitted data cannot restore invalid authority; refreshed assignments resolve "
          "actual lower color");
    role.state.id = 19;
    role.state.position = 12;
    role.state.color = 0xFF8800;
    check(message_event_pump_consume(&f.pump, role, {}).ok() && refresh.calls == 2 && refresh.applied &&
              apply(full) && guild_member_store_find(f.members, 7, 20)->color_role_id == 19 &&
              guild_member_store_find(f.members, 7, 20)->group_role_id == 19,
          "member roles: rank edit requests fresh assignments and recomputes hoist/color from current role "
          "metadata");
    role.dispatch_kind = GatewayClientDispatchKind::guild_role_delete;
    check(message_event_pump_consume(&f.pump, role, {}).ok() && refresh.calls == 3 && refresh.applied &&
              apply(full) && guild_member_store_find(f.members, 7, 20)->roles_known &&
              !guild_member_store_find(f.members, 7, 20)->role_color &&
              guild_member_store_find(f.members, 7, 20)->group_role_id == 18,
          "member roles: deleted winner invalidates then resolves only surviving assigned roles without "
          "fabricating color");
    check(
        apply(utf8_literal(
            R"({"op":0,"s":4,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"UPDATE","index":0,"item":{"member":{"user":{"id":"20","username":"Empty"},"roles":[]}}}]}})")) &&
            guild_member_store_find(f.members, 7, 20)->roles_known &&
            !guild_member_store_find(f.members, 7, 20)->role_count &&
            !guild_member_store_find(f.members, 7, 20)->role_color,
        "member roles: explicit empty assignments clear color with authoritative empty semantics");
    user_appearance_release(&f.users.appearance);
}

void test_nullable_presence(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = fixture;
    const bool initialized = initialize();
    check(initialized, "member pump: nullable-presence hydration fixture initializes");
    if (!initialized)
        return;
    const auto mixed_sync = utf8_literal(
        R"({"op":0,"s":1,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","member_count":3,"online_count":1,"groups":[{"id":"online","count":1},{"id":"offline","count":2}],"ops":[{"op":"SYNC","range":[0,4],"items":[{"member":null,"group":{"id":"online"}},{"group":null,"member":{"user":{"id":"20","username":"Active"},"roles":[],"presence":{"status":"idle","activities":[{"type":4,"state":"At work"}]}}},{"group":{"id":"offline"},"member":null},{"member":{"user":{"id":"21","username":"Quiet","global_name":"Quiet display","avatar":"0123456789abcdef0123456789abcdef"},"nick":"Quiet nickname","roles":[],"presence":null},"group":null},{"member":{"user":{"id":"22","username":"Absent"},"roles":[]}}]}]}})");
    const bool hydrated = apply(mixed_sync) && project();
    check(hydrated && f.projection.count == 5 && f.projection.resident_count == 5 && user(1) == 20 &&
              user(3) == 21 && user(4) == 22 && label(3, utf8_literal("Quiet nickname")) &&
              label(4, utf8_literal("Absent")),
          "member pump: mixed SYNC hydrates every embedded member without separate presence events");
    const auto* online_header = member_list_view_row(f.projection, 0);
    const auto* offline_header = member_list_view_row(f.projection, 2);
    check(hydrated && online_header && offline_header && online_header->group_count == 1 &&
              offline_header->group_count == 2,
          "member pump: count-less item headers receive authoritative counts from separate root summaries");
    const auto* quiet_user = user_store_find(f.users, 21);
    const auto* quiet_member = guild_member_store_find(f.members, 7, 21);
    const auto* absent_member = guild_member_store_find(f.members, 7, 22);
    check(hydrated && quiet_user && quiet_member && absent_member &&
              utf8_equal(user_store_name(f.users, *quiet_user), utf8_literal("Quiet display")) &&
              utf8_equal(user_store_username(f.users, *quiet_user), utf8_literal("Quiet")) &&
              utf8_equal(user_store_avatar_hash(*quiet_user),
                         utf8_literal("0123456789abcdef0123456789abcdef")) &&
              quiet_member->status == GuildMemberStatus::unknown &&
              absent_member->status == GuildMemberStatus::unknown,
          "member pump: null and omitted presence preserve profile fields without inventing a status");
    const auto nullable_update = utf8_literal(
        R"({"op":0,"s":2,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"UPDATE","index":1,"item":{"member":{"user":{"id":"20","username":"Renamed","global_name":"New display"},"roles":[],"presence":null}}}]}})");
    const bool updated = apply(nullable_update) && project();
    const auto* active = guild_member_store_find(f.members, 7, 20);
    const auto* appearance = user_appearance_find(f.users.appearance, 20, 7);
    check(updated && label(1, utf8_literal("New display")) && active &&
              active->status == GuildMemberStatus::idle && appearance &&
              utf8_equal(appearance->activity.view(), utf8_literal("At work")),
          "member pump: null-presence UPDATE refreshes identity without clearing known status or activity");
    check(
        apply(utf8_literal(
            R"({"op":0,"s":3,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","groups":[{"id":"online","count":1},{"id":"offline","count":3}],"ops":[{"op":"INSERT","index":5,"item":{"member":{"user":{"id":"23","username":"Inserted"},"roles":[],"presence":null}}}]}})")) &&
            project() && f.projection.resident_count == 6 && user(5) == 23 &&
            label(5, utf8_literal("Inserted")),
        "member pump: null-presence INSERT publishes a named row through the same bounded banks");
    check(
        apply(utf8_literal(
            R"({"op":0,"s":4,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"UPDATE","index":2,"item":{"member":null,"group":{"id":"offline"}}}]}})")) &&
            project() && member_list_view_row(f.projection, 2) &&
            member_list_view_row(f.projection, 2)->group_count == 3,
        "member pump: summary-less count-less header UPDATE preserves the same group's known count");

    GatewayClientEvent flag_event{};
    ConstSlice<u8> flag_payload{};
    const bool flag_routed = route(
        utf8_literal(
            R"({"op":0,"s":5,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","groups":[{"id":"online","count":1},{"id":"offline","count":3}],"ops":[{"op":"UPDATE","index":1,"item":{"member":{"user":{"id":"20","username":"Must not publish"},"roles":[]}}},{"op":"UPDATE","index":2,"item":{"group":{"id":"offline"}}}]}})"),
        &flag_event, &flag_payload);
    const u64 flag_before = state_hash();
    bool flag_rejected = flag_routed;
    if (flag_routed) {
        constexpr u32 record_bytes = static_cast<u32>(sizeof(GatewayClientMemberListMutation));
        const u32 group_offset = flag_payload.count - 3 * record_bytes;
        const u32 summary_offset = flag_payload.count - record_bytes;
        struct InvalidFlag {
            u32 offset;
            u8 flags;
            u32 count;
        };
        const InvalidFlag invalid_flags[] = {{0, gateway_member_group_count_omitted, 0},
                                             {summary_offset, gateway_member_group_count_omitted, 3},
                                             {group_offset, gateway_thread_member_known, 0},
                                             {group_offset, gateway_member_group_count_omitted, 1}};
        for (const auto invalid : invalid_flags) {
            GatewayClientMemberListMutation original{};
            copy_bytes(&original, flag_payload.data + invalid.offset, sizeof(original));
            auto corrupted = original;
            corrupted.hydration_flags = invalid.flags;
            corrupted.range_end = invalid.count;
            copy_bytes(f.payload + invalid.offset, &corrupted, sizeof(corrupted));
            flag_rejected &= !message_event_pump_consume(&f.pump, flag_event, flag_payload).ok() &&
                             state_hash() == flag_before;
            copy_bytes(f.payload + invalid.offset, &original, sizeof(original));
        }
    }
    check(flag_rejected && project() && label(1, utf8_literal("New display")),
          "member pump: kind-invalid count metadata and contradictory count flags reject before an earlier "
          "valid member update");

    const Utf8View malformed_fields[] = {
        utf8_literal(R"("presence":false)"),
        utf8_literal(R"("presence":[])"),
        utf8_literal(R"("presence":"offline")"),
        utf8_literal(R"("presence":0)"),
        utf8_literal(R"("presence":null,"presence":{})"),
        utf8_literal(R"("presence":null,"presence":null)"),
        utf8_literal(R"("presence":{},"pres\u0065nce":null)"),
    };
    const u64 before = state_hash();
    bool rejected = true;
    for (const auto fields : malformed_fields) {
        char source[1024];
        ProtocolWriter writer{{reinterpret_cast<u8*>(source), sizeof(source)}, 0};
        const bool written =
            protocol_writer_append_literal(
                &writer, "{\"op\":0,\"s\":4,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\","
                         "\"id\":\"70\",\"ops\":[{\"op\":\"SYNC\",\"range\":[0,1],\"items\":[{\"member\":{"
                         "\"user\":{\"id\":\"30\",\"username\":\"Earlier "
                         "valid\"},\"roles\":[],\"presence\":null}},{\"member\":{\"user\":{\"id\":\"31\","
                         "\"username\":\"Later invalid\"},\"roles\":[],") &&
            protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(fields.data), fields.bytes) &&
            protocol_writer_append_literal(&writer, "}}]}]}}");
        rejected &= written && !apply({source, writer.used}) && state_hash() == before &&
                    !user_store_find(f.users, 30) && !user_store_find(f.users, 31);
    }
    check(
        rejected && project() && label(1, utf8_literal("New display")) &&
            label(5, utf8_literal("Inserted")),
        "member pump: invalid presence types and duplicates reject the entire SYNC before any publication");
    GatewayEnvelope rejected_envelope{};
    GatewayClientEvent rejected_event{};
    ConstSlice<u8> rejected_payload{};
    bool handled = false;
    const auto invalid_presence = utf8_literal(
        R"({"op":0,"s":5,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"UPDATE","index":1,"item":{"member":{"user":{"id":"20","username":"Bad"},"roles":[],"presence":false}}}]}})");
    const bool envelope_decoded =
        gateway_decode_envelope(invalid_presence, {f.structurals, 4096}, 32, &rejected_envelope).ok();
    const auto rejected_route = gateway_client_route_member_list_dispatch(
        rejected_envelope, {f.structurals, 4096}, 32, {f.payload, sizeof(f.payload)}, &rejected_event,
        &rejected_payload, &handled);
    check(envelope_decoded && handled && !rejected_route.ok() && rejected_route.detail == 0x4D020304u &&
              rejected_route.event_error == GatewayEventDecodeError::invalid_data &&
              !rejected_payload.count && state_hash() == before,
          "member pump: skipped dispatch identifies member presence boolean categorically without "
          "publishing data");
    const Utf8View malformed_items[] = {
        utf8_literal(
            R"({"member":{"user":{"id":"31","username":"Both"},"roles":[]},"group":{"id":"online","count":1}})"),
        utf8_literal(R"({"member":null,"group":null})"),
        utf8_literal(R"({"member":null,"member":{"user":{"id":"31","username":"Duplicate"}}})"),
        utf8_literal(R"({"group":null,"gr\u006fup":{"id":"online","count":1}})"),
        utf8_literal(R"({"member":false,"group":{"id":"online","count":1}})"),
        utf8_literal(R"({"group":[],"member":{"user":{"id":"31","username":"Invalid"}}})"),
        utf8_literal(R"({"member":null,"group":{"id":"online","count":null}})"),
        utf8_literal(R"({"member":null,"group":{"count":1}})"),
    };
    bool rejected_items = true;
    for (const auto item : malformed_items) {
        char source[1024];
        ProtocolWriter writer{{reinterpret_cast<u8*>(source), sizeof(source)}, 0};
        const bool written =
            protocol_writer_append_literal(
                &writer,
                "{\"op\":0,\"s\":6,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\",\"id\":"
                "\"70\",\"ops\":[{\"op\":\"SYNC\",\"range\":[0,1],\"items\":[{\"member\":{\"user\":{\"id\":"
                "\"30\",\"username\":\"Earlier valid\"},\"roles\":[]},\"group\":null},") &&
            protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(item.data), item.bytes) &&
            protocol_writer_append_literal(&writer, "]}]}}");
        rejected_items &= written && !apply({source, writer.used}) && state_hash() == before &&
                          !user_store_find(f.users, 30) && !user_store_find(f.users, 31);
    }
    check(rejected_items && project() && label(1, utf8_literal("New display")),
          "member pump: conflicting/null/duplicate/invalid item arms and malformed groups reject the "
          "complete SYNC");
    const Utf8View malformed_summaries[] = {
        utf8_literal(R"({"id":"online"})"),
        utf8_literal(R"({"id":"online","count":null})"),
        utf8_literal(R"({"id":"online","count":1,"count":2})"),
        utf8_literal(R"({"id":"online","count":4294967295})"),
    };
    bool rejected_summaries = true;
    for (const auto summary : malformed_summaries) {
        char source[1024];
        ProtocolWriter writer{{reinterpret_cast<u8*>(source), sizeof(source)}, 0};
        const bool written =
            protocol_writer_append_literal(
                &writer,
                "{\"op\":0,\"s\":7,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\",\"id\":"
                "\"70\",\"ops\":[{\"op\":\"UPDATE\",\"index\":1,\"item\":{\"member\":{\"user\":{\"id\":"
                "\"30\",\"username\":\"Must not publish\"},\"roles\":[]}}}],\"groups\":[") &&
            protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(summary.data),
                                         summary.bytes) &&
            protocol_writer_append_literal(&writer, "]}}");
        rejected_summaries &= written && !apply({source, writer.used}) && state_hash() == before &&
                              !user_store_find(f.users, 30);
    }
    check(rejected_summaries,
          "member pump: authoritative summary count remains required, unique, numeric and bounded");
    check(
        apply(utf8_literal(
            R"({"op":0,"s":8,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"UPDATE","index":2,"item":{"group":{"id":"99"}}}]}})")) &&
            project() && member_list_view_row(f.projection, 2) &&
            member_list_view_row(f.projection, 2)->group_role_id == 99 &&
            member_list_view_row(f.projection, 2)->group_count == 0,
        "member pump: count-less replacement with another group identity cannot inherit the old group's "
        "count");
    user_appearance_release(&f.users.appearance);
}
} // namespace

bool storm_visible(void*, u64, u64) noexcept {
    return false;
}
// One UPDATE packet in the shape the server streams for an authoritative list:
// the group/count header repeats and one member item is replaced in place.
bool storm_update(u32 sequence, u32 index, u64 user_id, const char* status, const char* nick,
                  const char* role, MessageEventPumpConsumeResult* consumed) noexcept {
    char source[1024];
    ProtocolWriter writer{{reinterpret_cast<u8*>(source), sizeof(source)}, 0};
    protocol_writer_append_literal(&writer, R"({"op":0,"s":)");
    protocol_writer_append_u64(&writer, sequence);
    protocol_writer_append_literal(&writer, R"(,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70",)"
                                            R"("member_count":4,"online_count":4,"groups":[{"id":"online","count":4}],)"
                                            R"("ops":[{"op":"UPDATE","index":)");
    protocol_writer_append_u64(&writer, index);
    protocol_writer_append_literal(&writer, R"(,"item":{"member":{"user":{"id":")");
    protocol_writer_append_u64(&writer, user_id);
    protocol_writer_append_literal(&writer, R"(","username":"m"},"roles":[)");
    if (role) {
        protocol_writer_append_literal(&writer, "\"");
        protocol_writer_append_c_string(&writer, role);
        protocol_writer_append_literal(&writer, "\"");
    }
    protocol_writer_append_literal(&writer, "]");
    if (nick) {
        protocol_writer_append_literal(&writer, R"(,"nick":")");
        protocol_writer_append_c_string(&writer, nick);
        protocol_writer_append_literal(&writer, "\"");
    }
    protocol_writer_append_literal(&writer, R"(,"presence":{"status":")");
    protocol_writer_append_c_string(&writer, status);
    protocol_writer_append_literal(&writer, R"("}}}}]}})");
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    if (!route({source, writer.used}, &event, &payload))
        return false;
    *consumed = message_event_pump_consume(&fixture.pump, event, payload);
    return consumed->ok();
}
void test_member_update_storm(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = fixture;
    const bool initialized = initialize() && chat_state_store_upsert_role(&f.state, {17, 7, 0, {}, 5, 0, true});
    const auto sync = utf8_literal(
        R"({"op":0,"s":1,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","member_count":4,"online_count":4,"groups":[{"id":"online","count":4}],"ops":[{"op":"SYNC","range":[0,4],"items":[{"group":{"id":"online","count":4}},{"member":{"user":{"id":"21","username":"m"},"roles":[],"presence":{"status":"online"}}},{"member":{"user":{"id":"22","username":"m"},"roles":[],"presence":{"status":"online"}}},{"member":{"user":{"id":"23","username":"m"},"roles":[],"presence":{"status":"online"}}},{"member":{"user":{"id":"24","username":"m"},"roles":[],"presence":{"status":"online"}}}]}]}})");
    // Rows 0..2 (header, members 21 and 22) are on screen; 23 and 24 are not.
    const bool ready = initialized && apply(sync) && guild_member_store_set_visible_range(&f.members, 7, 70, 0, 2) &&
                       project() && f.projection.authoritative && user(3) == 23;
    check(ready, "member storm: authoritative four-member list initializes");
    if (!ready)
        return;
    message_event_pump_set_presence_visibility(&f.pump, storm_visible, nullptr);
    const u32 members_before = f.members.revision, users_before = f.users.revision;
    const u32 rebuilds_before = f.projection.rebuilds, projection_before = f.projection.revision;
    const u32 presence_before = f.members.presence_revision;
    bool storm_ok = true, repainted = false;
    PresenceCoalescer coalescer{};
    u32 flushes = 0, visible_flushes = 0;
    // 100 off-screen status UPDATEs over one second, 10 ms apart.
    for (u32 i = 0; i < 100; ++i) {
        MessageEventPumpConsumeResult consumed{};
        storm_ok = storm_update(2 + i, 3 + (i & 1), 23 + (i & 1), i % 3 == 0 ? "idle" : i % 3 == 1 ? "dnd" : "online",
                                nullptr, nullptr, &consumed) &&
                   storm_ok;
        repainted = repainted || consumed.repaint;
        const u64 now = i * 10ull;
        if (f.pump.presence_visible_dirty || f.projection.source_presence_revision != f.members.presence_revision)
            presence_coalescer_note(&coalescer, now);
        if (presence_coalescer_take(&coalescer, now)) {
            ++flushes;
            visible_flushes += member_list_view_refresh_presence(&f.projection, f.members, 0, 3).visible;
        }
    }
    const auto* off = guild_member_store_find(f.members, 7, 23);
    check(storm_ok && !repainted && f.members.revision == members_before && f.users.revision == users_before &&
              f.members.presence_revision != presence_before && off && off->list_index == 3,
          "member storm: off-screen in-place UPDATEs keep the structural member and user revisions");
    check(flushes >= 6 && flushes <= 7 && !visible_flushes && f.projection.rebuilds == rebuilds_before &&
              f.projection.revision == projection_before && f.projection.source_presence_revision != presence_before,
          "member storm: 100 UPDATEs coalesce into 150 ms flushes that update rows in place without a redraw");
    MessageEventPumpConsumeResult consumed{};
    const bool visible_status = storm_update(200, 1, 21, "dnd", nullptr, nullptr, &consumed) && !consumed.repaint &&
                                f.members.revision == members_before;
    const auto refreshed = member_list_view_refresh_presence(&f.projection, f.members, 0, 3);
    const auto* row = member_list_view_row(f.projection, 1);
    check(visible_status && refreshed.visible && refreshed.updated >= 1 && row &&
              row->status == GuildMemberStatus::dnd && f.projection.rebuilds == rebuilds_before &&
              f.projection.revision == projection_before + 1,
          "member storm: a visible row's status UPDATE refreshes in place and requests one redraw");
    const bool renamed = storm_update(201, 2, 22, "dnd", "Neo", nullptr, &consumed) && !consumed.repaint &&
                         f.members.revision == members_before && f.pump.presence_visible_dirty &&
                         label(2, utf8_literal("Neo"));
    f.pump.presence_visible_dirty = false;
    check(renamed, "member storm: a visible nickname UPDATE asks for one coalesced text refresh, not a rebuild");
    const u32 presence_renamed = f.members.presence_revision;
    const bool repeated = storm_update(202, 2, 22, "dnd", "Neo", nullptr, &consumed) && !consumed.repaint &&
                          !f.pump.presence_visible_dirty && f.members.presence_revision == presence_renamed &&
                          f.members.revision == members_before;
    check(repeated, "member storm: an identical repeated UPDATE changes nothing");
    const bool regrouped = storm_update(203, 4, 24, "online", nullptr, "17", &consumed) && consumed.repaint &&
                           f.members.revision != members_before;
    const auto* moved = guild_member_store_find(f.members, 7, 24);
    check(regrouped && moved && moved->group_role_id == 17,
          "member storm: an UPDATE that changes the sorted group stays structural");
    message_event_pump_set_presence_visibility(&f.pump, nullptr, nullptr);
    user_appearance_release(&f.users.appearance);
}

// Rich activities through the pump. A friend presence and the shared
// server's presence arrive separately; the server's empty list (its activity
// hidden there) must leave the friend's card in place.
bool presence(Utf8View source) noexcept {
    auto& f = fixture;
    GatewayEnvelope envelope{};
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    u64 bulk[4]{};
    if (!gateway_decode_envelope(source, {f.structurals, 4096}, 32, &envelope).ok() ||
        !gateway_client_route_dispatch(envelope, {f.structurals, 4096}, 32, {f.payload, sizeof(f.payload)},
                                       {bulk, 4}, &event, &payload)
             .ok())
        return false;
    event.kind = GatewayClientEventKind::dispatch;
    event.sequence = envelope.sequence;
    event.dispatch_payload_bytes = payload.count;
    return message_event_pump_consume(&f.pump, event, payload).ok();
}
void test_activity_scopes(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = fixture;
    const bool initialized = initialize();
    check(initialized, "member pump: activity scope fixture initializes");
    if (!initialized)
        return;
    const auto song = utf8_literal(
        R"({"op":0,"s":1,"t":"PRESENCE_UPDATE","d":{"user":{"id":"30"},"status":"online","activities":[)"
        R"({"type":2,"name":"Spotify","details":"Track","state":"Band","party":{"id":"spotify:30"},)"
        R"("timestamps":{"start":1700000000000,"end":1700000200000}}]}})");
    const auto hidden = utf8_literal(
        R"({"op":0,"s":2,"t":"PRESENCE_UPDATE","d":{"user":{"id":"30"},"guild_id":"7","status":"online",)"
        R"("activities":[]}})");
    const auto stopped = utf8_literal(
        R"({"op":0,"s":3,"t":"PRESENCE_UPDATE","d":{"user":{"id":"30"},"status":"online","activities":[]}})");
    check(presence(song) && user_activity_count(f.users.activities, 30) == 1 && presence(hidden) &&
              user_activity_count(f.users.activities, 30) == 1,
          "member pump: a server presence without activities keeps the friend presence's card");
    const auto* line = user_appearance_find(f.users.appearance, 30);
    check(line && utf8_equal(line->activity.view(), utf8_literal("Listening to Spotify")) && presence(stopped) &&
              !user_activity_count(f.users.activities, 30),
          "member pump: the friend presence clears its activities");

    const auto sync = utf8_literal(
        R"({"op":0,"s":4,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"SYNC",)"
        R"("range":[0,0],"items":[{"member":{"user":{"id":"31","username":"Player"},"roles":[],)"
        R"("presence":{"status":"online","activities":[{"type":0,"name":"Turbo Ball","details":"Duel",)"
        R"("timestamps":{"start":1700000000000}}]}}}]}]}})");
    UserActivitySet stored{};
    check(apply(sync) && user_activity_find(f.users.activities, 31, &stored) && stored.count == 1 &&
              stored.items[0].type == UserActivityType::playing,
          "member pump: member list presences store their rich activities");
    const auto idle = utf8_literal(
        R"({"op":0,"s":5,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"UPDATE",)"
        R"("index":0,"item":{"member":{"user":{"id":"31","username":"Player"},"roles":[],)"
        R"("presence":{"status":"online","activities":[]}}}}]}})");
    check(apply(idle) && !user_activity_count(f.users.activities, 31),
          "member pump: the same server's empty list clears them");

    // Regression: a SYNC whose members all carry activities lands whole.
    char source[8192];
    ProtocolWriter writer{{reinterpret_cast<u8*>(source), sizeof(source)}, 0};
    protocol_writer_append_literal(&writer, R"({"op":0,"s":6,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7",)"
                                            R"("id":"70","ops":[{"op":"SYNC","range":[0,5],"items":[)");
    for (u32 i = 0; i < 6; ++i) {
        char entry[512];
        ProtocolWriter item{{reinterpret_cast<u8*>(entry), sizeof(entry)}, 0};
        protocol_writer_append_literal(&item, R"({"member":{"user":{"id":"4)");
        protocol_writer_append_byte(&item, static_cast<u8>('0' + i));
        protocol_writer_append_literal(&item, R"(","username":"Member"},"roles":[],"presence":{"status":"online",)"
                                              R"("activities":[{"type":4,"name":"Custom Status","state":"Here"},)"
                                              R"({"type":2,"name":"Spotify","details":"Track","state":"A; B",)"
                                              R"("party":{"id":"spotify:4"},"timestamps":{"start":1700000000000,)"
                                              R"("end":1700000200000},"assets":{"large_image":"spotify:abc"}},)"
                                              R"({"type":0,"name":"Turbo Ball","details":"Duel"}]}}})");
        if (i)
            protocol_writer_append_byte(&writer, ',');
        protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(entry), item.used);
    }
    protocol_writer_append_literal(&writer, "]}]}}");
    const bool applied = apply({source, writer.used});
    u32 placed = 0, active = 0;
    for (u32 i = 0; i < 6; ++i) {
        const auto* member = guild_member_store_find(f.members, 7, 40 + i);
        placed += member && member->list_id == 70 && member->list_index == i;
        active += user_activity_count(f.users.activities, 40 + i) == 2;
    }
    check(applied && placed == 6 && active == 6,
          "member pump: a SYNC of members that all carry activities keeps every member and their activities");
    user_activity_release(&f.users.activities);
    user_appearance_release(&f.users.appearance);
}
void test_member_paging_pump(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = fixture;
    const bool initialized = initialize();
    check(initialized, "member pump: isolated bounded fixture initializes");
    if (!initialized)
        return;
    const auto initial = utf8_literal(
        R"({"op":0,"s":1,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","member_count":100000,"online_count":49999,"groups":[{"id":"online","count":49999},{"id":"offline","count":50001}],"ops":[{"op":"SYNC","range":[50000,50002],"items":[{"group":{"id":"offline","count":50001}},{"member":{"user":{"id":"11","username":"Zeta"},"nick":"Zed","roles":[],"presence":{"status":"offline"}}},{"member":{"user":{"id":"12","username":"Alpha"},"roles":[],"presence":{"status":"offline"}}}]},{"op":"SYNC","range":[0,1],"items":[{"group":{"id":"online","count":49999}},{"member":{"user":{"id":"8","username":"Bob"},"roles":[],"presence":{"status":"online"}}}]}]}})");
    check(apply(initial) && project() && f.projection.count == 100002 && f.projection.resident_count == 5 &&
              user(1) == 8 && user(50001) == 11 && user(50002) == 12 &&
              !member_list_view_row(f.projection, 40000),
          "member pump: far SYNC keeps server order, logical extent and sparse holes");
    const auto* offline = member_list_view_row(f.projection, 50000);
    check(offline && offline->kind == MemberListRowKind::group && offline->group_count == 50001,
          "member pump: group header uses authoritative population rather than cache sample");

    const auto update = utf8_literal(
        R"({"op":0,"s":2,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","groups":[{"id":"online","count":49999},{"id":"offline","count":50001}],"ops":[{"op":"UPDATE","index":50001,"item":{"member":{"user":{"id":"13","username":"Changed"},"nick":"New","roles":[]}}}]}})");
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    const bool routed = route(update, &event, &payload);
    check(routed, "member pump: replacement fixture routes");
    if (!routed)
        return;
    const u64 before = state_hash();
    GatewayClientMemberListMutation tail{};
    const u32 tail_offset = payload.count - sizeof(tail);
    copy_bytes(&tail, payload.data + tail_offset, sizeof(tail));
    const auto correct_tail = tail;
    ++tail.index;
    copy_bytes(f.payload + tail_offset, &tail, sizeof(tail));
    check(!message_event_pump_consume(&f.pump, event, payload).ok() && state_hash() == before &&
              user(50001) == 11 && !user_store_find(f.users, 13),
          "member pump: malformed trailing summary cannot partially replace a valid earlier member");
    copy_bytes(f.payload + tail_offset, &correct_tail, sizeof(correct_tail));
    tail = correct_tail;
    tail.user_id = kMemberListOnlineGroup;
    copy_bytes(f.payload + tail_offset, &tail, sizeof(tail));
    check(!message_event_pump_consume(&f.pump, event, payload).ok() && state_hash() == before,
          "member pump: duplicate typed group IDs preserve all earlier positional and identity state");
    copy_bytes(f.payload + tail_offset, &correct_tail, sizeof(correct_tail));
    ++event.member_list.logical_count;
    check(!message_event_pump_consume(&f.pump, event, payload).ok() && state_hash() == before,
          "member pump: descriptor summary-count mismatch preserves positional and identity state");
    --event.member_list.logical_count;
    const bool updated = message_event_pump_consume(&f.pump, event, payload).ok() && project();
    const auto* withdrawn = guild_member_store_find(f.members, 7, 11);
    check(updated && user(50001) == 13 && withdrawn && withdrawn->list_index == ~0u,
          "member pump: valid UPDATE withdraws old positional identity before replacement");

    check(
        apply(utf8_literal(
            R"({"op":0,"s":3,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"DELETE","index":40000}]}})")) &&
            project() && f.projection.count == 100001 && user(50000) == 13 && user(50001) == 12,
        "member pump: deleting an unknown row shifts retained distant indices and logical extent");
    check(
        apply(utf8_literal(
            R"({"op":0,"s":4,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"INSERT","index":40000,"item":{"group":{"id":"99","count":0}}}]}})")) &&
            project() && f.projection.count == 100002 && user(50001) == 13 && user(50002) == 12,
        "member pump: group INSERT shifts members and headers without dense list storage");
    check(
        apply(utf8_literal(
            R"({"op":0,"s":5,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"71","groups":[{"id":"online","count":10}],"ops":[{"op":"INVALIDATE","range":[0,99999]}]}})")) &&
            project() && user(50001) == 13,
        "member pump: another visibility list cannot invalidate selected list positions");
    GatewayClientEvent resumed{};
    resumed.kind = GatewayClientEventKind::resumed;
    const u64 resume_before = state_hash();
    check(message_event_pump_consume(&f.pump, resumed, {}).ok() && state_hash() == resume_before,
          "member pump: successful resume retains sparse positional knowledge");
    check(guild_member_store_pin_range(&f.members, 7, 70, 50000, 50099) &&
              guild_member_store_set_visible_range(&f.members, 7, 70, 50001, 50002) &&
              guild_member_store_refresh_visible_users(f.members, &f.users) == 2,
          "member pump: ready lifecycle fixture pins the selected visible identities");
    GatewayClientEvent fresh{};
    fresh.kind = GatewayClientEventKind::ready;
    fresh.state.id = 1;
    fresh.state.record_count = 1;
    const u64 before_bad_ready = state_hash();
    check(!message_event_pump_consume(&f.pump, fresh, {}).ok() && state_hash() == before_bad_ready &&
              f.state.guild_count == 1 && f.state.selected_channel_id == 80 && user(50001) == 13,
          "member pump: malformed READY cannot reset cached positions or visible profile pins");
    fresh.state.record_count = 0;
    const u32 identities_before_ready = f.members.count;
    check(message_event_pump_consume(&f.pump, fresh, {}).ok() &&
              f.members.count == identities_before_ready &&
              guild_member_store_find(f.members, 7, 13) && !f.members.pin_count &&
              !f.members.visible.guild_id && !guild_member_store_list(f.members, 7, 70) &&
              guild_member_store_find(f.members, 7, 13)->list_index == ~0u &&
              !user_store_find(f.users, 13)->pinned && project() && !f.projection.count,
          "member pump: successful fresh READY withdraws all list residency and pins while retaining "
          "cached guild identity");
    check(
        apply(utf8_literal(
            R"({"op":0,"s":6,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","groups":[],"ops":[]}})")) &&
            project() && f.projection.count == 0 && f.projection.resident_count == 0 &&
            guild_member_store_find(f.members, 7, 13),
        "member pump: empty authoritative groups clears extent without erasing cached identity");
    user_appearance_release(&f.users.appearance);
    check(initialize(), "member pump: profile replacement pressure fixture resets");
    char source[8192];
    const auto long_sync = [&](u64 base, bool offscreen) noexcept {
        ProtocolWriter writer{{reinterpret_cast<u8*>(source), sizeof(source)}, 0};
        protocol_writer_append_literal(
            &writer, "{\"op\":0,\"s\":7,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\","
                     "\"id\":\"70\",\"groups\":[{\"id\":\"online\",\"count\":200}],\"ops\":[{\"op\":"
                     "\"SYNC\",\"range\":[10,13],\"items\":[");
        const auto item = [&](u64 id) noexcept {
            protocol_writer_append_literal(&writer, "{\"member\":{\"user\":{\"id\":\"");
            protocol_writer_append_u64(&writer, id);
            protocol_writer_append_literal(&writer, "\",\"username\":\"");
            for (u32 character = 0; character < 128; ++character)
                protocol_writer_append_byte(&writer, 'u');
            protocol_writer_append_literal(&writer, "\",\"global_name\":\"");
            for (u32 character = 0; character < 128; ++character)
                protocol_writer_append_byte(&writer, 'D');
            protocol_writer_append_literal(
                &writer, "\",\"avatar\":\"0123456789abcdef0123456789abcdef\"},\"roles\":[]}}");
        };
        for (u32 member_index = 0; member_index < 4; ++member_index) {
            if (member_index)
                protocol_writer_append_byte(&writer, ',');
            item(base + member_index);
        }
        protocol_writer_append_literal(&writer, "]}");
        if (offscreen) {
            protocol_writer_append_literal(&writer, ",{\"op\":\"SYNC\",\"range\":[100,100],\"items\":[");
            item(99);
            protocol_writer_append_literal(&writer, "]}");
        }
        protocol_writer_append_literal(&writer, "]}}");
        return apply({source, writer.used});
    };
    check(long_sync(20, false) && guild_member_store_set_visible_range(&f.members, 7, 70, 10, 13) &&
              guild_member_store_refresh_visible_users(f.members, &f.users) == 4,
          "member pump: four maximal visible profiles occupy half the existing name bank");
    const bool replaced_profiles = long_sync(30, true);
    bool old_unpinned = true, visible_retained = true;
    u32 pinned_profiles = 0;
    for (u32 slot = 0; slot < f.users.capacity; ++slot)
        pinned_profiles += f.user_records[slot].occupied && f.user_records[slot].pinned;
    for (u32 member_index = 0; member_index < 4; ++member_index) {
        const auto* old_profile = user_store_find(f.users, 20 + member_index);
        const auto* current_profile = user_store_find(f.users, 30 + member_index);
        old_unpinned &= !old_profile || !old_profile->pinned;
        visible_retained &= current_profile && current_profile->pinned &&
                            current_profile->name_bytes == 128 && current_profile->username_bytes == 128 &&
                            current_profile->avatar_hash_bytes == 32;
    }
    check(replaced_profiles && old_unpinned && visible_retained && pinned_profiles == 4 &&
              user_store_find(f.users, 99) && !user_store_find(f.users, 99)->pinned &&
              project() && user(10) == 30 && user(100) == 99,
          "member pump: replacement SYNC releases obsolete pins before later offscreen profile admission");
    user_appearance_release(&f.users.appearance);
    test_nullable_presence(check);
    test_role_hydration(check);
    test_self_quarantine(check);
    test_member_update_storm(check);
    test_activity_scopes(check);
}
} // namespace maicord
