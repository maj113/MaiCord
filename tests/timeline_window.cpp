// Bottom-anchored timeline geometry (with the official channel-beginning block)
// and the detached history window: around -> after paging to the present,
// merge with the live tail, Jump to Present, and live creates while detached.
#include "maicord/app/message_history_sync.hpp"
#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/scene.hpp"

namespace maicord {
namespace {
constexpr u32 kRows = 16;
constexpr u64 kChannel = 7;
constexpr u32 kFields = gateway_message_field_id | gateway_message_field_channel_id |
                        gateway_message_field_author_id | gateway_message_field_timestamp |
                        gateway_message_field_content;
// One author per message keeps every row an ungrouped header row.
GatewayMessageEvent message(u64 id) noexcept {
    return GatewayMessageEvent{id, kChannel, 40 + id, 1'700'000'000'000ull + id * 3'600'000ull, 0,
                               utf8_literal("m"), 0, kFields};
}
MessageRecord g_records[kRows];
u32 g_order[kRows];
u8 g_text[4096];
u32 g_units[kRows];
u64 g_fenwick[kRows];
UiWidget g_widgets[kRows * kMessageSceneWidgetsPerRow + 1];
UiLayoutNode g_layout[kRows * kMessageSceneWidgetsPerRow + 1];

bool close_to(float a, float b) noexcept {
    return a - b < 0.51f && b - a < 0.51f;
}
const UiLayoutNode* row_node(const MessageScene& scene, u32 source_index) noexcept {
    const auto* row = message_scene_find_row(scene, source_index);
    return row ? message_scene_layout_node(scene, row->widget) : nullptr;
}
// The newest row ends at the viewport bottom and rows are contiguous.
bool anchored(const MessageScene& scene, const MessageTimeline& timeline) noexcept {
    const u32 count = timeline.list->count;
    if (!count)
        return true;
    const auto* last = row_node(scene, count - 1);
    if (!last || !close_to(last->rectangle.bottom, scene.viewport.bottom))
        return false;
    for (u32 index = 1; index < count; ++index) {
        const auto* above = row_node(scene, index - 1);
        const auto* below = row_node(scene, index);
        if (above && below && !close_to(above->rectangle.bottom, below->rectangle.top))
            return false;
    }
    return true;
}

void test_anchor_geometry(void (*check)(bool, const char*) noexcept) noexcept {
    MessageStore store{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageScene scene{};
    const u32 row_units = ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height);
    const UiRect tall{0.0f, 0.0f, 720.0f, 1'000.0f};
    const bool fixture =
        message_store_initialize(&store, {g_records, kRows}, {g_order, kRows}, {g_text, sizeof(g_text)}) &&
        ui_variable_list_initialize(&list, {g_units, kRows}, {g_fenwick, kRows}, 0, row_units) &&
        message_timeline_initialize(&timeline, &store, &list, row_units, ui_extent_units(1'000.0f), true) &&
        message_scene_initialize_owned(&scene, &timeline, kRows, 4,
                                       {g_widgets, kRows * kMessageSceneWidgetsPerRow + 1},
                                       {g_layout, kRows * kMessageSceneWidgetsPerRow + 1});
    check(fixture, "timeline anchor fixture");
    if (!fixture)
        return;
    scene.bottom_anchored = true;
    MessageSceneIntroGeometry none{};
    check(message_scene_update(&scene, tall).ok() && scene.lead_dips == 0.0f && scene.count == 0 &&
              !message_scene_intro_geometry(scene, &none),
          "an empty channel without a known beginning lays out nothing");

    MessageSceneIntro intro{};
    check(message_scene_intro_compose(&intro, MessageSceneIntroKind::channel, kChannel, utf8_literal("general"),
                                      {}, 0, false) &&
              utf8_equal(message_scene_intro_header(intro), utf8_literal("Welcome to #general!")) &&
              utf8_equal(message_scene_intro_description(intro),
                         utf8_literal("This is the start of the #general channel.")),
          "text channel beginning uses the official welcome strings");
    MessageSceneIntroGeometry geometry{};
    check(message_scene_set_intro(&scene, &intro, nullptr) && !message_scene_set_intro(&scene, &intro, nullptr) &&
              message_scene_update(&scene, tall).ok() && scene.intro_height > 0.0f &&
              message_scene_intro_geometry(scene, &geometry) && close_to(geometry.area.bottom, tall.bottom) &&
              close_to(geometry.area.top, tall.bottom - scene.intro_height) && close_to(ui_rect_width(geometry.icon), 68.0f),
          "an empty channel's beginning sits against the composer");

    check(message_timeline_apply_created(&timeline, message(10)).ok() && message_scene_update(&scene, tall).ok() &&
              anchored(scene, timeline) && scene.lead_dips > 0.0f,
          "one message is bottom-anchored");
    const auto* first = row_node(scene, 0);
    const auto* first_row = message_scene_find_row(scene, 0);
    check(first && first_row && message_scene_intro_geometry(scene, &geometry) &&
              close_to(geometry.area.top, first->rectangle.top) &&
              close_to(message_scene_message_rectangle(scene, *first_row).top, first->rectangle.top + scene.intro_height) &&
              close_to(first->rectangle.top, tall.bottom - ui_extent_dips(list.total_units)),
          "the beginning block heads the first message inside the anchored content");
    check(message_scene_hit_test(scene, 200.0f, geometry.header.top + 4.0f).message_id == 0 &&
              message_scene_hit_test(scene, 200.0f, first->rectangle.bottom - 4.0f).message_id == 10,
          "the beginning block is not part of the first message's hit area");

    for (u64 id = 11; id <= 12; ++id)
        message_timeline_apply_created(&timeline, message(id));
    check(message_scene_update(&scene, tall).ok() && list.count == 3 && anchored(scene, timeline),
          "a few messages stay bottom-anchored");
    const UiRect short_view{0.0f, 0.0f, 720.0f, 600.0f};
    check(message_scene_update(&scene, short_view).ok() && anchored(scene, timeline) &&
              message_scene_update(&scene, tall).ok() && anchored(scene, timeline),
          "resizing keeps sparse content against the bottom edge");
    const float before = row_node(scene, 0)->rectangle.top;
    check(message_timeline_apply_created(&timeline, message(13)).ok() && message_scene_update(&scene, tall).ok() &&
              anchored(scene, timeline) && row_node(scene, 0)->rectangle.top < before,
          "a new message appends at the bottom and lifts earlier rows");

    for (u64 id = 14; id < 10 + kRows; ++id)
        message_timeline_apply_created(&timeline, message(id));
    const UiRect small{0.0f, 0.0f, 720.0f, 300.0f};
    check(message_scene_update(&scene, small).ok() && scene.lead_dips == 0.0f && timeline.follow_bottom &&
              list.total_units > timeline.viewport_units && anchored(scene, timeline),
          "content taller than the viewport scrolls normally from the live edge");

    check(message_scene_set_intro(&scene, nullptr, nullptr) && message_scene_update(&scene, tall).ok() &&
              scene.intro_height == 0.0f && anchored(scene, timeline),
          "hiding the beginning block keeps the anchored layout");
    scene.bottom_anchored = false;
    scene.dirty = true;
    const UiRect huge{0.0f, 0.0f, 720.0f, 4'000.0f};
    check(message_scene_update(&scene, huge).ok() && row_node(scene, 0) &&
              close_to(row_node(scene, 0)->rectangle.top, 0.0f),
          "unanchored scenes (search results) keep top-down layout");

    MessageSceneIntro dm{};
    check(message_scene_intro_compose(&dm, MessageSceneIntroKind::direct_message, 9, utf8_literal("Rob"),
                                      utf8_literal("robin1"), 99, false) &&
              utf8_equal(message_scene_intro_subtitle(dm), utf8_literal("robin1")) &&
              utf8_equal(message_scene_intro_description(dm),
                         utf8_literal("This is the beginning of your direct message history with Rob.")) &&
              dm.description_span_count == 1 && dm.description_spans[0].byte_count == 3 && dm.user_id == 99,
          "direct message beginning names the recipient in bold");
    MessageSceneIntro group{};
    MessageSceneIntro thread{};
    check(message_scene_intro_compose(&group, MessageSceneIntroKind::group_direct_message, 9, utf8_literal("Crew"),
                                      {}, 0, false) &&
              utf8_equal(message_scene_intro_description(group),
                         utf8_literal("Welcome to the beginning of the Crew group.")) &&
              message_scene_intro_compose(&thread, MessageSceneIntroKind::thread, 9, utf8_literal("Plans"), {}, 0,
                                          true) &&
              utf8_equal(message_scene_intro_header(thread), utf8_literal("Plans")) &&
              utf8_equal(message_scene_intro_description(thread),
                         utf8_literal("@mention users to add them to this private thread.")),
          "group DM and private thread beginnings match the official client");
    message_scene_release(&scene);
}

bool target(const MessageHistoryQuery& query, Utf8View expected) noexcept {
    u8 output[128];
    const auto written = write_message_history_target(kChannel, query, {output, sizeof(output)});
    return written.error == MessageHistoryTargetError::none &&
           utf8_equal({reinterpret_cast<const char*>(output), written.bytes}, expected);
}
MessageHistoryPage summary(ConstSlice<GatewayMessageEvent> page) noexcept {
    return page.count ? MessageHistoryPage{kChannel, page[0].id, page[page.count - 1].id, page.count,
                                           page.count}
                      : MessageHistoryPage{};
}
// Newest-first page of ids [newest, newest - count).
u32 page_of(GatewayMessageEvent* page, u64 newest, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index)
        page[index] = message(newest - index);
    return count;
}

void test_detached_window(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr u32 limit = 4;
    check(message_history_jump_edges({}, 6, limit, 0).more_before &&
              message_history_jump_edges({}, 6, limit, 0).more_after,
          "jump edges stay open without a page");
    GatewayMessageEvent around[limit];
    page_of(around, 8, limit); // 8 7 6 5, target 6
    const auto middle = message_history_jump_edges({around, limit}, 6, limit, 30);
    const auto at_start = message_history_jump_edges({around, limit}, 5, limit, 30);
    const auto at_present = message_history_jump_edges({around, limit}, 8, limit, 8);
    const auto newest_is_last = message_history_jump_edges({around, limit}, 6, limit, 8);
    const auto raced = message_history_jump_edges({around, limit}, 8, limit, 9);
    check(middle.more_before && middle.more_after && !at_start.more_before && at_start.more_after &&
              !at_present.more_after && !newest_is_last.more_after && raced.more_after,
          "around pages infer history edges like the official client, plus a known newer channel tail");

    MessageStore store{};
    UiVariableList list{};
    MessageTimeline timeline{};
    const u32 row = ui_extent_units(100.0f);
    check(message_store_initialize(&store, {g_records, 6}, {g_order, 6}, {g_text, sizeof(g_text)}) &&
              ui_variable_list_initialize(&list, {g_units, 6}, {g_fenwick, 6}, 0, row) &&
              message_timeline_initialize(&timeline, &store, &list, row, row * 2, true),
          "detached window fixture");
    // The channel session had begun a latest catch-up before the jump landed.
    MessageEventPump pump{};
    pump.timeline = &timeline;
    pump.catchup_required = true;
    timeline.newer_refetch_required = true;
    MessageHistoryCatchup catchup{};
    MessageHistoryOlder older{};
    MessageHistoryQuery query{};
    check(message_history_catchup_begin(&catchup, &pump, kChannel, limit) &&
              catchup.state == MessageHistoryCatchupState::request_required,
          "channel selection queues its latest page");
    check(target({6, limit, MessageHistoryPosition::around}, utf8_literal("/api/v10/channels/7/messages?around=6&limit=4")),
          "jump requests messages around the target");
    // message_history_jump_accept_rest: reset, then the around page.
    check(message_timeline_reset(&timeline) &&
              message_timeline_apply_history_page(&timeline, {around, limit}, MessageHistoryDirection::newer,
                                                  MessageContentAvailability::available)
                  .ok() &&
              message_history_jump_settle(&catchup, &older, &pump, kChannel, {around, limit}, middle) &&
              message_timeline_set_scroll(&timeline, row, false),
          "jump replaces the resident window around the target");
    check(timeline.newer_refetch_required && !timeline.follow_bottom && catchup.incremental &&
              catchup.state == MessageHistoryCatchupState::paused && message_history_browsing_ready(catchup.state) &&
              !message_history_catchup_take_query(&catchup, &query),
          "a window short of the present is detached; the stale latest request is dropped");

    const auto live = message_timeline_apply_created(&timeline, message(31));
    check(live.ok() && live.kind == GatewayMessageApplyKind::deferred && !message_store_find(store, 31).ok() &&
              timeline.newer_refetch_required,
          "live messages while detached are deferred, not appended to the old window");

    check(message_history_older_demand(&older, &timeline, kChannel, limit) &&
              message_history_older_take_query(&older, &query) &&
              target(query, utf8_literal("/api/v10/channels/7/messages?before=5&limit=4")),
          "scrolling up pages before the window's oldest row");
    older.state = MessageHistoryOlderState::idle; // the before page is not under test here

    // Forward pagination to the present: 9..12, 13..16, then a short page.
    u64 newest = 8;
    for (u32 page_index = 0; page_index < 2; ++page_index) {
        check(message_timeline_set_scroll(&timeline, list.total_units, false) &&
                  message_history_newer_prefetch_needed(timeline) &&
                  message_history_catchup_begin(&catchup, &pump, kChannel, limit) && catchup.incremental &&
                  message_history_catchup_take_query(&catchup, &query) &&
                  query.position == MessageHistoryPosition::after && query.cursor == newest,
              "nearing the window bottom requests the next page after its newest row");
        GatewayMessageEvent page[limit];
        page_of(page, newest + limit, limit);
        check(message_history_catchup_apply_page(&catchup, &pump, summary({page, limit}), {page, limit},
                                                 MessageContentAvailability::available)
                      .ok() &&
                  catchup.state == MessageHistoryCatchupState::paused && timeline.newer_refetch_required &&
                  !timeline.follow_bottom && message_store_find(store, newest + limit).ok(),
              "a full after page extends the window and stays detached");
        newest += limit;
    }
    check(!message_store_find(store, 5).ok(), "forward paging slides the bounded window past its old top");
    check(message_timeline_set_scroll(&timeline, list.total_units, false) &&
              message_history_catchup_begin(&catchup, &pump, kChannel, limit) &&
              message_history_catchup_take_query(&catchup, &query) &&
              target(query, utf8_literal("/api/v10/channels/7/messages?after=16&limit=4")),
          "after cursor advances page by page");
    // A create lands while the final page is in flight.
    check(message_timeline_apply_created(&timeline, message(33)).kind == GatewayMessageApplyKind::deferred,
          "a create during the final request is deferred");
    GatewayMessageEvent tail[3]{message(31), message(30), message(17)};
    check(message_history_catchup_apply_page(&catchup, &pump, summary({tail, 3}), {tail, 3},
                                             MessageContentAvailability::available)
                  .ok() &&
              catchup.state == MessageHistoryCatchupState::request_required && timeline.newer_refetch_required &&
              message_history_catchup_take_query(&catchup, &query) && query.cursor == 31 &&
              query.position == MessageHistoryPosition::after,
          "a short page cannot close the gap past a create deferred while it was in flight");
    const GatewayMessageEvent last = message(33);
    check(message_history_catchup_apply_page(&catchup, &pump, summary({&last, 1}), {&last, 1},
                                             MessageContentAvailability::available)
                  .ok() &&
              catchup.state == MessageHistoryCatchupState::complete && !timeline.newer_refetch_required &&
              message_store_find(store, 33).ok(),
          "reaching the server tail merges the window with the live tail");
    // The reader has reached the merged bottom.
    check(message_timeline_set_scroll(&timeline, 0, true), "reader follows the merged tail");
    const auto attached = message_timeline_apply_created(&timeline, message(34));
    check(attached.ok() && attached.kind == GatewayMessageApplyKind::created && message_store_find(store, 34).ok(),
          "after merging, live messages append again");
    check(!message_history_jump_to_present(&catchup, &older, &pump, kChannel, limit),
          "Jump to Present on an attached window only scrolls");

    // Jump again, then Jump to Present replaces the detached window with the latest page.
    check(message_timeline_reset(&timeline) &&
              message_timeline_apply_history_page(&timeline, {around, limit}, MessageHistoryDirection::newer,
                                                  MessageContentAvailability::available)
                  .ok() &&
              message_history_jump_settle(&catchup, &older, &pump, kChannel, {around, limit}, middle) &&
              message_history_jump_to_present(&catchup, &older, &pump, kChannel, limit) && store.count == 0 &&
              timeline.follow_bottom && !catchup.incremental &&
              message_history_catchup_take_query(&catchup, &query) &&
              target(query, utf8_literal("/api/v10/channels/7/messages?limit=4")),
          "Jump to Present discards the detached window and requests the latest page");
    GatewayMessageEvent latest[limit];
    page_of(latest, 34, limit);
    check(message_history_catchup_apply_page(&catchup, &pump, summary({latest, limit}), {latest, limit},
                                             MessageContentAvailability::available)
                  .ok() &&
              catchup.state == MessageHistoryCatchupState::request_required &&
              message_history_catchup_take_query(&catchup, &query) && query.cursor == 34,
          "the latest page re-attaches through the regular catch-up");
    check(message_history_catchup_apply_page(&catchup, &pump, {}, {}, MessageContentAvailability::available).ok() &&
              catchup.state == MessageHistoryCatchupState::complete && !timeline.newer_refetch_required &&
              timeline.follow_bottom && message_timeline_apply_created(&timeline, message(35)).kind ==
                                            GatewayMessageApplyKind::created,
          "after Jump to Present the timeline follows the live tail");

    // A jump whose page reaches the present stays attached.
    GatewayMessageEvent recent[limit];
    page_of(recent, 35, limit);
    const auto present = message_history_jump_edges({recent, limit}, 34, limit, 35);
    check(!present.more_after && message_timeline_reset(&timeline) &&
              message_timeline_apply_history_page(&timeline, {recent, limit}, MessageHistoryDirection::newer,
                                                  MessageContentAvailability::available)
                  .ok() &&
              message_history_jump_settle(&catchup, &older, &pump, kChannel, {recent, limit}, present) &&
              !timeline.newer_refetch_required && catchup.state == MessageHistoryCatchupState::complete &&
              message_timeline_apply_created(&timeline, message(36)).kind == GatewayMessageApplyKind::created,
          "a jump that reaches the present keeps receiving live messages");
    GatewayMessageEvent start[2]{message(2), message(1)};
    check(message_timeline_reset(&timeline) &&
              message_timeline_apply_history_page(&timeline, {start, 2}, MessageHistoryDirection::newer,
                                                  MessageContentAvailability::available)
                  .ok() &&
              message_history_jump_settle(&catchup, &older, &pump, kChannel, {start, 2},
                                          message_history_jump_edges({start, 2}, 1, limit, 36)) &&
              older.state == MessageHistoryOlderState::exhausted && older.cursor_before == 1 &&
              timeline.newer_refetch_required,
          "a jump near the channel start marks the beginning reached");
}
} // namespace

void test_timeline_window(void (*check)(bool, const char*) noexcept) noexcept {
    test_anchor_geometry(check);
    test_detached_window(check);
}
} // namespace maicord
