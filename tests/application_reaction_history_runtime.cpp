// Exercise the shipping PRIMARY arbiter, response ownership and composer/history
// admission. The reaction host itself has a separate shipping-host fixture; here
// its public boundary is a deterministic transport owner, never legacy phases.
#include "application_poll_inactive.hpp"
#include "application_member_moderation_inactive.hpp"
#include "application_guild_invite_inactive.hpp"
#include "application_private_channel_inactive.hpp"
#include "application_soundboard_inactive.hpp"
#include "application_dm_profile_inactive.hpp"
#include "../src/app/application_message_history.cpp"
#include "../src/app/application_message_send.cpp"
#include "application_startup_history.hpp"
#include "application_startup_shell.hpp"
#include "maicord/app/startup_shell.hpp"
#include "application_composer_text_input.hpp"
#include "maicord/core/memory.hpp"
#include "application_action_captcha_stubs.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::reaction_history_test {
using namespace app_detail;
u32 failures, reaction_admissions, response_calls, sync_calls;
u32 send_admissions, history_admissions;
u32 scoped_attempts, scoped_admissions, scoped_responses, scoped_response_bytes;
u64 scoped_flight, scoped_response_id;
u16 scoped_response_status;
bool scoped_ready, scoped_busy;
RestClientError scoped_response_error;
u64 reaction_flight, last_send_id, last_history_id, last_response_id, last_history_channel;
MessageHistoryQuery last_history_query{};
u32 queued_reactions;
bool busy_reaction, retired;
u16 details_ready;
u32 details_admissions[9], browser_responses, creation_responses, forum_responses;
u64 details_flights[9];
RestClientError browser_response_error;
RestClientError creation_response_error;
RestClientError last_response_error;
RestClientResponse next_response{};
Utf8View next_body{};
RestClientTakeError next_take_error = RestClientTakeError::none;
bool receipt_ready;
u64 navigation_gate_request;
bool navigation_gate_current = true;
u32 navigation_gate_calls, navigation_history_responses;
MessageHistoryRestDisposition navigation_history_disposition;
u32 navigation_history_message_count;
MessageRecord records[16];
u32 order[16], heights[16];
u64 fenwick[16];
u8 text[8192], draft_text[256], request_text[kMessageComposerMaximumBytes];
char composer_text[256];
MessageDraftRecord drafts[2];
// The timeline binding requires rich capacity to cover all sixteen base slots,
// even when this focused fixture materializes only one resident message.
MessageRichRecord jump_rich_records[16];
u16 jump_rich_order[16];
u8 jump_rich_text[8192];
MessageSceneRowWidget jump_rows[4];
UiWidget jump_widgets[1 + 4 * kMessageSceneWidgetsPerRow];
UiLayoutNode jump_layouts[1 + 4 * kMessageSceneWidgetsPerRow];
void check(bool ok, const char* name) noexcept {
    test_check(&failures, ok, name);
}
RestClientTakeResult WINAPI take(void*, RestClientResponse* response,
                                 Slice<u8> output) noexcept {
    if (!receipt_ready)
        return {0, RestClientTakeError::empty};
    receipt_ready = false;
    *response = next_response;
    if (next_body.bytes > output.count)
        return {0, RestClientTakeError::output_too_small};
    if (next_body.bytes)
        copy_bytes(output.data, next_body.data, next_body.bytes);
    return {next_body.bytes, next_take_error};
}
RestClientSubmitResult WINAPI send(void*, u64 channel, Utf8View content, u64 nonce, u64,
                                   ConstSlice<UploadedAttachment>, ConstSlice<u64>, u64 id,
                                   bool, u32) noexcept {
    check(channel == 7 && content.bytes == 5 && nonce && id,
          "lower send passes immutable composer request and exact ID");
    ++send_admissions;
    last_send_id = id;
    return {};
}
RestClientSubmitResult WINAPI history(void*, u64 channel, const MessageHistoryQuery* query,
                                      u64 id) noexcept {
    check((channel == 7 || channel == 8) && query && query->limit && id,
          "lower history submits an actual prepared query");
    ++history_admissions;
    last_history_id = id;
    last_history_channel = channel;
    last_history_query = *query;
    return {};
}
void receipt(u64 id, u16 status = 204, Utf8View body = {},
             RestClientTakeError error = RestClientTakeError::none) noexcept {
    next_response = {};
    next_response.request_id = id;
    next_response.status_code = status;
    next_response.body_bytes = body.bytes;
    next_body = body;
    next_take_error = error;
    receipt_ready = true;
}
bool queued_send(AccountUiRuntime& ui) noexcept {
    if (message_composer_insert_utf8(&g_app.message_composer, utf8_literal("local")) !=
        MessageComposerResult::changed)
        return false;
    return message_composer_controller_queue_send(&ui.message_composer_controller, true, 11,
                                                  1725012296789ull, clock_monotonic_milliseconds())
        .ok();
}
bool is_primary(MessageHistoryRequestOwner owner, u64 id) noexcept {
    return g_app.message_history_request_owner == owner && g_app.primary_request_id == id && id != 0;
}
bool details_pending(u8 index) noexcept {
    return (details_ready & (1u << index)) && !details_flights[index];
}
bool details_admit(u8 index, u64 id, MessageHistoryRequestOwner owner) noexcept {
    if (!details_pending(index))
        return false;
    if (!id)
        id = next_message_history_request_id();
    details_flights[index] = id;
    ++details_admissions[index];
    g_app.details_request_owner = owner;
    g_app.details_request_id = id;
    return true;
}
RestClientSubmitResult WINAPI thread_submit(void*, const ThreadActionRequest* request,
                                            u64 id) noexcept {
    if (!request)
        return {};
    check(request->thread_id == 21 && id && !details_flights[0],
          "DETAILS thread action retains an immutable receipt");
    details_flights[0] = id;
    ++details_admissions[0];
    return {};
}
void details_fairness(AccountUiRuntime& ui) noexcept {
    auto& state = ui.chat_state;
    check(chat_state_store_upsert_guild(
              &state, {19, utf8_literal("Guild"), GuildMutationKind::create, 11})
                  .ok() &&
              chat_state_store_upsert_channel(&state, {20, 19, 0, utf8_literal("Parent"), 0, 0, 0}).ok(),
          "DETAILS arbitration seeds authoritative guild and parent");
    ChannelInput input{};
    input.id = 21;
    input.guild_id = 19;
    input.parent_id = 20;
    input.type = 11;
    input.name = utf8_literal("Thread");
    input.thread_archived_known = input.thread_locked_known = true;
    check(chat_state_store_upsert_channel(&state, input).ok(),
          "DETAILS arbitration seeds a real thread action source");
    constexpr u64 permissions = kPermissionViewChannel | kPermissionReadMessageHistory |
                                kPermissionSendMessagesInThreads;
    constexpr u64 thread_sources[]{20, 21};
    for (u64 id : thread_sources) {
        auto* value = const_cast<ChannelRecord*>(chat_state_store_find_channel(state, id));
        if (!value)
            return;
        value->permissions_resolved = true;
        value->effective_permissions = permissions;
        value->actions = channel_is_thread(value->type)
                             ? chat_state_store_thread_actions(state, *value)
                             : channel_actions(value->type, permissions, true);
    }
    g_app.account.api.thread_action = thread_submit;
    ApplicationThreadActionTicket ticket{};
    check(application_thread_actions_fetch(21, &ticket) == ApplicationThreadActionEnqueueResult::accepted,
          "DETAILS shipping thread owner queues membership discovery");
    details_ready = 0x1fe;
    details_request_turn = {g_app.plugin_account_scope_serial, 0};
    constexpr MessageHistoryRequestOwner expected[]{
        MessageHistoryRequestOwner::thread_actions,   MessageHistoryRequestOwner::search_navigation,
        MessageHistoryRequestOwner::pinned_messages,  MessageHistoryRequestOwner::thread_browser,
        MessageHistoryRequestOwner::channel_overview, MessageHistoryRequestOwner::user_profile,
        MessageHistoryRequestOwner::commands,         MessageHistoryRequestOwner::thread_creation,
        MessageHistoryRequestOwner::forum_post};
    const u64 now = clock_monotonic_milliseconds();
    u64 previous_id = 0;
    for (u32 turn = 0; turn < 18; ++turn) {
        const u8 index = static_cast<u8>(turn % 9);
        g_app.details_request_owner = MessageHistoryRequestOwner::none;
        g_app.details_request_id = 0;
        admit_details_request(now);
        const u64 id = g_app.details_request_id;
        check(g_app.details_request_owner == expected[index] && id && id > previous_id &&
                  id == details_flights[index],
              "DETAILS continuous demand rotates fairly through all nine exact request owners");
        previous_id = id;
        check(pump_message_history() && g_app.details_request_owner == expected[index] &&
                  g_app.details_request_id == id && details_admissions[index] == turn / 9 + 1,
              "DETAILS occupied receipt prevents duplicate admission during the next pump");
        if (index == 0) {
            RestClientResponse failure{};
            failure.request_id = id;
            failure.status_code = 500;
            check(application_thread_actions_response(failure, {}),
                  "DETAILS real thread receipt drains before requeue");
            details_flights[0] = 0;
            check(application_thread_actions_fetch(21, &ticket) ==
                      ApplicationThreadActionEnqueueResult::accepted,
                  "DETAILS continuous thread demand requeues after exact drain");
        } else
            details_flights[index] = 0;
    }
    details_ready = 0;
    application_thread_actions_release();
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    g_app.details_request_id = 0;
    details_ready = 1u << 3;
    details_request_turn.next = 3;
    admit_details_request(now);
    const u64 browser_id = g_app.details_request_id;
    details_ready = 0; // Closed/cancelled intent keeps the admitted receipt for draining.
    receipt(browser_id + 1000);
    check(!pump_message_history() && !browser_responses && details_flights[3] == browser_id &&
              g_app.details_request_owner == MessageHistoryRequestOwner::thread_browser,
          "DETAILS unrelated response cannot release a closed browser receipt");
    receipt(browser_id, 204, {}, RestClientTakeError::consumer_rejected);
    check(pump_message_history() && browser_responses == 1 && !details_flights[3] &&
              browser_response_error == RestClientError::response &&
              g_app.details_request_owner == MessageHistoryRequestOwner::none && !g_app.details_request_id,
          "DETAILS closed browser drains malformed exact receipt through its typed response handler");
    details_ready = 1u << 7;
    details_request_turn.next = 7;
    admit_details_request(now);
    const u64 creation_id = g_app.details_request_id;
    details_ready = 0;
    receipt(creation_id + 1000);
    check(!pump_message_history() && !creation_responses && details_flights[7] == creation_id &&
              g_app.details_request_owner == MessageHistoryRequestOwner::thread_creation,
          "DETAILS unrelated response cannot release a cancelled creation receipt");
    receipt(creation_id, 204, {}, RestClientTakeError::consumer_rejected);
    check(pump_message_history() && creation_responses == 1 && !details_flights[7] &&
              creation_response_error == RestClientError::response &&
              g_app.details_request_owner == MessageHistoryRequestOwner::none && !g_app.details_request_id,
          "DETAILS cancelled creation drains a malformed exact receipt through its typed handler");
    details_ready = 1u << 8;
    details_request_turn.next = 8;
    admit_details_request(now);
    const u64 forum_id = g_app.details_request_id;
    details_ready = 0;
    receipt(forum_id + 1000);
    check(!pump_message_history() && !forum_responses && details_flights[8] == forum_id &&
              g_app.details_request_owner == MessageHistoryRequestOwner::forum_post,
          "DETAILS unrelated response cannot release a cancelled forum receipt");
    receipt(forum_id, 204, {}, RestClientTakeError::consumer_rejected);
    check(pump_message_history() && forum_responses == 1 && !details_flights[8] &&
              g_app.details_request_owner == MessageHistoryRequestOwner::none && !g_app.details_request_id,
          "DETAILS cancelled forum drains a malformed exact receipt through its typed handler");
}
void navigation_history_gate(AccountUiRuntime& ui) noexcept {
    auto& view = g_app.message_view;
    // Drain the existing reaction/send control through their real PRIMARY path
    // before isolating around-history receipt ownership.
    retired = true;
    queued_reactions = 0;
    if (reaction_flight) {
        receipt(reaction_flight);
        check(pump_message_history(), "jump gate drains previous reaction");
    }
    if (g_app.message_history_request_owner == MessageHistoryRequestOwner::message_send) {
        receipt(
            last_send_id, 200,
            utf8_literal(
                R"({"id":"900","channel_id":"7","author":{"id":"11","username":"self"},"content":"server","timestamp":"2024-08-30T12:00:00Z"})"));
        check(pump_message_history(), "jump gate drains previous immutable send");
    }
    retired = false;
    check(g_app.message_history_request_owner == MessageHistoryRequestOwner::none &&
              chat_state_store_select_channel(&ui.chat_state, 7).ok() && ui.active_channel_id == 7 &&
              ui.chat_state.selected_channel_id == 7,
          "jump gate uses matching live and Core channel IDs despite a stale navigation session");
    g_app.message_catchup = {};
    g_app.message_catchup.state = MessageHistoryCatchupState::complete;
    g_app.message_older = {};
    g_app.message_pump.catchup_required = g_app.message_pump.resync_required = false;
    GatewayMessageEmbed retained_embed{};
    retained_embed.type = GatewayMessageEmbedType::rich;
    retained_embed.title = utf8_literal("Retained rich resident");
    GatewayMessageEvent retained{};
    retained.id = 900;
    retained.channel_id = 7;
    retained.author_id = 11;
    retained.timestamp_ms = 1725019200000ull;
    retained.content = utf8_literal("retained resident");
    retained.fields = gateway_message_field_id | gateway_message_field_channel_id |
                      gateway_message_field_author_id | gateway_message_field_timestamp |
                      gateway_message_field_content | gateway_message_field_embeds;
    retained.embeds = {&retained_embed, 1};
    const bool ready =
        message_timeline_reset(&view.timeline) &&
        message_rich_store_initialize(&ui.message_rich_store, {jump_rich_records, 16},
                                      {jump_rich_order, 16}, {jump_rich_text, sizeof(jump_rich_text)}) &&
        message_timeline_bind_rich_store(&view.timeline, &ui.message_rich_store) &&
        message_timeline_apply_created(&view.timeline, retained).ok() &&
        message_scene_initialize(
            &g_app.message_scene, &view.timeline, {jump_rows, 4},
            {jump_widgets, static_cast<u32>(sizeof(jump_widgets) / sizeof(jump_widgets[0]))},
            {jump_layouts, static_cast<u32>(sizeof(jump_layouts) / sizeof(jump_layouts[0]))}) &&
        message_scene_update(&g_app.message_scene, {0, 0, 400, 80}).ok() &&
        message_scene_focus_message(&g_app.message_scene, 900) &&
        message_timeline_set_scroll(&view.timeline, 0, false);
    check(ready, "jump gate retains a real resident timeline, rich store and focused message scene");
    if (!ready)
        return;
    view.timeline.newer_refetch_required = false;
    g_app.reply_refresh = {7, 899, 900, 898, 0, AttachmentRefreshState::awaiting_response};
    constexpr auto page = utf8_literal(
        R"([{"id":"901","channel_id":"7","author":{"id":"42","username":"new author"},"content":"new target","timestamp":"2024-08-30T12:00:01Z","embeds":[{"type":"rich","title":"New rich target"}],"poll":{"question":{"text":"Historical choice"},"answers":[{"answer_id":7,"poll_media":{"text":"First"}},{"answer_id":42,"poll_media":{"text":"Second"}}],"expiry":"2099-01-01T00:00:00Z","allow_multiselect":true,"layout_type":1}}])");
    g_app.pending_message_jump_channel_id = 7;
    g_app.pending_message_jump_message_id = 901;
    check(pump_message_history() && is_primary(MessageHistoryRequestOwner::message_jump, last_history_id),
          "jump gate admits actual around-history request while IDs remain equal");
    navigation_gate_request = last_history_id;
    navigation_gate_current = false;
    navigation_gate_calls = navigation_history_responses = 0;
    const auto timeline_before = view.timeline;
    const auto store_before = view.store;
    const auto rich_before = ui.message_rich_store;
    const auto users_before = ui.user_store;
    const auto members_before = ui.guild_member_store;
    const auto reply_before = g_app.reply_refresh;
    const auto decode_before = ui.history_decode_storage;
    const auto* context_before = g_app.context_focus;
    const u32 layout_before = g_app.message_scene.layout_passes;
    const u64 records_before = hash64_bytes(kHash64Seed, records, sizeof(records));
    const u64 text_before = hash64_bytes(kHash64Seed, text, sizeof(text));
    const u64 heights_before = hash64_bytes(kHash64Seed, heights, sizeof(heights));
    const u64 fenwick_before = hash64_bytes(kHash64Seed, fenwick, sizeof(fenwick));
    const u64 rich_records_before = hash64_bytes(kHash64Seed, jump_rich_records, sizeof(jump_rich_records));
    const u64 rich_text_before = hash64_bytes(kHash64Seed, jump_rich_text, sizeof(jump_rich_text));
    const u64 decode_bytes_before = hash64_bytes(kHash64Seed, decode_before.arena.base,
                                                 static_cast<u32>(decode_before.arena.committed));
    const auto focused_before = g_app.message_scene.widgets.focused;
    receipt(navigation_gate_request, 200, page);
    check(pump_message_history() && navigation_gate_calls == 1 && navigation_history_responses == 1 &&
              !navigation_history_message_count && !g_app.primary_request_id &&
              !g_app.pending_message_jump_message_id,
          "stale owned jump drains exact receipt through the predecode gate without applying");
    check(bytes_equal(&view.timeline, &timeline_before, sizeof(timeline_before)) &&
              bytes_equal(&view.store, &store_before, sizeof(store_before)) &&
              hash64_bytes(kHash64Seed, records, sizeof(records)) == records_before &&
              hash64_bytes(kHash64Seed, text, sizeof(text)) == text_before &&
              hash64_bytes(kHash64Seed, heights, sizeof(heights)) == heights_before &&
              hash64_bytes(kHash64Seed, fenwick, sizeof(fenwick)) == fenwick_before &&
              g_app.message_scene.focused_message_id == 900 &&
              g_app.message_scene.widgets.focused.slot == focused_before.slot &&
              g_app.message_scene.widgets.focused.generation == focused_before.generation &&
              g_app.message_scene.layout_passes == layout_before && g_app.context_focus == context_before,
          "stale same-channel jump cannot replace resident messages, scroll or focus the destination");
    check(bytes_equal(&ui.history_decode_storage, &decode_before, sizeof(decode_before)) &&
              hash64_bytes(kHash64Seed, decode_before.arena.base,
                           static_cast<u32>(decode_before.arena.committed)) == decode_bytes_before &&
              bytes_equal(&ui.message_rich_store, &rich_before, sizeof(rich_before)) &&
              hash64_bytes(kHash64Seed, jump_rich_records, sizeof(jump_rich_records)) ==
                  rich_records_before &&
              hash64_bytes(kHash64Seed, jump_rich_text, sizeof(jump_rich_text)) == rich_text_before &&
              bytes_equal(&ui.user_store, &users_before, sizeof(users_before)) &&
              bytes_equal(&ui.guild_member_store, &members_before, sizeof(members_before)) &&
              bytes_equal(&g_app.reply_refresh, &reply_before, sizeof(reply_before)) &&
              !user_store_find(ui.user_store, 42) &&
              !message_rich_store_find(ui.message_rich_store, 901) &&
              !message_poll_store_find(ui.message_poll_store, 901),
          "stale jump leaves decode storage, users, members, rich content and reply receipt untouched");
    navigation_gate_current = true;
    g_app.pending_message_jump_channel_id = 7;
    g_app.pending_message_jump_message_id = 901;
    check(pump_message_history() && is_primary(MessageHistoryRequestOwner::message_jump, last_history_id),
          "valid jump control admits the same real around-history query");
    navigation_gate_request = last_history_id;
    receipt(navigation_gate_request, 200, page);
    check(pump_message_history() && navigation_gate_calls == 2 && navigation_history_responses == 2 &&
              navigation_history_disposition == MessageHistoryRestDisposition::applied &&
              navigation_history_message_count == 1 &&
              view.timeline.reset_generation != timeline_before.reset_generation && view.store.count == 1 &&
              message_store_find(view.store, 901).ok() && !message_store_find(view.store, 900).ok() &&
              user_store_find(ui.user_store, 42) &&
              message_rich_store_find(ui.message_rich_store, 901) &&
              g_app.message_scene.focused_message_id == 901 &&
              g_app.context_focus == &g_app.message_scene.widgets && !g_app.pending_message_jump_message_id,
          "current exact jump reaches real decoder/history apply, hydrates rich author and scrolls focused "
          "target");
    const auto* historical_poll = message_poll_store_find(ui.message_poll_store, 901);
    check(historical_poll && historical_poll->answer_count == 2 && !historical_poll->results_known &&
              !historical_poll->answers[0].count_known && !historical_poll->answers[0].me_known &&
              !historical_poll->answers[1].count_known && !historical_poll->answers[1].me_known &&
              !historical_poll->last_gateway_serial,
          "history poll: shipping decoder and accepted history page preserve omitted results as unknown");
    navigation_gate_request = 0;
    message_scene_release(&g_app.message_scene);
    view.timeline.rich_store = nullptr;
    message_rich_store_release(&ui.message_rich_store);
}
void scoped_read_routing() noexcept {
    check(g_app.message_history_request_owner == MessageHistoryRequestOwner::none &&
              !g_app.primary_request_id && !reaction_flight && !queued_reactions,
          "scoped routing starts after existing PRIMARY owners drain");
    g_app.message_catchup.state = MessageHistoryCatchupState::complete;
    g_app.message_pump.catchup_required = g_app.message_pump.resync_required = false;
    g_app.message_view.timeline.newer_refetch_required = false;
    g_app.reply_refresh = {};
    application_message_history_reset_reaction_turn();
    scoped_ready = true;
    const u64 before = g_app.account_ui->message_history_request_id;
    check(pump_message_history() && scoped_admissions == 1 && scoped_attempts == 1 &&
              scoped_flight > before && is_primary(MessageHistoryRequestOwner::scoped_read, scoped_flight),
          "due scope admits the generated exact ID to the shipping PRIMARY owner");
    const u64 first = scoped_flight;
    check(pump_message_history() && scoped_admissions == 1 && scoped_attempts == 1 &&
              is_primary(MessageHistoryRequestOwner::scoped_read, first),
          "empty scope drain preserves the admitted ID without duplicate submission");
    receipt(first + 1000);
    check(!pump_message_history() && !scoped_responses && scoped_flight == first &&
              is_primary(MessageHistoryRequestOwner::scoped_read, first),
          "unrelated receipt cannot release or invoke the scoped owner");
    receipt(first, 204);
    check(pump_message_history() && scoped_responses == 1 && scoped_response_id == first &&
              scoped_response_status == 204 && scoped_response_error == RestClientError::none &&
              !scoped_response_bytes && !scoped_flight &&
              g_app.message_history_request_owner == MessageHistoryRequestOwner::none &&
              !g_app.primary_request_id,
          "exact 2xx receipt reaches scope once and clears only its PRIMARY route");
    check(pump_message_history() && scoped_responses == 1 && scoped_admissions == 1,
          "next pump cannot replay a consumed scoped receipt");

    scoped_ready = true;
    check(pump_message_history() && scoped_admissions == 2 &&
              is_primary(MessageHistoryRequestOwner::scoped_read, scoped_flight),
          "second scope owns an independent exact receipt");
    const u64 mismatched_body = scoped_flight;
    receipt(mismatched_body, 200, utf8_literal("{}"));
    ++next_response.body_bytes;
    check(pump_message_history() && scoped_responses == 2 && scoped_response_id == mismatched_body &&
              scoped_response_error == RestClientError::response && !scoped_response_bytes &&
              !scoped_flight && !g_app.primary_request_id &&
              g_app.message_history_request_owner == MessageHistoryRequestOwner::none,
          "exact scope receipt with mismatched body length reaches typed response failure");

    scoped_ready = true;
    check(pump_message_history() && scoped_admissions == 3 &&
              is_primary(MessageHistoryRequestOwner::scoped_read, scoped_flight),
          "third scope admits before a short transport take");
    const u64 short_take = scoped_flight;
    receipt(short_take, 200, {}, RestClientTakeError::output_too_small);
    next_response.body_bytes = 2;
    check(pump_message_history() && scoped_responses == 3 && scoped_response_id == short_take &&
              scoped_response_error == RestClientError::response && !scoped_response_bytes &&
              !scoped_flight && !g_app.primary_request_id &&
              g_app.message_history_request_owner == MessageHistoryRequestOwner::none,
          "failed short take preserves exact scope ID and supplies empty typed failure");

    scoped_ready = scoped_busy = true;
    queued_reactions = 1;
    const u32 attempts = scoped_attempts;
    const u32 reactions = reaction_admissions;
    check(pump_message_history() && scoped_attempts == attempts + 1 && scoped_admissions == 3 &&
              !scoped_flight && scoped_ready && reaction_admissions == reactions + 1 &&
              is_primary(MessageHistoryRequestOwner::message_reaction, reaction_flight),
          "busy due scope retains intent and lets a lower owner acquire its own PRIMARY ID");
    const u64 lower = reaction_flight;
    check(pump_message_history() && scoped_attempts == attempts + 1 && scoped_responses == 3 &&
              is_primary(MessageHistoryRequestOwner::message_reaction, lower),
          "busy scope cannot steal or resubmit over the lower owner's admitted route");
    scoped_busy = false;
    receipt(lower);
    const u32 lower_responses = response_calls;
    check(pump_message_history() && response_calls == lower_responses + 1 && last_response_id == lower &&
              scoped_responses == 3 && scoped_admissions == 4 &&
              is_primary(MessageHistoryRequestOwner::scoped_read, scoped_flight),
          "lower receipt drains to its owner before retained scope admits in the same pump");
    const u64 resumed = scoped_flight;
    receipt(resumed, 200, utf8_literal("{}"));
    check(pump_message_history() && scoped_responses == 4 && scoped_response_id == resumed &&
              scoped_response_status == 200 && scoped_response_error == RestClientError::none &&
              scoped_response_bytes == 2 && !scoped_flight && !g_app.primary_request_id &&
              g_app.message_history_request_owner == MessageHistoryRequestOwner::none,
          "resumed scope receives its exact successful body after lower-owner completion");
}
// Startup history needs a resident window that holds a whole latest page.
MessageRecord startup_records[32];
u32 startup_order[32], startup_heights[32];
u64 startup_fenwick[32];
u8 startup_text[8192];
constexpr auto kStartupPage = utf8_literal(
    R"([{"id":"1002","channel_id":"7","author":{"id":"11","username":"self"},"content":"b","timestamp":"2024-08-30T12:00:01Z"},)"
    R"({"id":"1001","channel_id":"7","author":{"id":"11","username":"self"},"content":"a","timestamp":"2024-08-30T12:00:00Z"}])");
