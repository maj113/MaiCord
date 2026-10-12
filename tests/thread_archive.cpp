#include "maicord/app/chat_state_dispatch.hpp"

namespace maicord {
namespace {
struct ArchiveFixture {
    ChatStateStore state;
    GuildRecord guild;
    ChannelRecord channels[4];
    u16 guild_order[1], channel_order[4];
    u8 names[1024];
    u32 structurals[1024];
    char payload[2048];
};
ArchiveFixture archive_fixture{};
bool archive_route(Utf8View wire, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    auto& f = archive_fixture;
    GatewayEnvelope envelope{}; bool handled = false;
    *event = {}; event->kind = GatewayClientEventKind::dispatch;
    const bool routed = gateway_decode_envelope(wire, {f.structurals, 1024}, 32, &envelope).ok() &&
        gateway_client_route_state_dispatch(envelope, {f.structurals, 1024}, 32,
            {f.payload, sizeof(f.payload)}, 1, event, payload, &handled).ok() && handled;
    if (routed) event->dispatch_payload_bytes = payload->count;
    return routed;
}
bool archive_apply(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    return chat_state_apply_gateway_event(&archive_fixture.state, event, payload, {}).ok();
}
}
void test_thread_archive(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = archive_fixture;
    const bool initialized = chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1},
        {f.channels, 4}, {f.channel_order, 4}, {f.names, sizeof(f.names)}) &&
        chat_state_store_upsert_guild(&f.state, {7, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok();
    check(initialized, "thread archive: isolated bounded state initializes");
    if (!initialized) return;
    GatewayClientEvent event{}; ConstSlice<u8> payload{};
    bool routed = archive_route(utf8_literal(R"({"op":0,"t":"THREAD_CREATE","s":1,"d":{"id":"81","guild_id":"7","parent_id":"80","type":11,"name":"Thread","thread_metadata":{"archived":false,"auto_archive_duration":1440}}})"), &event, &payload);
    check(routed && event.state.thread_archived_known && !event.state.thread_archived && archive_apply(event, payload),
        "thread archive: active metadata survives gateway routing and state apply");
    if (!routed) return;
    const auto* channel = chat_state_store_find_channel(f.state, 81);
    check(channel && channel->thread_archived_known && !channel->thread_archived,
        "thread archive: active state is explicitly known");
    routed = archive_route(utf8_literal(R"({"op":0,"t":"THREAD_UPDATE","s":2,"d":{"id":"81","guild_id":"7","type":11,"thread_metadata":{"archived":true}}})"), &event, &payload);
    check(routed && archive_apply(event, payload), "thread archive: archived update applies without renaming");
    channel = chat_state_store_find_channel(f.state, 81);
    check(channel && channel->thread_archived && channel->thread_archived_known &&
        chat_state_store_channel_name(f.state, *channel).bytes == 6,
        "thread archive: archive update preserves the channel name");
    check(archive_route(utf8_literal(R"({"op":0,"t":"THREAD_UPDATE","s":3,"d":{"id":"81","guild_id":"7","type":11,"name":"Renamed"}})"), &event, &payload) && archive_apply(event, payload),
        "thread archive: omitted metadata update applies");
    channel = chat_state_store_find_channel(f.state, 81);
    check(channel && channel->thread_archived && channel->thread_archived_known,
        "thread archive: omitted metadata preserves known archive state");
    const Utf8View malformed[] = {
        utf8_literal(R"({"op":0,"t":"THREAD_UPDATE","d":{"id":"81","guild_id":"7","type":11,"thread_metadata":{"archived":true,"archived":false}}})"),
        utf8_literal(R"({"op":0,"t":"THREAD_UPDATE","d":{"id":"81","guild_id":"7","type":11,"thread_metadata":{},"thread_metadata":{}}})"),
        utf8_literal(R"({"op":0,"t":"THREAD_UPDATE","d":{"id":"81","guild_id":"7","type":11,"thread_metadata":{"archived":null}}})"),
        utf8_literal(R"({"op":0,"t":"THREAD_UPDATE","d":{"id":"81","guild_id":"7","type":11,"thread_metadata":{"archived":"false"}}})"),
        utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"81","guild_id":"7","type":0,"thread_metadata":{"archived":true}}})")
    };
    for (const auto wire : malformed)
        check(!archive_route(wire, &event, &payload), "thread archive: invalid or duplicate metadata rejected before publication");
    routed = archive_route(utf8_literal(R"({"op":0,"t":"THREAD_UPDATE","d":{"id":"81","guild_id":"7","type":11,"thread_metadata":{"archived":false}}})"), &event, &payload);
    check(routed, "thread archive: unarchive fixture routes");
    if (!routed) return;
    event.state.thread_archived = true; event.state.thread_archived_known = false;
    check(!archive_apply(event, payload) && chat_state_store_find_channel(f.state, 81)->thread_archived,
        "thread archive: impossible packed flags preserve previous state");
    event.state.thread_archived = false; event.state.thread_archived_known = true;
    check(archive_apply(event, payload) && !chat_state_store_find_channel(f.state, 81)->thread_archived,
        "thread archive: valid unarchive restores active state");
}
} // namespace maicord
