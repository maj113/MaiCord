#include "maicord/app/message_history_sync.hpp"
#include "maicord/ui/message_scene.hpp"
#include "maicord/media/animation_timeline.hpp"
#include "../src/ui/message_scene_measurements.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
namespace {
constexpr u32 capacity = 128;
constexpr u32 widget_capacity = 64 * kMessageSceneWidgetsPerRow + 1;
struct Fixture {
    MessageRecord records[capacity];
    u32 order[capacity];
    u8 text[8192];
    MessageRichRecord rich_records[capacity];
    u16 rich_order[capacity];
    u8 rich_text[32768];
    u32 heights[capacity];
    u64 fenwick[capacity];
    UiWidget widgets[widget_capacity];
    UiLayoutNode nodes[widget_capacity];
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
};
Fixture fixture;

bool attach(Fixture& f, u64 id, u32 height) noexcept {
    const auto found = message_store_find(f.store, id);
    const auto* record = found.ok() ? message_store_resolve(f.store, found.handle) : nullptr;
    if (!record) return false;
    GatewayMessageAttachment attachment{};
    attachment.id = 50'000 + id;
    attachment.filename = utf8_literal("test.png");
    attachment.content_type = utf8_literal("image/png");
    attachment.url = utf8_literal("https://cdn.discordapp.com/attachments/7/50000/test.png");
    attachment.width = 640;
    attachment.height = height;
    attachment.has_width = attachment.has_height = true;
    GatewayMessageEvent event{};
    event.id = id;
    event.fields = gateway_message_field_attachments;
    event.attachments = {&attachment, 1};
    return message_rich_store_apply(&f.rich, id, record->revision, event).ok();
}
}

