#include "maicord/app/message_event_pump.hpp"
#include "maicord/app/chat_state_dispatch.hpp"

namespace maicord {
void test_private_snapshot(void (*check)(bool, const char*) noexcept) noexcept {
    ChatStateStore state{};
    GuildRecord guilds[2]{};
    ChannelRecord channels[8]{};
    u16 guild_order[2]{}, channel_order[8]{};
    u8 names[2048]{};
    PrivateChannelStore metadata{};
    check(chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 8},
        {channel_order, 8}, {names, 2048}) && private_channel_store_initialize_owned(&metadata, 8, 32),
        "private snapshot stores initialize");
    state.private_channels = &metadata;
    state.self_user_id = 1;
    const u64 peers[] = {2, 3};
    const PrivateNicknameInput nick{2, utf8_literal("Local peer")};
    const auto add_private = [&](u64 id) noexcept {
        return chat_state_store_upsert_channel(&state, {id, 0, 0, utf8_literal("Group"), 0, 0, 3}).ok() &&
            private_channel_store_replace(&metadata, {id, 2, {peers, 2}, {}, true, true, false}) &&
            private_channel_store_replace_nicknames(&metadata, id, {&nick, 1});
    };
    check(chat_state_store_upsert_guild(&state, {20, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() &&
        chat_state_store_upsert_channel(&state, {25, 20, 0, utf8_literal("Guild channel"), 0, 0, 0}).ok() &&
        add_private(10) && add_private(30) && chat_state_store_select_home(&state).ok(),
        "interleaved private and guild identities populate snapshot fixture");
    const auto* old = chat_state_store_find_channel(state, 10);
    const u32 old_slot = static_cast<u32>(old - state.channels);
    const u16 old_generation = old->generation;
    const u32 old_waste = state.name_wasted;
    const usize committed = metadata.nickname_backing.committed;
    void* nickname_base = metadata.nickname_backing.base;
    check(chat_state_store_clear_private_channels(&state) == 2 && state.channel_count == 1 &&
        chat_state_store_find_channel(state, 25) && !chat_state_store_find_channel(state, 10) &&
        !chat_state_store_find_channel(state, 30) && !metadata.count && !metadata.recipient_count && !metadata.nickname_count,
        "bulk reset compacts only private channels and clears all private metadata");
    check(state.home_selected && !state.selected_channel_id && state.channel_order[1] == 0xFFFF &&
        !state.channels[old_slot].occupied && state.channels[old_slot].generation != old_generation &&
        state.name_wasted == old_waste + 10 && metadata.nickname_backing.base == nickname_base &&
        metadata.nickname_backing.committed == committed,
        "bulk reset invalidates identities and names while retaining Home and committed backing");
    const u32 mutation = state.channel_mutations, selection = state.selection_revision, revision = metadata.revision;
    check(chat_state_store_clear_private_channels(&state) == 0 && state.channel_mutations == mutation &&
        state.selection_revision == selection && metadata.revision == revision,
        "empty private reset has no invalidation churn");
    check(add_private(10) && add_private(30), "private storage refills after bulk reset");

    MessageTimeline timeline{};
    MessageEventPump pump{};
    u64 ids[2]{};
    pump.timeline = &timeline;
    pump.state = &state;
    pump.guild_id_scratch = {ids, 2};
    pump.private_state_incomplete = true;
    pump.private_recovery_requested = true;
    GatewayClientEvent resumed{};
    resumed.kind = GatewayClientEventKind::resumed;
    check(message_event_pump_consume(&pump, resumed, {}).ok() && metadata.count == 2 && pump.private_state_incomplete && pump.private_recovery_requested,
        "RESUMED preserves private state and its unresolved diagnostic");
    GatewayClientEvent ready{};
    ready.kind = GatewayClientEventKind::ready;
    ready.state.id = 99;
    check(!message_event_pump_consume(&pump, ready, {}).ok() && metadata.count == 2 && pump.private_state_incomplete,
        "wrong-account READY cannot erase retained conversations");
    ready.state.id = 1;
    ready.state.record_count = 1;
    check(!message_event_pump_consume(&pump, ready, {}).ok() && metadata.count == 2,
        "invalid READY packet is rejected before resetting private state");
    ready.state.record_count = 0;
    pump.resync_required = false;
    const auto applied = message_event_pump_consume(&pump, ready, {});
    check(applied.ok() && applied.repaint && !state.channel_count && !state.guild_count &&
        !metadata.count && !metadata.nickname_count && state.home_selected && !pump.private_state_incomplete && !pump.private_recovery_requested,
        "fresh empty READY starts a clean private projection and diagnostic generation");
    check(add_private(40) && chat_state_store_finish_restore(&state) && state.channel_count == 1 && state.selected_channel_id == 40 &&
        !chat_state_store_find_channel(state, 10) && !chat_state_store_find_channel(state, 30),
        "new READY and supplemental hydration cannot resurrect omitted old conversations");
    GatewayClientEvent incomplete{};
    pump.resync_required = false; // The application's fresh-session history reset completed.
    incomplete.kind = GatewayClientEventKind::dispatch;
    incomplete.dispatch_kind = GatewayClientDispatchKind::private_state_incomplete;
    check(message_event_pump_consume(&pump, incomplete, {}).ok() && pump.private_state_incomplete && pump.resync_required &&
        state.channel_count == 1 && metadata.count == 1 && state.selected_channel_id == 40,
        "new projection failures request authority resync without resetting retained private conversations");
    private_channel_store_release(&metadata);
}
}
