#include "../src/app/application_message_history.hpp"
#include "maicord/app/message_history_sync.hpp"
#include "maicord/app/message_view_runtime.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

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

// A busy channel's latest page, synthetic: long content, mentions, authors
// with many roles, embeds, attachments and reactions.
constexpr u64 kBusyChannel = 1543331061273067660ull;
constexpr u64 kBusyUser = 1535269530429825000ull;
constexpr u64 kBusyRole = 1541055452693926100ull;
constexpr u64 kBusyNewest = 1543400000000000000ull;

struct PageWriter {
    char* data;
    u32 capacity;
    u32 used;
    void put(char c) noexcept {
        if (used < capacity)
            data[used] = c;
        ++used;
    }
    void text(const char* value) noexcept {
        while (*value)
            put(*value++);
    }
    void number(u64 value) noexcept {
        char digits[20];
        const u32 count = decimal_write(digits, value);
        for (u32 index = 0; index < count; ++index)
            put(digits[index]);
    }
    void id(u64 value) noexcept {
        put('"');
        number(value);
        put('"');
    }
    Utf8View view() const noexcept { return {data, used <= capacity ? used : 0}; }
};

void busy_message(PageWriter& w, u64 id, u32 index) noexcept {
    const u32 author = index % 7;
    w.text(R"({"type":0,"content":")");
    const u32 start = w.used;
    for (u32 mention = 0; mention < 6; ++mention) {
        w.text("<@");
        w.number(kBusyUser + mention);
        w.text("> ");
    }
    w.text("<@&");
    w.number(kBusyRole + 3);
    w.text("> ");
    const u32 bytes = index % 4 == 0 ? 1900u : 200u;
    while (w.used - start < bytes)
        w.text("busy channel line with caf\xc3\xa9 \xe2\x80\x94 and more words ");
    w.text(R"(","mentions":[)");
    for (u32 mention = 0; mention < 3; ++mention) {
        w.text(mention ? R"(,{"id":)" : R"({"id":)");
        w.id(kBusyUser + mention);
        w.text(R"(,"username":"user)");
        w.number(mention);
        w.text(R"(","avatar":null,"discriminator":"0","public_flags":0,"global_name":"User"})");
    }
    w.text(R"(],"mention_roles":[)");
    w.id(kBusyRole + 3);
    w.text(R"(],"attachments":[{"id":)");
    w.id(id + 1);
    w.text(R"(,"filename":"photo.png","size":48213,"url":"https://cdn.example.invalid/a/photo.png","proxy_url":"https://media.example.invalid/a/photo.png","width":1280,"height":720,"content_type":"image/png"}],)");
    w.text(R"("embeds":[{"type":"rich","url":"https://example.invalid/post","title":"Linked post","description":"A preview of the linked post.","color":5793266,"fields":[{"name":"Field","value":"Value","inline":true}]}],)");
    if (index % 3 == 0)
        w.text(R"("reactions":[{"emoji":{"id":null,"name":"❤"},"count":3,"count_details":{"burst":0,"normal":3},"burst_colors":[],"me_burst":false,"burst_me":false,"me":false,"burst_count":0}],)");
    w.text(R"("timestamp":"2026-10-19T14:30:00.000000+00:00","edited_timestamp":null,"flags":0,"components":[],"id":)");
    w.id(id);
    w.text(R"(,"channel_id":)");
    w.id(kBusyChannel);
    w.text(R"(,"author":{"id":)");
    w.id(kBusyUser + 100 + author);
    w.text(R"(,"username":"author","avatar":"0123456789abcdef0123456789abcdef","discriminator":"0","public_flags":0,"global_name":"Author"},"pinned":false,"mention_everyone":false,"tts":false,"member":{"roles":[)");
    const u32 roles = author == 0 ? 64u : author == 1 ? 16u : 2u;
    for (u32 role = 0; role < roles; ++role) {
        if (role)
            w.put(',');
        w.id(kBusyRole + role);
    }
    w.text(R"(],"nick":"Nick","joined_at":"2025-01-01T00:00:00.000000+00:00","deaf":false,"mute":false,"flags":0}})");
}

// Messages newest first: newest, newest - 16, ...
Utf8View busy_page(PageWriter& w, u64 newest, u32 count) noexcept {
    w.used = 0;
    w.put('[');
    for (u32 index = 0; index < count; ++index) {
        if (index)
            w.put(',');
        busy_message(w, newest - index * 16ull, index);
    }
    w.put(']');
    return w.view();
}

struct BusyWindow {
    app_detail::AccountUiRuntime* ui;
    MessageViewRuntime view;
    MessageEventPump pump;
    MessageHistoryCatchup catchup;
    MessageHistoryOlder older;
};

bool busy_open(BusyWindow& w) noexcept {
    w.ui = static_cast<app_detail::AccountUiRuntime*>(VirtualAlloc(
        nullptr, sizeof(app_detail::AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!w.ui || !app_detail::application_history_decode_storage_initialize(*w.ui) ||
        !message_view_runtime_initialize(&w.view, 100, 1'000, true).ok() ||
        !message_rich_store_initialize_owned_sparse(
            &w.ui->message_rich_store, &w.ui->message_rich_record_storage, kMessageViewItemCapacity,
            Slice<u16>{w.ui->message_rich_order, kMessageViewItemCapacity},
            Slice<u8>{w.ui->message_rich_text, app_detail::kMessageRichTextCapacity}) ||
        !message_view_runtime_bind_rich_store(&w.view, &w.ui->message_rich_store).ok())
        return false;
    w.pump.timeline = &w.view.timeline;
    return true;
}

void busy_close(BusyWindow& w) noexcept {
    message_view_runtime_release(&w.view);
    if (w.ui) {
        message_rich_store_release(&w.ui->message_rich_store);
        app_detail::application_history_decode_storage_release(*w.ui);
        VirtualFree(w.ui, 0, MEM_RELEASE);
    }
}

// What selecting the channel does: an empty window and a latest-page catch-up.
bool busy_select(BusyWindow& w) noexcept {
    MessageHistoryQuery query{};
    w.catchup = {};
    w.older = {};
    w.pump.catchup_required = true;
    w.view.timeline.newer_refetch_required = true;
    return message_timeline_reset(&w.view.timeline) && message_timeline_set_scroll(&w.view.timeline, 0, true) &&
           message_history_catchup_begin(&w.catchup, &w.pump, kBusyChannel, 25) &&
           message_history_catchup_take_query(&w.catchup, &query) &&
           query.position == MessageHistoryPosition::latest && query.limit == 25;
}

// The production catch-up boundary: decode admission, decode, observe, apply.
MessageHistoryRestResult busy_deliver(BusyWindow& w, Utf8View body) noexcept {
    MessageHistoryQuery query{};
    if (w.catchup.state == MessageHistoryCatchupState::request_required)
        message_history_catchup_take_query(&w.catchup, &query);
    RestClientResponse response{};
    response.status_code = 200;
    response.body_bytes = body.bytes;
    MessageHistoryRestResult accepted{};
    MessageHistoryDecodeScratch scratch{};
    if (!app_detail::application_message_history_catchup_response(*w.ui, w.catchup, w.pump, response, body, 0,
                                                                  &accepted, &scratch))
        accepted.error = MessageHistoryRestError::invalid_input;
    return accepted;
}

// A live MESSAGE_CREATE for the channel, decoded like any other message.
bool busy_live_create(BusyWindow& w, PageWriter& page, u64 id) noexcept {
    const Utf8View body = busy_page(page, id, 1);
    MessageHistoryDecodeScratch scratch{};
    if (!app_detail::application_message_decode_scratch(*w.ui, &scratch, body))
        return false;
    const MessageHistoryDecodeResult decoded =
        decode_message_history_page(body, scratch.page_structurals, scratch.message_structurals,
                                    scratch.maximum_depth, scratch.content, scratch.messages,
                                    scratch.mention_users, scratch.rich);
    if (!decoded.ok() || decoded.page.count != 1)
        return false;
    const MessageTimelineApplyResult applied =
        message_timeline_apply_created(&w.view.timeline, scratch.messages[0], MessageContentAvailability::unknown);
    return applied.ok() && applied.kind == GatewayMessageApplyKind::created;
}

// Resident rows ascend by ID with the list in step; returns the row count.
u32 busy_rows(const BusyWindow& w, u64* oldest, u64* newest) noexcept {
    const MessageStore& store = w.view.store;
    if (store.count != w.view.list.count)
        return ~0u;
    u64 previous = 0;
    for (u32 index = 0; index < store.count; ++index) {
        const MessageRecord* record = message_store_resolve(store, message_store_ordered_handle(store, index));
        if (!record || record->id <= previous || record->channel_id != kBusyChannel)
            return ~0u;
        previous = record->id;
        if (index == 0)
            *oldest = record->id;
    }
    *newest = previous;
    return store.count;
}

void history_busy_channel(void (*check)(bool, const char*) noexcept) noexcept {
    BusyWindow w{};
    const u32 capacity = 96 * 1024;
    char* page_bytes = static_cast<char*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    PageWriter page{page_bytes, capacity, 0};
    if (!page_bytes || !busy_open(w)) {
        check(false, "busy channel fixture");
        busy_close(w);
        if (page_bytes)
            VirtualFree(page_bytes, 0, MEM_RELEASE);
        return;
    }
    constexpr u64 oldest_in_page = kBusyNewest - 24 * 16ull;
    u64 oldest = 0, newest = 0;

    // A full ~59 KB latest page lands whole in an empty window.
    check(busy_select(w), "busy page selection starts a latest-page catch-up");
    const Utf8View body = busy_page(page, kBusyNewest, 25);
    check(body.bytes >= 55'000 && body.bytes <= 64'000, "busy page is the size of the reported page");
    MessageHistoryRestResult result = busy_deliver(w, body);
    check(result.accepted() && result.disposition == MessageHistoryRestDisposition::applied &&
              result.decode.page.count == 25 && result.apply.inserted == 25 && !result.apply.resident &&
              !result.apply.skipped && !result.decode.degraded_messages &&
              busy_rows(w, &oldest, &newest) == 25 && oldest == oldest_in_page && newest == kBusyNewest &&
              w.view.store.text_committed > kMessageStoreTextCommitStep &&
              message_rich_store_find(w.ui->message_rich_store, kBusyNewest) &&
              w.catchup.state == MessageHistoryCatchupState::request_required &&
              w.catchup.cursor_after == kBusyNewest,
          "busy 59 KB latest page decodes and inserts all 25 messages");
    result = busy_deliver(w, utf8_literal("[]"));
    check(result.accepted() && w.catchup.state == MessageHistoryCatchupState::complete &&
              message_history_window_retainable(w.catchup, w.older, w.view.timeline, kBusyChannel),
          "busy page follow-up completes and the full window may be retained");

    // A live message lands while the latest page is in flight and the page
    // already contains it: the rest of the page still fills the window.
    check(busy_select(w) && busy_live_create(w, page, kBusyNewest) && w.view.store.count == 1,
          "live create lands in the empty window during the latest request");
    result = busy_deliver(w, busy_page(page, kBusyNewest, 25));
    check(result.accepted() && result.disposition == MessageHistoryRestDisposition::applied &&
              result.decode.page.count == 25 && result.apply.inserted == 24 && result.apply.resident == 1 &&
              busy_rows(w, &oldest, &newest) == 25 && oldest == oldest_in_page && newest == kBusyNewest &&
              w.catchup.cursor_after == kBusyNewest,
          "latest page under an early live create inserts the 24 older messages");
    result = busy_deliver(w, utf8_literal("[]"));
    check(result.accepted() && w.catchup.state == MessageHistoryCatchupState::complete &&
              message_history_window_retainable(w.catchup, w.older, w.view.timeline, kBusyChannel),
          "early live create window completes whole");

    // The live message is newer than the page: every page row is a gap row.
    constexpr u64 live = kBusyNewest + 40;
    check(busy_select(w) && busy_live_create(w, page, live), "newer live create before the latest page");
    result = busy_deliver(w, busy_page(page, kBusyNewest, 25));
    check(result.accepted() && result.apply.inserted == 25 && !result.apply.resident &&
              busy_rows(w, &oldest, &newest) == 26 && oldest == oldest_in_page && newest == live &&
              w.catchup.state == MessageHistoryCatchupState::request_required &&
              w.catchup.cursor_after == kBusyNewest,
          "latest page older than a live create is inserted below it");
    result = busy_deliver(w, busy_page(page, live, 1));
    check(result.accepted() && !result.apply.inserted && result.apply.resident == 1 &&
              busy_rows(w, &oldest, &newest) == 26 && w.catchup.state == MessageHistoryCatchupState::complete,
          "after= follow-up returning the live message completes without duplicating it");

    // The window the regression left behind (one live row, latest page lost)
    // is partial: it is not retained, so the next visit reloads the latest page.
    check(busy_select(w) && busy_live_create(w, page, kBusyNewest), "partial window fixture");
    result = busy_deliver(w, utf8_literal("[]"));
    check(result.accepted() && w.catchup.state == MessageHistoryCatchupState::complete &&
              w.view.store.count == 1 &&
              !message_history_window_retainable(w.catchup, w.older, w.view.timeline, kBusyChannel),
          "a window shorter than a page that has not reached the channel start is not retained");
    w.older.channel_id = kBusyChannel;
    w.older.state = MessageHistoryOlderState::exhausted;
    check(message_history_window_retainable(w.catchup, w.older, w.view.timeline, kBusyChannel),
          "a short window that reached the channel start is retained");
    check(busy_select(w) && w.catchup.cursor_after == 0, "an unretained window's next visit asks for the latest page");
    RestClientResponse forbidden{};
    forbidden.status_code = 403;
    MessageHistoryRestResult failed{};
    MessageHistoryDecodeScratch scratch{};
    check(app_detail::application_message_history_catchup_response(*w.ui, w.catchup, w.pump, forbidden, {}, 0,
                                                                  &failed, &scratch) &&
              !failed.accepted() && w.catchup.state == MessageHistoryCatchupState::failed &&
              !message_history_window_retainable(w.catchup, w.older, w.view.timeline, kBusyChannel),
          "a failed latest page leaves nothing to retain");

    busy_close(w);
    VirtualFree(page_bytes, 0, MEM_RELEASE);
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
    history_busy_channel(check);
}
} // namespace maicord