// READY placed the restoration on channel_id and the channel session began its catch-up.
bool settle_restoration(AccountUiRuntime& ui, u64 channel_id) noexcept {
    auto& state = ui.chat_state;
    state.restore_channel_id = state.restore_guild_id = 0;
    state.selected_channel_id = channel_id;
    ++state.selection_revision;
    ui.observed_selection_revision = state.selection_revision;
    ui.active_channel_id = channel_id;
    reset_value(&g_app.message_catchup);
    reset_value(&g_app.message_older);
    auto& timeline = g_app.message_view.timeline;
    if (!message_timeline_reset(&timeline) || !message_timeline_set_scroll(&timeline, 0, true))
        return false;
    g_app.message_pump.catchup_required = timeline.newer_refetch_required = true;
    // READY with a pending restoration requests a resync.
    g_app.message_pump.resync_required = true;
    return message_history_catchup_begin(&g_app.message_catchup, &g_app.message_pump, channel_id,
                                         kMessageHistoryRequestLimit);
}
// Before READY the PRIMARY lane is not pumped: only submit and attach run.
bool startup_submitted(u64 channel_id) noexcept {
    g_app.account.status.running = false;
    const u32 before = history_admissions;
    const bool submitted = application_startup_history_submit(channel_id);
    application_startup_history_attach();
    return submitted && history_admissions == before + 1 && last_history_id == 1 &&
           last_history_channel == channel_id && last_history_query.cursor == 0 &&
           last_history_query.limit == kMessageHistoryRequestLimit &&
           last_history_query.position == MessageHistoryPosition::latest &&
           is_primary(MessageHistoryRequestOwner::startup_history, 1);
}
void startup_history(AccountUiRuntime& ui) noexcept {
    auto& view = g_app.message_view;
    view.timeline.rich_store = nullptr;
    check(message_store_initialize(&view.store, {startup_records, 32}, {startup_order, 32},
                                   {startup_text, sizeof(startup_text)}) &&
              ui_variable_list_initialize(&view.list, {startup_heights, 32}, {startup_fenwick, 32}, 0, 100) &&
              message_timeline_initialize(&view.timeline, &view.store, &view.list, 100, 100, true) &&
              chat_state_store_upsert_channel(&ui.chat_state, {8, 0, 0, utf8_literal("other"), 0, 0, 1}).ok(),
          "startup history: whole-page window and a second conversation");
    auto* channel = const_cast<ChannelRecord*>(chat_state_store_find_channel(ui.chat_state, 7));
    if (!channel)
        return;
    channel->last_message_id = 1002;
    receipt_ready = false;
    reset_value(&g_app.message_catchup);
    g_app.message_history_request_owner = MessageHistoryRequestOwner::none;
    g_app.primary_request_id = 0;

    void* const instance = g_app.account.instance;
    g_app.account.instance = nullptr;
    u32 admissions = history_admissions;
    check(!application_startup_history_submit(7) && history_admissions == admissions,
          "startup history: nothing is requested without a started account");
    g_app.account.instance = instance;
    check(!application_startup_history_submit(0) && history_admissions == admissions,
          "startup history: nothing is requested without a remembered channel");
    application_startup_history_begin(0xFEEDFACE12345678ull);
    check(history_admissions == admissions &&
              g_app.message_history_request_owner == MessageHistoryRequestOwner::none,
          "startup history: an account without channel memory requests nothing");

    check(startup_submitted(7), "startup history: latest page is submitted before READY on PRIMARY");
    ui.chat_state.restore_channel_id = 7;
    receipt(1, 200, kStartupPage);
    check(pump_message_history() && receipt_ready, "startup history: the response waits for READY");
    g_app.account.status.running = true;
    check(settle_restoration(ui, 7), "startup history: restored channel begins its catch-up");
    admissions = history_admissions;
    check(pump_message_history() && history_admissions == admissions && !receipt_ready &&
              message_store_find(view.store, 1002).ok() && message_store_find(view.store, 1001).ok() &&
              g_app.message_catchup.state == MessageHistoryCatchupState::complete &&
              g_app.message_catchup.requests == 1 &&
              g_app.message_history_request_owner == MessageHistoryRequestOwner::none,
          "startup history: matching restoration applies the page as its catch-up without a second request");

    check(startup_submitted(7), "startup history: resubmitted for the held case");
    g_app.account.status.running = true;
    ui.chat_state.restore_channel_id = 7;
    receipt(1, 200, kStartupPage);
    admissions = history_admissions;
    check(pump_message_history() && !receipt_ready && history_admissions == admissions &&
              g_app.message_history_request_owner == MessageHistoryRequestOwner::none &&
              !application_startup_history_next_wake(),
          "startup history: a page taken before the restoration settles is held and frees PRIMARY");
    check(settle_restoration(ui, 7) && !message_store_find(view.store, 1002).ok() &&
              application_startup_history_next_wake() == 1,
          "startup history: settled restoration wakes the held page");
    check(pump_message_history() && history_admissions == admissions &&
              message_store_find(view.store, 1002).ok() &&
              g_app.message_catchup.state == MessageHistoryCatchupState::complete &&
              !application_startup_history_next_wake(),
          "startup history: held page is applied once as the catch-up");

    check(startup_submitted(8), "startup history: remembered channel differs from the restored one");
    g_app.account.status.running = true;
    check(settle_restoration(ui, 7), "startup history: restoration lands on another channel");
    receipt(1, 200, kStartupPage);
    admissions = history_admissions;
    check(pump_message_history() && history_admissions == admissions + 1 && last_history_channel == 7 &&
              last_history_id != 1 && is_primary(MessageHistoryRequestOwner::catchup, last_history_id) &&
              !message_store_find(view.store, 1002).ok(),
          "startup history: a different selection drops the page and submits the normal request");
    receipt(last_history_id, 200, kStartupPage);
    check(pump_message_history() && message_store_find(view.store, 1002).ok(),
          "startup history: the normal request then loads the selected channel");

    check(startup_submitted(7), "startup history: resubmitted for the failure case");
    g_app.account.status.running = true;
    check(settle_restoration(ui, 7), "startup history: restoration matches before the failure");
    receipt(1, 403, utf8_literal(R"({"code":50001})"));
    admissions = history_admissions;
    check(pump_message_history() && history_admissions == admissions + 1 &&
              is_primary(MessageHistoryRequestOwner::catchup, last_history_id) &&
              g_app.message_catchup.state == MessageHistoryCatchupState::awaiting_page,
          "startup history: a failed request falls back to the normal request");
    receipt(last_history_id, 200, kStartupPage);
    check(pump_message_history() && message_store_find(view.store, 1002).ok(),
          "startup history: the fallback request loads the channel");

    check(startup_submitted(7), "startup history: resubmitted for the stale case");
    g_app.account.status.running = true;
    check(settle_restoration(ui, 7), "startup history: restoration matches a stale page");
    channel->last_message_id = 2000;
    receipt(1, 200, kStartupPage);
    admissions = history_admissions;
    check(pump_message_history() && history_admissions == admissions + 1 &&
              is_primary(MessageHistoryRequestOwner::catchup, last_history_id) &&
              !message_store_find(view.store, 1002).ok(),
          "startup history: a short page older than the channel's last message falls back");
    receipt(last_history_id, 200, kStartupPage);
    pump_message_history();
    channel->last_message_id = 1002;

    check(startup_submitted(7), "startup history: resubmitted for reset");
    application_startup_history_reset();
    check(g_app.message_history_request_owner == MessageHistoryRequestOwner::none && !g_app.primary_request_id,
          "startup history: reset releases the PRIMARY lane it owns");
    g_app.account.status.running = true;
}
// A startup shell for Home on conversation 7, written as the last session would.
bool startup_shell_file(AccountPaths* paths) noexcept {
    const DWORD length = GetTempPathW(512, paths->shell);
    constexpr wchar_t name[] = L"maicord-startup-shell-runtime.shell";
    if (!length || length + sizeof(name) / sizeof(wchar_t) > 512)
        return false;
    copy_bytes(paths->shell + length, name, sizeof(name));
    auto* shell = static_cast<StartupShell*>(
        VirtualAlloc(nullptr, sizeof(StartupShell), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!shell)
        return false;
    shell->profile_id = g_app.account.status.profile_id;
    shell->self_user_id = 11;
    shell->saved_unix_ms = clock_unix_milliseconds();
    shell->channel_id = 7;
    shell->conversations[0] = {7, 11, 1002, 0, 1, 1, {}, {}};
    shell->conversation_count = 1;
    shell->recipients[0] = 12;
    shell->recipient_count = 1;
    shell->users[0].id = 11;
    shell->users[1].id = 12;
    shell->user_count = 2;
    bool ok = startup_shell_add_text(shell, utf8_literal("self"), &shell->users[0].name) &&
              startup_shell_add_text(shell, utf8_literal("peer"), &shell->users[1].name);
    StartupShellKey key{};
    ok = ok && startup_shell_save_file(paths->shell, &key, *shell);
    startup_shell_key_release(&key);
    VirtualFree(shell, 0, MEM_RELEASE);
    return ok;
}
// Before READY the shell takes its channel's prefetched page; READY keeps it.
void startup_shell_page(AccountUiRuntime& ui) noexcept {
    auto& view = g_app.message_view;
    AccountPaths paths{};
    check(startup_shell_file(&paths), "startup shell page: the account's shell is saved");
    const auto shell_shown = [&]() noexcept {
        g_app.account.status.running = false;
        application_startup_shell_begin(paths, g_app.account.status.profile_id);
        return application_startup_shell_apply() && g_app.startup_shell_provisional &&
               ui.chat_state.selected_channel_id == 7 && settle_restoration(ui, 7) && startup_submitted(7);
    };
    check(shell_shown(), "startup shell page: the shell selects its channel before READY");
    receipt(1, 200, kStartupPage);
    u32 admissions = history_admissions;
    check(pump_message_history() && !receipt_ready && history_admissions == admissions &&
              message_store_find(view.store, 1002).ok() && message_store_find(view.store, 1001).ok() &&
              g_app.message_catchup.state == MessageHistoryCatchupState::complete &&
              g_app.message_history_request_owner == MessageHistoryRequestOwner::none,
          "startup shell page: the prefetched page is applied before READY");
    const u32 sends = send_admissions, requests = ui.message_composer_controller.request_count;
    check(queue_message_send() && send_admissions == sends && ui.message_composer_controller.request_count == requests,
          "startup shell page: Enter before READY sends nothing");

    g_app.account.status.running = true;
    g_app.message_pump.resync_required = true;
    application_startup_shell_ready();
    check(!g_app.startup_shell_provisional && message_store_find(view.store, 1002).ok() &&
              g_app.message_catchup.state == MessageHistoryCatchupState::request_required &&
              g_app.message_catchup.cursor_after == 1002,
          "startup shell page: READY keeps the page and asks only for newer messages");
    admissions = history_admissions;
    check(pump_message_history() && history_admissions == admissions + 1 && last_history_channel == 7 &&
              last_history_query.cursor == 1002 && last_history_query.position == MessageHistoryPosition::after &&
              message_store_find(view.store, 1002).ok(),
          "startup shell page: the page stays on screen while newer messages load");
    receipt(last_history_id, 200, utf8_literal("[]"));
    check(pump_message_history() && !g_app.message_pump.resync_required &&
              g_app.message_catchup.state == MessageHistoryCatchupState::complete &&
              message_store_find(view.store, 1002).ok(),
          "startup shell page: the catch-up after READY completes the resync");

    check(shell_shown(), "startup shell page: shown again for a full page");
    receipt(1, 200, kStartupPage);
    check(pump_message_history() && message_store_find(view.store, 1002).ok(),
          "startup shell page: page applied before READY");
    // A full page leaves its catch-up waiting for the next one.
    g_app.message_catchup.state = MessageHistoryCatchupState::request_required;
    g_app.account.status.running = true;
    g_app.message_pump.resync_required = true;
    application_startup_shell_ready();
    admissions = history_admissions;
    check(pump_message_history() && history_admissions == admissions + 1 && last_history_query.cursor == 1002 &&
              last_history_query.position == MessageHistoryPosition::after &&
              message_store_find(view.store, 1002).ok(),
          "startup shell page: READY keeps a full page and asks only for newer messages");
    receipt(last_history_id, 200, utf8_literal("[]"));
    check(pump_message_history() && !g_app.message_pump.resync_required && message_store_find(view.store, 1002).ok(),
          "startup shell page: the full page survives the resync");

    check(shell_shown(), "startup shell page: shown again for the lost-access case");
    receipt(1, 200, kStartupPage);
    check(pump_message_history() && message_store_find(view.store, 1002).ok(),
          "startup shell page: page applied before the lost-access READY");
    g_app.account.status.running = true;
    check(chat_state_store_remove_channel(&ui.chat_state, 7), "startup shell page: READY no longer lists the channel");
    application_startup_shell_ready();
    check(!message_store_find(view.store, 1002).ok() && !view.store.count &&
              g_app.message_catchup.state == MessageHistoryCatchupState::idle,
          "startup shell page: a channel READY took away loses its page at once");
    application_startup_shell_stop();
    DeleteFileW(paths.shell);
    check(chat_state_store_upsert_channel(&ui.chat_state, {7, 0, 0, utf8_literal("chat"), 0, 0, 1}).ok(),
          "startup shell page: conversation restored for teardown");
}
void run() noexcept {
    auto* ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(ui != nullptr, "history runtime owner allocates");
    if (!ui)
        return;
    g_app.account_ui = ui;
    g_app.account.instance = reinterpret_cast<void*>(1);
    g_app.account.status.running = true;
    g_app.account.status.profile_id = 9;
    g_app.plugin_account_scope_serial = 10;
    ui->active_channel_id = 7;
    g_app.account.api.rest_take = take;
    g_app.account.api.message_submit = send;
    g_app.account.api.history_submit = history;
    auto& view = g_app.message_view;
    const bool initialized =
        message_store_initialize(&view.store, {records, 16}, {order, 16}, {text, sizeof(text)}) &&
        ui_variable_list_initialize(&view.list, {heights, 16}, {fenwick, 16}, 0, 100) &&
        message_timeline_initialize(&view.timeline, &view.store, &view.list, 100, 100, true) &&
        message_composer_initialize(&g_app.message_composer, {composer_text, sizeof(composer_text)}) &&
        message_draft_store_initialize(&ui->message_drafts, {drafts, 2},
                                       {draft_text, sizeof(draft_text)}) &&
        message_composer_controller_initialize(&ui->message_composer_controller, &g_app.message_composer,
                                               &ui->message_drafts, &view.timeline,
                                               {request_text, sizeof(request_text)}) &&
        application_history_decode_storage_initialize(*ui) &&
        message_poll_store_initialize_owned(&ui->message_poll_store, 16, 8192) &&
        chat_state_store_initialize(&ui->chat_state, {ui->guild_record_storage, kChatStateGuildCapacity},
                                       {ui->guild_order_storage, kChatStateGuildCapacity},
                                       {ui->channel_record_storage, kChatStateChannelCapacity},
                                       {ui->channel_order_storage, kChatStateChannelCapacity},
                                       {ui->chat_state_name_storage, kChatStateNameBytes}) &&
        user_store_initialize(&ui->user_store, {ui->user_record_storage, kUserCapacity},
                                      {ui->user_order_storage, kUserCapacity},
                                      {ui->user_name_storage, kUserNameBytes}) &&
        guild_member_store_initialize(&ui->guild_member_store,
                                              {ui->guild_member_record_storage, kGuildMemberCapacity},
                                              {ui->guild_member_order_storage, kGuildMemberCapacity});
    check(initialized, "history real timeline/composer/decode storage initialize");
    if (!initialized) {
        g_app.account_ui = nullptr;
        VirtualFree(ui, 0, MEM_RELEASE);
        return;
    }
    message_composer_reset(&g_app.message_composer, 7, false);
    ui->chat_state.self_user_id = 11;
    // A guild-free source is a DM (type 1), not guild text (type 0). Keep
    // production channel admission strict and seed its real private authority.
    check(chat_state_store_upsert_channel(&ui->chat_state, {7, 0, 0, utf8_literal("chat"), 0, 0, 1})
              .ok(),
          "history source channel is authoritative");
    g_app.message_pump.timeline = &view.timeline;
    g_app.message_pump.users = &ui->user_store;
    g_app.message_pump.state = &ui->chat_state;
    g_app.message_pump.members = &ui->guild_member_store;
    g_app.message_catchup.state = MessageHistoryCatchupState::complete;
    queued_reactions = 6;
    check(pump_message_history() && reaction_admissions == 1 &&
              is_primary(MessageHistoryRequestOwner::message_reaction, reaction_flight),
          "ready reaction admits one immutable PRIMARY receipt");
    const u64 first = reaction_flight;
    check(pump_message_history() && reaction_admissions == 1 && reaction_flight == first,
          "empty drain preserves reaction ownership and does not resubmit");
    check(queued_send(*ui), "real composer queues lower send while reactions remain continuous");
    receipt(first);
    check(pump_message_history() && response_calls == 1 && last_response_id == first &&
              send_admissions == 1 && is_primary(MessageHistoryRequestOwner::message_send, last_send_id),
          "continuous reactions yield the very next admission to queued send");
    constexpr auto send_body = utf8_literal(
        R"({"id":"900","channel_id":"7","author":{"id":"11","username":"self"},"content":"server","timestamp":"2024-08-30T12:00:00Z"})");
    receipt(last_send_id, 200, send_body);
    check(pump_message_history() && reaction_admissions == 2 &&
              is_primary(MessageHistoryRequestOwner::message_reaction, reaction_flight) &&
              message_store_find(view.store, 900).ok() && !ui->message_composer_controller.inflight_nonce,
          "shipping send response reconciles then reaction lane resumes");
    g_app.message_pump.catchup_required = true;
    check(message_history_catchup_begin(&g_app.message_catchup, &g_app.message_pump, 7, 50),
          "real passive history demand queues while reaction is admitted");
    receipt(reaction_flight);
    check(pump_message_history() && history_admissions == 1 &&
              is_primary(MessageHistoryRequestOwner::catchup, last_history_id) &&
              g_app.message_catchup.state == MessageHistoryCatchupState::awaiting_page,
          "continuous reactions yield next admission to passive history");
    receipt(last_history_id, 429);
    next_response.rate_limited = true;
    next_response.retry_at_ms = clock_monotonic_milliseconds() + 60000;
    check(pump_message_history() &&
              g_app.message_catchup.state == MessageHistoryCatchupState::waiting_retry &&
              reaction_admissions == 3 &&
              is_primary(MessageHistoryRequestOwner::message_reaction, reaction_flight),
          "history retry floor remains authoritative while ready reactions progress");
    const u32 before_no_lower = reaction_admissions;
    receipt(reaction_flight);
    check(pump_message_history() && reaction_admissions == before_no_lower + 1 &&
              is_primary(MessageHistoryRequestOwner::message_reaction, reaction_flight),
          "yield with no admitted lower owner resumes reaction in same pump without wake polling");
    const u64 exact = reaction_flight;
    const u32 responses = response_calls;
    receipt(exact + 1000);
    check(!pump_message_history() && response_calls == responses && reaction_flight == exact &&
              is_primary(MessageHistoryRequestOwner::message_reaction, exact),
          "unowned REST ID cannot erase current reaction receipt");
    queued_reactions = 1;
    busy_reaction = true;
    receipt(exact, 204, {}, RestClientTakeError::consumer_rejected);
    check(pump_message_history() && last_response_id == exact &&
              last_response_error == RestClientError::response && !reaction_flight &&
              queued_reactions == 1 &&
              g_app.message_history_request_owner == MessageHistoryRequestOwner::none,
          "malformed drain routes exact ID and typed failure; busy newer intent is retained");
    busy_reaction = false;
    check(pump_message_history() && reaction_flight && queued_reactions == 0 &&
              is_primary(MessageHistoryRequestOwner::message_reaction, reaction_flight),
          "retained busy intent is admitted on next ready pump");
    retired = true;
    queued_reactions = 2;
    const u64 draining = reaction_flight;
    check(pump_message_history() && reaction_flight == draining &&
              is_primary(MessageHistoryRequestOwner::message_reaction, draining),
          "scope retirement retains admitted PRIMARY receipt until exact drain");
    receipt(draining);
    check(pump_message_history() && !reaction_flight && !queued_reactions &&
              g_app.message_history_request_owner == MessageHistoryRequestOwner::none,
          "retired receipt drains without admitting a newer retired action");
    retired = false;
    ++g_app.plugin_account_scope_serial;
    queued_reactions = 1;
    check(queued_send(*ui), "new account scope has both ready send and reaction");
    check(pump_message_history() &&
              is_primary(MessageHistoryRequestOwner::message_reaction, reaction_flight),
          "actor scope resets reaction-only fairness turn");
    check(sync_calls != 0, "shipping history pump synchronizes reaction lifecycle");
    details_fairness(*ui);
    navigation_history_gate(*ui);
    scoped_read_routing();
    startup_history(*ui);
    startup_shell_page(*ui);
    application_message_history_reset_reaction_turn();
    application_history_decode_storage_release(*ui);
    message_poll_store_release(&ui->message_poll_store);
    g_app.account_ui = nullptr;
    g_app.account.instance = nullptr;
    VirtualFree(ui, 0, MEM_RELEASE);
}
} // namespace maicord::reaction_history_test

