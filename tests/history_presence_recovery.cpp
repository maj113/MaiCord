#include "maicord/app/message_history_sync.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool route(Utf8View json, GatewayClientEvent* event, Slice<u8> output = {}, u32* bytes = nullptr) noexcept {
    u32 scratch[512]{};
    char text[512]{};
    u64 ids[8]{};
    GatewayEnvelope envelope{};
    ConstSlice<u8> payload{};
    const bool valid = gateway_decode_envelope(json, {scratch, 512}, 32, &envelope).ok() &&
        gateway_client_route_dispatch(envelope, {scratch, 512}, 32, {text, 512}, {ids, 8}, event, &payload).ok() &&
        event->dispatch_kind == GatewayClientDispatchKind::presence_update;
    if(!valid)return false;
    if(bytes){if(payload.count>output.count)return false;copy_bytes(output.data,payload.data,payload.count);*bytes=payload.count;}
    UserAppearance appearance{};
    return user_appearance_read(payload,event->presence.user_id,event->presence.guild_id,&appearance);
}

void history_retry_floors(void (*check)(bool, const char*) noexcept, MessageTimeline& timeline,
                          MessageEventPump& pump) noexcept {
    const u32 resident = timeline.store->count;
    const u64 scroll = timeline.scroll_units;
    struct Sample {
        u64 now_ms, server_at, expected;
        u8 attempts;
        RestClientError transport_error;
    };
    const Sample samples[]{
        {10'000, 70'000, 70'000, 0, RestClientError::none},
        {10'000, 10'100, 10'500, 0, RestClientError::none},
        {10'000, 9'000, 10'500, 0, RestClientError::none},
        {10'000, 0, 10'500, 0, RestClientError::none},
        {10'000, 10'100, 14'000, 3, RestClientError::none},
        {~0ull - 100, 1, ~0ull, 6, RestClientError::none},
        {10'000, ~0ull, ~0ull, 0, RestClientError::none},
        {10'000, 70'000, 10'500, 0, RestClientError::receive},
    };
    for (const auto& sample : samples) {
        RestClientResponse unavailable{};
        unavailable.status_code = 503;
        unavailable.error = sample.transport_error;
        unavailable.retry_at_ms = sample.server_at;
        MessageHistoryOlder paging{};
        paging.state = MessageHistoryOlderState::awaiting_page;
        paging.channel_id = 7;
        paging.cursor_before = 100;
        paging.request_limit = 2;
        paging.requests = 1;
        paging.transient_retries = sample.attempts;
        MessageHistoryCatchup recovery{};
        recovery.state = MessageHistoryCatchupState::awaiting_page;
        recovery.channel_id = 7;
        recovery.cursor_after = 300;
        recovery.request_limit = 2;
        recovery.requests = 1;
        recovery.transient_retries = sample.attempts;
        const auto paged =
            message_history_older_accept_rest(&paging, &timeline, nullptr, nullptr, nullptr, unavailable,
                                              {}, {}, MessageContentAvailability::unknown, sample.now_ms);
        const auto caught = message_history_catchup_accept_rest(
            &recovery, &pump, unavailable, {}, {}, MessageContentAvailability::unknown, sample.now_ms);
        const u32 attempts = minimum(static_cast<u32>(sample.attempts) + 1u, 6u);
        check(paged.accepted() && caught.accepted() &&
                  paged.disposition == MessageHistoryRestDisposition::retry_scheduled &&
                  caught.disposition == MessageHistoryRestDisposition::retry_scheduled &&
                  paging.retry_at_ms == sample.expected && recovery.retry_at_ms == sample.expected &&
                  paging.transient_retries == attempts && recovery.transient_retries == attempts &&
                  paging.failures == 1 && recovery.failures == 1 && paging.requests == 1 &&
                  recovery.requests == 1 && paging.cursor_before == 100 && recovery.cursor_after == 300 &&
                  timeline.store->count == resident && timeline.scroll_units == scroll,
              "history eligible retries honor server floor without shortening local backoff or moving "
              "cursors");
        const auto duplicate_page =
            message_history_older_accept_rest(&paging, &timeline, nullptr, nullptr, nullptr, unavailable,
                                              {}, {}, MessageContentAvailability::unknown, sample.now_ms);
        const auto duplicate_catchup = message_history_catchup_accept_rest(
            &recovery, &pump, unavailable, {}, {}, MessageContentAvailability::unknown, sample.now_ms);
        check(duplicate_page.error == MessageHistoryRestError::invalid_input &&
                  duplicate_catchup.error == MessageHistoryRestError::invalid_input &&
                  paging.failures == 1 && recovery.failures == 1 && paging.retry_at_ms == sample.expected &&
                  recovery.retry_at_ms == sample.expected,
              "history duplicate completion cannot change a waiting retry");
        MessageHistoryQuery older_query{}, newer_query{};
        check(!message_history_older_tick(&paging, sample.expected - 1) &&
                  !message_history_catchup_tick(&recovery, sample.expected - 1) &&
                  message_history_older_tick(&paging, sample.expected) &&
                  message_history_catchup_tick(&recovery, sample.expected) &&
                  message_history_older_take_query(&paging, &older_query) &&
                  message_history_catchup_take_query(&recovery, &newer_query) &&
                  older_query.position == MessageHistoryPosition::before && older_query.cursor == 100 &&
                  newer_query.position == MessageHistoryPosition::after && newer_query.cursor == 300 &&
                  paging.requests == 2 && recovery.requests == 2,
              "history retry becomes due exactly at the combined deadline in both directions");
    }
    struct Terminal {
        RestClientError error;
        u16 status;
        ApiRejection rejection;
    };
    const Terminal terminals[]{
        {RestClientError::none, 503, {0, false, true}}, {RestClientError::none, 503, {40002, true, false}},
        {RestClientError::none, 429, {0, false, true}}, {RestClientError::none, 403, {}},
        {RestClientError::cancelled, 503, {}},
    };
    for (const auto& terminal : terminals) {
        RestClientResponse response{};
        response.error = terminal.error;
        response.status_code = terminal.status;
        response.rejection = terminal.rejection;
        response.rate_limited = terminal.status == 429;
        response.retry_at_ms = 70'000;
        MessageHistoryOlder paging{};
        paging.state = MessageHistoryOlderState::awaiting_page;
        paging.channel_id = 7;
        paging.cursor_before = 100;
        paging.request_limit = 2;
        MessageHistoryCatchup recovery{};
        recovery.state = MessageHistoryCatchupState::awaiting_page;
        recovery.channel_id = 7;
        recovery.cursor_after = 300;
        recovery.request_limit = 2;
        const auto paged =
            message_history_older_accept_rest(&paging, &timeline, nullptr, nullptr, nullptr, response, {},
                                              {}, MessageContentAvailability::unknown, 10'000);
        const auto caught = message_history_catchup_accept_rest(
            &recovery, &pump, response, {}, {}, MessageContentAvailability::unknown, 10'000);
        check(
            !paged.accepted() && !caught.accepted() && paging.state == MessageHistoryOlderState::failed &&
                recovery.state == MessageHistoryCatchupState::failed && !paging.retry_at_ms &&
                !recovery.retry_at_ms && !paging.transient_retries && !recovery.transient_retries &&
                paging.failures == 1 && recovery.failures == 1 && timeline.store->count == resident &&
                timeline.scroll_units == scroll,
            "history server deadlines never enable retries for challenges cancellation or terminal status");
    }
}
}

void test_history_presence_recovery(void (*check)(bool, const char*) noexcept) noexcept {
    // No messages or members may be mutated by a failed HTTP attempt.
    MessageRecord records[4]{}; u32 order[4]{}; u8 text[128]{};
    u32 heights[4]{}; u64 fenwick[4]{};
    MessageStore store{}; UiVariableList list{}; MessageTimeline timeline{};
    check(message_store_initialize(&store, {records, 4}, {order, 4}, {text, 128}) &&
        message_store_upsert(&store, MessageInput{300, 7, 11, 300, 0, utf8_literal("keep me"), 0}).ok() &&
        ui_variable_list_initialize(&list, {heights, 4}, {fenwick, 4}, 1, 100) &&
        message_timeline_initialize(&timeline, &store, &list, 100, 50, false) &&
        message_timeline_set_scroll(&timeline, 25, false), "history_retry_fixture");
    MessageEventPump pump{}; pump.timeline = &timeline;
    MessageHistoryOlder older{};
    MessageHistoryQuery query{};
    check(message_history_older_demand(&older, &timeline, 7, 2) &&
        message_history_older_take_query(&older, &query), "history_retry_first_request");
    RestClientResponse response{};
    response.error = RestClientError::receive;
    response.reused_connection = true;
    u64 now = 10'000;
    const u64 delays[] = {500, 1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (u64 delay : delays) {
        const auto accepted = message_history_older_accept_rest(&older, &timeline, nullptr, nullptr, nullptr,
            response, {}, {}, MessageContentAvailability::unknown, now);
        check(accepted.accepted() && accepted.disposition == MessageHistoryRestDisposition::retry_scheduled &&
            older.state == MessageHistoryOlderState::waiting_retry && older.retry_at_ms == now + delay &&
            older.cursor_before == 300 && store.count == 1 && timeline.scroll_units == 25 &&
            !message_history_older_tick(&older, now + delay - 1) &&
            message_history_older_demand(&older, &timeline, 7, 2) && older.demands == 1 &&
            message_history_older_tick(&older, now + delay) &&
            message_history_older_take_query(&older, &query) && query.cursor == 300,
            "history_retry_backoff_keeps_cursor_anchor_and_deduplicates_scroll_demand");
        now += delay;
    }
    response.error = RestClientError::none;
    response.status_code = 503;
    const auto unavailable = message_history_older_accept_rest(&older, &timeline, nullptr, nullptr, nullptr,
        response, {}, {}, MessageContentAvailability::unknown, now);
    check(unavailable.accepted() && older.last_failure == MessageHistoryCatchupFailure::http_status &&
        older.retry_at_ms == now + 30'000 && message_history_older_tick(&older, now + 30'000) &&
        message_history_older_take_query(&older, &query), "history_retry_server_unavailable");

    // Complete the same older-page request after reconnect, through the actual
    // REST decoder, then demand the next page without switching channels.
    constexpr auto body = utf8_literal(R"([{"id":"200","channel_id":"7","author":{"id":"11"},"content":"older b","timestamp":"2026-09-05T10:00:00Z"},{"id":"100","channel_id":"7","author":{"id":"11"},"content":"older a","timestamp":"2026-09-05T09:59:59Z"}])");
    u32 page_indices[256]{}; u32 message_indices[256]{}; char decoded_text[256]{};
    GatewayMessageEvent messages[2]{}; GatewayMessageNamedUser names[4]{};
    MessageHistoryDecodeScratch scratch{{page_indices, 256}, {message_indices, 256}, {decoded_text, 256},
        {messages, 2}, {names, 4}, {}, 32};
    response.status_code = 200; response.body_bytes = body.bytes;
    response.retry_at_ms = now + 60'000;
    const auto recovered = message_history_older_accept_rest(&older, &timeline, nullptr, nullptr, nullptr,
        response, body, scratch, MessageContentAvailability::available, now + 30'001);
    check(recovered.accepted() && recovered.disposition == MessageHistoryRestDisposition::applied &&
        store.count == 3 && older.transient_retries == 0 && older.state == MessageHistoryOlderState::idle &&
        timeline.scroll_units == 225 && message_history_older_demand(&older, &timeline, 7, 2) &&
        message_history_older_take_query(&older, &query) && query.cursor == 100,
        "history_retry_success_preserves_anchor_and_next_scroll_loads_more");

    const auto before = store.count;
    const RestClientError fatal_errors[] = {RestClientError::cancelled,
        RestClientError::response, RestClientError::request};
    for (auto error : fatal_errors) {
        older.state = MessageHistoryOlderState::awaiting_page;
        response = {}; response.error = error;
        const auto failed = message_history_older_accept_rest(&older, &timeline, nullptr, nullptr, nullptr,
            response, {}, {}, MessageContentAvailability::unknown, now);
        check(!failed.accepted() && older.state == MessageHistoryOlderState::failed && store.count == before,
            "history_cancel_and_malformed_response_do_not_retry");
    }
    const u16 fatal_statuses[] = {401, 403, 404};
    for (u16 status : fatal_statuses) {
        older.state = MessageHistoryOlderState::awaiting_page;
        response = {}; response.status_code = status;
        const auto failed = message_history_older_accept_rest(&older, &timeline, nullptr, nullptr, nullptr,
            response, {}, {}, MessageContentAvailability::unknown, now);
        check(failed.error == MessageHistoryRestError::http_status && older.state == MessageHistoryOlderState::failed,
            "history_permissions_and_missing_channel_do_not_retry");
    }
    MessageHistoryCatchup catchup{};
    catchup.state = MessageHistoryCatchupState::awaiting_page;
    catchup.channel_id = 7; catchup.cursor_after = 300; catchup.request_limit = 2;
    response = {}; response.error = RestClientError::timeout;
    auto result = message_history_catchup_accept_rest(&catchup, &pump, response, {}, {},
        MessageContentAvailability::unknown, 20'000);
    check(result.accepted() && catchup.retry_at_ms == 20'500 && catchup.cursor_after == 300 &&
        message_history_catchup_tick(&catchup, 20'500) && message_history_catchup_take_query(&catchup, &query) &&
        query.position == MessageHistoryPosition::after && query.cursor == 300,
        "history_catchup_timeout_keeps_after_cursor");
    history_retry_floors(check, timeline, pump);

    u8 http_head[kRestMaximumHeaderBytes]{}; u8 http_body[128]{}; RestResponse http{};
    check(rest_response_initialize(&http, {http_head, kRestMaximumHeaderBytes}, {http_body, 128}), "history_eof_fixture");
    const auto eof = rest_response_feed(&http, {}, true);
    check(eof.error == RestResponseError::unexpected_close &&
        rest_client_response_error(eof.error) == RestClientError::receive &&
        rest_client_response_error(RestResponseError::body_capacity) == RestClientError::response,
        "history_idle_tls_eof_is_retryable_but_capacity_failure_is_not");
    check(rest_response_initialize(&http, {http_head, kRestMaximumHeaderBytes}, {http_body, 128}),
        "history_truncated_http_fixture");
    constexpr auto truncated = utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n[");
    const auto partial = rest_response_feed(&http,
        {reinterpret_cast<const u8*>(truncated.data), truncated.bytes}, true);
    check(partial.error == RestResponseError::unexpected_close &&
        rest_client_response_error(partial.error) == RestClientError::receive,
        "history_mid_response_disconnect_is_retryable");

    GuildMemberRecord members[4]{}; u32 member_order[4]{};
    GuildMemberStore member_store{}; ChatStateStore state{};
    check(guild_member_store_initialize(&member_store, {members, 4}, {member_order, 4}) &&
        guild_member_store_upsert(&member_store, state, 7, 11, utf8_literal("nickname"), {}, false,
            GuildMemberStatus::online, 3, 77).ok() &&
        guild_member_store_upsert(&member_store, state, 8, 11, utf8_literal("other guild"), {}, false,
            GuildMemberStatus::online, 5, 88).ok(), "presence_fixture");
    pump.members = &member_store;
    const GuildMemberRecord original = *guild_member_store_find(member_store, 7, 11);
    struct Sample { Utf8View json; GuildMemberStatus expected; };
    const Sample samples[] = {
        {utf8_literal(R"({"op":0,"t":"PRESENCE_UPDATE","d":{"guild_id":"7","user":{"id":"11"},"status":"idle","activities":[]}})"), GuildMemberStatus::idle},
        {utf8_literal(R"({"op":0,"t":"PRESENCE_UPDATE","d":{"guild_id":"7","user":{"id":"11"},"status":"dnd","client_status":{"desktop":"dnd"}}})"), GuildMemberStatus::dnd},
        {utf8_literal(R"({"op":0,"t":"PRESENCE_UPDATE","d":{"guild_id":"7","user":{"id":"11"},"status":"offline"}})"), GuildMemberStatus::offline},
        {utf8_literal(R"({"op":0,"t":"PRESENCE_UPDATE","d":{"guild_id":"7","user":{"id":"11"},"status":"online"}})"), GuildMemberStatus::online}};
    for (const auto& sample : samples) {
        GatewayClientEvent event{};
        u8 payload[512];u32 bytes=0;
        check(route(sample.json, &event,{payload,512},&bytes), "presence_partial_user_routes_without_username");
        const auto updated = message_event_pump_consume(&pump, event, {payload,bytes});
        auto retained = *guild_member_store_find(member_store, 7, 11);
        check(updated.ok() && updated.repaint && retained.status == sample.expected &&
            guild_member_store_find(member_store, 8, 11)->status == GuildMemberStatus::online,
            "presence_event_updates_retained_state_and_requests_repaint_in_correct_guild");
        retained.status = original.status; retained.revision = original.revision;
        check(bytes_equal(&retained, &original, sizeof(original)), "presence_preserves_profile_roles_and_list_position");
        check(!message_event_pump_consume(&pump, event, {payload,bytes}).repaint, "presence_identical_status_does_not_repaint");
    }
    const Utf8View invalid[] = {
        utf8_literal(R"({"op":0,"t":"PRESENCE_UPDATE","d":{"guild_id":"7","user":{},"status":"idle"}})"),
        utf8_literal(R"({"op":0,"t":"PRESENCE_UPDATE","d":{"guild_id":"7","user":{"id":"11"},"status":42}})"),
        utf8_literal(R"({"op":0,"t":"PRESENCE_UPDATE","d":{"user":{"id":"11","id":"12"},"status":"idle"}})"),
        utf8_literal(R"({"op":0,"t":"PRESENCE_UPDATE","d":{"user":{"id":"11"},"status":"idle","status":"dnd"}})")};
    for (auto json : invalid) {
        GatewayClientEvent event{};
        check(!route(json, &event), "presence_invalid_or_duplicate_fields_rejected");
    }
    GatewayClientEvent friend_event{};
    check(route(utf8_literal(R"({"op":0,"t":"PRESENCE_UPDATE","d":{"user":{"id":"11"},"status":"idle"}})"), &friend_event) &&
        !message_event_pump_consume(&pump, friend_event, {}).repaint &&
        member_store.count == 2, "presence_friend_scope_does_not_overwrite_guild_presence");
    GatewayClientEvent unknown_event{};
    check(route(utf8_literal(R"({"op":0,"t":"PRESENCE_UPDATE","d":{"guild_id":"7","user":{"id":"11"},"status":"future-status"}})"), &unknown_event) &&
        !message_event_pump_consume(&pump, unknown_event, {}).repaint &&
        guild_member_store_find(member_store, 7, 11)->status == GuildMemberStatus::online,
        "presence_unknown_future_status_preserves_known_state");
    unknown_event.presence.user_id = 999;
    unknown_event.presence.status = GatewayClientMemberListStatus::idle;
    check(!message_event_pump_consume(&pump, unknown_event, {}).repaint && member_store.count == 2,
        "presence_uncached_user_does_not_allocate_or_evict_members");
}
} // namespace maicord