void test_message_preparation(void (*check)(bool, const char*) noexcept) noexcept {
    MessageStore cache_store{};
    cache_store.capacity = 256;
    MessageTimeline cache_timeline{};
    cache_timeline.store = &cache_store;
    MessageScene cache_scene{};
    cache_scene.timeline = &cache_timeline;
    using message_scene_detail::measurement;
    const bool cache_ready = message_scene_detail::initialize_measurements(&cache_scene);
    check(cache_ready && cache_scene.owned_measurements.capacity == 256,
          "message_preparation_cache_preserves_maximum_capacity");
    if (cache_ready) {
        auto* first = measurement(&cache_scene, 0);
        if (first) first->signature = 99;
        auto* partial = measurement(&cache_scene, 24);
        check(first && partial && partial->signature == 0 && !partial->measured &&
              !partial->rich_measured && partial->content_height == 0,
              "message_preparation_fresh_cache_slots_have_zero_state");
        auto& bank = cache_scene.owned_measurements;
        const usize offset = bank.arena.offset;
        const usize committed = bank.arena.committed;
        const u32 commits = bank.arena.commit_count;
        const bool refused = !measurement(&cache_scene, 256);
        check(refused && bank.capacity == 256 && bank.arena.offset == offset &&
              bank.arena.committed == committed && bank.arena.commit_count == commits &&
              first && first->signature == 99,
              "message_preparation_invalid_cache_slot_preserves_state");
        auto* last = measurement(&cache_scene, 255);
        check(last && last->signature == 0 && measurement(&cache_scene, 0) == first,
              "message_preparation_cache_access_preserves_addresses_and_maximum");
        stable_storage_release(&cache_scene.owned_measurements);
        const bool fresh = message_scene_detail::initialize_measurements(&cache_scene);
        auto* recycled = fresh ? measurement(&cache_scene, 0) : nullptr;
        check(recycled && recycled->signature == 0 && !recycled->measured,
              "message_preparation_new_cache_does_not_retain_released_state");
        stable_storage_release(&cache_scene.owned_measurements);
        // Deferred: nothing committed until a slot is measured, then only its prefix.
        const bool deferred = message_scene_detail::initialize_measurements(&cache_scene, true);
        check(deferred && cache_scene.owned_measurements.capacity == 0 &&
                  cache_scene.owned_measurements.arena.committed == 0,
              "message_preparation_deferred_cache_commits_nothing");
        auto* lazy = deferred ? measurement(&cache_scene, 24) : nullptr;
        if (lazy) lazy->signature = 7;
        const u32 lazy_capacity = cache_scene.owned_measurements.capacity;
        check(lazy && lazy_capacity >= 25 && lazy_capacity < 256 && !lazy->measured &&
                  measurement(&cache_scene, 3) && measurement(&cache_scene, 3)->signature == 0,
              "message_preparation_deferred_cache_commits_measured_prefix");
        check(deferred && message_scene_reserve_measurements(&cache_scene) &&
                  cache_scene.owned_measurements.capacity == 256 && measurement(&cache_scene, 24) == lazy &&
                  lazy->signature == 7 && !measurement(&cache_scene, 256),
              "message_preparation_deferred_cache_reserves_full_layout_in_place");
        stable_storage_release(&cache_scene.owned_measurements);
    }
    Fixture& f = fixture;
    bool ready = message_store_initialize(&f.store, {f.records, capacity}, {f.order, capacity}, {f.text, sizeof(f.text)}) &&
        message_rich_store_initialize(&f.rich, {f.rich_records, capacity}, {f.rich_order, capacity},
                                     {f.rich_text, sizeof(f.rich_text)});
    for (u32 i = 0; ready && i < capacity; ++i) {
        ready = message_store_upsert(&f.store,
            MessageInput{1000 + i, 7, 20 + i % 2, 0, 0, utf8_literal("short message"), 0}).ok();
        if (ready && i % 16 == 0) ready = attach(f, 1000 + i, 400);
    }
    ready = ready && ui_variable_list_initialize(&f.list, {f.heights, capacity}, {f.fenwick, capacity},
                                                capacity, ui_extent_units(57)) &&
        message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(300), false) &&
        message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
        message_scene_initialize_owned(&f.scene, &f.timeline, 64, 8,
                                       {f.widgets, widget_capacity}, {f.nodes, widget_capacity});
    const UiRect viewport{0, 0, 740, 300};
    ready = ready && message_scene_update(&f.scene, viewport).ok();
    check(ready, "message_preparation_owned_fixture");
    if (!ready) { message_scene_release(&f.scene); return; }
    check(message_scene_source_count(f.scene) == capacity && f.scene.count < 32 &&
          !message_scene_find_row(f.scene, 96) && f.heights[96] > ui_extent_units(300),
          "message_preparation_reserves_unmaterialized_media_from_metadata");

    bool changed = false;
    // Row 0 carries an attachment: the accessories' 2 DIP top padding
    // follows its text. Row 1 is text alone and keeps 5 DIPs.
    const bool measured = message_scene_set_text_metrics(&f.scene, 0, 0, 180, true, nullptr, nullptr, {}, &changed) &&
                          message_scene_set_text_metrics(&f.scene, 1, 0, 180, true, nullptr, nullptr, {}, &changed) &&
                          message_scene_update(&f.scene, viewport).ok();
    const auto* text_only = message_scene_find_row(f.scene, 1);
    check(measured && text_only && text_only->measured_content_height == 185,
          "message_preparation_text_alone_keeps_its_5_dip_trailing_space");
    const u32 measured_height = f.heights[0];
    bool away = message_timeline_scroll_to_bottom(&f.timeline, &changed) && message_scene_update(&f.scene, viewport).ok();
    away = away && !message_scene_find_row(f.scene, 0);
    const bool returned = message_timeline_set_scroll(&f.timeline, 0, false) && message_scene_update(&f.scene, viewport).ok();
    const auto* row = message_scene_find_row(f.scene, 0);
    check(measured && away && returned && row && row->text_measured &&
          row->measured_content_height == 182 && f.heights[0] == measured_height,
          "message_preparation_measurements_survive_widget_recycling");

    // An update with nothing new runs no row pass (no layout, no change), yet a
    // store edit of a visible message is still picked up by the next one.
    const bool settled = message_scene_update(&f.scene, viewport).ok();
    const u32 layouts = f.scene.layout_passes;
    const MessageSceneResult idle = message_scene_update(&f.scene, viewport);
    const u32 layouts_after_idle = f.scene.layout_passes;
    const auto* edited_row = message_scene_find_row(f.scene, 1);
    const u32 revision_before = edited_row ? edited_row->message_revision : 0;
    const bool edited = edited_row && message_store_upsert(&f.store, MessageInput{edited_row->message_id, 7, 21, 0, 0,
                                                                                utf8_literal("edited message"), 0})
                                          .ok();
    const bool updated = edited && message_scene_update(&f.scene, viewport).ok();
    edited_row = message_scene_find_row(f.scene, 1);
    check(settled && idle.ok() && !idle.changed, "message_preparation_unchanged_update_reports_no_change");
    check(layouts_after_idle == layouts, "message_preparation_unchanged_update_runs_no_layout");
    check(edited && updated && edited_row && edited_row->message_revision != revision_before,
          "message_preparation_unchanged_update_still_sees_store_edits");

    const u32 passes = f.scene.preparation_passes;
    bool scrolled = true;
    // Each of the 100 distinct offsets is visited once; repeats added no new rows or passes.
    for (u32 i = 0; i < 100; ++i) {
        scrolled = scrolled && message_timeline_set_scroll(&f.timeline, ui_extent_units(static_cast<float>(i)), false) &&
                   message_scene_update(&f.scene, viewport).ok();
    }
    check(scrolled && f.scene.preparation_passes == passes && f.scene.count < 32,
          "message_preparation_scroll_does_not_repeat_resident_scan_or_grow_overscan");

    const u64 anchored = ui_variable_list_prefix_units(f.list, 60) + ui_extent_units(10);
    const bool anchored_ok = message_timeline_set_scroll(&f.timeline, anchored, false) &&
        message_scene_update(&f.scene, viewport).ok() && attach(f, 1000, 800) &&
        message_scene_update(&f.scene, viewport).ok();
    UiScrollAnchor anchor{};
    check(anchored_ok && ui_variable_list_capture_anchor(f.list, f.timeline.scroll_units, &anchor) &&
          anchor.index == 60 && anchor.within_item_units == ui_extent_units(10),
          "message_preparation_metadata_change_preserves_visible_anchor");

    GatewayMessageEvent older{};
    older.id = 999;
    older.channel_id = 7;
    older.author_id = 99;
    older.content = utf8_literal("prepended history");
    older.fields = gateway_message_field_id | gateway_message_field_channel_id |
                   gateway_message_field_author_id | gateway_message_field_content | gateway_message_field_timestamp;
    const auto prepended = message_timeline_apply_history_page(&f.timeline, {&older, 1},
        MessageHistoryDirection::older, MessageContentAvailability::available);
    check(prepended.ok() && message_scene_update(&f.scene, viewport).ok() &&
          ui_variable_list_capture_anchor(f.list, f.timeline.scroll_units, &anchor) &&
          anchor.index == 61 && anchor.within_item_units == ui_extent_units(10),
          "message_preparation_history_prepend_preserves_message_anchor");

    message_timeline_set_scroll(&f.timeline, ui_extent_units(501), false);
    const bool outside = message_history_older_prefetch_needed(f.timeline);
    message_timeline_set_scroll(&f.timeline, ui_extent_units(500), false);
    MessageHistoryOlder paging{};
    MessageHistoryQuery query{};
    const bool early = message_history_older_prefetch_needed(f.timeline) &&
        message_history_older_demand(&paging, &f.timeline, 7, 50) &&
        message_history_older_take_query(&paging, &query) &&
        message_history_older_demand(&paging, &f.timeline, 7, 50);
    check(!outside && early && query.cursor == 999 && paging.requests == 1 && paging.demands == 1,
          "message_history_prefetch_500px_boundary_and_single_flight");

    // A full reset can reproduce IDs, revisions and store counters. It must
    // still invalidate widths/heights retained from a previous channel load.
    const u32 generation = f.timeline.reset_generation;
    const bool reset = message_timeline_reset(&f.timeline) && f.timeline.reset_generation != generation &&
        message_store_upsert(&f.store, MessageInput{1000, 7, 20, 0, 0, utf8_literal("replacement"), 0}).ok() &&
        ui_variable_list_insert(&f.list, 0, 1, ui_extent_units(57)) &&
        message_scene_update(&f.scene, viewport).ok();
    row = message_scene_find_row(f.scene, 0);
    check(reset && row && !row->text_measured && f.heights[0] == ui_extent_units(57) &&
          !message_history_older_prefetch_needed(f.timeline),
          "message_preparation_reset_invalidates_measurements_no_empty_history_prefetch");
    message_scene_release(&f.scene);
    check(!f.scene.owned_measurements.arena.base && !f.scene.owned_rows.arena.base,
          "message_preparation_storage_released");

    u32 heights[10]{};
    u64 fenwick[10]{};
    UiVariableList list{};
    ui_variable_list_initialize(&list, {heights, 10}, {fenwick, 10}, 10, 100);
    const auto middle = ui_variable_list_buffered_range(list, 350, 200, 150);
    const auto beginning = ui_variable_list_buffered_range(list, 50, 200, 150);
    const auto end = ui_variable_list_buffered_range(list, 950, 200, 150);
    check(middle.first == 2 && middle.count == 5 && middle.first_offset_units == -150 &&
          beginning.first == 0 && beginning.count == 4 && beginning.first_offset_units == -50 &&
          end.first == 8 && end.count == 2 && end.first_offset_units == -150 &&
          ui_variable_list_buffered_range(list, 0, 0, 150).count == 0,
          "message_preparation_pixel_range_clamped_and_viewport_relative");

    AnimationTimeline animations{};
    AnimationTimelineEntry entries[2]{};
    u32 buckets[4]{};
    AnimationFrameAdvance advance[1]{};
    const MediaAssetKey key = media_asset_attachment(42, MediaAttachmentFormat::gif);
    const bool initialized = animation_timeline_initialize(&animations, {entries, 2}, {buckets, 4});
    const auto published = animation_timeline_publish(&animations, {key, 30'000, 1, 10, 0, false}, 1000);
    const auto offscreen = animation_timeline_collect_due(&animations, 1'000'000, {advance, 1});
    animation_timeline_begin_visibility(&animations);
    const bool visible = animation_timeline_observe_visible(&animations, key, 1'000'000);
    animation_timeline_end_visibility(&animations, 1'000'000);
    const auto onscreen = animation_timeline_collect_due(&animations, 1'030'000, {advance, 1});
    check(initialized && published.ok() && offscreen.ok() && offscreen.advances == 0 &&
          visible && onscreen.ok() && onscreen.advances == 1 && advance[0].frame_index == 1,
          "message_media_prefetched_animation_waits_for_visibility_without_skipping_frames");
}
}