// Inactive independent App owners are isolated; lower composer/history and the
// history PRIMARY loop above are shipping implementations.
namespace maicord::app_detail {
u64 application_channel_prefetch_take(u64) noexcept {
    return 0;
}
void application_channel_prefetch_submitted(u64, u64, u64) noexcept {}
void application_channel_prefetch_skip() noexcept {}
bool application_channel_prefetch_adopt(u64) noexcept {
    return false;
}
bool application_channel_prefetch_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_message_edit_pending_available() noexcept {
    return false;
}
bool application_message_delete_pending_available() noexcept {
    return false;
}
UiSearchScene* application_search_scene() noexcept {
    return nullptr;
}
bool application_search_source_ticket(u32, ApplicationSearchSourceTicket*) noexcept {
    return false;
}
bool application_search_source_resolve(const ApplicationSearchSourceTicket&, bool,
                                       ApplicationSearchSourceView*) noexcept {
    return false;
}
bool application_message_reaction_pending(u64) noexcept {
    return !reaction_history_test::retired && !reaction_history_test::reaction_flight &&
           reaction_history_test::queued_reactions;
}
bool application_message_reaction_submit(u64 id, u64) noexcept {
    if (reaction_history_test::busy_reaction || !application_message_reaction_pending(0))
        return false;
    reaction_history_test::reaction_flight = id;
    --reaction_history_test::queued_reactions;
    ++reaction_history_test::reaction_admissions;
    return true;
}
bool application_message_reaction_response(const RestClientResponse& response) noexcept {
    using namespace reaction_history_test;
    check(response.request_id == reaction_flight && reaction_flight,
          "typed reaction response has exact admitted owner");
    if (response.request_id != reaction_flight || !reaction_flight)
        return false;
    ++response_calls;
    last_response_id = response.request_id;
    last_response_error = response.error;
    reaction_flight = 0;
    if (retired)
        queued_reactions = 0;
    return true;
}
bool application_message_reaction_synchronize() noexcept {
    ++reaction_history_test::sync_calls;
    return true;
}
bool application_commands_tick(u64) noexcept {
    return true;
}
bool application_commands_pending(u64) noexcept {
    return false;
}
bool application_commands_submit(u64) noexcept {
    return reaction_history_test::details_admit(6, 0, MessageHistoryRequestOwner::commands);
}
bool application_commands_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_composer_prepare_send(bool* handled) noexcept {
    *handled = false;
    return true;
}
void application_composer_note_sent(u64) noexcept {}
void application_message_highlight_begin(u64) noexcept {}
bool application_mention_confirmation_request(bool* opened) noexcept {
    *opened = false;
    return true;
}
bool application_commands_send(bool* handled) noexcept {
    *handled = false;
    return true;
}
bool application_plugins_compose(bool* handled) noexcept {
    *handled = false;
    return true;
}
bool application_profile_submit(u64) noexcept {
    return reaction_history_test::details_admit(5, 0, MessageHistoryRequestOwner::user_profile);
}
bool application_profile_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_friends_pending(u64) noexcept {
    return false;
}
bool application_friends_submit(u64, u64) noexcept {
    return false;
}
bool application_friends_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_reaction_tooltip_pending() noexcept {
    return false;
}
bool application_reaction_tooltip_query(ReactionTooltipQuery*) noexcept {
    return false;
}
void application_reaction_tooltip_submitted(u64) noexcept {}
void application_reaction_tooltip_failed() noexcept {}
void application_reaction_tooltip_response(const RestClientResponse&, ConstSlice<u8>, Slice<u32>,
                                           Slice<u32>) noexcept {}
u64 application_read_state_next_wake() noexcept {
    return 0;
}
bool application_read_state_submit(u64, u64) noexcept {
    return false;
}
bool application_read_state_response(const RestClientResponse&, const u8*, u32, u64) noexcept {
    return true;
}
u64 application_scoped_read_next_wake() noexcept {
    return reaction_history_test::scoped_ready && !reaction_history_test::scoped_flight ? 1 : 0;
}
bool application_scoped_read_submit(u64 id, u64 now_ms) noexcept {
    using namespace reaction_history_test;
    ++scoped_attempts;
    check(id && id == g_app.account_ui->message_history_request_id && now_ms &&
              g_app.message_history_request_owner == MessageHistoryRequestOwner::none &&
              !g_app.primary_request_id && !scoped_flight,
          "scope admission boundary receives exact generated ID on an empty PRIMARY route");
    if (!scoped_ready || scoped_busy || scoped_flight)
        return false;
    scoped_flight = id;
    scoped_ready = false;
    ++scoped_admissions;
    return true;
}
bool application_scoped_read_response(const RestClientResponse& response, Utf8View body,
                                      u64 now_ms) noexcept {
    using namespace reaction_history_test;
    check(scoped_flight && response.request_id == scoped_flight && now_ms &&
              g_app.message_history_request_owner == MessageHistoryRequestOwner::none &&
              !g_app.primary_request_id,
          "scope response boundary receives exact admitted ID after PRIMARY route retirement");
    if (!scoped_flight || response.request_id != scoped_flight)
        return false;
    ++scoped_responses;
    scoped_response_id = response.request_id;
    scoped_response_status = response.status_code;
    scoped_response_error = response.error;
    scoped_response_bytes = body.bytes;
    if (body.bytes)
        check(body.bytes == 2 && bytes_equal(body.data, "{}", 2),
              "scope success boundary receives the exact transport body");
    scoped_flight = 0;
    return true;
}
bool application_attachment_url_pending(u64) noexcept {
    return false;
}
bool application_attachment_submit_url(u64, u64, bool* submitted) noexcept {
    *submitted = false;
    return true;
}
bool application_attachment_accept_url(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
void application_attachment_fail_url() noexcept {
}
bool application_attachment_prepare_send(u64, Slice<UploadedAttachment>, Slice<u64>, u32* count,
                                         Slice<u64>) noexcept {
    *count = 0;
    return true;
}
void application_attachment_did_queue(ConstSlice<u64>, u64) noexcept {
}
void application_attachment_complete(u64) noexcept {
}
bool application_search_pending(u64) noexcept {
    return false;
}
bool application_search_submit(u64, u64) noexcept {
    return false;
}
bool application_search_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
void application_search_navigation_synchronize() noexcept {
}
bool application_search_navigation_pending(u64) noexcept {
    return reaction_history_test::details_pending(1);
}
bool application_search_navigation_submit(u64 id, u64) noexcept {
    return reaction_history_test::details_admit(1, id, MessageHistoryRequestOwner::search_navigation);
}
bool application_search_navigation_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_search_navigation_history_admission_failed(u64, u64, bool) noexcept {
    return false;
}
void application_search_navigation_history_submitted(u64, u64, u64) noexcept {
}
bool application_message_navigation_history_current(u64 id, u64 channel, u64 message) noexcept {
    using namespace reaction_history_test;
    if (id != navigation_gate_request || !navigation_gate_request)
        return true;
    ++navigation_gate_calls;
    check(channel == 7 && message == 901, "navigation predecode gate receives exact admitted target IDs");
    return navigation_gate_current;
}
void application_search_navigation_history_response(const RestClientResponse& response,
                                                    const MessageHistoryRestResult& result) noexcept {
    using namespace reaction_history_test;
    if (response.request_id != navigation_gate_request || !navigation_gate_request)
        return;
    ++navigation_history_responses;
    navigation_history_disposition = result.disposition;
    navigation_history_message_count = result.decode.page.count;
    g_app.pending_message_jump_channel_id = g_app.pending_message_jump_message_id = 0;
}
bool application_pinned_messages_pending(u64) noexcept {
    return reaction_history_test::details_pending(2);
}
bool application_pinned_messages_submit(u64 id, u64) noexcept {
    return reaction_history_test::details_admit(2, id, MessageHistoryRequestOwner::pinned_messages);
}
bool application_pinned_messages_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_media_picker_pending(u64) noexcept {
    return false;
}
bool application_media_picker_submit(u64, u64) noexcept {
    return false;
}
bool application_media_picker_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_emoji_preferences_pending(u64) noexcept {
    return false;
}
bool application_emoji_preferences_submit(u64, u64) noexcept {
    return false;
}
bool application_emoji_preferences_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
void application_emoji_preferences_response_drained() noexcept {
}
Slice<u8> application_emoji_preferences_response_buffer() noexcept {
    return {};
}
ApplicationEmojiUse application_emoji_preferences_scan(Utf8View) noexcept {
    return {};
}
void application_emoji_preferences_track_message(const ApplicationEmojiUse&) noexcept {
}
bool application_gif_favorites_pending(u64) noexcept {
    return false;
}
bool application_gif_favorites_submit(u64, u64) noexcept {
    return false;
}
bool application_gif_favorites_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
void application_gif_favorites_response_drained() noexcept {
}
Slice<u8> application_gif_favorites_response_buffer() noexcept {
    return {};
}
u64 application_channel_overview_next_wake() noexcept {
    return 0;
}
bool application_channel_overview_submit(u64) noexcept {
    return reaction_history_test::details_admit(4, 0, MessageHistoryRequestOwner::channel_overview);
}
bool application_channel_overview_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_thread_browser_pending(u64) noexcept {
    return reaction_history_test::details_pending(3);
}
bool application_thread_browser_submit(u64 id, u64) noexcept {
    return reaction_history_test::details_admit(3, id, MessageHistoryRequestOwner::thread_browser);
}
bool application_thread_browser_synchronize() noexcept {
    return true;
}
bool application_thread_browser_read_blocked() noexcept {
    return false;
}
bool application_thread_creation_pending(u64) noexcept {
    return reaction_history_test::details_pending(7);
}
bool application_forum_post_pending(u64) noexcept {
    return reaction_history_test::details_pending(8);
}
bool application_forum_post_submit(u64 id, u64) noexcept {
    return reaction_history_test::details_admit(8, id, MessageHistoryRequestOwner::forum_post);
}
bool application_forum_post_synchronize() noexcept {
    return true;
}
bool application_forum_post_response(const RestClientResponse& response, Utf8View) noexcept {
    using namespace reaction_history_test;
    check(response.request_id == details_flights[8] && details_flights[8],
          "DETAILS forum response preserves exact ownership");
    if (!details_flights[8] || response.request_id != details_flights[8])
        return false;
    check(response.error == RestClientError::response,
          "DETAILS malformed forum receipt has typed response error");
    ++forum_responses;
    details_flights[8] = 0;
    return true;
}
bool application_thread_creation_submit(u64 id, u64) noexcept {
    return reaction_history_test::details_admit(7, id, MessageHistoryRequestOwner::thread_creation);
}
bool application_thread_creation_synchronize() noexcept {
    return true;
}
bool application_thread_creation_response(const RestClientResponse& response, Utf8View) noexcept {
    using namespace reaction_history_test;
    check(response.request_id == details_flights[7] && details_flights[7],
          "DETAILS creation response preserves exact ownership");
    if (!details_flights[7] || response.request_id != details_flights[7])
        return false;
    ++creation_responses;
    creation_response_error = response.error;
    details_flights[7] = 0;
    return true;
}
bool application_thread_browser_response(const RestClientResponse& response, Utf8View) noexcept {
    using namespace reaction_history_test;
    check(response.request_id == details_flights[3] && details_flights[3],
          "DETAILS browser response preserves exact ownership");
    if (!details_flights[3] || response.request_id != details_flights[3])
        return false;
    ++browser_responses;
    browser_response_error = response.error;
    details_flights[3] = 0;
    return true;
}
bool application_self_status_pending(u64) noexcept {
    return false;
}
bool application_self_status_submit(u64, u64) noexcept {
    return false;
}
bool application_self_status_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_user_settings_pending(u64) noexcept {
    return false;
}
bool application_user_settings_submit(u64, u64) noexcept {
    return false;
}
bool application_user_settings_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_account_settings_pending(u64) noexcept {
    return false;
}
bool application_account_settings_submit(u64, u64) noexcept {
    return false;
}
bool application_account_settings_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_notification_controls_pending(u64) noexcept {
    return false;
}
bool application_notification_controls_submit(u64, u64) noexcept {
    return false;
}
bool application_notification_controls_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_custom_status_pending(u64) noexcept {
    return false;
}
bool application_custom_status_submit(u64, u64) noexcept {
    return false;
}
bool application_custom_status_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_activity_sharing_pending(u64) noexcept {
    return false;
}
bool application_activity_sharing_submit(u64, u64) noexcept {
    return false;
}
bool application_activity_sharing_response(const RestClientResponse&, Utf8View) noexcept {
    return false;
}
bool application_inbox_pending(u64) noexcept {
    return false;
}
bool application_inbox_submit(u64, u64) noexcept {
    return false;
}
bool application_inbox_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_stream_preview_pending(u64) noexcept {
    return false;
}
bool application_stream_preview_submit(u64, u64) noexcept {
    return false;
}
bool application_stream_preview_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_stage_pending(u64) noexcept {
    return false;
}
bool application_stage_submit(u64, u64) noexcept {
    return false;
}
bool application_stage_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_message_components_pending(u64) noexcept {
    return false;
}
void application_message_components_tick(u64) noexcept {}
bool application_message_components_submit(u64, u64) noexcept {
    return false;
}
bool application_message_components_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_guild_folders_pending(u64) noexcept {
    return false;
}
u32 application_guild_folders_capture(Slice<u8>, Slice<u64>, u32* expanded) noexcept {
    *expanded = 0;
    return 0;
}
bool application_guild_folders_restore(ConstSlice<u8>, ConstSlice<u64>) noexcept {
    return true;
}
bool application_account_logout_blocked() noexcept {
    return false;
}
bool application_guild_folders_submit(u64, u64) noexcept {
    return false;
}
bool application_invite_cards_pending(u64) noexcept {
    return false;
}
bool application_invite_cards_submit(u64, u64) noexcept {
    return false;
}
bool application_invite_cards_response(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_message_forward_synchronize() noexcept {
    return true;
}
void application_message_forward_completion(const MessageSendCompletion&) noexcept {
}
bool refresh_account_text(bool) noexcept {
    return true;
}
bool refresh_account_frame(bool) noexcept {
    return true;
}
bool refresh_message_composer_ui() noexcept {
    return true;
}
bool synchronize_navigation_view() noexcept {
    return true;
}
void reset_message_delete() noexcept {
    g_app.message_delete = {};
}
void reset_message_edit() noexcept {
    g_app.message_edit.phase = MessageEditRequestPhase::idle;
}
bool accept_message_delete_response(const RestClientResponse&) noexcept {
    return true;
}
bool accept_message_edit_response(const RestClientResponse&) noexcept {
    return true;
}
bool queue_message_edit() noexcept {
    return true;
}
void application_message_mutation_notice_failed(ApplicationMessageMutationKind, u64, u64,
                                                const RestClientResponse&) noexcept {
}
void fail_private_conversation() noexcept {
}
bool accept_private_conversation(const RestClientResponse&, Utf8View) noexcept {
    return true;
}
bool application_composer_text_input_composing() noexcept {
    return false;
}
} // namespace maicord::app_detail
extern "C" __declspec(noreturn) void WINAPI maicord_application_reaction_history_runtime_entry() noexcept {
    maicord::reaction_history_test::run();
    ExitProcess(maicord::reaction_history_test::failures ? 1 : 0);
}
