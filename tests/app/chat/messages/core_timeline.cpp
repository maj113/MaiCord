#include "../../../support/test_main_shared.hpp"

#include "../../../support/secure_text_store_access.hpp"
#include "../../../support/ui_scene_fixtures.hpp"
#include "../../../support/model_store_fixtures.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "../../../support/gateway_message_scratch.hpp"
#include "maicord/app/gateway/client_dispatch.hpp"
#include "maicord/app/chat/messages/history_sync.hpp"
#include "maicord/app/chat/messages/edit_runtime.hpp"
#include "maicord/app/chat/messages/send_tracker.hpp"
#include "maicord/app/chat/messages/view_runtime.hpp"
#include "maicord/app/account/login/remote_auth_ticket_exchange.hpp"
#include "maicord/protocol/gateway/reaction_events.hpp"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "maicord/ui/text/text.hpp"

namespace maicord {
namespace {

MessageViewRuntime g_message_view_runtime{};
MessageRichRecord g_message_view_rich_records[kMessageViewItemCapacity];
u16 g_message_view_rich_order[kMessageViewItemCapacity];
u8 g_message_view_rich_text[2];

GatewayClientEvent timeline_message_event(GatewayClientDispatchKind kind, u64 id, u64 channel_id,
                                          u64 author_id, u32 fields, u32 payload_bytes,
                                          u32 flags = 0) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = kind;
    event.dispatch_payload_bytes = payload_bytes;
    event.message = GatewayClientMessageMutation{
        id,     channel_id, author_id,
        id,     0,          flags,
        fields, 0,          (fields & gateway_message_field_content) != 0 ? payload_bytes : 0};
    return event;
}

GatewayClientEvent timeline_delete_event(u64 id, u64 channel_id) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::message_delete;
    event.message.id = id;
    event.message.channel_id = channel_id;
    return event;
}

GatewayClientEvent timeline_bulk_delete_event(u64 channel_id, u32 id_count, u32 payload_bytes) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::message_bulk_delete;
    event.dispatch_payload_bytes = payload_bytes;
    event.message.channel_id = channel_id;
    event.message.id_count = id_count;
    return event;
}

void timeline_write_u64(u8* output, u32 index, u64 value) noexcept {
    for (u32 byte = 0; byte < sizeof(u64); ++byte) {
        output[index * sizeof(u64) + byte] = static_cast<u8>(value >> (byte * 8));
    }
}

struct PumpFixtureEntry {
    GatewayClientEvent event;
    ConstSlice<u8> payload;
};

struct PumpFixtureSource {
    PumpFixtureEntry* entries;
    u32 count;
    u32 cursor;
};

GatewayClientPopResult pump_fixture_pop(void* source_pointer, GatewayClientEvent* event,
                                        Slice<u8> payload_output) noexcept {
    if (!source_pointer || !event || (!payload_output.data && payload_output.count != 0)) {
        return GatewayClientPopResult{0, GatewayClientPopError::invalid_input};
    }
    auto* source = static_cast<PumpFixtureSource*>(source_pointer);
    if (source->cursor >= source->count) {
        return GatewayClientPopResult{0, GatewayClientPopError::empty};
    }
    const PumpFixtureEntry& entry = source->entries[source->cursor];
    if (entry.payload.count > payload_output.count) {
        return GatewayClientPopResult{entry.payload.count, GatewayClientPopError::output_too_small};
    }
    for (u32 index = 0; index < entry.payload.count; ++index) {
        payload_output[index] = entry.payload[index];
    }
    *event = entry.event;
    ++source->cursor;
    return GatewayClientPopResult{entry.payload.count, GatewayClientPopError::none};
}

} // namespace

void test_message_view_runtime() noexcept {
    g_message_view_runtime = MessageViewRuntime{};
    expect(message_view_runtime_initialize(nullptr, 120, 320, false).error ==
                   MessageViewRuntimeError::invalid_input &&
               message_view_runtime_initialize(&g_message_view_runtime, 0, 320, false).error ==
                   MessageViewRuntimeError::invalid_input &&
               !g_message_view_runtime.initialized,
           "message_view_runtime_invalid_initialize");

    expect(message_view_runtime_initialize(&g_message_view_runtime, 120, 320, true).ok() &&
               g_message_view_runtime.initialized &&
               g_message_view_runtime.timeline.rich_store == nullptr &&
               g_message_view_runtime.timeline.follow_bottom,
           "message_view_runtime_owns_model_and_index_storage");

    expect(message_store_upsert(&g_message_view_runtime.store,
                                MessageInput{30, 7, 3, 300, 0, utf8_literal("three"), 0})
                   .ok() &&
               message_store_upsert(&g_message_view_runtime.store,
                                    MessageInput{10, 7, 1, 100, 0, utf8_literal("one"), 0})
                   .ok() &&
               message_store_upsert(&g_message_view_runtime.store,
                                    MessageInput{20, 7, 2, 200, 0, utf8_literal("two"), 0})
                   .ok() &&
               message_view_runtime_reindex(&g_message_view_runtime, 120, 320, false).ok() &&
               g_message_view_runtime.store.count == 3 && g_message_view_runtime.list.count == 3 &&
               g_message_view_runtime.list.total_units == 360 &&
               g_message_view_runtime.timeline.store == &g_message_view_runtime.store &&
               g_message_view_runtime.timeline.list == &g_message_view_runtime.list &&
               !g_message_view_runtime.timeline.follow_bottom,
           "message_view_runtime_reindex_preserves_app_owned_model");

    MessageRichRecord small_records[1]{};
    u16 small_order[1]{};
    u8 small_text[2]{};
    MessageRichStore small_rich_store{};
    expect(message_rich_store_initialize(&small_rich_store, Slice<MessageRichRecord>{small_records, 1},
                                         Slice<u16>{small_order, 1}, Slice<u8>{small_text, 2}) &&
               message_view_runtime_bind_rich_store(&g_message_view_runtime, &small_rich_store).error ==
                   MessageViewRuntimeError::rich_store &&
               g_message_view_runtime.timeline.rich_store == nullptr,
           "message_view_runtime_rejects_undersized_rich_store");

    MessageRichStore rich_store{};
    expect(message_rich_store_initialize(
               &rich_store, Slice<MessageRichRecord>{g_message_view_rich_records, kMessageViewItemCapacity},
               Slice<u16>{g_message_view_rich_order, kMessageViewItemCapacity},
               Slice<u8>{g_message_view_rich_text, 2}) &&
               message_view_runtime_bind_rich_store(&g_message_view_runtime, &rich_store).ok() &&
               g_message_view_runtime.timeline.rich_store == &rich_store &&
               message_view_runtime_reindex(&g_message_view_runtime, 144, 480, true).ok() &&
               g_message_view_runtime.timeline.rich_store == &rich_store &&
               g_message_view_runtime.list.count == 3 && g_message_view_runtime.list.total_units == 432 &&
               g_message_view_runtime.timeline.follow_bottom,
           "message_view_runtime_rich_binding_survives_reindex");

    message_view_runtime_unbind_rich_store(&g_message_view_runtime);
    expect(g_message_view_runtime.timeline.rich_store == nullptr &&
               message_view_runtime_initialize(&g_message_view_runtime, 120, 320, false).ok() &&
               g_message_view_runtime.store.count == 0 && g_message_view_runtime.list.count == 0 &&
               g_message_view_runtime.timeline.rich_store == nullptr,
           "message_view_runtime_unbind_and_reinitialize");
    {
        // Long messages fill the whole window; the text bank commits only what they use.
        static char long_content[4000];
        for (auto& c : long_content) c = 'x';
        MessageStore& store = g_message_view_runtime.store;
        bool inserted = store.text_committed == kMessageStoreTextCommitStep;
        for (u64 id = 1; inserted && id <= 200; ++id)
            inserted = message_store_upsert(&store, MessageInput{id, 7, 1, id, 0, {long_content, 4000}, 0}).ok();
        const usize committed = g_message_view_runtime.text_storage.committed;
        expect(inserted && store.count == 200 && store.text_used == 800000 && store.text_committed >= 800000 &&
                   committed == store.text_committed && committed < 800000 + kMessageStoreTextCommitStep,
               "message_view_runtime_long_messages_commit_text_on_demand");
        // Replacing deleted content compacts inside the committed bank instead of growing it.
        bool churned = true;
        for (u64 id = 1; churned && id <= 100; ++id) churned = message_store_delete(&store, id);
        for (u64 id = 201; churned && id <= 300; ++id)
            churned = message_store_upsert(&store, MessageInput{id, 7, 1, id, 0, {long_content, 4000}, 0}).ok();
        expect(churned && store.count == 200 && store.compactions == 1 &&
                   g_message_view_runtime.text_storage.committed == committed,
               "message_view_runtime_text_churn_compacts_before_committing");
        expect(message_timeline_reset(&g_message_view_runtime.timeline) && store.count == 0 &&
                   store.text_arena == &g_message_view_runtime.text_storage &&
                   g_message_view_runtime.text_storage.committed == kMessageStoreTextCommitStep,
               "message_view_runtime_reset_returns_text_pages");
    }
    message_view_runtime_release(&g_message_view_runtime);
    expect(!g_message_view_runtime.initialized && !g_message_view_runtime.storage.base &&
               !g_message_view_runtime.text_storage.base && !g_message_view_runtime.records,
           "message_view_runtime_release");
}

