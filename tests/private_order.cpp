#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/model/private_channel_store.hpp"

namespace maicord {
void test_private_order(void (*check)(bool, const char*) noexcept) noexcept {
    ChatStateStore state{};
    GuildRecord guilds[2]{};
    ChannelRecord channels[8]{};
    u16 guild_order[2]{}, channel_order[8]{};
    u8 names[2048]{};
    PrivateChannelStore metadata{};
    check(chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 8},
        {channel_order, 8}, {names, 2048}) && private_channel_store_initialize_owned(&metadata, 8, 32),
        "private order initializes bounded metadata");
    state.private_channels = &metadata;
    state.self_user_id = 1;
    u32 scratch[1024]{};
    char packet[2048]{};
    u64 ids[8]{};
    const auto apply = [&](Utf8View json) noexcept {
        GatewayEnvelope envelope{};
        GatewayClientEvent event{};
        ConstSlice<u8> payload{};
        return gateway_decode_envelope(json, {scratch, 1024}, 64, &envelope).ok() &&
            gateway_client_route_dispatch(envelope, {scratch, 1024}, 64, {packet, 2048}, {ids, 8}, 1, &event, &payload).ok() &&
            chat_state_apply_gateway_event(&state, event, payload, {ids, 8}).ok();
    };
    check(apply(utf8_literal(R"({"op":0,"t":"CHANNEL_CREATE","s":1,"d":{"id":"41943040","type":1}})")) &&
        apply(utf8_literal(R"({"op":0,"t":"CHANNEL_CREATE","s":2,"d":{"id":"83886080","type":3}})")) &&
        apply(utf8_literal(R"({"op":0,"t":"CHANNEL_CREATE","s":3,"d":{"id":"125829120","type":1}})")),
        "private channels receive insertion-order stamps through the gateway pipeline");
    NavigationView view{};
    GuildViewRow guild_rows[2]{};
    ChannelViewRow rows[8]{};
    u16 ordering[8]{};
    u8 visibility[8]{};
    ChannelSortKey keys[8]{};
    check(navigation_view_initialize(&view, {guild_rows, 2}, {rows, 8}, {ordering, 8}, {visibility, 8}, {keys, 8}),
        "private order initializes the shared navigation projection");
    const auto ordered = [&](u64 first, u64 second, u64 third) noexcept {
        return navigation_view_rebuild(&view, state).ok() && view.channel_count == 3 &&
            rows[0].id == first && rows[1].id == second && rows[2].id == third;
    };
    check(ordered(125829120, 83886080, 41943040), "unmessaged private channels sort by creation timestamp descending");
    check(chat_state_store_note_message(&state, 41943040, 838860801, 2, false, false) &&
        ordered(41943040, 125829120, 83886080), "new message activity moves a private row without changing identity");
    check(chat_state_store_note_message(&state, 83886080, 838860802, 2, false, false) &&
        ordered(83886080, 41943040, 125829120), "same-millisecond activity puts the latest updated conversation first");
    check(apply(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","s":4,"d":{"id":"41943040","type":1}})")) &&
        ordered(41943040, 83886080, 125829120), "equal timestamp channel updates reinsert ahead of existing ties");
    const u64 sequence = metadata.sort_sequence;
    check(!chat_state_store_note_message(&state, 41943040, 838860801, 2, false, false) && metadata.sort_sequence == sequence,
        "duplicate message observations cannot reorder the list");
    check(chat_state_store_select_channel(&state, 83886080).ok() &&
        chat_state_store_upsert_guild(&state, {20, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() &&
        chat_state_store_upsert_channel(&state, {25, 20, 0, utf8_literal("Chat"), 0, 0, 0}).ok() &&
        chat_state_store_select_guild(&state, 20).ok() && chat_state_store_select_home(&state).ok() &&
        state.selected_channel_id == 83886080,
        "Home restores its last selected conversation instead of jumping to the newest row");
    check(chat_state_store_select_guild(&state, 20).ok() && chat_state_store_remove_channel(&state, 83886080) &&
        chat_state_store_select_home(&state).ok() && state.selected_channel_id == 41943040,
        "closing the remembered conversation makes Home fall back to recent activity");
    check(chat_state_store_select_guild(&state, 20).ok(), "private projection can leave Home");
    state.selected_guild_id = 0;
    state.selected_channel_id = 0;
    check(navigation_view_rebuild(&view, state).ok() && !view.channel_count,
        "a temporarily absent guild selection does not leak private rows into guild scope");
    private_channel_store_release(&metadata);
}
}
