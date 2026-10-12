#include "maicord/app/message_history_sync.hpp"
#include "../src/app/application_state.hpp"
#include "../src/app/application_scrolling.hpp"
namespace maicord::app_detail {
extern u32 history_pump_calls;
bool synchronize_navigation_view() noexcept { return true; }
bool application_search_read_blocked() noexcept { return false; }
bool application_pinned_messages_read_blocked() noexcept { return false; }
bool application_thread_browser_read_blocked() noexcept { return false; }
}
namespace maicord {
void test_history_scrollback(void (*check)(bool, const char*) noexcept) noexcept {
    MessageRecord records[6]{}; u32 order[6]{}, heights[6]{}; u64 sums[6]{}; u8 text[512]{};
    MessageStore store{}; UiVariableList list{}; MessageTimeline timeline{};
    const u32 row = ui_extent_units(300.0f);
    check(message_store_initialize(&store, {records, 6}, {order, 6}, {text, 512}) &&
        ui_variable_list_initialize(&list, {heights, 6}, {sums, 6}, 0, row) &&
        message_timeline_initialize(&timeline, &store, &list, row, row, false), "scrollback sliding window fixture");
    constexpr u32 fields = gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_author_id |
        gateway_message_field_timestamp | gateway_message_field_content;
    const auto message = [](u64 id) noexcept { return GatewayMessageEvent{id, 7, 42, id, 0, utf8_literal("m"), 0, fields}; };
    GatewayMessageEvent initial[6]{};
    for (u32 i = 0; i < 6; ++i) initial[i] = message(12 - i);
    check(message_timeline_apply_history_page(&timeline, {initial, 6}, MessageHistoryDirection::newer,
        MessageContentAvailability::available).ok(), "scrollback seeds latest resident messages");
    MessageHistoryOlder older{}; MessageHistoryQuery query{};
    for (u64 newest = 6; newest >= 4; newest -= 2) {
        GatewayMessageEvent page[]{message(newest), message(newest - 1)};
        check(message_timeline_set_scroll(&timeline, 0, false) && message_history_older_demand(&older, &timeline, 7, 2) &&
            message_history_older_take_query(&older, &query) &&
            message_history_older_apply_page(&older, &timeline, {7, newest, newest - 1, 2, 2}, {page, 2},
                MessageContentAvailability::available).ok(), "scrollback older pages evict the loaded newer edge");
    }
    check(timeline.newer_refetch_required && !message_store_find(store, 12).ok(), "scrollback records missing newer history");
    MessageEventPump pump{}; pump.timeline = &timeline;
    MessageHistoryCatchup catchup{};
    for (u64 cursor = 8; cursor <= 10; cursor += 2) {
        check(message_timeline_set_scroll(&timeline, row * 4, false) && message_history_newer_prefetch_needed(timeline) &&
            message_history_catchup_begin(&catchup, &pump, 7, 2) && catchup.incremental &&
            message_history_catchup_take_query(&catchup, &query) && query.position == MessageHistoryPosition::after && query.cursor == cursor,
            "scrolling toward loaded edge requests the next contiguous newer page before the bottom");
        const auto live = message_timeline_apply_created(&timeline, message(100));
        check(live.ok() && live.kind == GatewayMessageApplyKind::deferred && !message_store_find(store, 100).ok(),
            "live arrivals cannot advance the cursor across missing history");
        GatewayMessageEvent page[]{message(cursor + 2), message(cursor + 1)};
        check(message_history_catchup_apply_page(&catchup, &pump, {7, cursor + 2, cursor + 1, 2, 2}, {page, 2},
            MessageContentAvailability::available).ok() && catchup.state == MessageHistoryCatchupState::paused &&
            timeline.newer_refetch_required && !timeline.follow_bottom && timeline.scroll_units == row * 2 &&
            !message_history_newer_prefetch_needed(timeline), "newer page preserves reader anchor and pauses outside prefetch range");
    }
    check(message_store_find(store, 12).ok(), "scrollback returns to messages evicted by older paging");
    check(message_timeline_set_scroll(&timeline, row * 4, false) && message_history_catchup_begin(&catchup, &pump, 7, 2) &&
        message_history_catchup_take_query(&catchup, &query) && query.cursor == 12 &&
        message_history_catchup_apply_page(&catchup, &pump, {}, {}, MessageContentAvailability::available).ok() &&
        !timeline.newer_refetch_required && !pump.catchup_required && catchup.state == MessageHistoryCatchupState::complete,
        "only reaching the actual server tail clears the newer-history gap");
    older.state = MessageHistoryOlderState::exhausted; older.cursor_before = 3;
    check(message_timeline_set_scroll(&timeline, 0, false) && message_history_older_demand(&older, &timeline, 7, 2) &&
        message_history_older_take_query(&older, &query) && query.cursor == 7,
        "older history can be loaded again after forward paging evicts a previously exhausted edge");
    // A live create must also stay deferred when deletion frees a resident slot.
    timeline.newer_refetch_required = true;
    check(message_store_delete(&store, 12) && ui_variable_list_remove(&list, list.count - 1, 1, nullptr), "gap with spare resident capacity fixture");
    const auto live = message_timeline_apply_created(&timeline, message(100));
    check(live.ok() && live.kind == GatewayMessageApplyKind::deferred && store.count == 5,
        "spare capacity cannot allow a live message to skip the unresolved gap");

    // Exercise the real wheel/keyboard/scrollbar adapter with a counted account
    // boundary: forward paging must not wait for unrelated Gateway activity.
    using namespace app_detail;
    const auto saved_timeline = g_app.message_view.timeline;
    auto* saved_scene_timeline = g_app.message_scene.timeline;
    const bool saved_dirty = g_app.message_scene.dirty;
    auto* saved_ui = g_app.account_ui;
    const auto saved_catchup = g_app.message_catchup;
    g_app.account_ui = nullptr;
    g_app.message_view.timeline = timeline;
    g_app.message_scene.timeline = &g_app.message_view.timeline;
    message_timeline_set_scroll(&g_app.message_view.timeline, row, false);
    history_pump_calls = 0;
    check(scroll_message_view_by(row * 2, false) && history_pump_calls == 1 && !g_app.message_view.timeline.follow_bottom,
        "shipping forward scroll immediately pumps missing history while retaining reader position");
    g_app.message_catchup.incremental = true;
    check(scroll_message_view_to(true) && history_pump_calls == 2 && g_app.message_view.timeline.follow_bottom && !g_app.message_catchup.incremental,
        "explicit End immediately requests the live tail and overrides paused forward paging");
    g_app.message_view.timeline = saved_timeline; g_app.message_scene.timeline = saved_scene_timeline;
    g_app.message_scene.dirty = saved_dirty; g_app.account_ui = saved_ui; g_app.message_catchup = saved_catchup;

    // A short latest page already contains the beginning; a short 'after' page
    // only proves the newest edge. Exercise both through real catchup applies.
    for (u32 count = 0; count <= 2; ++count) {
        check(message_timeline_reset(&timeline), "latest exhaustion resets resident history");
        timeline.follow_bottom = true;
        pump = {}; pump.timeline = &timeline; pump.catchup_required = true;
        older = {};
        check(message_history_catchup_begin(&catchup, &pump, 7, 2) &&
            message_history_catchup_take_query(&catchup, &query) &&
            query.position == MessageHistoryPosition::latest,
            "latest exhaustion starts with an unbounded latest query");
        GatewayMessageEvent latest[]{message(count == 2 ? 11 : 10), message(10)};
        const MessageHistoryPage summary = count
            ? MessageHistoryPage{7, latest[0].id, 10, count, count}
            : MessageHistoryPage{};
        check(message_history_catchup_apply_page(&catchup, &pump, summary, {latest, count},
            MessageContentAvailability::available).ok(), "latest exhaustion applies actual page");
        message_history_older_observe_catchup(older, catchup, summary);
        if (count == 1) {
            check(message_history_older_demand(&older, &timeline, 7, 2) &&
                older.state == MessageHistoryOlderState::exhausted && older.cursor_before == 10 &&
                !message_history_older_take_query(&older, &query) && store.count == 1,
                "complete short history suppresses an empty older request without losing content");
            for (u64 id = 11; id <= 16; ++id)
                check(message_timeline_apply_created(&timeline, message(id)).ok(),
                    "live arrivals extend the complete small channel");
            check(!message_store_find(store, 10).ok() &&
                message_history_older_demand(&older, &timeline, 7, 2) &&
                message_history_older_take_query(&older, &query) && query.cursor == 11,
                "eviction reopens history initially proven complete by the latest page");
        } else if (count == 2) {
            check(older.state == MessageHistoryOlderState::idle &&
                message_history_catchup_take_query(&catchup, &query) && query.cursor == 11,
                "a full latest page does not imply the beginning was reached");
            const auto next = message(12);
            const MessageHistoryPage tail{7, 12, 12, 1, 1};
            check(message_history_catchup_apply_page(&catchup, &pump, tail, {&next, 1},
                MessageContentAvailability::available).ok(), "short after page finishes newer catchup");
            message_history_older_observe_catchup(older, catchup, tail);
            check(message_history_older_demand(&older, &timeline, 7, 2) &&
                message_history_older_take_query(&older, &query) && query.cursor == 10,
                "short after response leaves older history available");
        } else {
            check(older.state == MessageHistoryOlderState::idle &&
                message_timeline_apply_created(&timeline, message(10)).ok() &&
                message_history_older_demand(&older, &timeline, 7, 2) &&
                message_history_older_take_query(&older, &query) && query.cursor == 10,
                "empty initial history cannot leave a permanent zero-cursor exhaustion marker");
        }
    }
}
}
