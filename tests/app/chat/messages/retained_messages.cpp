#include "../../../support/gateway_message_scratch.hpp"
#include "../../../support/test_main_shared.hpp"

#include "maicord/app/chat/messages/history_sync.hpp"
#include "maicord/ui/chat/messages/scene.hpp"
#include "maicord/app/chat/messages/timeline.hpp"
#include "maicord/app/gateway/client_event.hpp"

// Tombstones: a deleted message a plugin keeps stays in the live timeline as a retained row, survives
// history merges that remove local rows, comes back through message_timeline_insert_retained only inside
// the resident window, and is never kept by a timeline that did not ask (the snapshot cache's).
namespace maicord {
namespace {

constexpr u32 kFields = gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_author_id |
                        gateway_message_field_content | gateway_message_field_timestamp;

GatewayMessageEvent message(u64 id, const char* text) noexcept {
    GatewayMessageEvent event{};
    event.id = id;
    event.channel_id = 7;
    event.author_id = 11;
    event.timestamp_ms = id;
    u32 bytes = 0;
    while (text[bytes]) ++bytes;
    event.content = {text, bytes};
    event.fields = kFields;
    return event;
}

GatewayClientEvent delete_event(u64 id) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::message_delete;
    event.message.id = id;
    event.message.channel_id = 7;
    return event;
}

GatewayClientEvent bulk_event(u32 count, u32 payload_bytes) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::message_bulk_delete;
    event.dispatch_payload_bytes = payload_bytes;
    event.message.channel_id = 7;
    event.message.id_count = count;
    return event;
}

// The query a plugin that keeps ids 20, 30 and 50 would install.
bool keeps(u64 id) noexcept {
    return id == 20 || id == 30 || id == 50;
}

const MessageRecord* record_of(const MessageStore& store, u64 id) noexcept {
    const MessageStoreResult found = message_store_find(store, id);
    return found.ok() ? message_store_resolve(store, found.handle) : nullptr;
}

struct Fixture {
    MessageRecord records[16];
    u32 order[16];
    u8 text[1024];
    MessageStore store;
    u32 heights[16];
    u64 fenwick[16];
    UiVariableList list;
    MessageTimeline timeline;
};
Fixture g_fixture;

bool initialize(Fixture* fixture) noexcept {
    reset_value(fixture);
    return message_store_initialize(&fixture->store, Slice<MessageRecord>{fixture->records, 16},
                                    Slice<u32>{fixture->order, 16}, Slice<u8>{fixture->text, 1024}) &&
           ui_variable_list_initialize(&fixture->list, Slice<u32>{fixture->heights, 16},
                                       Slice<u64>{fixture->fenwick, 16}, 0, 100) &&
           message_timeline_initialize(&fixture->timeline, &fixture->store, &fixture->list, 100, 100, false);
}

} // namespace

