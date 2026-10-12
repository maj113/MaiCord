#include "maicord/app/message_event_pump.hpp"

namespace maicord {
namespace thread_pump_probe {
struct Fixture {
    ThreadMemberStore threads;
    u64 banks[2][8];
    GuildMemberStore members;
    GuildMemberRecord member_records[8];
    u32 member_order[8];
    UserStore users;
    UserRecord user_records[16];
    u32 user_order[16];
    u8 user_names[4096];
    ChatStateStore state;
    GuildRecord guild;
    ChannelRecord channels[4];
    u16 guild_order[1], channel_order[4];
    u8 state_names[1024];
    u64 guild_scratch[1];
    MessageTimeline timeline;
    MessageEventPump pump;
    char packet[16 * 1024];
    u32 structurals[4096];
};
Fixture fixture{};
bool initialize() noexcept {
    auto& f = fixture;
    if (!thread_member_store_initialize(&f.threads, {f.banks[0], 8}, {f.banks[1], 8}) ||
        !guild_member_store_initialize(&f.members, {f.member_records, 8}, {f.member_order, 8}) ||
        !user_store_initialize(&f.users, {f.user_records, 16}, {f.user_order, 16},
            {f.user_names, sizeof(f.user_names)}) ||
        !chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1},
            {f.channels, 4}, {f.channel_order, 4}, {f.state_names, sizeof(f.state_names)})) return false;
    f.guild.id = 7; f.guild.owner_id = 1; f.guild.occupied = true;
    f.guild.permissions_resolved = f.guild.member_roles_known = true;
    f.guild.base_permissions = ~0ull;
    f.guild_order[0] = 0; f.state.guild_count = 1; f.state.self_user_id = 1;
    for (u32 i = 0; i < 4; ++i) {
        auto& channel = f.channels[i];
        channel.id = 80 + i; channel.guild_id = 7; channel.occupied = true;
        channel.type = i == 0 ? 0 : i == 3 ? 10 : i == 1 ? 11 : 12;
        channel.parent_id = i ? 80 : 0;
        channel.thread_archived_known = i != 0;
        channel.permissions_resolved = true; channel.effective_permissions = ~0ull;
        f.channel_order[i] = static_cast<u16>(i);
    }
    f.state.channel_count = 4; f.state.selected_guild_id = 7; f.state.selected_channel_id = 81;
    f.pump = {}; f.pump.timeline = &f.timeline;
    return message_event_pump_set_state_store(&f.pump, &f.state, {f.guild_scratch, 1}) &&
        message_event_pump_set_member_store(&f.pump, &f.members) &&
        message_event_pump_set_user_store(&f.pump, &f.users) &&
        message_event_pump_set_thread_member_store(&f.pump, &f.threads);
}
bool route(Utf8View source, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    auto& f = fixture;
    GatewayEnvelope envelope{};
    bool handled = false;
    if (!gateway_decode_envelope(source, {f.structurals, 4096}, 32, &envelope).ok() ||
        !gateway_client_route_thread_member_dispatch(envelope, {f.structurals, 4096}, 32,
            {f.packet, sizeof(f.packet)}, event, payload, &handled).ok() || !handled) return false;
    event->kind = GatewayClientEventKind::dispatch;
    event->dispatch_payload_bytes = payload->count;
    return true;
}
MessageEventPumpConsumeResult apply(Utf8View source) noexcept {
    GatewayClientEvent event{}; ConstSlice<u8> payload{};
    if (!route(source, &event, &payload)) return {{}, {}, MessageEventPumpConsumeKind::rejected, MessageEventPumpError::apply, false};
    return message_event_pump_consume(&fixture.pump, event, payload);
}
constexpr auto snapshot = utf8_literal(R"({"op":0,"t":"THREAD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","thread_id":"81","members":[{"user_id":"2","member":{"user":{"id":"2","username":"Member"},"roles":[]},"presence":{"user":{"id":"2"},"status":"online"}}]}})");
GatewayClientEvent channel_event(GatewayClientDispatchKind kind, u64 id, u8 type, bool archived = false) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch; event.dispatch_kind = kind;
    event.state.id = id; event.state.guild_id = 7; event.state.type = type;
    event.state.parent_id = type ? 80 : 0;
    event.state.fields = gateway_client_state_field_id | gateway_client_state_field_guild_id |
        gateway_client_state_field_type | gateway_client_state_field_parent_id;
    event.state.thread_archived_known = type == 10 || type == 11 || type == 12;
    event.state.thread_archived = archived;
    return event;
}
}

