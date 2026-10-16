#include "../src/app/application_reaction_tooltip.cpp"
#include "application_tooltip.hpp"

namespace maicord::app_detail {
namespace {
u32 tooltip_requests;
void offer_reaction() noexcept {
    const u64 scope = g_app.plugin_account_scope_serial ? g_app.plugin_account_scope_serial
                                                        : g_app.account.status.profile_id;
    track(scope, 8, 43, 0, utf8_literal("like"), MessageReactionType::normal, 2);
}
} // namespace

// Existing PRIMARY admission is injected; the shipping reaction owner still
// chooses whether there is any current interest to submit.
bool pump_message_history() noexcept {
    ReactionTooltipQuery query{};
    if (application_reaction_tooltip_query(&query)) {
        ++tooltip_requests;
        application_reaction_tooltip_submitted(query.epoch);
    }
    return true;
}

void test_reaction_tooltip_owner(void (*check)(bool, const char*) noexcept) noexcept {
    auto& ui = *g_app.account_ui;
    const auto saved_state = ui.chat_state;
    const auto saved_store = g_app.message_view.store;
    const u64 saved_active = ui.active_channel_id;
    const u64 saved_scope = g_app.plugin_account_scope_serial;
    const u64 saved_epoch = g_app.load_trace.channel_epoch;
    const auto saved_request_owner = g_app.message_history_request_owner;
    GuildRecord guilds[1]{};
    ChannelRecord channels[2]{};
    u16 guild_order[1]{}, channel_order[2]{};
    u8 names[256]{}, text[256]{};
    MessageRecord messages[2]{};
    u32 message_order[2]{};
    check(chat_state_store_initialize(&ui.chat_state, {guilds, 1}, {guild_order, 1}, {channels, 2},
                                      {channel_order, 2}, {names, sizeof(names)}) &&
              chat_state_store_upsert_channel(&ui.chat_state, {8, 0, 0, utf8_literal("Direct"), 0, 0, 1})
                  .ok() &&
              message_store_initialize(&g_app.message_view.store, {messages, 2}, {message_order, 2},
                                       {text, sizeof(text)}) &&
              message_store_upsert(&g_app.message_view.store,
                                   {43, 8, 9, 1, 0, utf8_literal("Reaction source"), 0})
                  .ok(),
          "reaction tooltip actual readable channel and resident source initialize");
    ui.active_channel_id = ui.chat_state.selected_channel_id = 8;
    g_app.message_history_request_owner = MessageHistoryRequestOwner::none;
    tooltip_requests = 0;
    offer_reaction();
    check(application_reaction_tooltip_pending(), "reaction hover creates one current read interest");
    application_tooltip_clear();
    ReactionTooltipQuery query{};
    application_reaction_tooltip_pump();
    check(!application_reaction_tooltip_pending() && !application_reaction_tooltip_query(&query) &&
              !tooltip_requests,
          "tooltip clear before admission withdraws interest even with no overlay owner");

    offer_reaction();
    application_reaction_tooltip_pump();
    check(tooltip_requests == 1 && hovered.phase == Phase::submitted,
          "current reaction interest admits once through the existing lane");
    application_reaction_tooltip_leave();
    check(!application_reaction_tooltip_query(&query) && !application_reaction_tooltip_pending(),
          "leaving a submitted reaction cannot revive it when PRIMARY resets");
    RestClientResponse rejected{};
    rejected.status_code = 429;
    rejected.rate_limited = true;
    rejected.retry_at_ms = 9000;
    rejected.rejection.captcha = true;
    application_reaction_tooltip_response(rejected, {}, {}, {});
    application_reaction_tooltip_pump();
    check(tooltip_requests == 1 && hovered.phase == Phase::idle && !hovered.name_count,
          "late retired response drains without publication or another read");

    for (u32 kind = 0; kind < 4; ++kind) {
        offer_reaction();
        application_reaction_tooltip_pump();
        const u32 before = tooltip_requests;
        auto response = rejected;
        response.rejection.captcha = kind == 0;
        response.rate_limited = kind != 1;
        response.retry_at_ms = kind == 2 ? 0 : 9000;
        if (kind == 3)
            response.status_code = 403;
        application_reaction_tooltip_response(response, {}, {}, {});
        application_reaction_tooltip_pump();
        check(!application_reaction_tooltip_pending() && !application_reaction_tooltip_query(&query) &&
                  tooltip_requests == before && hovered.phase == Phase::failed,
              "reaction tooltip challenge, unknown429 and denied reads remain terminal");
        application_reaction_tooltip_leave();
    }

    offer_reaction();
    ++g_app.plugin_account_scope_serial;
    check(!application_reaction_tooltip_query(&query),
          "reaction account retirement blocks old source admission");
    offer_reaction();
    application_reaction_tooltip_pump();
    ++g_app.load_trace.channel_epoch;
    check(!application_reaction_tooltip_query(&query), "reaction channel ABA retires a submitted source");
    offer_reaction();
    auto* channel = const_cast<ChannelRecord*>(chat_state_store_find_channel(ui.chat_state, 8));
    if (channel)
        channel->actions = 0;
    check(channel && !application_reaction_tooltip_pending(),
          "reaction permission withdrawal prevents admission");
    application_reaction_tooltip_leave();
    ui.chat_state = saved_state;
    ui.active_channel_id = saved_active;
    g_app.message_view.store = saved_store;
    g_app.plugin_account_scope_serial = saved_scope;
    g_app.load_trace.channel_epoch = saved_epoch;
    g_app.message_history_request_owner = saved_request_owner;
}
} // namespace maicord::app_detail