void test_retained_messages() noexcept {
    Fixture& f = g_fixture;
    u64 bulk_ids[kGatewayBulkDeleteMaximumIds]{};
    const Slice<u64> bulk{bulk_ids, kGatewayBulkDeleteMaximumIds};
    expect(initialize(&f), "retained_fixture");
    for (u64 id = 10; id <= 60; id += 10) {
        const GatewayMessageEvent created = message(id, "text");
        message_timeline_apply_created(&f.timeline, created);
    }
    expect(f.store.count == 6 && f.list.count == 6, "retained_seeded");

    // A timeline that did not ask forgets the message, whatever the query says.
    g_message_retain_query = keeps;
    const MessageTimelineApplyResult forgotten =
        message_timeline_apply(&f.timeline, delete_event(20), {}, bulk);
    expect(forgotten.ok() && f.store.count == 5 && !record_of(f.store, 20) && f.list.count == 5,
           "retained_scratch_timeline_deletes");
    message_timeline_apply_created(&f.timeline, message(20, "text"));

    // The live timeline keeps what the query keeps, as one row repaint.
    f.timeline.retains = true;
    const MessageTimelineApplyResult kept = message_timeline_apply(&f.timeline, delete_event(20), {}, bulk);
    const MessageRecord* tombstone = record_of(f.store, 20);
    expect(kept.ok() && tombstone && tombstone->delivery == MessageDeliveryState::retained && f.store.count == 6 &&
               f.list.count == 6 && !kept.invalidation.structural && kept.invalidation.count == 1 &&
               kept.invalidation.first == 1,
           "retained_delete_keeps_row");
    const MessageTimelineApplyResult dropped = message_timeline_apply(&f.timeline, delete_event(40), {}, bulk);
    expect(dropped.ok() && !record_of(f.store, 40) && f.store.count == 5 && dropped.invalidation.structural,
           "retained_delete_of_other_message_removes");
    expect(message_timeline_apply(&f.timeline, delete_event(20), {}, bulk).ok() && record_of(f.store, 20) &&
               record_of(f.store, 20)->delivery == MessageDeliveryState::retained && f.store.count == 5,
           "retained_second_delete_is_idempotent");
    const MessageRecord* live = record_of(f.store, 10);
    expect(live && live->delivery == MessageDeliveryState::authoritative, "retained_others_stay_authoritative");

    // A bulk delete keeps what the query keeps and removes the rest.
    u8 payload[3 * sizeof(u64)]{};
    const u64 ids[3]{30, 50, 60};
    for (u32 i = 0; i < 3; ++i)
        for (u32 byte = 0; byte < 8; ++byte) payload[i * 8 + byte] = static_cast<u8>(ids[i] >> (byte * 8));
    const MessageTimelineApplyResult bulk_result =
        message_timeline_apply(&f.timeline, bulk_event(3, sizeof(payload)), ConstSlice<u8>{payload, sizeof(payload)}, bulk);
    expect(bulk_result.ok() && bulk_result.bulk.deleted == 1 && record_of(f.store, 30) && record_of(f.store, 50) &&
               !record_of(f.store, 60) && f.store.count == 4 && f.list.count == 4 && bulk_result.invalidation.structural,
           "retained_bulk_delete");
    expect(record_of(f.store, 30)->delivery == MessageDeliveryState::retained &&
               record_of(f.store, 50)->delivery == MessageDeliveryState::retained,
           "retained_bulk_rows_are_tombstones");

    // Resetting keeps the flag (the next channel's messages are kept too) but not the rows.
    expect(message_timeline_reset(&f.timeline) && f.timeline.retains && f.store.count == 0 && f.list.count == 0,
           "retained_reset_keeps_the_flag_only");

    // Putting tombstones back: only inside the window of confirmed rows.
    message_timeline_apply_created(&f.timeline, message(10, "a"));
    message_timeline_apply_created(&f.timeline, message(40, "b"));
    message_timeline_apply_created(&f.timeline, message(60, "c"));
    const auto input = [](u64 id) noexcept {
        MessageInput value{};
        value.id = id;
        value.channel_id = 7;
        value.author_id = 11;
        value.timestamp_ms = id;
        value.content = utf8_literal("gone");
        value.delivery = MessageDeliveryState::retained;
        return value;
    };
    const MessageTimelineApplyResult inside = message_timeline_insert_retained(&f.timeline, input(20));
    expect(inside.ok() && inside.mutation.inserted && inside.mutation.order_index == 1 && f.store.count == 4 &&
               f.list.count == 4 && record_of(f.store, 20)->delivery == MessageDeliveryState::retained,
           "retained_insert_inside_window");
    expect(!message_timeline_insert_retained(&f.timeline, input(5)).mutation.inserted && f.store.count == 4,
           "retained_insert_older_than_the_window_waits");
    expect(!message_timeline_insert_retained(&f.timeline, input(70)).mutation.inserted && f.store.count == 4,
           "retained_insert_newer_than_the_window_waits_unless_following");
    f.timeline.follow_bottom = true;
    expect(message_timeline_insert_retained(&f.timeline, input(70)).mutation.inserted && f.store.count == 5,
           "retained_insert_at_the_live_edge_when_following");
    expect(!message_timeline_insert_retained(&f.timeline, input(20)).mutation.inserted && f.store.count == 5,
           "retained_insert_is_idempotent");
    MessageInput wrong = input(25);
    wrong.delivery = MessageDeliveryState::authoritative;
    expect(!message_timeline_insert_retained(&f.timeline, wrong).ok() || !record_of(f.store, 25),
           "retained_insert_needs_the_retained_state");
    MessageInput local = input(26);
    local.delivery = MessageDeliveryState::pending;
    expect(!message_timeline_insert_local(&f.timeline, input(27)).ok() && message_timeline_insert_local(&f.timeline, local).ok(),
           "retained_rows_are_not_local_inserts");

    // A history page removes local rows and keeps tombstones in place.
    expect(record_of(f.store, 26) && record_of(f.store, 26)->delivery == MessageDeliveryState::pending,
           "retained_local_row_present");
    GatewayMessageEvent page[2]{message(9, "older"), message(8, "oldest")};
    const MessageHistoryApplyResult merged = message_timeline_apply_history_page(
        &f.timeline, ConstSlice<GatewayMessageEvent>{page, 2}, MessageHistoryDirection::older,
        MessageContentAvailability::available);
    expect(merged.ok() && record_of(f.store, 8) && record_of(f.store, 9) && !record_of(f.store, 26) &&
               record_of(f.store, 20) && record_of(f.store, 20)->delivery == MessageDeliveryState::retained &&
               record_of(f.store, 70) && f.store.count == f.list.count,
           "retained_history_merge_removes_locals_and_keeps_tombstones");
    // Rows are ordered by id whatever their kind.
    bool ordered = true;
    u64 previous = 0;
    for (u32 i = 0; i < f.store.count; ++i) {
        const MessageRecord* row = message_store_resolve(f.store, message_store_ordered_handle(f.store, i));
        ordered = ordered && row && row->id > previous;
        previous = row ? row->id : previous;
    }
    expect(ordered, "retained_rows_stay_in_id_order");

    // A confirmed create of the same id turns a tombstone back into a normal message.
    message_timeline_apply_created(&f.timeline, message(20, "back"));
    expect(record_of(f.store, 20)->delivery == MessageDeliveryState::authoritative, "retained_create_replaces_tombstone");

    // The row shows "deleted" after the author's name in the critical tone; other rows are untouched.
    static MessageScene scene;
    const MessageRecord* normal = record_of(f.store, 20);
    const MessageRecord* tomb = record_of(f.store, 70);
    const auto* shown = tomb ? message_scene_row_decoration(scene, *tomb) : nullptr;
    expect(tomb && tomb->delivery == MessageDeliveryState::retained && shown &&
               utf8_equal(shown->name_suffix, utf8_literal("deleted")) && shown->name_tone == 4 &&
               shown->visibility == ui_plugin_render_normal && !shown->replace_content,
           "retained_tombstone_decoration");
    expect(normal && !message_scene_row_decoration(scene, *normal), "retained_normal_rows_have_no_decoration");
    g_message_retain_query = nullptr;
}

} // namespace maicord
