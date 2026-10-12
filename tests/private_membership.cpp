#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/ui/private_channel_title.hpp"
#include "maicord/app/message_event_pump.hpp"

namespace maicord {
void test_private_membership(void (*check)(bool, const char*) noexcept) noexcept {
    ChatStateStore state{};
    GuildRecord guilds[2]{};
    ChannelRecord channels[8]{};
    u16 guild_order[2]{}, channel_order[8]{};
    u8 names[4096]{};
    PrivateChannelStore metadata{};
    UserStore users{};
    UserRecord user_records[16]{};
    u32 user_order[16]{};
    u8 user_names[4096]{};
    u32 scratch[4096]{};
    char packet[8192]{};
    u64 ids[256]{};
    check(chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 8},
        {channel_order, 8}, {names, 4096}) && private_channel_store_initialize_owned(&metadata, 8, 256) &&
        user_store_initialize(&users, {user_records, 16}, {user_order, 16}, {user_names, 4096}), "membership stores initialize");
    state.private_channels = &metadata;
    state.self_user_id = 1;
    const auto apply = [&](Utf8View json) noexcept {
        GatewayEnvelope envelope{};
        GatewayClientEvent event{};
        ConstSlice<u8> payload{};
        return gateway_decode_envelope(json, {scratch, 4096}, 64, &envelope).ok() &&
            gateway_client_route_dispatch(envelope, {scratch, 4096}, 64, {packet, 8192}, {ids, 256}, 1, &event, &payload).ok() &&
            chat_state_apply_gateway_event(&state, event, payload, {ids, 2}, &users).ok();
    };
    check(apply(utf8_literal(R"({"op":0,"t":"CHANNEL_CREATE","s":1,"d":{"id":"10","type":3,"owner_id":"2","recipients":[{"id":"3","username":"C"},{"id":"2","username":"B"}],"nicks":{"2":"Bee"}}})")),
        "initial private snapshot retains channel-local nicknames");
    char title[128];
    const auto title_equals = [&](Utf8View expected) noexcept {
        const auto* channel = chat_state_store_find_channel(state, 10);
        if (!channel) return false;
        const auto actual = private_channel_title(state, &users, *channel, {title, 128});
        return actual.bytes == expected.bytes && bytes_equal(actual.data, expected.data, actual.bytes);
    };
    check(title_equals(utf8_literal("C, Bee")), "group fallback label resolves channel nickname");
    check(apply(utf8_literal(R"({"op":0,"t":"CHANNEL_RECIPIENT_ADD","s":2,"d":{"channel_id":"10","user":{"id":"4","username":"D","global_name":"Dee"},"nick":"D\u00e9"}})")) &&
        title_equals(utf8_literal("Bee, C, Dé")), "recipient add hydrates user nickname and sorts snowflake identities");
    const auto* peer = user_store_find(users, 4);
    check(peer && user_store_name(users, *peer).bytes == 3 &&
        bytes_equal(user_store_name(users, *peer).data, "Dee", 3), "channel nickname does not overwrite global user identity");
    const u32 revision = metadata.revision;
    const u32 mutations = state.channel_mutations;
    check(apply(utf8_literal(R"({"op":0,"t":"CHANNEL_RECIPIENT_ADD","s":3,"d":{"channel_id":"10","user":{"id":"4"},"nick":null}})")) &&
        metadata.revision == revision && state.channel_mutations == mutations && title_equals(utf8_literal("Bee, C, Dé")),
        "duplicate add and null nickname preserve existing state");
    check(apply(utf8_literal(R"({"op":0,"t":"CHANNEL_RECIPIENT_ADD","s":4,"d":{"channel_id":"10","user":{"id":"1","username":"Self"},"nick":"Me"}})")) &&
        metadata.revision == revision, "self recipient add leaves the private list unchanged");
    check(apply(utf8_literal(R"({"op":0,"t":"CHANNEL_RECIPIENT_REMOVE","s":5,"d":{"channel_id":"10","user":{"id":"3"}}})")) &&
        title_equals(utf8_literal("Bee, Dé")) && state.channel_mutations > mutations,
        "recipient removal invalidates generated labels");
    check(apply(utf8_literal(R"({"op":0,"t":"CHANNEL_RECIPIENT_REMOVE","s":6,"d":{"channel_id":"10","user":{"id":"4"}}})")) &&
        apply(utf8_literal(R"({"op":0,"t":"CHANNEL_RECIPIENT_ADD","s":7,"d":{"channel_id":"10","user":{"id":"4"}}})")) &&
        title_equals(utf8_literal("Bee, Dé")), "removing and readding a peer preserves its retained nickname");
    check(apply(utf8_literal(R"({"op":0,"t":"CHANNEL_RECIPIENT_ADD","s":8,"d":{"channel_id":"10","user":{"id":"4"},"nick":""}})")) &&
        title_equals(utf8_literal("Bee, Dee")), "explicit empty nickname removes only its local override");
    check(apply(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","s":9,"d":{"id":"10","type":3,"nicks":{}}})")) &&
        title_equals(utf8_literal("B, Dee")), "complete empty nickname snapshot clears prior overrides");
    check(apply(utf8_literal(R"({"op":0,"t":"CHANNEL_RECIPIENT_ADD","s":10,"d":{"channel_id":"999","user":{"id":"5","username":"Unknown channel peer"}}})")) &&
        state.channel_count == 1 && user_store_find(users, 5), "unknown private-channel membership does not invent a channel");
    const u32 before_invalid = state.channel_mutations;
    check(!apply(utf8_literal(R"({"op":0,"t":"CHANNEL_RECIPIENT_ADD","s":11,"d":{"channel_id":"10","user":{"id":"4"},"nick":9}})")) &&
        !apply(utf8_literal(R"({"op":0,"t":"CHANNEL_RECIPIENT_ADD","s":11,"d":{"channel_id":"10","channel_id":"11","user":{"id":"4"}}})")) &&
        !apply(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","s":11,"d":{"id":"10","type":3,"nicks":{"2":"one","2":"two"}}})")) &&
        state.channel_mutations == before_invalid, "malformed membership and duplicate nickname snapshots reject without mutation");

    const auto supplemental = [&](Utf8View json, JsonValue* array) noexcept {
        GatewayEnvelope envelope{};
        return gateway_decode_envelope(json, {scratch, 4096}, 64, &envelope).ok() &&
            gateway_decode_supplemental_private_channels(envelope.data, {scratch, 4096}, 64, array).ok();
    };
    JsonValue array{};
    check(supplemental(utf8_literal(R"({"op":0,"t":"READY_SUPPLEMENTAL","s":12,"d":{"guilds":[],"lazy_private_channels":[{"id":"12","type":1,"recipients":[{"id":"6","username":"Late peer"}]}]}})"), &array) &&
        array.kind == JsonValueKind::array, "supplemental decoder retains lazy private channels as a JSON view");
    struct Context { ChatStateStore* state; UserStore* users; u32 scratch[1024]; char packet[2048]; u64 ids[4]; bool ok; } context{};
    context.state = &state;
    context.users = &users;
    context.ok = true;
    const auto scan = json_scan_structurals_avx1(array.source, {scratch, 4096});
    const auto read = json_visit_root_array(array.source, {scratch, scan.count}, 64, [](void* opaque, u32, JsonValue value) noexcept {
        auto& c = *static_cast<Context*>(opaque);
        GatewayEnvelope envelope{};
        envelope.data = value;
        envelope.event_name = {utf8_literal("CHANNEL_CREATE"), 0, false};
        envelope.has_event_name = true;
        GatewayClientEvent event{};
        ConstSlice<u8> payload{};
        c.ok = gateway_client_route_dispatch(envelope, {c.scratch, 1024}, 64, {c.packet, 2048}, {c.ids, 4}, 1, &event, &payload).ok() &&
            chat_state_apply_gateway_event(c.state, event, payload, {c.ids, 2}, c.users).ok();
        return c.ok;
    }, &context);
    check(scan.ok() && read.ok() && context.ok && chat_state_store_find_channel(state, 12) && private_channel_peer(state, 12) == 6,
        "supplemental private records use the same channel and user pipeline");
    check(supplemental(utf8_literal(R"({"op":0,"t":"READY_SUPPLEMENTAL","s":13,"d":{"lazy_private_channels":null}})"), &array) &&
        supplemental(utf8_literal(R"({"op":0,"t":"READY_SUPPLEMENTAL","s":13,"d":{}})"), &array) &&
        !supplemental(utf8_literal(R"({"op":0,"t":"READY_SUPPLEMENTAL","s":13,"d":{"lazy_private_channels":{}}})"), &array) &&
        !supplemental(utf8_literal(R"({"op":0,"t":"READY_SUPPLEMENTAL","s":13,"d":{"lazy_private_channels":[],"lazy_private_channels":[]}})"), &array),
        "supplemental optional null arrays and malformed duplicates follow bounded schema rules");
    check(apply(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","s":14,"d":{"id":"10","type":3,"nicks":{"2":"Bee"}}})")),
        "nickname snapshot can refill cleared backing");
    check(metadata.nickname_count == 1, "nickname backing contains the replacement record");
    MessageTimeline timeline{};
    MessageEventPump pump{};
    pump.timeline = &timeline;
    GatewayClientEvent incomplete{};
    incomplete.kind = GatewayClientEventKind::dispatch;
    incomplete.dispatch_kind = GatewayClientDispatchKind::private_state_incomplete;
    const auto rejected = message_event_pump_consume(&pump, incomplete, {});
    check(rejected.ok() && rejected.kind == MessageEventPumpConsumeKind::rejected && pump.private_state_incomplete && !pump.resync_required && !pump.catchup_required,
        "private-state errors do not trigger unrelated message history reset");
    u32 queued = 0;
    const auto drained = message_event_pump_drain(&pump, &queued, [](void* opaque, GatewayClientEvent* event, Slice<u8>) noexcept {
        auto& count = *static_cast<u32*>(opaque);
        if (count == 2) return GatewayClientPopResult{0, GatewayClientPopError::empty};
        *event = {};
        event->kind = GatewayClientEventKind::dispatch;
        event->dispatch_kind = count++ == 0 ? GatewayClientDispatchKind::private_state_incomplete : GatewayClientDispatchKind::none;
        return GatewayClientPopResult{};
    }, 4);
    check(drained.ok() && drained.popped == 2 && drained.rejected == 1 && drained.private_state_incomplete,
        "private-state rejection leaves subsequent gateway events draining");
    check(apply(utf8_literal(R"({"op":0,"t":"CHANNEL_DELETE","s":15,"d":{"id":"10","type":3}})")),
        "private channel with nicknames closes through gateway state");
    check(!metadata.nickname_count, "closing a private channel removes its nickname records");
    private_channel_store_release(&metadata);
    check(!metadata.nickname_backing.base, "nickname reservation releases with the private store");
}
}
