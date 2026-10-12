#include "maicord/app/chat_state_dispatch.hpp"

namespace maicord {
namespace {
struct ContentPolicyFixture {
    ChatStateStore state{}; GuildRecord guild{}; ChannelRecord channels[8]{};
    RoleRecord roles[4]{}; PermissionOverwriteRecord overwrites[4]{};
    u16 guild_order[1]{}, channel_order[8]{}; u8 names[2048]{};
    u32 structurals[2048]{}; char payload[4096]{};
    bool route(Utf8View wire, GatewayClientEvent* event, ConstSlice<u8>* output) noexcept {
        GatewayEnvelope envelope{}; bool handled = false;
        *event = {}; event->kind = GatewayClientEventKind::dispatch;
        const bool routed = gateway_decode_envelope(wire, {structurals, 2048}, 32, &envelope).ok() &&
            gateway_client_route_state_dispatch(envelope, {structurals, 2048}, 32,
                {payload, sizeof(payload)}, 1, event, output, &handled).ok() && handled;
        if (routed) event->dispatch_payload_bytes = output->count;
        return routed;
    }
    bool apply(Utf8View wire) noexcept {
        GatewayClientEvent event{}; ConstSlice<u8> output{};
        return route(wire, &event, &output) && chat_state_apply_gateway_event(&state, event, output, {}).ok();
    }
};
ContentPolicyFixture policy_fixture{};
} // namespace
void test_channel_content_policy(void (*check)(bool, const char*) noexcept) noexcept {
    static_assert(sizeof(ChannelRecord) == 104 && sizeof(GatewayClientStateMutation) == 168);
    auto& f = policy_fixture;
    check(chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1}, {f.channels, 8}, {f.channel_order, 8},
        {f.names, sizeof(f.names)}) && chat_state_store_bind_permissions(&f.state, {f.roles, 4}, {f.overwrites, 4}),
        "channel content policy uses the existing compact record allocation");
    f.state.self_user_id = 1;
    check(f.apply(utf8_literal(R"({"op":0,"t":"GUILD_CREATE","d":{"id":"7","name":"Guild","owner_id":"1","roles":[{"id":"7","name":"Everyone","permissions":"1024"}],"members":[{"user":{"id":"1"},"roles":[]}],"channels":[{"id":"80","type":0,"name":"Restricted","nsfw":true,"permission_overwrites":[]},{"id":"90","type":0,"name":"Ordinary","nsfw":false,"permission_overwrites":[]}],"threads":[{"id":"81","parent_id":"80","type":11,"name":"Thread","thread_metadata":{"archived":false}}]}})")),
        "guild create carries explicit true and false age metadata through packed mailbox channel records");
    const auto* parent = chat_state_store_find_channel(f.state, 80);
    const auto* ordinary = chat_state_store_find_channel(f.state, 90);
    const auto* thread = chat_state_store_find_channel(f.state, 81);
    check(parent && parent->content_policy_known && (parent->content_policy & kChannelContentAgeRestricted) &&
        ordinary && ordinary->content_policy_known && !ordinary->content_policy && thread &&
        chat_state_store_channel_age_restricted(f.state, *thread), "threads inherit a live parent's age restriction");
    check(f.apply(utf8_literal(R"({"op":0,"t":"THREAD_UPDATE","d":{"id":"81","guild_id":"7","type":11,"name":"Renamed"}})")) &&
        f.apply(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"80","guild_id":"7","type":0,"name":"Renamed parent"}})")),
        "partial parent and thread updates omit age metadata safely");
    parent = chat_state_store_find_channel(f.state, 80); thread = chat_state_store_find_channel(f.state, 81);
    check(parent && parent->content_policy_known && thread && thread->parent_id == 80 &&
        chat_state_store_channel_age_restricted(f.state, *thread), "omitted age and parent metadata preserve inherited restriction");
    check(f.apply(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"80","guild_id":"7","type":0,"rate_limit_per_user":300}})")) &&
        channel_slowmode_seconds(*parent) == 300 &&
        f.apply(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"80","guild_id":"7","type":0,"name":"Slow"}})")) &&
        channel_slowmode_seconds(*parent) == 300 &&
        f.apply(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"80","guild_id":"7","type":0,"rate_limit_per_user":"x"}})")) &&
        channel_slowmode_seconds(*parent) == 300 &&
        f.apply(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"80","guild_id":"7","type":0,"rate_limit_per_user":0}})")) &&
        channel_slowmode_seconds(*parent) == 0,
        "slowmode is stored from channel updates, kept when omitted and ignored when malformed");
    check(f.apply(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"80","guild_id":"7","type":0,"nsfw":false}})")) &&
        !chat_state_store_channel_age_restricted(f.state, *thread), "explicit false removes parent age restriction without mutating every thread");
    const Utf8View malformed[] = {
        utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"80","guild_id":"7","type":0,"nsfw":null}})"),
        utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"80","guild_id":"7","type":0,"nsfw":"true"}})"),
        utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"80","guild_id":"7","type":0,"nsfw":0}})"),
        utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"80","guild_id":"7","type":0,"nsfw":true,"nsfw":false}})")};
    GatewayClientEvent event{}; ConstSlice<u8> output{};
    for (const auto wire : malformed)
        check(!f.route(wire, &event, &output), "invalid or duplicate nonnullable nsfw metadata never publishes");
    check(f.route(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"80","guild_id":"7","type":0,"nsfw":true}})"), &event, &output),
        "individual channel age update routes");
    event.state.content_policy_known = false;
    check(!chat_state_apply_gateway_event(&f.state, event, output, {}).ok() && !parent->content_policy,
        "impossible packed policy without presence cannot overwrite the resident channel");
    check(f.apply(utf8_literal(R"({"op":0,"t":"GUILD_UPDATE","d":{"id":"7","name":"Guild","owner_id":"1","roles":[{"id":"7","name":"Everyone","permissions":"1024"}],"nsfw":false,"nsfw_level":3}})")) &&
        f.guild.nsfw_level_known && f.guild.nsfw_level == 3 && chat_state_store_channel_age_restricted(f.state, *ordinary),
        "documented age-restricted guild classification restricts every channel");
    check(f.apply(utf8_literal(R"({"op":0,"t":"GUILD_UPDATE","d":{"id":"7","name":"Renamed guild","owner_id":"1","roles":[{"id":"7","name":"Everyone","permissions":"1024"}]}})")) &&
        f.guild.nsfw_level == 3, "omitted guild age classification preserves known metadata");
    check(f.apply(utf8_literal(R"({"op":0,"t":"GUILD_UPDATE","d":{"id":"7","name":"Guild","owner_id":"1","roles":[{"id":"7","name":"Everyone","permissions":"1024"}],"nsfw":false}})")) &&
        f.guild.nsfw_level == 3, "a legacy guild boolean cannot clear an omitted documented guild level");
    check(f.apply(utf8_literal(R"({"op":0,"t":"GUILD_UPDATE","d":{"id":"7","name":"Guild","owner_id":"1","roles":[{"id":"7","name":"Everyone","permissions":"1024"}],"nsfw_level":2}})")) &&
        !chat_state_store_channel_age_restricted(f.state, *ordinary), "explicit safe guild level clears inherited guild age restriction");
    check(f.apply(utf8_literal(R"({"op":0,"t":"GUILD_UPDATE","d":{"id":"7","name":"Guild","owner_id":"1","roles":[{"id":"7","name":"Everyone","permissions":"1024"}],"properties":{"nsfw_level":1}}})")) &&
        chat_state_store_channel_age_restricted(f.state, *ordinary), "READY-style guild properties preserve explicit guild classification");
    check(f.apply(utf8_literal(R"({"op":0,"t":"GUILD_UPDATE","d":{"id":"7","name":"Guild","owner_id":"1","roles":[{"id":"7","name":"Everyone","permissions":"1024"}],"nsfw_level":0}})")) &&
        !chat_state_store_channel_age_restricted(f.state, *ordinary), "explicit default guild level clears previous guild age classification");
    check(!f.route(utf8_literal(R"({"op":0,"t":"GUILD_UPDATE","d":{"id":"7","nsfw_level":null}})"), &event, &output) &&
        !f.route(utf8_literal(R"({"op":0,"t":"GUILD_UPDATE","d":{"id":"7","nsfw_level":4}})"), &event, &output) &&
        !f.route(utf8_literal(R"({"op":0,"t":"GUILD_UPDATE","d":{"id":"7","nsfw_level":1,"nsfw_level":3}})"), &event, &output),
        "invalid and duplicate guild classification cannot publish");
}
} // namespace maicord