void test_thread_member_pump(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace thread_pump_probe;
    auto& f = fixture;
    const bool initialized = initialize();
    check(initialized && f.threads.thread_id == 81, "thread pump: setter selects active known-parent ordinary thread");
    if (!initialized) return;
    check(apply(utf8_literal(R"({"op":0,"t":"THREAD_MEMBERS_UPDATE","d":{"guild_id":"7","id":"81","member_count":50,"added_members":[{"user_id":"9"}]}})")).ok() &&
        !f.threads.known && !f.threads.count, "thread pump: delta cannot fabricate an initial membership snapshot");
    check(apply(snapshot).ok() && f.threads.known && f.threads.count == 1 &&
        thread_member_store_contains(f.threads, 2) && f.members.count == 1 && f.users.count == 1,
        "thread pump: typed snapshot hydrates profiles and separate thread membership");
    const u32 before_resume = f.threads.revision;
    GatewayClientEvent lifecycle{}; lifecycle.kind = GatewayClientEventKind::resumed;
    check(message_event_pump_consume(&f.pump, lifecycle, {}).ok() && f.threads.revision == before_resume &&
        f.threads.known && f.threads.count == 1, "thread pump: RESUMED preserves coherent membership snapshot");
    message_event_pump_set_view_active(&f.pump, false);
    check(apply(utf8_literal(R"({"op":0,"t":"THREAD_MEMBERS_UPDATE","d":{"guild_id":"7","id":"81","member_count":50,"added_members":[{"user_id":"3"}],"removed_member_ids":["2"]}})")).ok() &&
        f.threads.known && f.threads.count == 1 && thread_member_store_contains(f.threads, 3),
        "thread pump: hidden selected view retains late delta and ignores approximate count");
    check(apply(utf8_literal(R"({"op":0,"t":"THREAD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","thread_id":"81","members":[]}})")).ok() &&
        f.threads.known && !f.threads.count, "thread pump: empty snapshot is a known-empty replacement");

    check(apply(snapshot).ok(), "thread pump: retained snapshot ready for malformed packet");
    GatewayClientEvent event{}; ConstSlice<u8> payload{};
    const bool routed = route(utf8_literal(R"({"op":0,"t":"THREAD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","thread_id":"81","members":[{"user_id":"4"},{"user_id":"5","member":{"user":{"id":"5","username":"Later"},"roles":[]}}]}})"), &event, &payload);
    const u32 before_invalid = f.threads.revision, member_revision = f.members.revision, user_revision = f.users.revision;
    if (routed && payload.count) {
        --payload.count; event.dispatch_payload_bytes = payload.count;
        const auto rejected = message_event_pump_consume(&f.pump, event, payload);
        check(!rejected.ok() && rejected.kind == MessageEventPumpConsumeKind::rejected &&
            f.threads.revision == before_invalid && f.members.revision == member_revision && f.users.revision == user_revision &&
            thread_member_store_contains(f.threads, 2), "thread pump: malformed trailing record applies no earlier membership or profiles");
    } else check(false, "thread pump: malformed trailing fixture routes");

    // A rejected packet deliberately retires durable thread proof. A fresh
    // typed metadata/self update is required before exercising another scope.
    event = channel_event(GatewayClientDispatchKind::channel_update, 82, 12);
    event.state.thread_self_joined_known = event.state.thread_self_joined = true;
    event.state.thread_locked_known = true;
    check(message_event_pump_consume(&f.pump,event,{}).ok(),
        "thread pump: fresh private thread metadata and self proof restore after malformed packet gap");
    f.state.selected_channel_id = 82;
    const auto old_scope = apply(snapshot);
    check(old_scope.ok() && old_scope.kind == MessageEventPumpConsumeKind::ignored_dispatch &&
        f.threads.thread_id == 82 && !f.threads.known && !f.threads.count,
        "thread pump: local selection synchronizes before late old-scope snapshot");
    check(apply(utf8_literal(R"({"op":0,"t":"THREAD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","thread_id":"82","members":[{"user_id":"8"}]}})")).ok() &&
        f.threads.known && f.threads.count == 1, "thread pump: selected private-thread snapshot accepted");
    f.state.selected_channel_id = 83;
    check(message_event_pump_synchronize_thread_scope(&f.pump) && !f.threads.thread_id && !f.threads.known,
        "thread pump: announcement thread releases ordinary-thread source");
    f.state.selected_channel_id = 81; f.channels[1].thread_archived_known = false;
    check(apply(snapshot).ok() && !f.threads.thread_id, "thread pump: unknown archive metadata is not active-thread proof");
    f.channels[1].thread_archived_known = true; f.channels[1].parent_id = 999;
    check(apply(snapshot).ok() && !f.threads.thread_id, "thread pump: unknown parent cannot establish membership scope");
    f.channels[1].parent_id = 80;
    check(apply(snapshot).ok() && f.threads.known, "thread pump: restored active selected parent establishes scope");

    event = channel_event(GatewayClientDispatchKind::channel_update, 81, 11, true);
    check(message_event_pump_consume(&f.pump, event, {}).ok() && !f.threads.thread_id && !f.threads.known,
        "thread pump: archive channel mutation releases scope immediately");
    check(apply(snapshot).ok() && !f.threads.thread_id, "thread pump: archived late snapshot is a validated no-op");
    f.state.selected_channel_id = 81;
    event = channel_event(GatewayClientDispatchKind::channel_update, 81, 11, false);
    check(message_event_pump_consume(&f.pump, event, {}).ok() && f.threads.thread_id == 81 && !f.threads.known && apply(snapshot).ok(),
        "thread pump: unarchive establishes fresh selected scope without old identities");
    event = channel_event(GatewayClientDispatchKind::channel_delete, 81, 11);
    check(message_event_pump_consume(&f.pump, event, {}).ok() && !thread_member_store_owns(f.threads, 7, 81) && !f.threads.known,
        "thread pump: deleted selected thread withdraws its membership");
    check(apply(snapshot).ok() && !thread_member_store_contains(f.threads, 2),
        "thread pump: late deleted-thread snapshot cannot resurrect membership");

    check(initialize() && apply(snapshot).ok(), "thread pump: fresh READY fixture initializes");
    u8 guild_ids[8] = {7}; lifecycle = {}; lifecycle.kind = GatewayClientEventKind::ready;
    lifecycle.state.id = 1; lifecycle.state.record_count = 1; lifecycle.dispatch_payload_bytes = 8;
    lifecycle.timestamp_ms = 1'000;
    check(message_event_pump_consume(&f.pump, lifecycle, {guild_ids, 8}).ok() && !f.threads.known && !f.threads.count &&
        !f.threads.thread_id && f.state.restore_channel_id == 81 && f.members.count == 1 && f.users.count == 1,
        "thread pump: fresh READY clears thread ownership and identities while retaining bounded profiles");
    check(initialize() && apply(snapshot).ok(), "thread pump: guild cleanup fixture initializes");
    event = {}; event.kind = GatewayClientEventKind::dispatch; event.dispatch_kind = GatewayClientDispatchKind::guild_delete;
    event.state.id = 7; event.state.fields = gateway_client_state_field_id;
    check(message_event_pump_consume(&f.pump, event, {}).ok() && !f.threads.guild_id && !f.threads.thread_id && !f.threads.known,
        "thread pump: guild deletion releases the selected thread scope");
}
} // namespace maicord