void test_message_timeline() noexcept {
    MessageRecord records[8]{};
    u32 order[8]{};
    u8 text[512]{};
    MessageStore store{};
    u32 heights[8]{};
    u64 fenwick[8]{};
    UiVariableList list{};
    MessageTimeline timeline{};
    constexpr u32 create_fields = gateway_message_field_id | gateway_message_field_channel_id |
                                  gateway_message_field_author_id | gateway_message_field_content |
                                  gateway_message_field_timestamp;
    constexpr u32 update_fields =
        gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_content;
    u64 bulk_ids[kGatewayBulkDeleteMaximumIds]{};
    expect(message_store_initialize(&store, Slice<MessageRecord>{records, 8}, Slice<u32>{order, 8},
                                    Slice<u8>{text, 512}) &&
               ui_variable_list_initialize(&list, Slice<u32>{heights, 8}, Slice<u64>{fenwick, 8}, 0, 100) &&
               message_timeline_initialize(&timeline, &store, &list, 100, 100, false),
           "message_timeline_initialize");

    const GatewayClientEvent create_100 =
        timeline_message_event(GatewayClientDispatchKind::message_create, 100, 7, 11, create_fields, 3);
    const GatewayClientEvent create_300 =
        timeline_message_event(GatewayClientDispatchKind::message_create, 300, 7, 11, create_fields, 5);
    const GatewayClientEvent create_500 =
        timeline_message_event(GatewayClientDispatchKind::message_create, 500, 7, 11, create_fields, 4);
    const MessageTimelineApplyResult seed_100 = message_timeline_apply(
        &timeline, create_100, byte_literal("one"), Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    const MessageTimelineApplyResult seed_300 = message_timeline_apply(
        &timeline, create_300, byte_literal("three"), Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    const MessageTimelineApplyResult seed_500 = message_timeline_apply(
        &timeline, create_500, byte_literal("five"), Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    expect(seed_100.ok() && seed_300.ok() && seed_500.ok() && store.count == 3 && list.count == 3 &&
               list.total_units == 300 && seed_500.invalidation.first == 2 &&
               seed_500.invalidation.structural,
           "message_timeline_seed_creates");

    expect(message_timeline_set_scroll(&timeline, 105, false) && timeline.scroll_units == 105,
           "message_timeline_set_anchor_scroll");
    const GatewayClientEvent create_200 =
        timeline_message_event(GatewayClientDispatchKind::message_create, 200, 7, 11, create_fields, 3);
    const MessageTimelineApplyResult inserted = message_timeline_apply(
        &timeline, create_200, byte_literal("two"), Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    expect(inserted.ok() && inserted.mutation.inserted && inserted.mutation.order_index == 1 &&
               store.count == 4 && list.count == 4 && timeline.scroll_units == 205 &&
               inserted.invalidation.first == 1 && inserted.invalidation.count == 3 &&
               inserted.invalidation.structural && inserted.invalidation.scroll_changed,
           "message_timeline_insert_preserves_anchor");

    const GatewayClientEvent update_300 =
        timeline_message_event(GatewayClientDispatchKind::message_update, 300, 7, 0, update_fields, 12);
    const MessageTimelineApplyResult updated =
        message_timeline_apply(&timeline, update_300, byte_literal("three edited"),
                               Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    const MessageStoreResult found_300 = message_store_find(store, 300);
    expect(
        updated.ok() && !updated.mutation.inserted && updated.mutation.order_index == 2 &&
            store.count == 4 && list.count == 4 && timeline.scroll_units == 205 &&
            updated.invalidation.first == 2 && updated.invalidation.count == 1 &&
            !updated.invalidation.structural &&
            utf8_equal(message_store_content(store, found_300.handle), utf8_literal("three edited")),
        "message_timeline_update_targeted_invalidation");

    const MessageTimelineApplyResult deleted_200 =
        message_timeline_apply(&timeline, timeline_delete_event(200, 7), ConstSlice<u8>{nullptr, 0},
                               Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    expect(deleted_200.ok() && deleted_200.mutation.order_index == 1 && store.count == 3 &&
               list.count == 3 && timeline.scroll_units == 105 && deleted_200.invalidation.first == 1 &&
               deleted_200.invalidation.structural && deleted_200.invalidation.scroll_changed,
           "message_timeline_delete_before_anchor");
    const MessageTimelineApplyResult deleted_anchor =
        message_timeline_apply(&timeline, timeline_delete_event(300, 7), ConstSlice<u8>{nullptr, 0},
                               Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    expect(deleted_anchor.ok() && store.count == 2 && list.count == 2 && timeline.scroll_units == 100 &&
               message_store_ordered_handle(store, 1).slot == message_store_find(store, 500).handle.slot,
           "message_timeline_deleted_anchor_uses_successor");

    expect(message_timeline_set_scroll(&timeline, 0, true) && timeline.scroll_units == 100 &&
               timeline.follow_bottom && message_timeline_is_at_bottom(timeline, 0),
           "message_timeline_follow_bottom_enable");
    const GatewayClientEvent create_700 =
        timeline_message_event(GatewayClientDispatchKind::message_create, 700, 7, 11, create_fields, 5);
    const MessageTimelineApplyResult followed = message_timeline_apply(
        &timeline, create_700, byte_literal("seven"), Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    expect(followed.ok() && store.count == 3 && list.count == 3 && timeline.scroll_units == 200 &&
               followed.invalidation.scroll_changed && message_timeline_is_at_bottom(timeline, 0),
           "message_timeline_insert_follows_bottom");

    bool scroll_changed = false;
    expect(message_timeline_scroll_by(&timeline, -75, 0, &scroll_changed) && scroll_changed &&
               timeline.scroll_units == 125 && !timeline.follow_bottom &&
               message_timeline_scroll_by(&timeline, 1'000, 0, &scroll_changed) && scroll_changed &&
               timeline.scroll_units == 200 && timeline.follow_bottom &&
               message_timeline_scroll_to_start(&timeline, &scroll_changed) && scroll_changed &&
               timeline.scroll_units == 0 && !timeline.follow_bottom &&
               message_timeline_scroll_to_bottom(&timeline, &scroll_changed) && scroll_changed &&
               timeline.scroll_units == 200 && timeline.follow_bottom,
           "message_timeline_user_scroll_policy");

    MessageTimelineInvalidation height_invalidation{};
    expect(message_timeline_set_viewport(&timeline, 50), "message_timeline_reflow_viewport_fixture");
    const double before_manual_scroll_origin = timeline.scroll_origin_units;
    expect(message_timeline_set_scroll(&timeline, 205, false) &&
        timeline.scroll_origin_units == before_manual_scroll_origin,
        "message_timeline_user_move_does_not_translate_coordinate_origin");
    expect(message_timeline_set_item_height(&timeline, 0, 150, &height_invalidation) &&
               timeline.scroll_units == 255 && list.total_units == 350 && height_invalidation.first == 0 &&
               height_invalidation.count == 3 && !height_invalidation.structural &&
               height_invalidation.scroll_changed &&
               timeline.scroll_origin_units == before_manual_scroll_origin + 50,
           "message_timeline_height_preserves_anchor");

    u8 bulk_payload[3 * sizeof(u64)]{};
    timeline_write_u64(bulk_payload, 0, 100);
    timeline_write_u64(bulk_payload, 1, 999);
    timeline_write_u64(bulk_payload, 2, 500);
    const MessageTimelineApplyResult bulk =
        message_timeline_apply(&timeline, timeline_bulk_delete_event(7, 3, sizeof(bulk_payload)),
                               ConstSlice<u8>{bulk_payload, sizeof(bulk_payload)},
                               Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    expect(bulk.ok() && bulk.bulk.deleted == 2 && bulk.bulk.not_found == 1 &&
               bulk.bulk.channel_mismatch == 0 && store.count == 1 && list.count == 1 &&
               message_store_find(store, 700).ok() && bulk.invalidation.first == 0 &&
               bulk.invalidation.structural,
           "message_timeline_bulk_delete_sync");

    const GatewayClientEvent create_800 =
        timeline_message_event(GatewayClientDispatchKind::message_create, 800, 8, 12, create_fields, 5);
    expect(message_timeline_apply(&timeline, create_800, byte_literal("eight"),
                                  Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds})
               .ok(),
           "message_timeline_cross_channel_seed");
    timeline_write_u64(bulk_payload, 0, 700);
    timeline_write_u64(bulk_payload, 1, 800);
    timeline_write_u64(bulk_payload, 2, 404);
    const MessageTimelineApplyResult classified =
        message_timeline_apply(&timeline, timeline_bulk_delete_event(7, 3, sizeof(bulk_payload)),
                               ConstSlice<u8>{bulk_payload, sizeof(bulk_payload)},
                               Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    expect(classified.ok() && classified.bulk.deleted == 1 && classified.bulk.not_found == 1 &&
               classified.bulk.channel_mismatch == 1 && store.count == 1 && list.count == 1 &&
               message_store_find(store, 800).ok(),
           "message_timeline_bulk_delete_classification");

    GatewayClientEvent bad_payload =
        timeline_message_event(GatewayClientDispatchKind::message_create, 900, 7, 11, create_fields, 2);
    GatewayClientEvent unsupported{};
    unsupported.kind = GatewayClientEventKind::dispatch;
    unsupported.dispatch_kind = GatewayClientDispatchKind::unknown;
    expect(message_timeline_apply(&timeline, bad_payload, byte_literal("x"),
                                  Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds})
                       .error == MessageTimelineError::invalid_payload &&
               message_timeline_apply(&timeline, unsupported, ConstSlice<u8>{nullptr, 0},
                                      Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds})
                       .error == MessageTimelineError::unsupported_event &&
               timeline.applied == 11 && timeline.rejected == 2,
           "message_timeline_payload_and_event_rejections");

    MessageRecord capacity_records[4]{};
    u32 capacity_order[4]{};
    u8 capacity_text[128]{};
    MessageStore capacity_store{};
    u32 capacity_heights[3]{};
    u64 capacity_fenwick[3]{};
    UiVariableList capacity_list{};
    MessageTimeline capacity_timeline{};
    expect(
        message_store_initialize(&capacity_store, Slice<MessageRecord>{capacity_records, 4},
                                 Slice<u32>{capacity_order, 4}, Slice<u8>{capacity_text, 128}) &&
            message_store_upsert(&capacity_store, MessageInput{1, 7, 1, 1, 0, utf8_literal("a"), 0}).ok() &&
            message_store_upsert(&capacity_store, MessageInput{2, 7, 1, 2, 0, utf8_literal("b"), 0}).ok() &&
            message_store_upsert(&capacity_store, MessageInput{3, 7, 1, 3, 0, utf8_literal("c"), 0}).ok() &&
            ui_variable_list_initialize(&capacity_list, Slice<u32>{capacity_heights, 3},
                                        Slice<u64>{capacity_fenwick, 3}, 3, 100) &&
            message_timeline_initialize(&capacity_timeline, &capacity_store, &capacity_list, 100, 100,
                                        false),
        "message_timeline_capacity_fixture");
    const MessageTimelineApplyResult capacity = message_timeline_apply(
        &capacity_timeline,
        timeline_message_event(GatewayClientDispatchKind::message_create, 4, 7, 1, create_fields, 1),
        byte_literal("d"), Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    expect(capacity.ok() && capacity.kind == GatewayMessageApplyKind::deferred &&
               capacity_store.count == 3 && capacity_list.count == 3 && capacity_timeline.deferred == 1 &&
               capacity_timeline.newer_refetch_required &&
               message_store_find(capacity_store, 4).error == MessageStoreError::not_found,
           "message_timeline_scrolled_back_defers_newer_at_capacity");
    expect(message_timeline_set_scroll(&capacity_timeline, 0, true),
           "message_timeline_capacity_follow_fixture");
    const auto still_gapped = message_timeline_apply(
        &capacity_timeline,
        timeline_message_event(GatewayClientDispatchKind::message_create, 4, 7, 1, create_fields, 1),
        byte_literal("d"), Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    expect(still_gapped.ok() && still_gapped.kind == GatewayMessageApplyKind::deferred,
           "message_timeline_bottom_does_not_skip_unresolved_history");
    // The following assertion exercises normal live-tail eviction, after the
    // history owner has confirmed that the intervening history is complete.
    capacity_timeline.newer_refetch_required = false;
    const MessageTimelineApplyResult retained = message_timeline_apply(
        &capacity_timeline,
        timeline_message_event(GatewayClientDispatchKind::message_create, 4, 7, 1, create_fields, 1),
        byte_literal("d"), Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    expect(retained.ok() && retained.kind == GatewayMessageApplyKind::created && retained.evicted == 1 &&
               retained.invalidation.structural && capacity_store.count == 3 && capacity_list.count == 3 &&
               capacity_timeline.evicted == 1 &&
               message_store_find(capacity_store, 1).error == MessageStoreError::not_found &&
               message_store_find(capacity_store, 4).ok() && capacity_timeline.follow_bottom &&
               message_timeline_is_at_bottom(capacity_timeline, 0),
           "message_timeline_follow_bottom_evicts_oldest_for_newer");
    const u32 evicted_before_invalid = capacity_timeline.evicted;
    const MessageTimelineApplyResult invalid_at_capacity =
        message_timeline_apply(&capacity_timeline,
                               timeline_message_event(GatewayClientDispatchKind::message_create, 5, 7, 1,
                                                      create_fields & ~gateway_message_field_author_id, 1),
                               byte_literal("e"), Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    expect(invalid_at_capacity.error == MessageTimelineError::reconcile &&
               capacity_timeline.evicted == evicted_before_invalid && capacity_store.count == 3 &&
               capacity_list.count == 3 && message_store_find(capacity_store, 2).ok() &&
               message_store_find(capacity_store, 5).error == MessageStoreError::not_found,
           "message_timeline_validates_before_capacity_eviction");

    MessageRecord pressure_records[3]{};
    u32 pressure_order[3]{};
    u8 pressure_text[8]{};
    MessageStore pressure_store{};
    u32 pressure_heights[3]{};
    u64 pressure_fenwick[3]{};
    UiVariableList pressure_list{};
    MessageTimeline pressure_timeline{};
    expect(message_store_initialize(&pressure_store, Slice<MessageRecord>{pressure_records, 3},
                                    Slice<u32>{pressure_order, 3}, Slice<u8>{pressure_text, 8}) &&
               message_store_upsert(&pressure_store, MessageInput{10, 7, 1, 1, 0, utf8_literal("aa"), 0})
                   .ok() &&
               message_store_upsert(&pressure_store, MessageInput{20, 7, 1, 2, 0, utf8_literal("bb"), 0})
                   .ok() &&
               message_store_upsert(&pressure_store, MessageInput{30, 7, 1, 3, 0, utf8_literal("cc"), 0})
                   .ok() &&
               ui_variable_list_initialize(&pressure_list, Slice<u32>{pressure_heights, 3},
                                           Slice<u64>{pressure_fenwick, 3}, 3, 100) &&
               message_timeline_initialize(&pressure_timeline, &pressure_store, &pressure_list, 100, 100,
                                           false) &&
               message_timeline_set_scroll(&pressure_timeline, 100, false),
           "message_timeline_text_pressure_fixture");
    const MessageTimelineApplyResult pressure_update = message_timeline_apply(
        &pressure_timeline,
        timeline_message_event(GatewayClientDispatchKind::message_update, 20, 7, 0, update_fields, 6),
        byte_literal("123456"), Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds});
    expect(pressure_update.ok() && pressure_update.evicted == 1 && pressure_update.compacted &&
               pressure_update.invalidation.structural && pressure_store.count == 2 &&
               pressure_list.count == 2 &&
               message_store_find(pressure_store, 10).error == MessageStoreError::not_found &&
               message_store_find(pressure_store, 20).ok() &&
               utf8_equal(
                   message_store_content(pressure_store, message_store_find(pressure_store, 20).handle),
                   utf8_literal("123456")) &&
               pressure_timeline.evicted == 1 && pressure_timeline.compactions == 1 &&
               pressure_store.rejected == 0,
           "message_timeline_update_pressure_preserves_target");

    MessageRecord rich_timeline_records[2]{};
    u32 rich_timeline_order[2]{};
    u8 rich_timeline_text[128]{};
    MessageStore rich_timeline_store{};
    MessageRichRecord rich_timeline_rich_records[2]{};
    u16 rich_timeline_rich_order[2]{};
    u8 rich_timeline_rich_text[2 * 1024]{};
    MessageRichStore rich_timeline_rich_store{};
    u32 rich_timeline_heights[2]{};
    u64 rich_timeline_fenwick[2]{};
    UiVariableList rich_timeline_list{};
    MessageTimeline rich_timeline{};
    u32 rich_route_structurals[1'024]{};
    char rich_route_payload_storage[8 * 1024]{};
    u64 rich_route_ids[kGatewayBulkDeleteMaximumIds]{};
    GatewayEnvelope rich_route_envelope{};
    GatewayClientEvent rich_route_event{};
    ConstSlice<u8> rich_route_payload{};
    const Utf8View rich_route_create = utf8_literal(
        R"({"op":0,"d":{"id":"900","channel_id":"7","author":{"id":"11","username":"Writer"},"content":"rich","timestamp":"2024-08-30T12:00:00Z","message_reference":{"message_id":"800","channel_id":"7"},"referenced_message":null,"reactions":[{"count":2,"count_details":{"normal":2,"burst":0},"me":true,"emoji":{"id":"44","name":"party"}}],"attachments":[{"id":"70","filename":"clip.mp4","size":123,"url":"https://cdn.discordapp.com/attachments/7/70/clip.mp4"}]},"s":1,"t":"MESSAGE_CREATE"})");
    const bool rich_route_ready =
        message_store_initialize(&rich_timeline_store, Slice<MessageRecord>{rich_timeline_records, 2},
                                 Slice<u32>{rich_timeline_order, 2}, Slice<u8>{rich_timeline_text, 128}) &&
        message_rich_store_initialize(
            &rich_timeline_rich_store, Slice<MessageRichRecord>{rich_timeline_rich_records, 2},
            Slice<u16>{rich_timeline_rich_order, 2}, Slice<u8>{rich_timeline_rich_text, 2 * 1024}) &&
        ui_variable_list_initialize(&rich_timeline_list, Slice<u32>{rich_timeline_heights, 2},
                                    Slice<u64>{rich_timeline_fenwick, 2}, 0, 100) &&
        message_timeline_initialize(&rich_timeline, &rich_timeline_store, &rich_timeline_list, 100, 100,
                                    true) &&
        message_timeline_bind_rich_store(&rich_timeline, &rich_timeline_rich_store) &&
        gateway_decode_envelope(rich_route_create, Slice<u32>{rich_route_structurals, 1'024}, 32,
                                &rich_route_envelope)
            .ok() &&
        gateway_client_route_dispatch(rich_route_envelope, Slice<u32>{rich_route_structurals, 1'024}, 32,
                                      Slice<char>{rich_route_payload_storage, 8 * 1024},
                                      Slice<u64>{rich_route_ids, kGatewayBulkDeleteMaximumIds},
                                      &rich_route_event, &rich_route_payload, gateway_test_message_scratch())
            .ok();
    const MessageTimelineApplyResult rich_timeline_created =
        rich_route_ready ? message_timeline_apply(&rich_timeline, rich_route_event, rich_route_payload,
                                                  Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds})
                         : MessageTimelineApplyResult{};
    const MessageRichRecord* rich_resident = message_rich_store_find(rich_timeline_rich_store, 900);
    expect(rich_route_ready, "message_timeline_rich_route_fixture");
    expect(rich_timeline_created.ok(), "message_timeline_rich_create_apply");
    expect(rich_timeline_created.mutation.ok() && message_store_find(rich_timeline_store, 900).ok(),
           "message_timeline_rich_base_create_apply");
    expect(rich_resident && rich_resident->reaction_count == 1 && rich_resident->reactions[0].count == 2 &&
               rich_resident->attachment_count == 1 &&
               rich_resident->reference.state == GatewayMessageReferenceState::deleted &&
               utf8_equal(message_rich_store_string(rich_timeline_rich_store,
                                                    rich_resident->attachments[0].filename),
                          utf8_literal("clip.mp4")),
           "message_timeline_rich_gateway_persistence");

    const Utf8View rich_route_update = utf8_literal(
        R"({"op":0,"d":{"id":"900","channel_id":"7","reactions":[]},"s":2,"t":"MESSAGE_UPDATE"})");
    const bool rich_update_ready =
        gateway_decode_envelope(rich_route_update, Slice<u32>{rich_route_structurals, 1'024}, 32,
                                &rich_route_envelope)
            .ok() &&
        gateway_client_route_dispatch(rich_route_envelope, Slice<u32>{rich_route_structurals, 1'024}, 32,
                                      Slice<char>{rich_route_payload_storage, 8 * 1024},
                                      Slice<u64>{rich_route_ids, kGatewayBulkDeleteMaximumIds},
                                      &rich_route_event, &rich_route_payload, gateway_test_message_scratch())
            .ok() &&
        message_timeline_apply(&rich_timeline, rich_route_event, rich_route_payload,
                               Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds})
            .ok();
    rich_resident = message_rich_store_find(rich_timeline_rich_store, 900);
    expect(rich_update_ready, "message_timeline_rich_update_apply");
    expect(rich_resident && rich_resident->reaction_count == 0 && rich_resident->attachment_count == 1 &&
               message_timeline_apply(&rich_timeline, timeline_delete_event(900, 7),
                                      ConstSlice<u8>{nullptr, 0},
                                      Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds})
                   .ok() &&
               !message_rich_store_find(rich_timeline_rich_store, 900) &&
               message_timeline_reset(&rich_timeline) &&
               rich_timeline.rich_store == &rich_timeline_rich_store && rich_timeline_rich_store.count == 0,
           "message_timeline_rich_partial_update_delete_reset");

    u32 mismatched_heights[4]{};
    u64 mismatched_fenwick[4]{};
    UiVariableList mismatched_list{};
    MessageTimeline mismatched_timeline{};
    expect(ui_variable_list_initialize(&mismatched_list, Slice<u32>{mismatched_heights, 4},
                                       Slice<u64>{mismatched_fenwick, 4}, 2, 100) &&
               !message_timeline_initialize(&mismatched_timeline, &capacity_store, &mismatched_list, 100,
                                            100, false),
           "message_timeline_reject_count_mismatch_initialize");
}

void test_message_send_tracker() noexcept {
    MessageRecord records[8]{};
    u32 order[8]{};
    u8 text[512]{};
    MessageStore store{};
    u32 heights[8]{};
    u64 fenwick[8]{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageSendTracker tracker{};
    expect(message_store_initialize(&store, Slice<MessageRecord>{records, 8}, Slice<u32>{order, 8},
                                    Slice<u8>{text, 512}) &&
               ui_variable_list_initialize(&list, Slice<u32>{heights, 8}, Slice<u64>{fenwick, 8}, 0, 100) &&
               message_timeline_initialize(&timeline, &store, &list, 100, 100, true) &&
               message_send_tracker_initialize(&tracker),
           "message_send_tracker_initialize");

    const MessageSendBeginResult http_first = message_send_tracker_begin(
        &tracker, &timeline, 7, 11, utf8_literal("hello"), 1'725'012'296'789ull, 100);
    const MessageStoreResult http_first_local = message_store_find(store, http_first.nonce);
    const MessageRecord* http_first_record = message_store_resolve(store, http_first_local.handle);
    expect(http_first.ok() && http_first.nonce != 0 && http_first_local.ok() && http_first_record &&
               http_first_record->delivery == MessageDeliveryState::pending && store.count == 1 &&
               list.count == 1 && http_first.invalidation.structural,
           "message_send_tracker_inserts_pending_local_row");
    const MessageSendObserveResult http_ack = message_send_tracker_observe_http(
        &tracker, &timeline, http_first.nonce, 200, MessageSendHttpDisposition::success, 110);
    const MessageSendObserveResult premature_retry =
        message_send_tracker_retry(&tracker, &timeline, http_first.nonce, 111);
    expect(http_ack.ok() && !http_ack.completed && !http_ack.changed && tracker.active_count == 1 &&
               premature_retry.error == MessageSendTrackerError::not_found &&
               message_store_find(store, http_first.nonce).ok(),
           "message_send_tracker_http_before_gateway_waits_for_echo");
    const MessageSendObserveResult gateway_after_http =
        message_send_tracker_observe_gateway(&tracker, &timeline, http_first.nonce, 7, 11, 900, 115);
    MessageSendCompletion completion{};
    expect(gateway_after_http.ok() && gateway_after_http.completed && gateway_after_http.changed &&
               message_store_find(store, http_first.nonce).error == MessageStoreError::not_found &&
               store.count == 0 && list.count == 0 &&
               message_send_tracker_take_completion(&tracker, &completion) &&
               completion.kind == MessageSendCompletionKind::accepted &&
               completion.nonce == http_first.nonce && completion.authoritative_id == 900 &&
               completion.http_observed && completion.gateway_observed && completion.api_elapsed_ms == 10 &&
               completion.gateway_elapsed_ms == 15,
           "message_send_tracker_http_gateway_completion");

    const MessageSendBeginResult gateway_first = message_send_tracker_begin(
        &tracker, &timeline, 7, 11, utf8_literal("world"), 1'725'012'296'790ull, 200);
    const MessageSendObserveResult gateway_ack =
        message_send_tracker_observe_gateway(&tracker, &timeline, gateway_first.nonce, 7, 11, 901, 205);
    const MessageSendObserveResult http_after_gateway = message_send_tracker_observe_http(
        &tracker, &timeline, gateway_first.nonce, 200, MessageSendHttpDisposition::success, 215);
    completion = MessageSendCompletion{};
    expect(gateway_first.ok() && gateway_ack.ok() && !gateway_ack.completed &&
               !message_store_find(store, gateway_first.nonce).ok() && http_after_gateway.ok() &&
               http_after_gateway.completed && tracker.active_count == 0 &&
               message_send_tracker_take_completion(&tracker, &completion) &&
               completion.kind == MessageSendCompletionKind::accepted &&
               completion.authoritative_id == 901 && completion.api_elapsed_ms == 15 &&
               completion.gateway_elapsed_ms == 5,
           "message_send_tracker_gateway_http_completion");

    const MessageSendBeginResult retry_send = message_send_tracker_begin(
        &tracker, &timeline, 7, 11, utf8_literal("retry"), 1'725'012'296'791ull, 300);
    const MessageSendObserveResult failed = message_send_tracker_observe_http(
        &tracker, &timeline, retry_send.nonce, 400, MessageSendHttpDisposition::terminal_failure, 310);
    const MessageStoreResult failed_local = message_store_find(store, retry_send.nonce);
    const MessageRecord* failed_record = message_store_resolve(store, failed_local.handle);
    expect(retry_send.ok() && failed.ok() && !failed.completed && failed.changed && failed_record &&
               failed_record->delivery == MessageDeliveryState::failed,
           "message_send_tracker_terminal_failure_marks_row");
    const MessageSendObserveResult retried =
        message_send_tracker_retry(&tracker, &timeline, retry_send.nonce, 320);
    const MessageStoreResult retried_local = message_store_find(store, retry_send.nonce);
    const MessageRecord* retried_record = message_store_resolve(store, retried_local.handle);
    bool expiry_changed = false;
    expect(retried.ok() && retried.changed && retried_record &&
               retried_record->delivery == MessageDeliveryState::pending &&
               message_send_tracker_tick(&tracker, &timeline, 320 + kMessageSendTrackerTimeoutMs,
                                         &expiry_changed) &&
               expiry_changed && tracker.active_count == 1 &&
               message_send_tracker_take_completion(&tracker, &completion) &&
               completion.kind == MessageSendCompletionKind::expired && completion.retries == 1,
           "message_send_tracker_retry_and_expiry");
    const MessageStoreResult expired_local = message_store_find(store, retry_send.nonce);
    const MessageRecord* expired_record = message_store_resolve(store, expired_local.handle);
    const bool expiry_left_failed_row =
        expired_record && expired_record->delivery == MessageDeliveryState::failed;
    const MessageSendObserveResult retry_after_expiry = message_send_tracker_retry(
        &tracker, &timeline, retry_send.nonce, 321 + kMessageSendTrackerTimeoutMs);
    const MessageSendObserveResult failed_after_expiry =
        retry_after_expiry.ok()
            ? message_send_tracker_observe_http(&tracker, &timeline, retry_send.nonce, 503,
                                                MessageSendHttpDisposition::terminal_failure,
                                                322 + kMessageSendTrackerTimeoutMs)
            : MessageSendObserveResult{};
    const MessageSendObserveResult discarded =
        failed_after_expiry.ok() ? message_send_tracker_discard(&tracker, &timeline, retry_send.nonce)
                                 : MessageSendObserveResult{};
    expect(expiry_left_failed_row && retry_after_expiry.ok() && failed_after_expiry.ok() &&
               discarded.ok() && discarded.invalidation.structural && tracker.active_count == 0 &&
               message_store_find(store, retry_send.nonce).error == MessageStoreError::not_found,
           "message_send_tracker_expiry_retry_and_discard");

    const MessageSendBeginResult rate_limited = message_send_tracker_begin(
        &tracker, &timeline, 7, 11, utf8_literal("later"), 1'725'012'296'792ull, 40'000);
    const MessageSendObserveResult deferred =
        message_send_tracker_observe_http(&tracker, &timeline, rate_limited.nonce, 429,
                                          MessageSendHttpDisposition::retry_wait, 40'010, 100'000);
    bool prematurely_expired = false;
    const bool retained_through_wait =
        message_send_tracker_tick(&tracker, &timeline, 80'000, &prematurely_expired);
    const MessageSendObserveResult deferred_gateway =
        message_send_tracker_observe_gateway(&tracker, &timeline, rate_limited.nonce, 7, 11, 902, 100'010);
    const MessageSendObserveResult deferred_http = message_send_tracker_observe_http(
        &tracker, &timeline, rate_limited.nonce, 200, MessageSendHttpDisposition::success, 100'020);
    completion = MessageSendCompletion{};
    expect(rate_limited.ok() && deferred.ok() && retained_through_wait && !prematurely_expired &&
               deferred_gateway.ok() && !deferred_gateway.completed && deferred_http.ok() &&
               deferred_http.completed && message_send_tracker_take_completion(&tracker, &completion) &&
               completion.kind == MessageSendCompletionKind::accepted &&
               completion.authoritative_id == 902 && completion.retries == 1,
           "message_send_tracker_rate_limit_extends_echo_deadline");

    const MessageSendObserveResult unmatched =
        message_send_tracker_observe_gateway(&tracker, &timeline, 44, 7, 11, 903, 100'100);
    const MessageSendBeginResult mismatch_send = message_send_tracker_begin(
        &tracker, &timeline, 7, 11, utf8_literal("mismatch"), 1'725'012'296'793ull, 100'200);
    const MessageSendObserveResult mismatch =
        message_send_tracker_observe_gateway(&tracker, &timeline, mismatch_send.nonce, 8, 11, 904, 100'205);
    expect(unmatched.ok() && !unmatched.changed && tracker.unmatched_gateway == 1 &&
               mismatch.error == MessageSendTrackerError::channel_mismatch &&
               message_store_find(store, mismatch_send.nonce).ok(),
           "message_send_tracker_unmatched_and_channel_collision_guards");
    expect(message_send_tracker_observe_gateway(&tracker, &timeline, mismatch_send.nonce, 7, 12, 904, 100'206).error ==
               MessageSendTrackerError::author_mismatch && message_store_find(store, mismatch_send.nonce).ok() &&
               message_timeline_reset(&timeline) &&
               message_send_tracker_observe_gateway(&tracker, &timeline, mismatch_send.nonce, 7, 12, 904, 100'207).error ==
                   MessageSendTrackerError::author_mismatch && tracker.active_count == 1 && !store.count,
           "message_send_tracker_author_identity_survives_local_eviction");
    const auto first_identity = message_send_tracker_observe_gateway(&tracker, &timeline, mismatch_send.nonce, 7, 11, 904, 100'210);
    const auto replay_identity = message_send_tracker_observe_gateway(&tracker, &timeline, mismatch_send.nonce, 7, 11, 904, 100'220);
    const auto conflicting_identity = message_send_tracker_observe_gateway(&tracker, &timeline, mismatch_send.nonce, 7, 11, 905, 100'225);
    const auto wrong_rest_author = message_send_tracker_confirm(&tracker, &timeline, mismatch_send.nonce, 7, 12, 904,
        MessageSendConfirmationSource::history);
    expect(first_identity.ok() && replay_identity.ok() && !replay_identity.changed &&
               conflicting_identity.error == MessageSendTrackerError::message_mismatch &&
               wrong_rest_author.error == MessageSendTrackerError::author_mismatch &&
               message_send_tracker_observe_http(&tracker, &timeline, mismatch_send.nonce, 200,
                   MessageSendHttpDisposition::success, 100'230).completed &&
               message_send_tracker_take_completion(&tracker, &completion) && completion.authoritative_id == 904 &&
               completion.gateway_elapsed_ms == 10 && completion.api_elapsed_ms == 30,
           "message_send_tracker_replay_preserves_first_identity_and_observation_time");
    const auto other_author = message_send_tracker_begin(&tracker, &timeline, 7, 12, utf8_literal("other"), 1'725'012'296'794ull, 100'300);
    expect(other_author.ok() &&
               message_send_tracker_observe_gateway(&tracker, &timeline, other_author.nonce, 7, 11, 906, 100'301).error ==
                   MessageSendTrackerError::author_mismatch &&
               message_send_tracker_confirm(&tracker, &timeline, other_author.nonce, 7, 12, 906,
                   MessageSendConfirmationSource::response).completed &&
               message_send_tracker_take_completion(&tracker, &completion) && !tracker.active_count,
           "message_send_tracker_retains_author_per_attempt");
}

void test_message_event_pump() noexcept {
    MessageRecord records[8]{};
    u32 order[8]{};
    u8 text[256]{};
    MessageStore store{};
    u32 heights[8]{};
    u64 fenwick[8]{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageRichRecord rich_records[8]{};
    u16 rich_order[8]{};
    u8 rich_text[2 * 1024]{};
    MessageRichStore rich_store{};
    u8 payload_scratch[kMessageEventPumpPayloadCapacity]{};
    u64 bulk_ids[kGatewayBulkDeleteMaximumIds]{};
    TypingRecord typing_records[4]{};
    TypingStore typing_store{};
    MessageEventPump pump{};
    constexpr u32 create_fields = gateway_message_field_id | gateway_message_field_channel_id |
                                  gateway_message_field_author_id | gateway_message_field_content |
                                  gateway_message_field_timestamp;
    constexpr u32 update_fields =
        gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_content;
    expect(
        message_store_initialize(&store, Slice<MessageRecord>{records, 8}, Slice<u32>{order, 8},
                                 Slice<u8>{text, 256}) &&
            ui_variable_list_initialize(&list, Slice<u32>{heights, 8}, Slice<u64>{fenwick, 8}, 0, 100) &&
            message_timeline_initialize(&timeline, &store, &list, 100, 100, false) &&
            message_rich_store_initialize(&rich_store, Slice<MessageRichRecord>{rich_records, 8},
                                          Slice<u16>{rich_order, 8}, Slice<u8>{rich_text, 2 * 1024}) &&
            message_timeline_bind_rich_store(&timeline, &rich_store) &&
            message_event_pump_initialize(&pump, &timeline,
                                          Slice<u8>{payload_scratch, kMessageEventPumpPayloadCapacity},
                                          Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds},
                                          MessageContentAvailability::restricted) &&
            typing_store_initialize(&typing_store, Slice<TypingRecord>{typing_records, 4}) &&
            message_event_pump_set_typing_store(&pump, &typing_store),
        "message_event_pump_initialize");

    GatewayClientEvent typing_start{};
    typing_start.kind = GatewayClientEventKind::dispatch;
    typing_start.dispatch_kind = GatewayClientDispatchKind::typing_start;
    typing_start.timestamp_ms = 1'000;
    typing_start.typing = GatewayClientTypingStartEvent{7, 5, 11, 100};
    const MessageEventPumpConsumeResult typing_applied =
        message_event_pump_consume(&pump, typing_start, ConstSlice<u8>{nullptr, 0});
    expect(typing_applied.ok() && typing_applied.kind == MessageEventPumpConsumeKind::state_applied &&
               typing_applied.repaint && typing_store.count == 1,
           "message_event_pump_applies_typing_start");

    GatewayClientEvent empty_create =
        timeline_message_event(GatewayClientDispatchKind::message_create, 10, 7, 11, create_fields, 0);
    empty_create.sequence = 10;
    empty_create.has_sequence = true;
    const MessageEventPumpConsumeResult restricted =
        message_event_pump_consume(&pump, empty_create, ConstSlice<u8>{nullptr, 0});
    const MessageStoreResult found_10 = message_store_find(store, 10);
    const MessageRecord* record_10 = message_store_resolve(store, found_10.handle);
    expect(restricted.ok() && restricted.kind == MessageEventPumpConsumeKind::applied &&
               restricted.repaint && record_10 &&
               record_10->content_availability == MessageContentAvailability::available &&
               pump.has_sequence && pump.last_sequence == 10 && typing_store.count == 0,
           "message_event_pump_explicit_empty_content_is_available");

    GatewayClientEvent visible_update =
        timeline_message_event(GatewayClientDispatchKind::message_update, 10, 7, 0, update_fields, 3);
    const MessageEventPumpConsumeResult visible =
        message_event_pump_consume(&pump, visible_update, byte_literal("now"));
    record_10 = message_store_resolve(store, found_10.handle);
    expect(visible.ok() && visible.kind == MessageEventPumpConsumeKind::applied && record_10 &&
               record_10->content_availability == MessageContentAvailability::available &&
               utf8_equal(message_store_content(store, found_10.handle), utf8_literal("now")),
           "message_event_pump_nonempty_content_is_available");

    GatewayClientEvent metadata_update = timeline_message_event(
        GatewayClientDispatchKind::message_update, 10, 7, 0,
        gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_flags, 0, 9);
    expect(message_event_pump_consume(&pump, metadata_update, ConstSlice<u8>{nullptr, 0}).ok() &&
               message_store_resolve(store, found_10.handle)->content_availability ==
                   MessageContentAvailability::available,
           "message_event_pump_absent_content_preserves_availability");

    GatewayClientEvent reaction_add{};
    reaction_add.kind = GatewayClientEventKind::dispatch;
    reaction_add.dispatch_kind = GatewayClientDispatchKind::message_reaction_add;
    reaction_add.dispatch_payload_bytes = 5;
    reaction_add.reaction.message_id = 10;
    reaction_add.reaction.channel_id = 7;
    reaction_add.reaction.user_id = 99;
    reaction_add.reaction.emoji_id = 44;
    reaction_add.reaction.name_bytes = 5;
    reaction_add.reaction.type = static_cast<u8>(MessageReactionType::normal);
    const MessageEventPumpConsumeResult reaction_applied =
        message_event_pump_consume(&pump, reaction_add, byte_literal("party"));
    const MessageRichRecord* pumped_rich = message_rich_store_find(rich_store, 10);
    const bool reaction_add_state =
        pumped_rich && pumped_rich->reaction_count == 1 && pumped_rich->reactions[0].count == 1;
    reaction_add.dispatch_kind = GatewayClientDispatchKind::message_reaction_remove;
    const MessageEventPumpConsumeResult reaction_removed =
        message_event_pump_consume(&pump, reaction_add, byte_literal("party"));
    expect(reaction_applied.ok() && reaction_applied.kind == MessageEventPumpConsumeKind::applied &&
               reaction_applied.repaint && reaction_add_state && reaction_removed.ok() &&
               reaction_removed.kind == MessageEventPumpConsumeKind::applied &&
               !message_rich_store_find(rich_store, 10),
           "message_event_pump_applies_gateway_reaction_deltas");

    GatewayClientEvent lifecycle{};
    lifecycle.kind = GatewayClientEventKind::ready;
    const MessageEventPumpConsumeResult ready =
        message_event_pump_consume(&pump, lifecycle, ConstSlice<u8>{nullptr, 0});
    GatewayClientEvent unknown{};
    unknown.kind = GatewayClientEventKind::dispatch;
    unknown.dispatch_kind = GatewayClientDispatchKind::unknown;
    const MessageEventPumpConsumeResult ignored =
        message_event_pump_consume(&pump, unknown, ConstSlice<u8>{nullptr, 0});
    const MessageEventPumpConsumeResult missing =
        message_event_pump_consume(&pump, timeline_delete_event(999, 7), ConstSlice<u8>{nullptr, 0});
    expect(ready.ok() && ready.kind == MessageEventPumpConsumeKind::lifecycle && ready.repaint &&
               ignored.ok() && ignored.kind == MessageEventPumpConsumeKind::ignored_dispatch &&
               missing.ok() && missing.kind == MessageEventPumpConsumeKind::not_resident &&
               pump.lifecycle == 1 && pump.ignored == 1 && pump.not_resident == 1,
           "message_event_pump_lifecycle_unknown_not_resident");

    GatewayClientEvent catchup_marker{};
    catchup_marker.kind = GatewayClientEventKind::dispatch;
    catchup_marker.dispatch_kind = GatewayClientDispatchKind::message_catchup_required;
    const MessageEventPumpConsumeResult catchup_requested =
        message_event_pump_consume(&pump, catchup_marker, ConstSlice<u8>{nullptr, 0});
    expect(catchup_requested.ok() &&
               catchup_requested.kind == MessageEventPumpConsumeKind::ignored_dispatch &&
               pump.catchup_required && timeline.newer_refetch_required && !pump.resync_required,
           "message_event_pump_decode_skip_requests_nondestructive_catchup");

    GatewayClientEvent malformed =
        timeline_message_event(GatewayClientDispatchKind::message_create, 20, 7, 11, create_fields, 2);
    const MessageEventPumpConsumeResult rejected =
        message_event_pump_consume(&pump, malformed, byte_literal("x"));
    expect(rejected.error == MessageEventPumpError::apply &&
               rejected.kind == MessageEventPumpConsumeKind::rejected && pump.resync_required &&
               pump.rejected == 1 && store.count == 1,
           "message_event_pump_rejection_requires_resync");

    {
        MessageEventPump normalization_pump{};
        normalization_pump.timeline = &timeline;
        normalization_pump.has_sequence = true;
        normalization_pump.last_sequence = 40;
        GatewayClientEvent input{};
        input.kind = GatewayClientEventKind::dispatch;
        input.dispatch_kind = GatewayClientDispatchKind::unknown;
        input.has_sequence = true;
        input.sequence = 41;
        const GatewayClientEvent original = input;
        const auto borrowed = message_event_pump_consume(&normalization_pump, input, {});
        expect(borrowed.ok() && borrowed.kind == MessageEventPumpConsumeKind::ignored_dispatch &&
                   normalization_pump.last_sequence == 41 && normalization_pump.ignored == 1 &&
                   maicord::bytes_equal(&input, &original, sizeof(input)),
               "message_event_pump_plain_event_advances_sequence_without_input_mutation");

        input.sequence = 42;
        input.dispatch_payload_bytes = 1;
        const auto bad_length = message_event_pump_consume(&normalization_pump, input, {});
        expect(bad_length.error == MessageEventPumpError::apply &&
                   bad_length.kind == MessageEventPumpConsumeKind::rejected &&
                   normalization_pump.rejected == 1 && normalization_pump.resync_required &&
                   normalization_pump.last_sequence == 41 && normalization_pump.ignored == 1,
               "message_event_pump_length_rejection_precedes_sequence_observation");
        const auto null_body = message_event_pump_consume(&normalization_pump, input, {nullptr, 1});
        expect(null_body.error == MessageEventPumpError::invalid_input &&
                   normalization_pump.rejected == 1 && normalization_pump.last_sequence == 41,
               "message_event_pump_null_body_preserves_sequence_and_rejection_count");

        GatewayClientNotificationSidecar sidecar{};
        sidecar.guild_id = 10;
        sidecar.thread_count = 1;
        GatewayClientNotificationThreadRecord thread{};
        thread.thread.guild_id = 10;
        thread.thread.thread_id = 20;
        u8 body[sizeof(sidecar) + sizeof(thread)]{};
        copy_bytes(body, &sidecar, sizeof(sidecar));
        copy_bytes(body + sizeof(sidecar), &thread, sizeof(thread));
        input.dispatch_payload_bytes = sizeof(body);
        input.notification_payload_bytes = sizeof(body);
        const GatewayClientEvent original_sidecar_event = input;
        u8 original_body[sizeof(body)]{};
        copy_bytes(original_body, body, sizeof(body));
        const auto trimmed = message_event_pump_consume(&normalization_pump, input, {body, sizeof(body)});
        expect(trimmed.ok() && trimmed.kind == MessageEventPumpConsumeKind::ignored_dispatch &&
                   normalization_pump.last_sequence == 42 && normalization_pump.ignored == 2 &&
                   maicord::bytes_equal(&input, &original_sidecar_event, sizeof(input)) &&
                   maicord::bytes_equal(body, original_body, sizeof(body)),
               "message_event_pump_valid_sidecar_advances_sequence_without_input_mutation");

        input.sequence = 43;
        sidecar.flags = 0x80000000u;
        copy_bytes(body, &sidecar, sizeof(sidecar));
        const auto bad_sidecar = message_event_pump_consume(&normalization_pump, input, {body, sizeof(body)});
        sidecar.flags = 0;
        copy_bytes(body, &sidecar, sizeof(sidecar));
        thread.reserved = 1;
        copy_bytes(body + sizeof(sidecar), &thread, sizeof(thread));
        const auto bad_thread = message_event_pump_consume(&normalization_pump, input, {body, sizeof(body)});
        input.notification_payload_bytes = sizeof(body) + 1;
        const auto bad_tail = message_event_pump_consume(&normalization_pump, input, {body, sizeof(body)});
        expect(bad_sidecar.error == MessageEventPumpError::apply &&
                   bad_thread.error == MessageEventPumpError::apply && bad_tail.error == MessageEventPumpError::apply &&
                   bad_sidecar.kind == MessageEventPumpConsumeKind::rejected &&
                   bad_thread.kind == MessageEventPumpConsumeKind::rejected &&
                   bad_tail.kind == MessageEventPumpConsumeKind::rejected && normalization_pump.rejected == 4 &&
                   normalization_pump.last_sequence == 42 && normalization_pump.ignored == 2,
               "message_event_pump_malformed_sidecars_reject_before_sequence_observation");

        input.dispatch_payload_bytes = 0;
        input.notification_payload_bytes = kGatewayNotificationMetadataIncomplete;
        const auto incomplete = message_event_pump_consume(&normalization_pump, input, {});
        expect(incomplete.ok() && incomplete.kind == MessageEventPumpConsumeKind::ignored_dispatch &&
                   normalization_pump.last_sequence == 43 && normalization_pump.ignored == 3 &&
                   input.notification_payload_bytes == kGatewayNotificationMetadataIncomplete && store.count == 1,
               "message_event_pump_incomplete_sidecar_preserves_event_and_ordinary_state");

        input.sequence = 44;
        input.notification_payload_bytes = 0;
        input.dispatch_payload_bytes = 3;
        const auto plain_body = message_event_pump_consume(&normalization_pump, input, byte_literal("raw"));
        expect(plain_body.ok() && plain_body.kind == MessageEventPumpConsumeKind::ignored_dispatch &&
                   !plain_body.repaint && normalization_pump.last_sequence == 44 &&
                   normalization_pump.has_sequence && normalization_pump.ignored == 4 &&
                   normalization_pump.rejected == 4 && normalization_pump.applied == 0 &&
                   normalization_pump.lifecycle == 0 && store.count == 1,
               "message_event_pump_plain_unknown_body_preserves_counters_and_sequence");
        input.has_sequence = false;
        input.sequence = 99;
        const auto unsequenced = message_event_pump_consume(&normalization_pump, input, byte_literal("raw"));
        expect(unsequenced.ok() && unsequenced.kind == MessageEventPumpConsumeKind::ignored_dispatch &&
                   normalization_pump.last_sequence == 44 && normalization_pump.has_sequence &&
                   normalization_pump.ignored == 5 && normalization_pump.rejected == 4 &&
                   store.count == 1,
               "message_event_pump_plain_unknown_without_sequence_preserves_last_sequence");
    }

    MessageRecord drain_records[8]{};
    u32 drain_order[8]{};
    u8 drain_text[256]{};
    MessageStore drain_store{};
    u32 drain_heights[8]{};
    u64 drain_fenwick[8]{};
    UiVariableList drain_list{};
    MessageTimeline drain_timeline{};
    MessageEventPump drain_pump{};
    expect(message_store_initialize(&drain_store, Slice<MessageRecord>{drain_records, 8},
                                    Slice<u32>{drain_order, 8}, Slice<u8>{drain_text, 256}) &&
               ui_variable_list_initialize(&drain_list, Slice<u32>{drain_heights, 8},
                                           Slice<u64>{drain_fenwick, 8}, 0, 100) &&
               message_timeline_initialize(&drain_timeline, &drain_store, &drain_list, 100, 100, true) &&
               message_event_pump_initialize(&drain_pump, &drain_timeline,
                                             Slice<u8>{payload_scratch, kMessageEventPumpPayloadCapacity},
                                             Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds},
                                             MessageContentAvailability::available),
           "message_event_pump_drain_fixture");
    PumpFixtureEntry entries[4]{
        {timeline_message_event(GatewayClientDispatchKind::message_create, 100, 7, 11, create_fields, 3),
         byte_literal("one")},
        {timeline_message_event(GatewayClientDispatchKind::message_create, 300, 7, 11, create_fields, 5),
         byte_literal("three")},
        {timeline_message_event(GatewayClientDispatchKind::message_create, 200, 7, 11, create_fields, 3),
         byte_literal("two")},
        {timeline_message_event(GatewayClientDispatchKind::message_update, 300, 7, 0, update_fields, 4),
         byte_literal("THRE")},
    };
    for (u32 index = 0; index < 4; ++index) {
        entries[index].event.sequence = index + 1;
        entries[index].event.has_sequence = true;
    }
    PumpFixtureSource source{entries, 4, 0};
    const MessageEventPumpDrainResult first_drain =
        message_event_pump_drain(&drain_pump, &source, pump_fixture_pop, 3);
    const MessageEventPumpDrainResult second_drain =
        message_event_pump_drain(&drain_pump, &source, pump_fixture_pop, 3);
    const MessageEventPumpDrainResult empty_drain =
        message_event_pump_drain(&drain_pump, &source, pump_fixture_pop, 3);
    expect(first_drain.ok() && first_drain.popped == 3 && first_drain.applied == 3 && first_drain.repaint &&
               first_drain.invalidation.first == 0 && first_drain.invalidation.structural &&
               drain_store.count == 3 && drain_list.count == 3 && second_drain.ok() &&
               second_drain.popped == 1 && second_drain.applied == 1 &&
               second_drain.invalidation.first == 2 && !second_drain.invalidation.structural &&
               empty_drain.ok() && empty_drain.popped == 0 && drain_pump.drained == 4 &&
               drain_pump.last_sequence == 4,
           "message_event_pump_bounded_drain_and_damage_merge");

    MessageSendTracker drain_sends{};
    const MessageSendBeginResult optimistic =
        message_send_tracker_initialize(&drain_sends) &&
                message_event_pump_set_send_tracker(&drain_pump, &drain_sends)
            ? message_send_tracker_begin(&drain_sends, &drain_timeline, 7, 11, utf8_literal("echo"),
                                         1'725'012'296'900ull, 100)
            : MessageSendBeginResult{};
    const MessageSendObserveResult optimistic_http = message_send_tracker_observe_http(
        &drain_sends, &drain_timeline, optimistic.nonce, 200, MessageSendHttpDisposition::success, 110);
    GatewayClientEvent optimistic_echo =
        timeline_message_event(GatewayClientDispatchKind::message_create, 400, 7, 11,
                               create_fields | gateway_message_field_nonce, 4);
    optimistic_echo.message.nonce = optimistic.nonce;
    optimistic_echo.timestamp_ms = 115;
    const MessageEventPumpConsumeResult echo_applied =
        message_event_pump_consume(&drain_pump, optimistic_echo, byte_literal("echo"));
    MessageSendCompletion echo_completion{};
    expect(optimistic.ok() && optimistic_http.ok() && echo_applied.ok() &&
               echo_applied.kind == MessageEventPumpConsumeKind::applied && echo_applied.repaint &&
               echo_applied.invalidation.structural &&
               message_store_find(drain_store, optimistic.nonce).error == MessageStoreError::not_found &&
               message_store_find(drain_store, 400).ok() && drain_store.count == 4 &&
               message_send_tracker_take_completion(&drain_sends, &echo_completion) &&
               echo_completion.kind == MessageSendCompletionKind::accepted &&
               echo_completion.authoritative_id == 400 && echo_completion.api_elapsed_ms == 10 &&
               echo_completion.gateway_elapsed_ms == 15,
           "message_event_pump_reconciles_optimistic_nonce_echo");

    expect(message_timeline_reset(&drain_timeline), "message_event_pump_collision_fixture_reset");
    const auto collision_send = message_send_tracker_begin(&drain_sends, &drain_timeline, 7, 11,
        utf8_literal("ours"), 1'725'012'296'901ull, 200);
    expect(collision_send.ok() && message_send_tracker_observe_http(&drain_sends, &drain_timeline, collision_send.nonce,
        200, MessageSendHttpDisposition::success, 210).ok(), "message_event_pump_collision_send_submitted");
    auto unrelated_echo = timeline_message_event(GatewayClientDispatchKind::message_create, 500, 7, 12,
        create_fields | gateway_message_field_nonce, 4);
    unrelated_echo.message.nonce = collision_send.nonce; unrelated_echo.timestamp_ms = 215;
    const auto unrelated_applied = message_event_pump_consume(&drain_pump, unrelated_echo, byte_literal("them"));
    expect(unrelated_applied.ok() && unrelated_applied.kind == MessageEventPumpConsumeKind::applied &&
        !drain_pump.resync_required && message_store_find(drain_store, collision_send.nonce).ok() &&
        message_store_find(drain_store, 500).ok() && drain_sends.active_count == 1 &&
        !message_send_tracker_take_completion(&drain_sends, &echo_completion),
        "message_event_pump_renders_unrelated_author_without_confirming_colliding_nonce");
    unrelated_echo.message.author_id = 11; unrelated_echo.message.id = 501; unrelated_echo.timestamp_ms = 220;
    const auto own_applied = message_event_pump_consume(&drain_pump, unrelated_echo, byte_literal("ours"));
    expect(own_applied.ok() && !drain_pump.resync_required && drain_store.count == 2 &&
        message_store_find(drain_store, 500).ok() && message_store_find(drain_store, 501).ok() &&
        !message_store_find(drain_store, collision_send.nonce).ok() &&
        message_send_tracker_take_completion(&drain_sends, &echo_completion) && echo_completion.authoritative_id == 501,
        "message_event_pump_only_matching_author_confirms_local_send");

    // A record beyond the largest (guild) record capacity is refused and kept.
    PumpFixtureEntry oversized_entry{timeline_message_event(GatewayClientDispatchKind::message_create, 400,
                                                            7, 11, create_fields,
                                                            kGatewayClientGuildRecordCapacity + 1),
                                     ConstSlice<u8>{payload_scratch, kGatewayClientGuildRecordCapacity + 1}};
    PumpFixtureSource oversized_source{&oversized_entry, 1, 0};
    const MessageEventPumpDrainResult oversized =
        message_event_pump_drain(&drain_pump, &oversized_source, pump_fixture_pop, 1);
    expect(oversized.error == MessageEventPumpError::payload_capacity && oversized.resync_required &&
               oversized_source.cursor == 0 && !drain_pump.large_payload.committed &&
               !message_event_pump_initialize(
                   &pump, &timeline, Slice<u8>{payload_scratch, kMessageEventPumpPayloadCapacity - 1},
                   Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds},
                   MessageContentAvailability::available),
           "message_event_pump_payload_capacity_is_nondestructive");
    // A record larger than the pump scratch but within it is popped into the
    // on-demand reservation, which holds no committed pages afterwards.
    static u8 large_bytes[kMessageEventPumpPayloadCapacity + 1]{};
    PumpFixtureEntry large_entry{timeline_message_event(GatewayClientDispatchKind::message_create, 401, 7, 11,
                                                        create_fields, kMessageEventPumpPayloadCapacity + 1),
                                 ConstSlice<u8>{large_bytes, kMessageEventPumpPayloadCapacity + 1}};
    PumpFixtureSource large_source{&large_entry, 1, 0};
    const MessageEventPumpDrainResult large =
        message_event_pump_drain(&drain_pump, &large_source, pump_fixture_pop, 1);
    expect(large.popped == 1 && large_source.cursor == 1 && drain_pump.large_payload.base &&
               !drain_pump.large_payload.committed && drain_pump.large_payload.peak != 0,
           "message_event_pump_pops_oversized_records_into_on_demand_storage");
    reserved_bytes_release(&drain_pump.large_payload);

    MessageRecord deferred_records[2]{};
    u32 deferred_order[2]{};
    u8 deferred_text[16]{};
    MessageStore deferred_store{};
    u32 deferred_heights[2]{};
    u64 deferred_fenwick[2]{};
    UiVariableList deferred_list{};
    MessageTimeline deferred_timeline{};
    MessageEventPump deferred_pump{};
    expect(message_store_initialize(&deferred_store, Slice<MessageRecord>{deferred_records, 2},
                                    Slice<u32>{deferred_order, 2}, Slice<u8>{deferred_text, 16}) &&
               message_store_upsert(&deferred_store, MessageInput{10, 7, 1, 1, 0, utf8_literal("a"), 0})
                   .ok() &&
               message_store_upsert(&deferred_store, MessageInput{20, 7, 1, 2, 0, utf8_literal("b"), 0})
                   .ok() &&
               ui_variable_list_initialize(&deferred_list, Slice<u32>{deferred_heights, 2},
                                           Slice<u64>{deferred_fenwick, 2}, 2, 100) &&
               message_timeline_initialize(&deferred_timeline, &deferred_store, &deferred_list, 100, 100,
                                           false) &&
               message_event_pump_initialize(&deferred_pump, &deferred_timeline,
                                             Slice<u8>{payload_scratch, kMessageEventPumpPayloadCapacity},
                                             Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds},
                                             MessageContentAvailability::available),
           "message_event_pump_deferred_fixture");
    PumpFixtureEntry deferred_entry{
        timeline_message_event(GatewayClientDispatchKind::message_create, 30, 7, 1, create_fields, 1),
        byte_literal("c")};
    PumpFixtureSource deferred_source{&deferred_entry, 1, 0};
    const MessageEventPumpDrainResult deferred_drain =
        message_event_pump_drain(&deferred_pump, &deferred_source, pump_fixture_pop, 1);
    expect(deferred_drain.ok() && deferred_drain.popped == 1 && deferred_drain.deferred == 1 &&
               deferred_drain.applied == 0 && !deferred_drain.repaint && !deferred_drain.resync_required &&
               deferred_drain.catchup_required && deferred_pump.catchup_required &&
               deferred_pump.deferred == 1 && deferred_pump.rejected == 0 && deferred_store.count == 2,
           "message_event_pump_deferred_is_not_resync_failure");

    struct ReactionObserveProbe {
        MessageStore* messages; MessageRichStore* rich;
        u32 reactions{}, sources{}, deletions{}, count{}, normal{}, snapshot_count{};
        u64 user{}, id{};
        MessageHandle handle{}; u32 revision{};
        bool applied{}, mine{}, source_committed{}, snapshot_unnamed{}, deleted{};
    } observed{&store, &rich_store};
    const auto reaction_observer = +[](void* context, const GatewayMessageReactionEvent& value,
                                       MessageHandle handle, u32 revision, bool applied) noexcept {
        auto& probe = *static_cast<ReactionObserveProbe*>(context);
        ++probe.reactions; probe.user = value.user_id; probe.id = value.message_id;
        probe.handle = handle; probe.revision = revision; probe.applied = applied;
        probe.count = probe.normal = 0; probe.mine = false;
        const auto* rich = applied ? message_rich_store_find(*probe.rich, value.message_id) : nullptr;
        for (u32 i = 0; rich && i < rich->reaction_count; ++i) {
            if (rich->reactions[i].emoji_id != value.emoji_id) continue;
            probe.count = rich->reactions[i].count; probe.normal = rich->reactions[i].normal_count;
            probe.mine = (rich->reactions[i].flags & 1u) != 0;
        }
    };
    const auto source_observer = +[](void* context, const GatewayMessageEvent& value,
                                     MessageHandle handle, u32 revision, bool applied) noexcept {
        auto& probe = *static_cast<ReactionObserveProbe*>(context);
        ++probe.sources; probe.id = value.id;
        probe.handle = handle; probe.revision = revision; probe.applied = applied;
        probe.snapshot_count = value.reactions.count;
        probe.snapshot_unnamed = value.reactions.count && value.reactions[0].emoji_id && !value.reactions[0].name.bytes;
        const auto* record = message_store_resolve(*probe.messages, handle);
        const auto* rich = message_rich_store_find(*probe.rich, value.id);
        probe.source_committed = applied && record && record->id == value.id && record->revision == revision &&
            (!(value.fields & gateway_message_field_reactions) || (rich && rich->message_revision == revision &&
                rich->reaction_count == value.reactions.count));
    };
    const auto delete_observer = +[](void* context, u64, u64 id) noexcept {
        auto& probe = *static_cast<ReactionObserveProbe*>(context);
        ++probe.deletions; probe.id = id;
        probe.deleted = !message_store_find(*probe.messages, id).ok();
    };
    message_event_pump_set_reaction_observers(&pump, reaction_observer, source_observer, delete_observer, &observed);
    ChatStateStore reaction_state{}; reaction_state.selected_channel_id = 7; reaction_state.self_user_id = 99;
    pump.state = &reaction_state;
    auto observed_add = reaction_add; observed_add.dispatch_kind = GatewayClientDispatchKind::message_reaction_add;
    expect(message_event_pump_consume(&pump, observed_add, byte_literal("party")).ok() && observed.reactions == 1 &&
        observed.applied && observed.mine && observed.count == 1 && observed.normal == 1 && observed.revision &&
        message_store_resolve(store, observed.handle)->id == 10,
        "message_event_pump_reaction_observer_sees_exact_self_source_after_canonical_commit");
    observed_add.reaction.user_id = 100; observed_add.reaction.name_bytes = 7; observed_add.dispatch_payload_bytes = 7;
    expect(message_event_pump_consume(&pump, observed_add, byte_literal("renamed")).ok() && observed.reactions == 2 &&
        observed.user == 100 && observed.mine && observed.count == 2 && observed.normal == 2 &&
        message_rich_store_find(rich_store, 10)->reaction_count == 1,
        "message_event_pump_reaction_observer_preserves_foreign_count_and_custom_rename_identity");
    observed_add.reaction.user_id = 99; observed_add.dispatch_kind = GatewayClientDispatchKind::message_reaction_remove;
    expect(message_event_pump_consume(&pump, observed_add, byte_literal("renamed")).ok() && observed.reactions == 3 &&
        !observed.mine && observed.count == 1 && observed.normal == 1,
        "message_event_pump_reaction_observer_sees_self_remove_without_erasing_foreign_count");
    observed_add.reaction.user_id = 0; observed_add.dispatch_kind = GatewayClientDispatchKind::message_reaction_remove_emoji;
    expect(message_event_pump_consume(&pump, observed_add, byte_literal("renamed")).ok() && observed.reactions == 4 &&
        observed.applied && !message_rich_store_find(rich_store, 10),
        "message_event_pump_reaction_observer_sees_administrative_emoji_barrier_after_commit");
    auto remove_all = observed_add; remove_all.dispatch_kind = GatewayClientDispatchKind::message_reaction_remove_all;
    remove_all.reaction.emoji_id = 0; remove_all.reaction.name_bytes = 0; remove_all.dispatch_payload_bytes = 0;
    expect(message_event_pump_consume(&pump, remove_all, {}).ok() && observed.reactions == 5,
        "message_event_pump_reaction_observer_sees_valid_empty_remove_all_barrier");
    observed_add.dispatch_kind = GatewayClientDispatchKind::message_reaction_add; observed_add.reaction.user_id = 99;
    observed_add.reaction.message_id = 999;
    expect(message_event_pump_consume(&pump, observed_add, byte_literal("renamed")).ok() && observed.reactions == 6 &&
        !observed.applied && !observed.revision && observed.handle.slot == ~u32{0},
        "message_event_pump_reaction_observer_accepts_nonresident_authority_without_mutating_live_store");
    observed_add.reaction.message_id = 10; observed_add.reaction.channel_id = 8;
    expect(message_event_pump_consume(&pump, observed_add, byte_literal("renamed")).ok() && observed.reactions == 7 &&
        !observed.applied && !message_rich_store_find(rich_store, 10),
        "message_event_pump_reaction_observer_observes_other_channel_without_cross_scope_model_mutation");
    observed_add.reaction.channel_id = 7;
    expect(!message_event_pump_consume(&pump, observed_add, byte_literal("party")).ok() && observed.reactions == 7,
        "message_event_pump_reaction_observer_never_publishes_malformed_payload");
    pump.state = nullptr;
    GatewayClientMessageRichHeader snapshot_header{}; snapshot_header.reaction_count = 1;
    GatewayClientMessageReactionMutation snapshot_reaction{};
    snapshot_reaction.emoji_id = 44; snapshot_reaction.count = 4; snapshot_reaction.normal_count = 3;
    snapshot_reaction.burst_count = 1; snapshot_reaction.flags = 1;
    u8 snapshot_body[sizeof(snapshot_header) + sizeof(snapshot_reaction)]{};
    copy_bytes(snapshot_body, &snapshot_header, sizeof(snapshot_header));
    copy_bytes(snapshot_body + sizeof(snapshot_header), &snapshot_reaction, sizeof(snapshot_reaction));
    auto replacement = timeline_message_event(GatewayClientDispatchKind::message_update, 10, 7, 0,
        gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_reactions, 0);
    replacement.message.rich_payload_bytes = replacement.dispatch_payload_bytes = sizeof(snapshot_body);
    expect(message_event_pump_consume(&pump, replacement, {snapshot_body, sizeof(snapshot_body)}).ok() &&
        observed.sources == 1 && observed.applied && observed.source_committed && observed.snapshot_count == 1 &&
        observed.snapshot_unnamed && message_rich_store_find(rich_store, 10)->reactions[0].count == 4 &&
        message_rich_store_find(rich_store, 10)->reactions[0].burst_count == 1,
        "message_event_pump_source_observer_borrows_full_null_custom_snapshot_after_exact_revision_commit");
    replacement.message.id = 999;
    expect(message_event_pump_consume(&pump, replacement, {snapshot_body, sizeof(snapshot_body)}).ok() &&
        observed.sources == 2 && !observed.applied && observed.snapshot_unnamed && observed.snapshot_count == 1,
        "message_event_pump_source_observer_sees_valid_nonresident_reaction_snapshot_without_model_copy");
    expect(!message_event_pump_consume(&pump, replacement, {snapshot_body, sizeof(snapshot_body) - 1}).ok() &&
        observed.sources == 2, "message_event_pump_source_observer_never_publishes_truncated_replacement");
    replacement.message.id = 0;
    expect(!message_event_pump_consume(&pump, replacement, {snapshot_body, sizeof(snapshot_body)}).ok() &&
        observed.sources == 2, "message_event_pump_source_observer_rejects_zero_identity_before_nonresident_notification");
    expect(message_event_pump_consume(&pump, timeline_delete_event(10, 7), {}).ok() && observed.deletions == 1 &&
        observed.deleted, "message_event_pump_delete_observer_runs_after_message_and_rich_retirement");
    u8 deleted_body[16]{}; byte_store_le64(deleted_body, 999); byte_store_le64(deleted_body + 8, 998);
    auto bulk_delete = timeline_delete_event(999, 7); bulk_delete.dispatch_kind = GatewayClientDispatchKind::message_bulk_delete;
    bulk_delete.message.id_count = 2; bulk_delete.dispatch_payload_bytes = sizeof(deleted_body);
    expect(message_event_pump_consume(&pump, bulk_delete, {deleted_body, sizeof(deleted_body)}).ok() &&
        observed.deletions == 3 && observed.id == 998,
        "message_event_pump_delete_observer_notifies_all_valid_nonresident_bulk_identities");
    byte_store_le64(deleted_body + 8, 0);
    expect(!message_event_pump_consume(&pump, bulk_delete, {deleted_body, sizeof(deleted_body)}).ok() &&
        observed.deletions == 3, "message_event_pump_delete_observer_rejects_entire_malformed_bulk_before_notification");
    observed.messages = &deferred_store; observed.rich = &rich_store;
    message_event_pump_set_reaction_observers(&deferred_pump, nullptr, source_observer, nullptr, &observed);
    const auto evicted_handle = message_store_find(deferred_store, 10).handle;
    deferred_timeline.newer_refetch_required = false; deferred_timeline.follow_bottom = true;
    expect(message_event_pump_consume(&deferred_pump, deferred_entry.event, deferred_entry.payload).ok() &&
        observed.applied && observed.source_committed && observed.id == 30 &&
        !message_store_resolve(deferred_store, evicted_handle),
        "message_event_pump_source_observer_detects_canonical_create_eviction_and_recycled_handle_after_commit");
    message_event_pump_set_reaction_observers(&pump, nullptr, nullptr, nullptr, nullptr);
    message_event_pump_reset(&deferred_pump);
    expect(!pump.reaction_observe && !pump.reaction_source_observe && !pump.reaction_delete_observe &&
        !pump.reaction_observe_context && !deferred_pump.reaction_source_observe,
        "message_event_pump_reaction_observers_clear_without_extra_owner_or_work_bank");
}

void test_message_edit_runtime() noexcept {
    MessageEditRuntime runtime{};
    expect(message_edit_runtime_initialize(&runtime) && runtime.content && runtime.saved_draft &&
               runtime.content_storage.capacity == 0 && runtime.saved_draft_storage.capacity == 0,
           "message_edit_runtime_initialize_lazy");
    expect(message_edit_runtime_reserve(&runtime, 17, 33), "message_edit_runtime_reserve");
    char* const content = runtime.content;
    char* const saved_draft = runtime.saved_draft;
    for (u32 index = 0; index < 17; ++index) {
        content[index] = static_cast<char>(index + 1);
    }
    for (u32 index = 0; index < 33; ++index) {
        saved_draft[index] = static_cast<char>(index + 2);
    }
    runtime.content_bytes = 17;
    runtime.saved_draft_bytes = 33;
    runtime.message_id = 44;
    runtime.editor_active = true;
    message_edit_runtime_reset(&runtime);
    bool erased = runtime.content == content && runtime.saved_draft == saved_draft &&
                  runtime.content_bytes == 0 && runtime.saved_draft_bytes == 0 && runtime.message_id == 0 &&
                  !runtime.editor_active;
    for (u32 index = 0; index < 17; ++index) {
        erased = erased && content[index] == 0;
    }
    for (u32 index = 0; index < 33; ++index) {
        erased = erased && saved_draft[index] == 0;
    }
    expect(erased, "message_edit_runtime_reset_preserves_storage_and_erases_text");
    expect(message_edit_runtime_reserve(&runtime, kMessageComposerMaximumBytes,
                                        kMessageComposerMaximumBytes) &&
               !message_edit_runtime_reserve(&runtime, kMessageComposerMaximumBytes + 1, 0),
           "message_edit_runtime_capacity_bound");
    message_edit_runtime_release(&runtime);
    expect(!runtime.content && !runtime.saved_draft && !runtime.content_storage.arena.base &&
               !runtime.saved_draft_storage.arena.base,
           "message_edit_runtime_release");
}

void test_message_history_protocol() noexcept {
    u8 target[128]{};
    const MessageHistoryTargetResult after = write_message_history_target(
        7, MessageHistoryQuery{100, 100, MessageHistoryPosition::after},
        Slice<u8>{target, 128});
    expect(after.ok() && bytes_equal(ConstSlice<u8>{target, after.bytes},
                                     byte_literal("/api/v10/channels/7/messages?after=100&limit=100")),
           "message_history_after_target");
    expect(write_message_history_target(
               7, MessageHistoryQuery{1, 50, MessageHistoryPosition::latest},
               Slice<u8>{target, 128})
                       .error == MessageHistoryTargetError::invalid_input &&
               write_message_history_target(
                   7, MessageHistoryQuery{0, 50, MessageHistoryPosition::before},
                   Slice<u8>{target, 128})
                       .error == MessageHistoryTargetError::invalid_input &&
               write_message_history_target(
                   7, MessageHistoryQuery{1, 101, MessageHistoryPosition::around},
                   Slice<u8>{target, 128})
                       .error == MessageHistoryTargetError::invalid_input &&
               write_message_history_target(
                   7, MessageHistoryQuery{1, 25, MessageHistoryPosition::before},
                   Slice<u8>{target, 8})
                       .error == MessageHistoryTargetError::output_capacity,
           "message_history_target_bounds");

    constexpr Utf8View page_body = utf8_literal(
        R"([{"id":"300","channel_id":"7","author":{"id":"11"},"nonce":"1234567890123456789","content":"three","timestamp":"2024-08-30T12:00:00Z","flags":1},{"id":"200","channel_id":"7","author":{"id":"12"},"nonce":1234567890123456788,"content":"tw\u006f","timestamp":"2024-08-30T11:00:00Z"},{"id":"100","channel_id":"7","author":{"id":"13"},"content":"","timestamp":"2024-08-30T10:00:00Z","future":{"nested":true}}])");
    u32 page_structurals[256]{};
    char content[32]{};
    GatewayMessageEvent messages[3]{};
    const MessageHistoryDecodeResult decoded = decode_message_history_page(
        page_body, Slice<u32>{page_structurals, 256}, 16,
        Slice<char>{content, 32}, Slice<GatewayMessageEvent>{messages, 3});
    expect(decoded.ok() && decoded.page.count == 3 && decoded.page.channel_id == 7 &&
               decoded.page.newest_id == 300 && decoded.page.oldest_id == 100 &&
               decoded.page.content_bytes == 8 && messages[0].id == 300 && messages[1].id == 200 &&
               messages[2].id == 100 && messages[0].flags == 1 &&
               utf8_equal(messages[0].content, utf8_literal("three")) &&
               utf8_equal(messages[1].content, utf8_literal("two")) &&
               messages[0].content.data == content && messages[1].content.data == content + 5,
           "message_history_page_decode_and_pack");
    expect(messages[0].nonce == 1234567890123456789ull && messages[1].nonce == 1234567890123456788ull &&
        (messages[0].fields & gateway_message_field_nonce) && (messages[1].fields & gateway_message_field_nonce) &&
        !messages[2].nonce && !(messages[2].fields & gateway_message_field_nonce),
        "message_history_optional_string_and_integer_nonces_preserve_exact_identity");
    const auto opaque_nonce_page = decode_message_history_page(utf8_literal(
        R"([{"id":"300","channel_id":"7","author":{"id":"11"},"nonce":"other-client-request","content":"three","timestamp":"2024-08-30T12:00:00Z"},{"id":"200","channel_id":"7","author":{"id":"12"},"nonce":"9999999999999999999999999","content":"two","timestamp":"2024-08-30T11:00:00Z"},{"id":"100","channel_id":"7","author":{"id":"13"},"nonce":0,"content":"","timestamp":"2024-08-30T10:00:00Z"}])"),
        {page_structurals, 256}, 16, {content, 32}, {messages, 3});
    expect(opaque_nonce_page.ok() && opaque_nonce_page.page.count == 3 &&
        !messages[0].nonce && !messages[1].nonce && !messages[2].nonce,
        "message_history_opaque_overflow_and_zero_nonces_are_valid_but_cannot_match_outbox");

    constexpr Utf8View named_page_body = utf8_literal(
        R"([{"id":"400","channel_id":"7","author":{"id":"21","username":"alpha","global_name":"Display","avatar":"0123456789abcdef0123456789abcdef"},"content":"hi","timestamp":"2024-08-30T13:00:00Z","mentions":[{"id":"22","username":"Beta"}]}])");
    char named_content[64]{};
    GatewayMessageEvent named_messages[1]{};
    GatewayMessageNamedUser named_users[2]{};
    const MessageHistoryDecodeResult named_decoded = decode_message_history_page(
        named_page_body, Slice<u32>{page_structurals, 256}, 16,
        Slice<char>{named_content, 64}, Slice<GatewayMessageEvent>{named_messages, 1},
        Slice<GatewayMessageNamedUser>{named_users, 2});
    expect(named_decoded.ok() && named_messages[0].content.bytes == 2 &&
               utf8_equal(named_messages[0].author_name, utf8_literal("Display")) &&
               utf8_equal(named_messages[0].author_avatar_hash,
                          utf8_literal("0123456789abcdef0123456789abcdef")) &&
               named_messages[0].mention_users.count == 1 && named_messages[0].mention_users[0].id == 22 &&
               utf8_equal(named_messages[0].mention_users[0].name, utf8_literal("Beta")) &&
               named_messages[0].transient_bytes == 45,
           "message_history_page_author_and_mentions_pack");

    constexpr Utf8View rich_page_body = utf8_literal(
        R"([{"id":"500","channel_id":"7","author":{"id":"21","username":"alpha"},"content":"rich","timestamp":"2024-08-30T14:00:00Z","reactions":[{"count":2,"count_details":{"normal":2,"burst":0},"me":true,"emoji":{"id":"44","name":"party","animated":true}}],"attachments":[{"id":"70","filename":"clip.mp4","width":0,"height":null,"size":123,"url":"https://cdn.discordapp.com/attachments/7/70/clip.mp4"}],"embeds":[{"title":"Build","type":"rich","thumbnail":{"url":"https://example.com/thumb.png","width":null,"height":0},"fields":[{"name":"Target","value":"Release"}]}],"sticker_items":[{"id":"88","name":"Wave","format_type":2}],"components":[{"type":10,"id":1,"content":"Component"}]},{"id":"450","channel_id":"7","author":{"id":"22","username":"beta"},"content":"reply","timestamp":"2024-08-30T13:30:00Z","message_reference":{"message_id":"400","channel_id":"7"},"referenced_message":null,"reactions":[{"count":1,"me":false,"emoji":{"id":null,"name":"🔥"}}],"attachments":[],"embeds":[],"sticker_items":[],"components":[]}])");
    u32 rich_page_structurals[1'024]{};
    char rich_page_text[8 * 1024]{};
    GatewayMessageEvent rich_messages[2]{};
    GatewayMessageNamedUser rich_named_users[4]{};
    GatewayMessageReaction rich_reactions[4]{};
    GatewayMessageAttachment rich_attachments[2]{};
    GatewayMessageEmbed rich_embeds[2]{};
    GatewayMessageEmbedField rich_fields[4]{};
    GatewayMessageSticker rich_stickers[2]{};
    GatewayMessageComponent rich_components[4]{};
    const MessageHistoryRichScratch history_rich_scratch{
        Slice<GatewayMessageReaction>{rich_reactions, 4},
        Slice<GatewayMessageAttachment>{rich_attachments, 2},
        Slice<GatewayMessageEmbed>{rich_embeds, 2},
        Slice<GatewayMessageEmbedField>{rich_fields, 4},
        Slice<GatewayMessageSticker>{rich_stickers, 2},
        Slice<GatewayMessageComponent>{rich_components, 4}};
    const MessageHistoryDecodeResult rich_history_decoded = decode_message_history_page(
        rich_page_body, Slice<u32>{rich_page_structurals, 1'024}, 32, Slice<char>{rich_page_text, 8 * 1024},
        Slice<GatewayMessageEvent>{rich_messages, 2}, Slice<GatewayMessageNamedUser>{rich_named_users, 4},
        history_rich_scratch);
    expect(rich_history_decoded.ok() && rich_history_decoded.page.count == 2 &&
               rich_messages[0].reactions.count == 1 && rich_messages[1].reactions.count == 1 &&
               rich_messages[0].reactions.data == rich_reactions &&
               rich_messages[1].reactions.data == rich_reactions + 1 && rich_messages[0].reactions[0].me &&
               rich_messages[0].attachments.count == 1 && rich_messages[0].embeds.count == 1 &&
               rich_messages[0].embed_fields.count == 1 &&
               utf8_equal(rich_messages[0].embed_fields[0].value, utf8_literal("Release")) &&
               rich_messages[0].stickers.count == 1 && rich_messages[0].components.count == 1 &&
               rich_messages[1].reference.state == GatewayMessageReferenceState::deleted &&
               rich_messages[1].reference.message_id == 400,
           "message_history_rich_aggregate_slices");
    expect(rich_history_decoded.ok() && !rich_attachments[0].has_width &&
        !rich_attachments[0].has_height && !rich_embeds[0].thumbnail.has_width &&
        !rich_embeds[0].thumbnail.has_height,
        "pending_media_dimensions_do_not_reject_channel_history");

    GatewayMessageEvent small_messages[2]{};
    expect(decode_message_history_page(
               page_body, Slice<u32>{page_structurals, 256}, 16,
               Slice<char>{content, 32}, Slice<GatewayMessageEvent>{small_messages, 2})
                       .error == MessageHistoryDecodeError::message_capacity &&
               decode_message_history_page(
                   page_body, Slice<u32>{page_structurals, 256}, 16,
                   Slice<char>{content, 4}, Slice<GatewayMessageEvent>{messages, 3})
                       .message_error == GatewayEventDecodeError::message_content_capacity &&
               decode_message_history_page(
                   page_body, Slice<u32>{page_structurals, 1}, 16,
                   Slice<char>{content, 32}, Slice<GatewayMessageEvent>{messages, 3})
                       .error == MessageHistoryDecodeError::scan_failed,
           "message_history_page_capacity_bounds");

    constexpr Utf8View wrong_order = utf8_literal(
        R"([{"id":"100","channel_id":"7","author":{"id":"11"},"content":"a","timestamp":"2024-08-30T10:00:00Z"},{"id":"200","channel_id":"7","author":{"id":"12"},"content":"b","timestamp":"2024-08-30T11:00:00Z"}])");
    constexpr Utf8View wrong_channel = utf8_literal(
        R"([{"id":"200","channel_id":"7","author":{"id":"11"},"content":"a","timestamp":"2024-08-30T11:00:00Z"},{"id":"100","channel_id":"8","author":{"id":"12"},"content":"b","timestamp":"2024-08-30T10:00:00Z"}])");
    constexpr Utf8View invalid_message =
        utf8_literal(R"([{"id":"100","channel_id":"7","content":"a","timestamp":"2024-08-30T10:00:00Z"}])");
    expect(decode_message_history_page(
               wrong_order, Slice<u32>{page_structurals, 256}, 16,
               Slice<char>{content, 32}, Slice<GatewayMessageEvent>{messages, 3})
                       .error == MessageHistoryDecodeError::invalid_order &&
               decode_message_history_page(
                   wrong_channel, Slice<u32>{page_structurals, 256}, 16,
                   Slice<char>{content, 32}, Slice<GatewayMessageEvent>{messages, 3})
                       .error == MessageHistoryDecodeError::channel_mismatch &&
               decode_message_history_page(
                   invalid_message, Slice<u32>{page_structurals, 256}, 16,
                   Slice<char>{content, 32}, Slice<GatewayMessageEvent>{messages, 3})
                       .message_error == GatewayEventDecodeError::missing_required_field,
           "message_history_page_order_channel_and_message_validation");

    const MessageHistoryDecodeResult empty = decode_message_history_page(
        utf8_literal("[]"), Slice<u32>{page_structurals, 256}, 16,
        Slice<char>{nullptr, 0}, Slice<GatewayMessageEvent>{nullptr, 0});
    expect(empty.ok() && empty.page.count == 0 && empty.page.channel_id == 0, "message_history_empty_page");
}

void test_message_history_sync() noexcept {
    constexpr u32 create_fields = gateway_message_field_id | gateway_message_field_channel_id |
                                  gateway_message_field_author_id | gateway_message_field_content |
                                  gateway_message_field_timestamp;
    MessageRecord records[6]{};
    u32 order[6]{};
    u8 text[64]{};
    MessageStore store{};
    u32 heights[6]{};
    u64 fenwick[6]{};
    UiVariableList list{};
    MessageTimeline timeline{};
    expect(message_store_initialize(&store, Slice<MessageRecord>{records, 6}, Slice<u32>{order, 6},
                                    Slice<u8>{text, 64}) &&
               message_store_upsert(&store, MessageInput{300, 7, 11, 300, 0, utf8_literal("c"), 0}).ok() &&
               message_store_upsert(&store, MessageInput{400, 7, 11, 400, 0, utf8_literal("d"), 0}).ok() &&
               ui_variable_list_initialize(&list, Slice<u32>{heights, 6}, Slice<u64>{fenwick, 6}, 2, 100) &&
               message_timeline_initialize(&timeline, &store, &list, 100, 100, false) &&
               message_timeline_set_scroll(&timeline, 50, false),
           "message_history_sync_fixture");

    const GatewayMessageEvent older[2]{
        {200, 7, 12, 200, 0, utf8_literal("b"), 0, create_fields},
        {100, 7, 13, 100, 0, utf8_literal(""), 0, create_fields},
    };
    const MessageHistoryApplyResult prepended = message_timeline_apply_history_page(
        &timeline, ConstSlice<GatewayMessageEvent>{older, 2}, MessageHistoryDirection::older,
        MessageContentAvailability::restricted);
    expect(prepended.ok() && prepended.inserted == 2 && prepended.evicted == 0 &&
               prepended.invalidation.first == 0 && prepended.invalidation.structural &&
               prepended.invalidation.scroll_changed && store.count == 4 && list.count == 4 &&
               list.structural_updates == 1 && timeline.scroll_units == 250 &&
               timeline.scroll_origin_units == 200 &&
               timeline.history_pages == 1 &&
               message_store_resolve(store, message_store_find(store, 100).handle)->content_availability ==
                   MessageContentAvailability::available,
           "message_history_batch_prepend_preserves_anchor_once");

    expect(message_timeline_set_scroll(&timeline, 0, true), "message_history_batch_follow_bottom");
    const GatewayMessageEvent newer[2]{
        {600, 7, 14, 600, 0, utf8_literal("f"), 0, create_fields},
        {500, 7, 15, 500, 0, utf8_literal("e"), 0, create_fields},
    };
    const MessageHistoryApplyResult appended = message_timeline_apply_history_page(
        &timeline, ConstSlice<GatewayMessageEvent>{newer, 2}, MessageHistoryDirection::newer,
        MessageContentAvailability::available);
    expect(appended.ok() && appended.inserted == 2 && appended.evicted == 0 && store.count == 6 &&
               list.count == 6 && list.structural_updates == 2 && timeline.scroll_units == 500 &&
               timeline.scroll_origin_units == 400 &&
               message_timeline_is_at_bottom(timeline, 0),
           "message_history_batch_append_once");

    const GatewayMessageEvent retained[2]{
        {800, 7, 16, 800, 0, utf8_literal("h"), 0, create_fields},
        {700, 7, 17, 700, 0, utf8_literal("g"), 0, create_fields},
    };
    const MessageHistoryApplyResult replaced_edge = message_timeline_apply_history_page(
        &timeline, ConstSlice<GatewayMessageEvent>{retained, 2}, MessageHistoryDirection::newer,
        MessageContentAvailability::available);
    expect(replaced_edge.ok() && replaced_edge.inserted == 2 && replaced_edge.evicted == 2 &&
               replaced_edge.compactions == 0 && store.count == 6 && list.count == 6 &&
               list.structural_updates == 4 && timeline.evicted == 2 && timeline.history_pages == 3 &&
               message_store_find(store, 100).error == MessageStoreError::not_found &&
               message_store_find(store, 200).error == MessageStoreError::not_found &&
               message_store_find(store, 700).ok() && message_store_find(store, 800).ok() &&
               message_timeline_is_at_bottom(timeline, 0),
           "message_history_batch_edge_replace_two_rebuilds");

    const GatewayMessageEvent overlap[1]{
        {750, 7, 18, 750, 0, utf8_literal("x"), 0, create_fields},
    };
    const u32 count_before_invalid = store.count;
    const u32 updates_before_invalid = list.structural_updates;
    const MessageHistoryApplyResult ignored_overlap = message_timeline_apply_history_page(
        &timeline, ConstSlice<GatewayMessageEvent>{overlap, 1}, MessageHistoryDirection::newer,
        MessageContentAvailability::available);
    const GatewayMessageEvent future[1]{
        {900, 7, 18, 900, 0, utf8_literal("x"), 0, create_fields},
    };
    const bool entered_scrollback = message_timeline_set_scroll(&timeline, 0, false);
    const MessageHistoryApplyResult appended_while_scrolled =
        entered_scrollback
            ? message_timeline_apply_history_page(&timeline, ConstSlice<GatewayMessageEvent>{future, 1},
                                                  MessageHistoryDirection::newer,
                                                  MessageContentAvailability::available)
            : MessageHistoryApplyResult{};
    expect(ignored_overlap.ok() && ignored_overlap.inserted == 0 && count_before_invalid == 6 &&
               updates_before_invalid == list.structural_updates - 2 && entered_scrollback &&
               appended_while_scrolled.ok() && appended_while_scrolled.inserted == 1 &&
               appended_while_scrolled.evicted == 1 && !timeline.follow_bottom &&
               timeline.scroll_units == 0 && store.count == 6 && message_store_find(store, 900).ok(),
           "message_history_batch_overlap_dedup_and_inflight_scrollback_anchor");

    GuildRecord mention_guilds[1]{};
    ChannelRecord mention_channels[1]{};
    RoleRecord mention_roles[2]{};
    PermissionOverwriteRecord mention_overwrites[1]{};
    u16 mention_guild_order[1]{};
    u16 mention_channel_order[1]{};
    u8 mention_names[256]{};
    ChatStateStore mention_state{};
    const u64 self_roles[1]{55};
    const bool mention_state_ready =
        chat_state_store_initialize(
            &mention_state, Slice<GuildRecord>{mention_guilds, 1},
            Slice<u16>{mention_guild_order, 1}, Slice<ChannelRecord>{mention_channels, 1},
            Slice<u16>{mention_channel_order, 1}, Slice<u8>{mention_names, sizeof(mention_names)}) &&
        chat_state_store_bind_permissions(
            &mention_state, Slice<RoleRecord>{mention_roles, 2},
            Slice<PermissionOverwriteRecord>{mention_overwrites, 1}) &&
        chat_state_store_upsert_guild(
            &mention_state,
            GuildInput{100, utf8_literal("Guild"), GuildMutationKind::create, 99})
            .ok() &&
        chat_state_store_upsert_channel(&mention_state,
                                           ChannelInput{7, 100, 0, utf8_literal("general"), 0, 0, 0})
            .ok() &&
        chat_state_store_upsert_role(&mention_state,
                                        RoleInput{55, 100, 0, utf8_literal("pink")}) &&
        chat_state_store_set_self_roles(&mention_state, 100, 99, 42, ConstSlice<u64>{self_roles, 1});
    const GatewayMessageEvent historical_mentions[2]{
        {900, 7, 18, 900, 0, utf8_literal("<@42>"), 0, create_fields},
        {800, 7, 16, 800, 0, utf8_literal("<@&55>"), 0, create_fields},
    };
    const bool mention_flags_applied =
        mention_state_ready &&
        message_history_apply_mention_flags(&timeline, &mention_state,
                                            ConstSlice<GatewayMessageEvent>{historical_mentions, 2});
    const MessageRecord* direct_mention_record =
        message_store_resolve(store, message_store_find(store, 900).handle);
    const MessageRecord* role_mention_record =
        message_store_resolve(store, message_store_find(store, 800).handle);
    expect(mention_flags_applied && direct_mention_record && role_mention_record &&
               direct_mention_record->presentation_flags == message_presentation_mentions_current_user &&
               role_mention_record->presentation_flags == message_presentation_mentions_current_user,
           "message_history_persists_direct_and_self_role_mention_highlights");

    MessageRecord catchup_records[4]{};
    u32 catchup_order[4]{};
    u8 catchup_text[32]{};
    MessageStore catchup_store{};
    u32 catchup_heights[4]{};
    u64 catchup_fenwick[4]{};
    UiVariableList catchup_list{};
    MessageTimeline catchup_timeline{};
    u8 payload_scratch[kMessageEventPumpPayloadCapacity]{};
    u64 bulk_ids[kGatewayBulkDeleteMaximumIds]{};
    MessageEventPump pump{};
    UserStore catchup_users{};
    UserRecord catchup_user_records[4]{};
    u32 catchup_user_order[4]{};
    u8 catchup_user_names[256]{};
    MessageRichStore catchup_rich{};
    MessageRichRecord catchup_rich_records[4]{};
    u16 catchup_rich_order[4]{};
    u8 catchup_rich_text[4'096]{};
    expect(
        message_store_initialize(&catchup_store, Slice<MessageRecord>{catchup_records, 4},
                                 Slice<u32>{catchup_order, 4}, Slice<u8>{catchup_text, 32}) &&
            message_store_upsert(&catchup_store, MessageInput{10, 7, 1, 10, 0, utf8_literal("a"), 0})
                .ok() &&
            message_store_upsert(&catchup_store, MessageInput{20, 7, 1, 20, 0, utf8_literal("b"), 0})
                .ok() &&
            ui_variable_list_initialize(&catchup_list, Slice<u32>{catchup_heights, 4},
                                        Slice<u64>{catchup_fenwick, 4}, 2, 100) &&
            message_timeline_initialize(&catchup_timeline, &catchup_store, &catchup_list, 100, 100, true) &&
            message_event_pump_initialize(&pump, &catchup_timeline,
                                          Slice<u8>{payload_scratch, kMessageEventPumpPayloadCapacity},
                                          Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds},
                                          MessageContentAvailability::available) &&
            user_store_initialize(&catchup_users, Slice<UserRecord>{catchup_user_records, 4},
                                          Slice<u32>{catchup_user_order, 4},
                                          Slice<u8>{catchup_user_names, 256}) &&
            message_event_pump_set_user_store(&pump, &catchup_users) &&
            message_rich_store_initialize(&catchup_rich, Slice<MessageRichRecord>{catchup_rich_records, 4},
                                          Slice<u16>{catchup_rich_order, 4},
                                          Slice<u8>{catchup_rich_text, sizeof(catchup_rich_text)}) &&
            message_timeline_bind_rich_store(&catchup_timeline, &catchup_rich),
        "message_history_catchup_fixture");
    pump.catchup_required = true;
    catchup_timeline.newer_refetch_required = true;
    MessageHistoryCatchup catchup{};
    MessageHistoryQuery query{};
    expect(message_history_catchup_begin(&catchup, &pump, 7, 2) && catchup.cursor_after == 20 &&
               message_history_catchup_take_query(&catchup, &query) &&
               query.position == MessageHistoryPosition::after && query.cursor == 20 &&
               query.limit == 2 && catchup.requests == 1,
           "message_history_catchup_query_shape");

    const GatewayMessageEvent catchup_messages[2]{
        {40, 7, 2, 40, 0, utf8_literal("d"), 0, create_fields},
        {30, 7, 2, 30, 0, utf8_literal("c"), 0, create_fields},
    };
    const MessageHistoryPage catchup_page{7, 40, 30, 2, 2};
    const MessageHistoryApplyResult caught_up = message_history_catchup_apply_page(
        &catchup, &pump, catchup_page, ConstSlice<GatewayMessageEvent>{catchup_messages, 2},
        MessageContentAvailability::restricted);
    expect(caught_up.ok() && catchup.state == MessageHistoryCatchupState::request_required &&
               catchup.cursor_after == 40 && catchup.pages == 1 && catchup.received == 2 &&
               catchup.inserted == 2 && pump.catchup_required && catchup_timeline.newer_refetch_required &&
               message_history_catchup_take_query(&catchup, &query) && query.cursor == 40,
           "message_history_catchup_full_page_continues");
    const MessageHistoryApplyResult completed = message_history_catchup_apply_page(
        &catchup, &pump, MessageHistoryPage{}, ConstSlice<GatewayMessageEvent>{nullptr, 0},
        MessageContentAvailability::restricted);
    expect(completed.ok() && catchup.state == MessageHistoryCatchupState::complete && catchup.pages == 2 &&
               !pump.catchup_required && !catchup_timeline.newer_refetch_required &&
               catchup_store.count == 4 && catchup_list.count == 4,
           "message_history_catchup_empty_tail_completes_and_clears");

    pump.catchup_required = true;
    catchup_timeline.newer_refetch_required = true;
    MessageHistoryCatchup failed{};
    expect(message_history_catchup_begin(&failed, &pump, 7, 2) &&
               message_history_catchup_take_query(&failed, &query) &&
               message_history_catchup_fail(&failed, MessageHistoryCatchupFailure::decode) &&
               failed.state == MessageHistoryCatchupState::failed && failed.failures == 1 &&
               pump.catchup_required && catchup_timeline.newer_refetch_required,
           "message_history_catchup_terminal_failure_keeps_sticky_gap");

    pump.catchup_required = true;
    catchup_timeline.newer_refetch_required = true;
    MessageHistoryCatchup rest_catchup{};
    constexpr Utf8View rest_page = utf8_literal(
        R"([{"id":"50","channel_id":"7","author":{"id":"2","username":"RestUser"},"content":"e","timestamp":"2024-08-30T12:00:00Z","reactions":[{"emoji":{"id":"44","name":"party","animated":true},"count":7,"count_details":{"normal":7,"burst":0},"me":true}]}])");
    u32 rest_page_structurals[256]{};
    char rest_content[256]{};
    GatewayMessageEvent rest_messages[2]{};
    GatewayMessageNamedUser rest_named_users[4]{};
    GatewayMessageReaction rest_reactions[2]{};
    GatewayMessageAttachment rest_attachments[2]{};
    GatewayMessageEmbed rest_embeds[2]{};
    GatewayMessageEmbedField rest_embed_fields[2]{};
    GatewayMessageSticker rest_stickers[2]{};
    GatewayMessageComponent rest_components[2]{};
    const MessageHistoryRichScratch rest_rich_scratch{
        Slice<GatewayMessageReaction>{rest_reactions, 2},
        Slice<GatewayMessageAttachment>{rest_attachments, 2},
        Slice<GatewayMessageEmbed>{rest_embeds, 2},
        Slice<GatewayMessageEmbedField>{rest_embed_fields, 2},
        Slice<GatewayMessageSticker>{rest_stickers, 2},
        Slice<GatewayMessageComponent>{rest_components, 2},
    };
    const MessageHistoryDecodeScratch rest_scratch{Slice<u32>{rest_page_structurals, 256},
                                                   Slice<char>{rest_content, 256},
                                                   Slice<GatewayMessageEvent>{rest_messages, 2},
                                                   Slice<GatewayMessageNamedUser>{rest_named_users, 4},
                                                   rest_rich_scratch,
                                                   16};
    RestClientResponse rest_response{};
    rest_response.status_code = 200;
    rest_response.body_bytes = rest_page.bytes;
    const MessageHistoryRestResult rest_applied =
        message_history_catchup_begin(&rest_catchup, &pump, 7, 2) &&
                message_history_catchup_take_query(&rest_catchup, &query)
            ? message_history_catchup_accept_rest(&rest_catchup, &pump, rest_response, rest_page,
                                                  rest_scratch, MessageContentAvailability::available, 0)
            : MessageHistoryRestResult{};
    expect(
        rest_applied.accepted() && rest_applied.disposition == MessageHistoryRestDisposition::applied &&
            rest_applied.decode.page.count == 1 && rest_applied.apply.inserted == 1 &&
            rest_catchup.state == MessageHistoryCatchupState::complete &&
            message_store_find(catchup_store, 50).ok() && !pump.catchup_required &&
            !catchup_timeline.newer_refetch_required && user_store_find(catchup_users, 2) &&
            message_rich_store_find(catchup_rich, 50) &&
            message_rich_store_find(catchup_rich, 50)->reaction_count == 1 &&
            message_rich_store_find(catchup_rich, 50)->reactions[0].count == 7 &&
            utf8_equal(message_rich_store_string(
                           catchup_rich, message_rich_store_find(catchup_rich, 50)->reactions[0].name),
                       utf8_literal("party")) &&
            utf8_equal(
                user_store_name(catchup_users, *user_store_find(catchup_users, 2)),
                utf8_literal("RestUser")),
        "message_history_rest_bridge_decode_apply_complete");

    const MessageStoreResult refresh_found = message_store_find(catchup_store, 50);
    const MessageRecord* refresh_before =
        refresh_found.ok() ? message_store_resolve(catchup_store, refresh_found.handle) : nullptr;
    const u32 refresh_revision = refresh_before ? refresh_before->revision : 0;
    GatewayMessageAttachment refreshed_attachment{};
    refreshed_attachment.id = 8'008;
    refreshed_attachment.size = 321;
    refreshed_attachment.filename = utf8_literal("refresh.jpg");
    refreshed_attachment.content_type = utf8_literal("image/jpeg");
    refreshed_attachment.url =
        utf8_literal("https://cdn.discordapp.com/attachments/7/8008/refresh.jpg"
                     "?ex=1000&is=900&hm="
                     "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    refreshed_attachment.width = 16;
    refreshed_attachment.height = 8;
    refreshed_attachment.has_width = true;
    refreshed_attachment.has_height = true;
    GatewayMessageEvent refreshed_message{
        50, 7, 2, 50, 0, utf8_literal("e"), 0, create_fields | gateway_message_field_attachments};
    refreshed_message.attachments = ConstSlice<GatewayMessageAttachment>{&refreshed_attachment, 1};
    const MessageHistoryApplyResult refreshed =
        message_history_refresh_attachment(&catchup_timeline, refreshed_message, 50, 7, refresh_revision,
                                           8'008, MessageContentAvailability::available);
    const MessageRecord* refresh_after =
        message_store_resolve(catchup_store, message_store_find(catchup_store, 50).handle);
    const MessageRichRecord* refreshed_rich = message_rich_store_find(catchup_rich, 50);
    expect(refreshed.ok() && refreshed.invalidation.count == 1 && !refreshed.invalidation.structural &&
               refresh_after && refresh_after->revision == refresh_revision + 1 && refreshed_rich &&
               refreshed_rich->message_revision == refresh_after->revision &&
               refreshed_rich->reaction_count == 1 && refreshed_rich->attachment_count == 1 &&
               refreshed_rich->attachments[0].id == 8'008 &&
               utf8_equal(message_rich_store_string(catchup_rich, refreshed_rich->attachments[0].url),
                          refreshed_attachment.url),
           "message_history_exact_attachment_refresh_revision_commit");
    expect(message_history_refresh_attachment(&catchup_timeline, refreshed_message, 50, 7, refresh_revision,
                                              8'008, MessageContentAvailability::available)
                   .error == MessageHistoryApplyError::stale_revision,
           "message_history_attachment_refresh_rejects_stale_owner");

    AttachmentRefresh attachment_refresh{};
    const u32 bridge_refresh_revision = refresh_after ? refresh_after->revision : 0;
    expect(
        message_history_attachment_queue(&attachment_refresh, 7, 50, bridge_refresh_revision, 8'008) &&
            message_history_attachment_queue(&attachment_refresh, 7, 50, bridge_refresh_revision, 8'008) &&
            !message_history_attachment_queue(&attachment_refresh, 7, 50, bridge_refresh_revision, 9'009) &&
            attachment_refresh.state == AttachmentRefreshState::request_required,
        "message_history_attachment_queue_deduplicates_owner");
    attachment_refresh.state = AttachmentRefreshState::awaiting_response;
    constexpr Utf8View refreshed_body = utf8_literal(
        R"([{"id":"50","channel_id":"7","author":{"id":"2"},"content":"e","timestamp":"2024-08-30T12:00:00Z","attachments":[{"id":"8008","filename":"refresh.jpg","size":321,"url":"https://cdn.discordapp.com/attachments/7/8008/refresh.jpg?ex=1100&is=1000&hm=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","content_type":"image/jpeg","width":16,"height":8}]}])");
    RestClientResponse refreshed_response{};
    refreshed_response.status_code = 200;
    refreshed_response.body_bytes = refreshed_body.bytes;
    const MessageHistoryRestResult refresh_bridge = message_history_attachment_accept_rest(
        &attachment_refresh, &catchup_timeline, refreshed_response, refreshed_body, rest_scratch,
        MessageContentAvailability::available);
    expect(refresh_bridge.accepted() &&
               refresh_bridge.disposition == MessageHistoryRestDisposition::applied &&
               attachment_refresh.state == AttachmentRefreshState::idle,
           "message_history_attachment_bridge_applies_and_releases_owner");

    expect(message_history_attachment_queue(&attachment_refresh, 7, 50, bridge_refresh_revision + 1, 8'008),
           "message_history_attachment_retry_fixture");
    attachment_refresh.state = AttachmentRefreshState::awaiting_response;
    constexpr Utf8View refresh_limited_body = utf8_literal(R"({"retry_after":1,"global":false})");
    RestClientResponse refresh_limited{};
    refresh_limited.status_code = 429;
    refresh_limited.body_bytes = refresh_limited_body.bytes;
    refresh_limited.rate_limited = true;
    refresh_limited.retry_at_ms = 9'000;
    const MessageHistoryRestResult refresh_retry = message_history_attachment_accept_rest(
        &attachment_refresh, &catchup_timeline, refresh_limited, refresh_limited_body, rest_scratch,
        MessageContentAvailability::available);
    expect(refresh_retry.accepted() &&
               refresh_retry.disposition == MessageHistoryRestDisposition::retry_scheduled &&
               attachment_refresh.state == AttachmentRefreshState::waiting_retry &&
               message_history_attachment_tick(&attachment_refresh, 8'999) &&
               attachment_refresh.state == AttachmentRefreshState::waiting_retry &&
               message_history_attachment_tick(&attachment_refresh, 9'000) &&
               attachment_refresh.state == AttachmentRefreshState::request_required,
           "message_history_attachment_bridge_retries_at_server_deadline");
    message_history_attachment_reset(&attachment_refresh);

    pump.catchup_required = true;
    catchup_timeline.newer_refetch_required = true;
    MessageHistoryCatchup limited_catchup{};
    constexpr Utf8View limited_body = utf8_literal(R"({"retry_after":1,"global":false})");
    RestClientResponse limited_response{};
    limited_response.status_code = 429;
    limited_response.body_bytes = limited_body.bytes;
    limited_response.retry_at_ms = 5'000;
    limited_response.rate_limited = true;
    const MessageHistoryRestResult retry =
        message_history_catchup_begin(&limited_catchup, &pump, 7, 2) &&
                message_history_catchup_take_query(&limited_catchup, &query)
            ? message_history_catchup_accept_rest(&limited_catchup, &pump, limited_response, limited_body,
                                                  rest_scratch, MessageContentAvailability::available, 0)
            : MessageHistoryRestResult{};
    expect(retry.accepted() && retry.disposition == MessageHistoryRestDisposition::retry_scheduled &&
               limited_catchup.state == MessageHistoryCatchupState::waiting_retry &&
               limited_catchup.retry_at_ms == 5'000 &&
               message_history_catchup_tick(&limited_catchup, 5'000) &&
               message_history_catchup_take_query(&limited_catchup, &query),
           "message_history_rest_bridge_server_rate_deadline");
    RestClientResponse forbidden_response{};
    forbidden_response.status_code = 403;
    const MessageHistoryRestResult forbidden =
        message_history_catchup_accept_rest(&limited_catchup, &pump, forbidden_response, Utf8View{},
                                            rest_scratch, MessageContentAvailability::available, 0);
    expect(forbidden.error == MessageHistoryRestError::http_status &&
               forbidden.disposition == MessageHistoryRestDisposition::failed &&
               limited_catchup.state == MessageHistoryCatchupState::failed &&
               limited_catchup.last_failure == MessageHistoryCatchupFailure::http_status &&
               pump.catchup_required && catchup_timeline.newer_refetch_required,
           "message_history_rest_bridge_http_failure_sticky");

    MessageHistoryCatchup transport_catchup{};
    RestClientResponse transport_response{};
    transport_response.error = RestClientError::receive;
    const bool transport_started = message_history_catchup_begin(&transport_catchup, &pump, 7, 2) &&
                                   message_history_catchup_take_query(&transport_catchup, &query);
    const MessageHistoryRestResult transport_failed =
        transport_started
            ? message_history_catchup_accept_rest(&transport_catchup, &pump, transport_response, Utf8View{},
                                                  rest_scratch, MessageContentAvailability::available, 0)
            : MessageHistoryRestResult{};
    expect(transport_started && transport_failed.accepted() &&
               transport_failed.disposition == MessageHistoryRestDisposition::retry_scheduled &&
               transport_catchup.state == MessageHistoryCatchupState::waiting_retry &&
               transport_catchup.retry_at_ms == 500 &&
               transport_catchup.last_failure == MessageHistoryCatchupFailure::transport,
           "message_history_rest_bridge_transport_retry");

    catchup_timeline.newer_refetch_required = false;
    pump.catchup_required = false;
    expect(message_timeline_set_scroll(&catchup_timeline, 0, false) &&
               !message_history_catchup_begin(&transport_catchup, &pump, 7, 2),
           "message_history_older_scroll_fixture");
    MessageHistoryOlder older_pager{};
    expect(
        message_history_older_demand(&older_pager, &catchup_timeline, 7, 2) &&
            older_pager.state == MessageHistoryOlderState::request_required &&
            older_pager.cursor_before == 20 && older_pager.demands == 1 &&
            message_history_older_demand(&older_pager, &catchup_timeline, 7, 2) &&
            older_pager.demands == 1 && message_history_older_take_query(&older_pager, &query) &&
            query.position == MessageHistoryPosition::before && query.cursor == 20 &&
            query.limit == 2 && older_pager.requests == 1,
        "message_history_older_demand_dedup_and_query");
    const GatewayMessageEvent first_older_page[2]{
        {15, 7, 3, 15, 0, utf8_literal("older-b"), 0, create_fields},
        {10, 7, 3, 10, 0, utf8_literal("older-a"), 0, create_fields},
    };
    const MessageHistoryApplyResult first_older = message_history_older_apply_page(
        &older_pager, &catchup_timeline, MessageHistoryPage{7, 15, 10, 2, 14},
        ConstSlice<GatewayMessageEvent>{first_older_page, 2}, MessageContentAvailability::available);
    expect(first_older.ok(), "message_history_older_full_page_applies");
    expect(first_older.inserted == 2 && first_older.evicted == 2, "message_history_older_full_page_counts");
    expect(older_pager.state == MessageHistoryOlderState::idle && older_pager.cursor_before == 10 &&
               older_pager.pages == 1,
           "message_history_older_full_page_stops");
    expect(catchup_timeline.scroll_units == 200 && catchup_timeline.newer_refetch_required &&
               message_store_find(catchup_store, 20).ok() &&
               message_store_find(catchup_store, 50).error == MessageStoreError::not_found,
           "message_history_older_full_page_preserves_anchor_and_marks_newer_gap");

    expect(message_timeline_set_scroll(&catchup_timeline, 0, false) &&
               message_history_older_demand(&older_pager, &catchup_timeline, 7, 2) &&
               older_pager.cursor_before == 10 && older_pager.demands == 2 &&
               message_history_older_take_query(&older_pager, &query),
           "message_history_older_second_user_demand");
    const GatewayMessageEvent final_older_page[1]{
        {5, 7, 4, 5, 0, utf8_literal("oldest"), 0, create_fields},
    };
    const MessageHistoryApplyResult final_older = message_history_older_apply_page(
        &older_pager, &catchup_timeline, MessageHistoryPage{7, 5, 5, 1, 6},
        ConstSlice<GatewayMessageEvent>{final_older_page, 1}, MessageContentAvailability::available);
    expect(final_older.ok() && final_older.inserted == 1 && final_older.evicted == 1 &&
               older_pager.state == MessageHistoryOlderState::exhausted &&
               message_history_older_demand(&older_pager, &catchup_timeline, 7, 2) &&
               older_pager.demands == 2 && older_pager.pages == 2 && older_pager.received == 3 &&
               older_pager.inserted == 3 && older_pager.evicted == 3,
           "message_history_older_short_page_marks_exhausted");

    MessageHistoryOlder older_rest{};
    expect(message_timeline_set_scroll(&catchup_timeline, 0, false) &&
               message_history_older_demand(&older_rest, &catchup_timeline, 7, 2) &&
               message_history_older_take_query(&older_rest, &query),
           "message_history_older_rest_fixture");
    const MessageHistoryRestResult older_retry = message_history_older_accept_rest(
        &older_rest, &catchup_timeline, nullptr, nullptr, nullptr, limited_response, limited_body,
        rest_scratch, MessageContentAvailability::available, 0);
    constexpr Utf8View oldest_body = utf8_literal(
        R"([{"id":"1","channel_id":"7","author":{"id":"4"},"content":"start","timestamp":"2024-08-30T12:00:00Z"}])");
    RestClientResponse oldest_response{};
    oldest_response.status_code = 200;
    oldest_response.body_bytes = oldest_body.bytes;
    const bool older_retried = older_retry.accepted() &&
                               older_retry.disposition == MessageHistoryRestDisposition::retry_scheduled &&
                               older_rest.state == MessageHistoryOlderState::waiting_retry &&
                               message_history_older_tick(&older_rest, 5'000) &&
                               message_history_older_take_query(&older_rest, &query);
    const MessageHistoryRestResult oldest_applied =
        older_retried
            ? message_history_older_accept_rest(&older_rest, &catchup_timeline, nullptr, nullptr, nullptr,
                                                oldest_response, oldest_body, rest_scratch,
                                                MessageContentAvailability::available, 0)
            : MessageHistoryRestResult{};
    expect(older_retried && oldest_applied.accepted() &&
               oldest_applied.disposition == MessageHistoryRestDisposition::applied &&
               oldest_applied.apply.inserted == 1 &&
               older_rest.state == MessageHistoryOlderState::exhausted &&
               message_store_find(catchup_store, 1).ok(),
           "message_history_older_rest_retry_decode_apply");

    MessageHistoryOlder empty_older{};
    const u32 count_before_empty = catchup_store.count;
    expect(message_history_older_demand(&empty_older, &catchup_timeline, 7, 2) &&
               message_history_older_take_query(&empty_older, &query) &&
               message_history_older_apply_page(
                   &empty_older, &catchup_timeline, MessageHistoryPage{},
                   ConstSlice<GatewayMessageEvent>{nullptr, 0}, MessageContentAvailability::available)
                   .ok() &&
               empty_older.state == MessageHistoryOlderState::exhausted && empty_older.pages == 1 &&
               empty_older.received == 0 && catchup_store.count == count_before_empty,
           "message_history_older_empty_page_marks_exhausted");

    MessageHistoryOlder invalid_older{};
    expect(message_timeline_set_scroll(&catchup_timeline, 0, false) &&
               message_history_older_demand(&invalid_older, &catchup_timeline, 7, 2) &&
               message_history_older_take_query(&invalid_older, &query),
           "message_history_older_invalid_fixture");
    const GatewayMessageEvent invalid_cursor_page[1]{
        {invalid_older.cursor_before, 7, 4, 1, 0, utf8_literal("bad"), 0, create_fields},
    };
    expect(message_history_older_apply_page(
               &invalid_older, &catchup_timeline,
               MessageHistoryPage{7, invalid_older.cursor_before, invalid_older.cursor_before, 1, 3},
               ConstSlice<GatewayMessageEvent>{invalid_cursor_page, 1},
               MessageContentAvailability::available)
                       .error == MessageHistoryApplyError::invalid_page &&
               invalid_older.state == MessageHistoryOlderState::failed &&
               invalid_older.last_failure == MessageHistoryCatchupFailure::decode,
           "message_history_older_rejects_nonadvancing_cursor");

    GuildMemberRecord jump_member_records[2]{};
    u32 jump_member_order[2]{};
    GuildMemberStore jump_members{};
    constexpr Utf8View jump_body = utf8_literal(
        R"([{"id":"777","channel_id":"7","author":{"id":"2","username":"Jump Author"},"content":"target","timestamp":"2024-08-30T12:00:00Z"}])");
    RestClientResponse jump_response{};
    jump_response.status_code = 200;
    jump_response.body_bytes = jump_body.bytes;
    const bool jump_fixture = guild_member_store_initialize(
        &jump_members, Slice<GuildMemberRecord>{jump_member_records, 2},
        Slice<u32>{jump_member_order, 2});
    const u32 before_rejected_jump = catchup_store.count;
    const MessageHistoryRestResult rejected_jump =
        jump_fixture ? message_history_jump_accept_rest(&catchup_timeline, &catchup_users, &jump_members,
                                                        &mention_state, jump_response, jump_body, 7, 778,
                                                        rest_scratch, MessageContentAvailability::available)
                     : MessageHistoryRestResult{};
    expect(jump_fixture && rejected_jump.error == MessageHistoryRestError::decode &&
               catchup_store.count == before_rejected_jump,
           "message_history_jump_rejects_page_without_target_before_reset");
    const MessageHistoryRestResult accepted_jump = message_history_jump_accept_rest(
        &catchup_timeline, &catchup_users, &jump_members, &mention_state, jump_response, jump_body, 7, 777,
        rest_scratch, MessageContentAvailability::available);
    expect(accepted_jump.accepted() &&
               accepted_jump.disposition == MessageHistoryRestDisposition::applied &&
               catchup_store.count == 1 && message_store_find(catchup_store, 777).ok(),
           "message_history_jump_bridge_atomically_replaces_resident_window");
}

} // namespace maicord
