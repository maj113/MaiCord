#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/model/read_state.hpp"

namespace maicord {
namespace {
struct ArchiveFixture {
    ChatStateStore state;
    GuildRecord guild;
    ChannelRecord channels[16];
    u16 guild_order[1], channel_order[16];
    u8 names[2048];
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
bool archive_dispatch(Utf8View wire) noexcept {
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    return archive_route(wire, &event, &payload) && archive_apply(event, payload);
}
const ChannelRecord* archive_channel(u64 id) noexcept {
    return chat_state_store_find_channel(archive_fixture.state, id);
}
bool archive_read(u64 id, u64 cursor, u32 mentions, bool manual = false) noexcept {
    return chat_state_store_apply_read_state(&archive_fixture.state, {id, cursor, mentions, 70'000,
        0x80000005u, static_cast<u8>(read_state_cursor | read_state_mentions | read_state_last_viewed |
            read_state_flags | (manual ? read_state_manual : 0)), {}});
}
void forum_parent_activity(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = archive_fixture;
    check(chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1},
        {f.channels, 16}, {f.channel_order, 16}, {f.names, sizeof(f.names)}) &&
        chat_state_store_upsert_guild(&f.state, {7, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() &&
        chat_state_store_upsert_channel(&f.state, {80, 7, 0, utf8_literal("Forum"), 0, 0, 15, 100, true}).ok() &&
        archive_read(80, 90, 3), "forum reads: parent starts with authoritative unread and mention state");
    f.state.self_user_id = 1;
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    check(archive_route(utf8_literal(R"({"op":0,"t":"THREAD_CREATE","d":{"id":"130","guild_id":"7","parent_id":"80","type":11,"owner_id":"1","name":"Existing","member":{"id":"130","user_id":"1","join_timestamp":"2026-10-01T00:00:00Z","flags":0}}})"), &event, &payload) &&
        !event.state.channel_event_flags && archive_apply(event, payload) && archive_channel(130)->thread_self_joined &&
        archive_channel(80)->last_message_id == 100 && archive_channel(80)->last_read_message_id == 90 &&
        archive_channel(80)->mention_count == 3,
        "forum reads: old membership THREAD_CREATE retains membership without treating the post as new");
    check(archive_dispatch(utf8_literal(R"({"op":0,"t":"THREAD_CREATE","d":{"id":"110","guild_id":"7","parent_id":"80","type":11,"owner_id":"2","newly_created":true,"name":"Other","last_message_id":"900"}})")) &&
        archive_channel(80)->last_message_id == 110 && archive_channel(80)->last_read_message_id == 90 &&
        archive_channel(80)->mention_count == 3 && f.guild.mention_count == 3,
        "forum reads: true creation advances parent to thread ID without fabricating badges or using reply recency");
    check(archive_dispatch(utf8_literal(R"({"op":0,"t":"THREAD_CREATE","d":{"id":"120","guild_id":"7","parent_id":"80","type":11,"owner_id":"1","newly_created":true,"name":"Own"}})")) &&
        archive_channel(80)->last_message_id == 120 && archive_channel(80)->last_read_message_id == 120 &&
        !archive_channel(80)->mention_count && !f.guild.mention_count &&
        archive_channel(120)->thread_parent_creation_noted && archive_channel(80)->read_last_viewed == 70'000 &&
        archive_channel(80)->read_flags == 0x80000005u,
        "forum reads: own creation resets parent cursor and counts while preserving server metadata");
    // Forum parents open a post browser rather than a selected message chat;
    // their manual read state does not use selected-chat auto-ACK suppression.
    check(archive_read(80, 90, 2, true) &&
        archive_dispatch(utf8_literal(R"({"op":0,"t":"THREAD_CREATE","d":{"id":"120","guild_id":"7","parent_id":"80","type":11,"owner_id":"1","newly_created":true,"name":"Own again"}})")) &&
        archive_channel(80)->last_read_message_id == 90 && archive_channel(80)->mention_count == 2 &&
        !archive_channel(80)->read_mentions_incomplete && archive_channel(120)->thread_parent_creation_noted &&
        archive_channel(80)->read_last_viewed == 70'000 && archive_channel(80)->read_flags == 0x80000005u,
        "forum reads: retained creation receipt prevents duplicate Gateway events erasing manual unread");
    check(archive_dispatch(utf8_literal(R"({"op":0,"t":"THREAD_CREATE","d":{"id":"140","guild_id":"7","parent_id":"80","type":11,"owner_id":"2","newly_created":true,"name":"Newer"}})")) &&
        archive_dispatch(utf8_literal(R"({"op":0,"t":"THREAD_CREATE","d":{"id":"130","guild_id":"7","parent_id":"80","type":11,"owner_id":"1","newly_created":true,"name":"Delayed own"}})")) &&
        archive_channel(80)->last_message_id == 140 && archive_channel(80)->last_read_message_id == 130 &&
        archive_channel(80)->mention_count == 2 && archive_channel(80)->read_mentions_incomplete &&
        chat_state_store_channel_unread(f.state, *archive_channel(80)),
        "forum reads: delayed own creation acknowledges only its ID and preserves newer unread/count uncertainty");
    check(archive_read(80, 170, 7) &&
        archive_dispatch(utf8_literal(R"({"op":0,"t":"THREAD_CREATE","d":{"id":"150","guild_id":"7","parent_id":"80","type":11,"owner_id":"1","newly_created":true,"name":"Already read"}})")) &&
        archive_channel(80)->last_message_id == 150 && archive_channel(80)->last_read_message_id == 170 &&
        archive_channel(80)->mention_count == 7 && !archive_channel(80)->read_mentions_incomplete,
        "forum reads: own creation never rolls back a newer authoritative parent read state");
    check(archive_dispatch(utf8_literal(R"({"op":0,"t":"THREAD_UPDATE","d":{"id":"150","guild_id":"7","parent_id":"80","type":11,"newly_created":true,"last_message_id":"999"}})")) &&
        archive_dispatch(utf8_literal(R"({"op":0,"t":"THREAD_CREATE","d":{"id":"160","guild_id":"7","parent_id":"80","type":11,"newly_created":false,"name":"Sync"}})")) &&
        archive_channel(80)->last_message_id == 150,
        "forum reads: thread updates and explicit old-thread sync do not advance parent recency");
    check(chat_state_store_upsert_channel(&f.state, {180, 7, 0, utf8_literal("Media"), 0, 0, 16, 200, true}).ok() &&
        archive_read(180, 200, 4) &&
        archive_dispatch(utf8_literal(R"({"op":0,"t":"THREAD_CREATE","d":{"id":"210","guild_id":"7","parent_id":"180","type":11,"owner_id":"1","newly_created":true,"name":"Media own"}})")) &&
        archive_channel(180)->last_message_id == 210 && archive_channel(180)->last_read_message_id == 210 &&
        !archive_channel(180)->mention_count,
        "forum reads: media parents use the same latest-post and own-post ACK automation");
    check(chat_state_store_upsert_channel(&f.state, {181, 7, 0, utf8_literal("Text"), 0, 0, 0, 220, true}).ok() &&
        archive_dispatch(utf8_literal(R"({"op":0,"t":"THREAD_CREATE","d":{"id":"230","guild_id":"7","parent_id":"181","type":11,"owner_id":"1","newly_created":true,"name":"Text thread"}})")) &&
        archive_channel(181)->last_message_id == 220 && !archive_channel(230)->thread_parent_creation_noted &&
        !chat_state_store_note_thread(&f.state, 180, 230, 1),
        "forum reads: text parents and mismatched retained thread parents cannot use post automation");
    const Utf8View malformed[] = {
        utf8_literal(R"({"op":0,"t":"THREAD_CREATE","d":{"id":"240","guild_id":"7","parent_id":"80","type":11,"newly_created":null}})"),
        utf8_literal(R"({"op":0,"t":"THREAD_CREATE","d":{"id":"240","guild_id":"7","parent_id":"80","type":11,"newly_created":"true"}})"),
        utf8_literal(R"({"op":0,"t":"THREAD_CREATE","d":{"id":"240","guild_id":"7","parent_id":"80","type":11,"newly_created":true,"newly_created":false}})")
    };
    for (const auto wire : malformed)
        check(!archive_route(wire, &event, &payload), "forum reads: malformed and duplicate newly_created fields reject before publication");
    check(archive_route(utf8_literal(R"({"op":0,"t":"THREAD_UPDATE","d":{"id":"150","guild_id":"7","type":11}})"), &event, &payload),
        "forum reads: normalized flag-validation fixture routes");
    event.state.channel_event_flags = gateway_client_channel_new_thread;
    check(!archive_apply(event, payload), "forum reads: a forged creation bit on an update cannot alter the parent");
    event.dispatch_kind = GatewayClientDispatchKind::channel_create;
    event.state.channel_event_flags = 2;
    check(!archive_apply(event, payload), "forum reads: unknown normalized channel-event flags reject before mutation");
    check(chat_state_store_remove_channel(&f.state, 120) &&
        chat_state_store_upsert_channel(&f.state, {120, 7, 80, utf8_literal("Recreated"), 0, 0, 11}).ok() &&
        !archive_channel(120)->thread_parent_creation_noted,
        "forum reads: deleting and recreating a retained thread resets its creation receipt");
}
}
void test_thread_archive(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = archive_fixture;
    const bool initialized = chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1},
        {f.channels, 16}, {f.channel_order, 16}, {f.names, sizeof(f.names)}) &&
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
    forum_parent_activity(check);
}
} // namespace maicord
