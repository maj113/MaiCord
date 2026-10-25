#include "maicord/app/message_event_pump.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/model/guild_member_store.hpp"

namespace maicord {
void test_navigation_restore(void (*check)(bool, const char*) noexcept) noexcept {
    ChatStateStore state{};
    GuildRecord guilds[4]{};
    ChannelRecord channels[12]{};
    u16 guild_order[4]{}, channel_order[12]{};
    u8 names[4096]{};
    check(chat_state_store_initialize(&state, {guilds, 4}, {guild_order, 4}, {channels, 12},
        {channel_order, 12}, {names, 4096}), "navigation restore initializes caller-owned state");
    state.self_user_id = 1;
    MessageTimeline timeline{};
    MessageEventPump pump{};
    u64 ids[4]{};
    pump.state = &state;
    pump.timeline = &timeline;
    pump.guild_id_scratch = {ids, 4};
    const auto add = [&](u64 id) noexcept {
        return chat_state_store_upsert_channel(&state, {id, 0, 0, {}, 0, 0, 3}).ok();
    };
    const auto ready = [&](u64 time) noexcept {
        GatewayClientEvent event{};
        event.kind = GatewayClientEventKind::ready;
        event.state.id = 1;
        event.timestamp_ms = time;
        return message_event_pump_consume(&pump, event, {}).ok();
    };
    const auto complete = [&](bool supplemental) noexcept {
        GatewayClientEvent event{};
        event.kind = GatewayClientEventKind::dispatch;
        event.dispatch_kind = supplemental ? GatewayClientDispatchKind::supplemental_state_complete :
                                            GatewayClientDispatchKind::ready_state_complete;
        return message_event_pump_consume(&pump, event, {}).ok();
    };
    check(add(10) && add(30) && chat_state_store_select_channel(&state, 30).ok() && ready(100),
        "fresh READY captures the user's selected conversation");
    check(state.restore_channel_id == 30 && state.restore_deadline_ms == 10'100 && !state.selected_channel_id && state.home_selected,
        "removed target remains a bounded selection intent without a stale resident channel");
    check(pump.resync_required, "fresh-session selection restoration requests message history reconciliation");
    check(add(10) && !state.selected_channel_id && complete(false) && state.restore_channel_id == 30,
        "earlier initial channels do not steal selection while the target may arrive in supplemental state");
    check(add(30) && state.selected_channel_id == 30 && !state.restore_channel_id && !state.restore_deadline_ms && complete(true),
        "late target restores selection and cancels its timeout");
    check(ready(200) && add(10) && chat_state_store_select_channel(&state, 10).ok() && add(30) &&
        state.selected_channel_id == 10 && !state.restore_channel_id,
        "explicit channel navigation cancels restoration before the old target arrives");
    check(chat_state_store_select_channel(&state, 30).ok() && ready(300) && add(10) &&
        !chat_state_store_select_channel(&state, 999).ok() && state.restore_channel_id == 30 && complete(true) &&
        state.selected_channel_id == 10 && !state.restore_deadline_ms,
        "failed navigation preserves intent, but completed hydration falls back when the target is absent");
    check(add(30) && chat_state_store_select_channel(&state, 30).ok() && ready(400) && add(10) &&
        !chat_state_store_expire_restore(&state, 10'399) && !state.selected_channel_id &&
        chat_state_store_expire_restore(&state, 10'400) && state.selected_channel_id == 10 &&
        add(30) && state.selected_channel_id == 10,
        "missing supplemental completion expires exactly once and late channels cannot steal focus");
    check(chat_state_store_select_channel(&state, 30).ok() && ready(500) && add(10) &&
        chat_state_store_select_home(&state).ok() && add(30) && state.selected_channel_id == 10 && !state.restore_channel_id,
        "explicit Home selection cancels restoration through the shared navigation API");
    check(chat_state_store_select_channel(&state, 30).ok() && ready(600) && ready(700) && state.restore_channel_id == 30 &&
        state.restore_deadline_ms == 10'700 && add(30) && state.selected_channel_id == 30,
        "another fresh READY retains the original target while replacing partial hydration");

    check(ready(710), "empty restoration fixture starts fresh");
    const u32 before_empty_timeout = state.selection_revision;
    check(chat_state_store_expire_restore(&state, 10'710) && !state.selected_channel_id &&
        state.selection_revision != before_empty_timeout && !state.restore_deadline_ms,
        "empty fallback invalidates the restoring label even without a channel ID change");
    check(add(30) && ready(720), "empty Home cancellation fixture starts fresh");
    const u32 before_empty_home = state.selection_revision;
    check(chat_state_store_select_home(&state).ok() && !state.restore_channel_id && !state.selected_channel_id &&
        state.selection_revision != before_empty_home,
        "explicit empty Home cancels and invalidates a pending restoring label");

    check(chat_state_store_upsert_guild(&state, {20, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() &&
        chat_state_store_upsert_channel(&state, {25, 20, 0, utf8_literal("Chat"), 0, 0, 0}).ok() &&
        chat_state_store_select_channel(&state, 25).ok() && ready(800),
        "guild conversation also participates in reconnect selection restoration");
    check(state.restore_guild_id == 20 && !state.home_selected &&
        chat_state_store_upsert_guild(&state, {20, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() &&
        chat_state_store_upsert_channel(&state, {26, 20, 0, utf8_literal("Other chat"), 0, 0, 0}).ok() &&
        complete(false) && state.selected_channel_id == 26 && !state.restore_channel_id,
        "missing guild target falls back within its guild when initial hydration completes");
}
}
