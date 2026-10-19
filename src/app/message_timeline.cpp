#include "maicord/app/message_timeline.hpp"
#include "maicord/app/gateway_client.hpp"

namespace maicord {
namespace {

MessageTimelineApplyResult timeline_error(MessageTimelineError error) noexcept {
    MessageTimelineApplyResult result{};
    result.error = error;
    return result;
}

void clamp_or_follow(MessageTimeline* timeline, UiScrollAnchor* anchor, u64 previous_scroll) noexcept {
    if (timeline->follow_bottom) {
        message_timeline_restore_scroll(timeline, message_timeline_maximum_scroll(*timeline), true);
        return;
    }
    u64 restored = previous_scroll;
    if (anchor && anchor->valid && ui_variable_list_restore_anchor(*timeline->list, *anchor, &restored)) {
        message_timeline_restore_scroll(timeline, restored, false);
        return;
    }
    message_timeline_restore_scroll(timeline, previous_scroll, false);
}

UiScrollAnchor capture_anchor(const MessageTimeline& timeline) noexcept {
    UiScrollAnchor anchor{};
    if (!timeline.follow_bottom && timeline.list && timeline.list->count != 0) {
        ui_variable_list_capture_anchor(*timeline.list, timeline.scroll_units, &anchor);
    }
    return anchor;
}

void install_removed_anchor_fallback(UiVariableList* list, UiScrollAnchor* anchor, bool removed_anchor,
                                     u32 removal_index) noexcept {
    if (!removed_anchor || list->count == 0) {
        return;
    }
    anchor->index = minimum(removal_index, list->count - 1);
    anchor->within_item_units = 0;
    anchor->generation = list->generation;
    anchor->valid = true;
}

bool anchor_in_range(const UiScrollAnchor& anchor, u32 index, u32 count) noexcept {
    return anchor.valid && anchor.index >= index && anchor.index - index < count;
}

MessageTimelineInvalidation structural_invalidation(u32 first, u32 old_count, u32 new_count,
                                                    bool scroll_changed) noexcept {
    const u32 end = maximum(old_count, new_count);
    return MessageTimelineInvalidation{
        first,
        first < end ? end - first : 1,
        true,
        scroll_changed,
    };
}

MessageTimelineError validate_timeline(MessageTimeline* timeline) noexcept {
    if (!timeline || !timeline->store || !timeline->list || !timeline->store->records ||
        !timeline->list->item_units || timeline->viewport_units == 0) {
        return MessageTimelineError::invalid_input;
    }
    return timeline->store->count == timeline->list->count ? MessageTimelineError::none
                                                           : MessageTimelineError::count_mismatch;
}

bool insertion_requires_eviction(const MessageTimeline& timeline, u32 content_bytes) noexcept {
    const MessageStore& store = *timeline.store;
    const u32 live_bytes = store.text_used - store.text_wasted;
    return timeline.list->count >= timeline.list->capacity || store.count >= store.capacity ||
           content_bytes > store.text_capacity - live_bytes;
}

bool update_requires_eviction(const MessageTimeline& timeline, const GatewayMessageEvent& message,
                              MessageHandle handle) noexcept {
    if ((message.fields & gateway_message_field_content) == 0) {
        return false;
    }
    const MessageStore& store = *timeline.store;
    const MessageRecord* record = message_store_resolve(store, handle);
    if (!record) {
        return true;
    }
    const u32 live_bytes = store.text_used - store.text_wasted;
    const u32 live_without_target = live_bytes - record->content_bytes;
    return message.content.bytes > store.text_capacity - live_without_target;
}

bool newer_than_resident(const MessageStore& store, u64 id) noexcept {
    if (store.count == 0) {
        return false;
    }
    const MessageHandle handle = message_store_ordered_handle(store, store.count - 1);
    const MessageRecord* record = message_store_resolve(store, handle);
    return record && id > record->id;
}

bool evict_resident_edge(MessageTimeline* timeline, UiScrollAnchor* anchor, u64 protected_id,
                         u32* first_changed) noexcept {
    if (!timeline || !timeline->store || !timeline->list || timeline->list->count == 0) {
        return false;
    }
    const u32 last = timeline->list->count - 1;
    u32 removal_index = 0;
    if (!timeline->follow_bottom && anchor && anchor->valid) {
        const u32 distance_to_first = anchor->index;
        const u32 distance_to_last = last - minimum(anchor->index, last);
        removal_index = distance_to_last > distance_to_first ? last : 0;
    }
    MessageHandle handle = message_store_ordered_handle(*timeline->store, removal_index);
    const MessageRecord* record = message_store_resolve(*timeline->store, handle);
    if (!record) {
        return false;
    }
    if (record->id == protected_id) {
        if (last == 0) {
            return false;
        }
        removal_index = removal_index == 0 ? last : 0;
        handle = message_store_ordered_handle(*timeline->store, removal_index);
        record = message_store_resolve(*timeline->store, handle);
        if (!record || record->id == protected_id) {
            return false;
        }
    }
    const u64 id = record->id;
    const bool removed_anchor = anchor && anchor_in_range(*anchor, removal_index, 1);
    if (!message_store_delete(timeline->store, id) ||
        (timeline->rich_store && !message_rich_store_delete(timeline->rich_store, id)) ||
        !ui_variable_list_remove(timeline->list, removal_index, 1, anchor)) {
        return false;
    }
    if (anchor) {
        install_removed_anchor_fallback(timeline->list, anchor, removed_anchor, removal_index);
    }
    *first_changed = minimum(*first_changed, removal_index);
    ++timeline->evicted;
    return true;
}

// Keep the rich event's cold temporary storage out of text-only insertion.
__declspec(noinline) bool insert_local_attachments(
    MessageTimeline* timeline, const MessageInput& input, u32 revision,
    ConstSlice<GatewayMessageAttachment> attachments) noexcept {
    GatewayMessageEvent rich{};
    rich.id = input.id;
    rich.fields = gateway_message_field_attachments;
    rich.attachments = attachments;
    // A pending voice message keeps the voice presentation until it is confirmed.
    if (attachments.count == 1 && attachments[0].waveform.bytes && attachments[0].has_duration) {
        rich.fields |= gateway_message_field_flags;
        rich.flags = kGatewayMessageVoiceFlag;
    }
    return message_rich_store_apply(timeline->rich_store, input.id, revision, rich).ok();
}

// Fills an unknown same-channel reply quote from its resident target.
__declspec(noinline) bool resolve_resident_reference(const MessageTimeline& timeline, u64 channel_id,
                                                     GatewayMessageReference* reference) noexcept {
    const auto target = message_store_find(*timeline.store, reference->message_id);
    const auto* referenced = target.ok() ? message_store_resolve(*timeline.store, target.handle) : nullptr;
    if (!referenced || referenced->channel_id != channel_id ||
        referenced->content_availability != MessageContentAvailability::available)
        return false;
    reference->author_id = referenced->author_id;
    reference->content = message_store_content(*timeline.store, target.handle);
    reference->state = GatewayMessageReferenceState::available;
    reference->message_type = static_cast<u8>(referenced->type < 256 ? referenced->type : 0);
    u8 content_flags = 0;
    if (referenced->flags & (1u << 13))
        content_flags |= gateway_reference_voice_message;
    if (referenced->flags & (1u << 15))
        content_flags |= gateway_reference_components_v2;
    if (const auto* rich = message_rich_store_find(*timeline.rich_store, referenced->id)) {
        if (rich->attachment_count)
            content_flags |= gateway_reference_attachments;
        if (rich->embed_count)
            content_flags |= gateway_reference_embeds;
        if (rich->sticker_count)
            content_flags |= gateway_reference_stickers;
        if (rich->interaction.id)
            content_flags |= gateway_reference_interaction;
        if (rich->voice_message)
            content_flags |= gateway_reference_voice_message;
        // A lone image/gifv link shows no text (its simple embed replaces it).
        const auto& embed = rich->embeds[0];
        const Utf8View url = message_rich_store_string(*timeline.rich_store, embed.url);
        if (rich->embed_count == 1 && !embed.title.bytes && !embed.description.bytes &&
            (embed.type == GatewayMessageEmbedType::image || embed.type == GatewayMessageEmbedType::gifv) &&
            utf8_equal(reference->content, url))
            reference->content = {};
    }
    reference->content_flags = content_flags;
    return true;
}
} // namespace

u64 message_timeline_maximum_scroll(const MessageTimeline& timeline) noexcept {
    if (!timeline.list || timeline.list->total_units <= timeline.viewport_units) {
        return 0;
    }
    return timeline.list->total_units - timeline.viewport_units;
}

bool message_timeline_initialize(MessageTimeline* timeline, MessageStore* store, UiVariableList* list,
                                 u32 default_item_units, u64 viewport_units, bool follow_bottom) noexcept {
    if (!timeline || !store || !list || !store->records || !list->item_units || !list->fenwick ||
        store->count != list->count || default_item_units == 0 ||
        default_item_units > kMaximumUiItemExtentUnits || viewport_units == 0) {
        return false;
    }
    *timeline = MessageTimeline{};
    timeline->store = store;
    timeline->list = list;
    timeline->default_item_units = default_item_units;
    timeline->viewport_units = viewport_units;
    timeline->follow_bottom = follow_bottom;
    if (follow_bottom) {
        timeline->scroll_units = message_timeline_maximum_scroll(*timeline);
    }
    return true;
}

bool message_timeline_bind_rich_store(MessageTimeline* timeline, MessageRichStore* rich_store) noexcept {
    if (!timeline || !timeline->store || !timeline->list || !rich_store || !rich_store->records ||
        !rich_store->order || !rich_store->text || rich_store->capacity < timeline->store->capacity ||
        rich_store->text_capacity < 2 || (rich_store->text_capacity & 1u) != 0) {
        return false;
    }
    timeline->rich_store = rich_store;
    return true;
}

bool message_timeline_reset(MessageTimeline* timeline) noexcept {
    if (!timeline || !timeline->store || !timeline->list || !timeline->store->records ||
        !timeline->store->order || !timeline->store->text || !timeline->list->item_units ||
        !timeline->list->fenwick || timeline->default_item_units == 0 || timeline->viewport_units == 0) {
        return false;
    }
    MessageStore* const store = timeline->store;
    MessageRichStore* const rich_store = timeline->rich_store;
    UiVariableList* const list = timeline->list;
    const u32 record_capacity = store->capacity;
    const u32 text_capacity = store->text_capacity;
    const u32 list_capacity = list->capacity;
    const u32 default_item_units = timeline->default_item_units;
    const u64 viewport_units = timeline->viewport_units;
    const bool follow_bottom = timeline->follow_bottom;
    const u32 reset_generation = timeline->reset_generation + 1u;
    if (!message_store_initialize(store, Slice<MessageRecord>{store->records, record_capacity},
                                  Slice<u32>{store->order, record_capacity},
                                  Slice<u8>{store->text, text_capacity}) ||
        (rich_store && !message_rich_store_reset(rich_store)) ||
        !ui_variable_list_initialize(list, Slice<u32>{list->item_units, list_capacity},
                                     Slice<u64>{list->fenwick, list_capacity}, 0, default_item_units)) {
        return false;
    }
    if (!message_timeline_initialize(timeline, store, list, default_item_units, viewport_units,
                                     follow_bottom)) {
        return false;
    }
    timeline->reset_generation = reset_generation;
    return !rich_store || message_timeline_bind_rich_store(timeline, rich_store);
}

bool message_timeline_set_viewport(MessageTimeline* timeline, u64 viewport_units) noexcept {
    if (!timeline || !timeline->list || viewport_units == 0) {
        return false;
    }
    timeline->viewport_units = viewport_units;
    return message_timeline_restore_scroll(timeline, timeline->scroll_units, timeline->follow_bottom);
}

bool message_timeline_set_scroll(MessageTimeline* timeline, u64 scroll_units, bool follow_bottom) noexcept {
    if (!timeline || !timeline->list || timeline->viewport_units == 0) {
        return false;
    }
    timeline->follow_bottom = follow_bottom;
    const u64 bottom = message_timeline_maximum_scroll(*timeline);
    timeline->scroll_units = follow_bottom ? bottom : minimum(scroll_units, bottom);
    return true;
}

bool message_timeline_restore_scroll(MessageTimeline* timeline, u64 scroll_units,
                                     bool follow_bottom) noexcept {
    if (!timeline)
        return false;
    const double previous = static_cast<double>(timeline->scroll_units);
    if (!message_timeline_set_scroll(timeline, scroll_units, follow_bottom))
        return false;
    timeline->scroll_origin_units += static_cast<double>(timeline->scroll_units) - previous;
    return true;
}

bool message_timeline_scroll_by(MessageTimeline* timeline, i64 delta_units, u64 bottom_tolerance_units,
                                bool* changed) noexcept {
    if (changed) {
        *changed = false;
    }
    if (!timeline || !timeline->list || timeline->viewport_units == 0 || !changed) {
        return false;
    }
    const u64 previous = timeline->scroll_units;
    if (delta_units == 0)
        return true;
    u64 target = previous;
    if (delta_units < 0) {
        const u64 magnitude = static_cast<u64>(-(delta_units + 1)) + 1;
        target = magnitude > target ? 0 : target - magnitude;
    } else {
        const u64 magnitude = static_cast<u64>(delta_units);
        saturating_accumulate(target, magnitude);
    }
    if (!message_timeline_set_scroll(timeline, target, false)) {
        return false;
    }
    if (delta_units > 0 && message_timeline_is_at_bottom(*timeline, bottom_tolerance_units)) {
        timeline->follow_bottom = true;
        timeline->scroll_units = message_timeline_maximum_scroll(*timeline);
    }
    *changed = timeline->scroll_units != previous;
    return true;
}

bool message_timeline_scroll_to_start(MessageTimeline* timeline, bool* changed) noexcept {
    if (changed) {
        *changed = false;
    }
    if (!timeline || !changed) {
        return false;
    }
    const u64 previous = timeline->scroll_units;
    if (!message_timeline_set_scroll(timeline, 0, false)) {
        return false;
    }
    *changed = timeline->scroll_units != previous;
    return true;
}

bool message_timeline_scroll_to_bottom(MessageTimeline* timeline, bool* changed) noexcept {
    if (changed) {
        *changed = false;
    }
    if (!timeline || !changed) {
        return false;
    }
    const u64 previous = timeline->scroll_units;
    if (!message_timeline_set_scroll(timeline, 0, true)) {
        return false;
    }
    *changed = timeline->scroll_units != previous;
    return true;
}

bool message_timeline_is_at_bottom(const MessageTimeline& timeline, u64 tolerance_units) noexcept {
    if (!timeline.list || timeline.viewport_units == 0) {
        return false;
    }
    const u64 bottom = message_timeline_maximum_scroll(timeline);
    return timeline.scroll_units >= bottom || bottom - timeline.scroll_units <= tolerance_units;
}

bool message_timeline_set_item_height(MessageTimeline* timeline, u32 index, u32 item_units,
                                      MessageTimelineInvalidation* invalidation) noexcept {
    if (invalidation) {
        *invalidation = MessageTimelineInvalidation{};
    }
    if (validate_timeline(timeline) != MessageTimelineError::none || !invalidation ||
        index >= timeline->list->count) {
        if (timeline) {
            ++timeline->rejected;
        }
        return false;
    }
    const u64 previous_scroll = timeline->scroll_units;
    UiScrollAnchor anchor = capture_anchor(*timeline);
    if (!ui_variable_list_set_height(timeline->list, index, item_units)) {
        ++timeline->rejected;
        return false;
    }
    clamp_or_follow(timeline, &anchor, previous_scroll);
    *invalidation = MessageTimelineInvalidation{
        index,
        timeline->list->count - index,
        false,
        timeline->scroll_units != previous_scroll,
    };
    return true;
}

MessageTimelineApplyResult message_timeline_insert_local(
    MessageTimeline* timeline, const MessageInput& input,
    ConstSlice<GatewayMessageAttachment> attachments) noexcept {
    const MessageTimelineError validation = validate_timeline(timeline);
    if (validation != MessageTimelineError::none || input.delivery == MessageDeliveryState::authoritative ||
        (attachments.count &&
         (!timeline->rich_store || !message_rich_store_validate_attachments(attachments)))) {
        if (timeline) {
            ++timeline->rejected;
        }
        return timeline_error(
            validation != MessageTimelineError::none ? validation : MessageTimelineError::invalid_input);
    }
    const MessageStoreError input_error = message_store_validate_input(timeline->store, input);
    const MessageStoreResult existing = message_store_find(*timeline->store, input.id);
    if (input_error != MessageStoreError::none || existing.error != MessageStoreError::not_found ||
        input.content.bytes > timeline->store->text_capacity) {
        MessageTimelineApplyResult result = timeline_error(MessageTimelineError::reconcile);
        result.mutation.store_error = input_error != MessageStoreError::none ? input_error
                                      : existing.ok() ? MessageStoreError::invalid_input
                                                      : existing.error;
        result.mutation.error = MessageReconcileError::store_failure;
        ++timeline->rejected;
        return result;
    }

    MessageTimelineApplyResult result{};
    const u32 old_count = timeline->list->count;
    const u64 previous_scroll = timeline->scroll_units;
    UiScrollAnchor anchor = capture_anchor(*timeline);
    u32 first_changed = old_count;
    const u32 previous_compactions = timeline->store->compactions;
    while (insertion_requires_eviction(*timeline, input.content.bytes)) {
        if (!evict_resident_edge(timeline, &anchor, 0, &first_changed)) {
            ++timeline->rejected;
            result.error = timeline->list->count >= timeline->list->capacity
                               ? MessageTimelineError::ui_capacity
                               : MessageTimelineError::reconcile;
            return result;
        }
        ++result.evicted;
    }

    const MessageStoreResult inserted = message_store_upsert(timeline->store, input);
    if (!inserted.ok() || !inserted.inserted) {
        ++timeline->rejected;
        result.mutation.store_error = inserted.error;
        result.mutation.error = MessageReconcileError::store_failure;
        result.error = MessageTimelineError::reconcile;
        return result;
    }
    result.mutation = MessageReconcileResult{inserted.handle, inserted.order_index, MessageStoreError::none,
                                             MessageReconcileError::none, true};
    if (attachments.count &&
        !insert_local_attachments(timeline, input,
                                  message_store_resolve(*timeline->store, inserted.handle)->revision,
                                  attachments)) {
        message_rich_store_delete(timeline->rich_store, input.id);
        message_store_delete(timeline->store, input.id);
        ++timeline->rejected;
        result.error = MessageTimelineError::rich_store;
        return result;
    }
    if (!ui_variable_list_insert(timeline->list, inserted.order_index, 1, timeline->default_item_units,
                                 &anchor)) {
        if (timeline->rich_store)
            message_rich_store_delete(timeline->rich_store, input.id);
        message_store_delete(timeline->store, input.id);
        ++timeline->rejected;
        result.error = MessageTimelineError::ui_mutation;
        return result;
    }
    first_changed = minimum(first_changed, inserted.order_index);
    clamp_or_follow(timeline, &anchor, previous_scroll);
    result.kind = GatewayMessageApplyKind::created;
    result.invalidation = structural_invalidation(first_changed, old_count, timeline->list->count,
                                                  timeline->scroll_units != previous_scroll);
    const u32 compactions = timeline->store->compactions - previous_compactions;
    result.compacted = compactions != 0;
    timeline->compactions += compactions;
    ++timeline->applied;
    return result;
}

MessageTimelineApplyResult message_timeline_remove_local(MessageTimeline* timeline,
                                                         u64 message_id) noexcept {
    const MessageTimelineError validation = validate_timeline(timeline);
    if (validation != MessageTimelineError::none || message_id == 0) {
        if (timeline) {
            ++timeline->rejected;
        }
        return timeline_error(
            validation != MessageTimelineError::none ? validation : MessageTimelineError::invalid_input);
    }
    const MessageStoreResult found = message_store_find(*timeline->store, message_id);
    if (found.error == MessageStoreError::not_found) {
        return MessageTimelineApplyResult{};
    }
    if (!found.ok()) {
        ++timeline->rejected;
        return timeline_error(MessageTimelineError::reconcile);
    }
    const u32 old_count = timeline->list->count;
    const u64 previous_scroll = timeline->scroll_units;
    UiScrollAnchor anchor = capture_anchor(*timeline);
    const bool removed_anchor = anchor_in_range(anchor, found.order_index, 1);
    if (!message_store_delete(timeline->store, message_id) ||
        (timeline->rich_store && !message_rich_store_delete(timeline->rich_store, message_id)) ||
        !ui_variable_list_remove(timeline->list, found.order_index, 1, &anchor)) {
        ++timeline->rejected;
        return timeline_error(MessageTimelineError::ui_mutation);
    }
    install_removed_anchor_fallback(timeline->list, &anchor, removed_anchor, found.order_index);
    clamp_or_follow(timeline, &anchor, previous_scroll);
    MessageTimelineApplyResult result{};
    result.kind = GatewayMessageApplyKind::deleted;
    result.invalidation = structural_invalidation(found.order_index, old_count, timeline->list->count,
                                                  timeline->scroll_units != previous_scroll);
    ++timeline->applied;
    return result;
}

bool message_timeline_set_delivery(MessageTimeline* timeline, u64 message_id, MessageDeliveryState delivery,
                                   MessageTimelineInvalidation* invalidation) noexcept {
    if (invalidation) {
        *invalidation = MessageTimelineInvalidation{};
    }
    if (validate_timeline(timeline) != MessageTimelineError::none || !invalidation || message_id == 0) {
        if (timeline) {
            ++timeline->rejected;
        }
        return false;
    }
    const MessageStoreResult found = message_store_find(*timeline->store, message_id);
    if (!found.ok()) {
        return false;
    }
    const MessageRecord* record = message_store_resolve(*timeline->store, found.handle);
    if (!record) {
        return false;
    }
    const bool changed = record->delivery != delivery;
    const u32 previous_revision = record->revision;
    if (!message_store_set_delivery(timeline->store, message_id, delivery)) {
        return false;
    }
    if (changed) {
        if (timeline->rich_store)
            message_rich_store_advance_revision(timeline->rich_store, message_id, previous_revision,
                                                record->revision);
        *invalidation = MessageTimelineInvalidation{found.order_index, 1, false, false};
    }
    return true;
}

bool message_timeline_set_local_reply(MessageTimeline* timeline, u64 message_id, u64 reply_message_id) noexcept {
    if (validate_timeline(timeline) != MessageTimelineError::none || !message_id || !reply_message_id ||
        message_id == reply_message_id)
        return false;
    const MessageStoreResult found = message_store_find(*timeline->store, message_id);
    const MessageRecord* record = found.ok() ? message_store_resolve(*timeline->store, found.handle) : nullptr;
    if (!record || record->delivery == MessageDeliveryState::authoritative)
        return false;
    if (!timeline->rich_store)
        return true;
    GatewayMessageEvent rich{};
    rich.id = message_id;
    rich.reference.message_id = reply_message_id;
    rich.reference.channel_id = record->channel_id;
    rich.reference.state = GatewayMessageReferenceState::unknown;
    // An unresolved quote would draw "could not be loaded" until the echo.
    if (!resolve_resident_reference(*timeline, record->channel_id, &rich.reference))
        return true;
    rich.fields = gateway_message_field_message_reference | gateway_message_field_referenced_message;
    return message_rich_store_apply(timeline->rich_store, message_id, record->revision, rich).ok();
}

static MessageTimelineApplyResult apply_decoded_message(
    MessageTimeline* timeline, const GatewayMessageEvent& message, bool create,
    MessageContentAvailability content_availability) noexcept {
    const auto validation = validate_timeline(timeline);
    if (validation != MessageTimelineError::none) {
        if (timeline)
            ++timeline->rejected;
        return timeline_error(validation);
    }
    MessageTimelineApplyResult result{};
    const u32 old_count = timeline->list->count;
    const u64 previous_scroll = timeline->scroll_units;
    const MessageStoreResult existing = message_store_find(*timeline->store, message.id);
    const bool inserting = create && existing.error == MessageStoreError::not_found;
    // A restored/clamped viewport can reach the live edge without another
    // positive wheel sample. Decide before insertion moves the bottom.
    if (inserting && old_count && message_timeline_maximum_scroll(*timeline) &&
        !timeline->newer_refetch_required && newer_than_resident(*timeline->store, message.id) &&
        message_timeline_is_at_bottom(*timeline, ui_extent_units(1.0f)))
        timeline->follow_bottom = true;
    const bool create_pressure = inserting && insertion_requires_eviction(*timeline, message.content.bytes);
    const bool update_pressure =
        !create && existing.ok() && update_requires_eviction(*timeline, message, existing.handle);
    if (create_pressure || (inserting && timeline->newer_refetch_required)) {
        result.mutation = message_reconcile_validate_create(timeline->store, message, content_availability);
        if (!result.mutation.ok() ||
            (inserting && message.content.bytes > timeline->store->text_capacity)) {
            if (result.mutation.ok()) {
                result.mutation.store_error = MessageStoreError::text_capacity;
                result.mutation.error = MessageReconcileError::store_failure;
            }
            ++timeline->rejected;
            result.error = MessageTimelineError::reconcile;
            return result;
        }
    } else if (update_pressure) {
        result.mutation = message_reconcile_validate_update(timeline->store, message, content_availability);
        if (!result.mutation.ok() || (((message.fields & gateway_message_field_content) != 0) &&
                                      message.content.bytes > timeline->store->text_capacity)) {
            if (result.mutation.ok()) {
                result.mutation.store_error = MessageStoreError::text_capacity;
                result.mutation.error = MessageReconcileError::store_failure;
            }
            ++timeline->rejected;
            result.error = MessageTimelineError::reconcile;
            return result;
        }
    }
    if (inserting && (timeline->newer_refetch_required || (create_pressure && !timeline->follow_bottom)) &&
        newer_than_resident(*timeline->store, message.id)) {
        result.kind = GatewayMessageApplyKind::deferred;
        ++timeline->deferred;
        timeline->newer_refetch_required = true;
        return result;
    }

    UiScrollAnchor anchor = inserting || update_pressure ? capture_anchor(*timeline) : UiScrollAnchor{};
    u32 first_changed = old_count;
    const u32 previous_compactions = timeline->store->compactions;
    while (inserting && insertion_requires_eviction(*timeline, message.content.bytes)) {
        if (!evict_resident_edge(timeline, &anchor, 0, &first_changed)) {
            ++timeline->rejected;
            result.error = timeline->list->count >= timeline->list->capacity
                               ? MessageTimelineError::ui_capacity
                               : MessageTimelineError::reconcile;
            return result;
        }
        ++result.evicted;
    }
    while (!create && update_pressure && update_requires_eviction(*timeline, message, existing.handle)) {
        if (!evict_resident_edge(timeline, &anchor, message.id, &first_changed)) {
            ++timeline->rejected;
            result.error = MessageTimelineError::reconcile;
            return result;
        }
        ++result.evicted;
    }

    result.kind = create ? GatewayMessageApplyKind::created : GatewayMessageApplyKind::updated;
    result.mutation = create ? message_reconcile_create(timeline->store, message, content_availability)
                             : message_reconcile_update(timeline->store, message, content_availability);
    if (!result.mutation.ok()) {
        ++timeline->rejected;
        result.error = MessageTimelineError::reconcile;
        return result;
    }
    if (result.mutation.inserted) {
        if (!ui_variable_list_insert(timeline->list, result.mutation.order_index, 1,
                                     timeline->default_item_units, &anchor)) {
            ++timeline->rejected;
            result.error = MessageTimelineError::ui_mutation;
            return result;
        }
    }
    first_changed = minimum(first_changed, result.mutation.order_index);
    if (timeline->rich_store &&
        ((message.fields &
          (gateway_message_field_message_reference | gateway_message_field_referenced_message |
           gateway_message_field_reactions | gateway_message_field_attachments |
           gateway_message_field_embeds | gateway_message_field_sticker_items |
           gateway_message_field_components | gateway_message_field_interaction |
           gateway_message_field_message_snapshots | gateway_message_field_flags)) != 0 ||
         message.rich_truncations != 0)) {
        const MessageRecord* resident = message_store_resolve(*timeline->store, result.mutation.handle);
        GatewayMessageEvent enriched = message;
        const auto* cached = message_rich_store_find(*timeline->rich_store, message.id);
        const bool known_quote = cached && cached->reference.message_id == enriched.reference.message_id &&
                                 cached->reference.channel_id == enriched.reference.channel_id &&
                                 (cached->reference.state == GatewayMessageReferenceState::available ||
                                  cached->reference.state == GatewayMessageReferenceState::deleted);
        if (enriched.reference.state == GatewayMessageReferenceState::unknown && !known_quote &&
            enriched.reference.message_id != message.id &&
            enriched.reference.channel_id == message.channel_id &&
            resolve_resident_reference(*timeline, message.channel_id, &enriched.reference))
            enriched.fields |= gateway_message_field_referenced_message;
        if (!resident ||
            !message_rich_store_apply(timeline->rich_store, message.id, resident->revision, enriched)
                 .ok()) {
            ++timeline->rejected;
            result.error = MessageTimelineError::rich_store;
            return result;
        }
    } else if (timeline->rich_store && !result.mutation.inserted) {
        const auto* resident = message_store_resolve(*timeline->store, result.mutation.handle);
        const auto* rich = message_rich_store_find(*timeline->rich_store, message.id);
        if (resident && rich)
            message_rich_store_advance_revision(timeline->rich_store, message.id, rich->message_revision,
                                                resident->revision);
    }
    if (result.evicted != 0 || result.mutation.inserted) {
        clamp_or_follow(timeline, &anchor, previous_scroll);
        result.invalidation = structural_invalidation(first_changed, old_count, timeline->list->count,
                                                      timeline->scroll_units != previous_scroll);
    } else {
        result.invalidation = MessageTimelineInvalidation{result.mutation.order_index, 1, false, false};
    }
    const u32 compactions = timeline->store->compactions - previous_compactions;
    result.compacted = compactions != 0;
    timeline->compactions += compactions;
    ++timeline->applied;
    return result;
}

MessageTimelineApplyResult message_timeline_apply_created(
    MessageTimeline* timeline, const GatewayMessageEvent& message,
    MessageContentAvailability content_availability) noexcept {
    return apply_decoded_message(timeline, message, true, content_availability);
}
MessageTimelineApplyResult message_timeline_apply_updated(
    MessageTimeline* timeline, const GatewayMessageEvent& message,
    MessageContentAvailability content_availability) noexcept {
    return apply_decoded_message(timeline, message, false, content_availability);
}

MessageTimelineApplyResult message_timeline_apply(
    MessageTimeline* timeline, const GatewayClientEvent& event, ConstSlice<u8> payload, Slice<u64> bulk_ids,
    MessageContentAvailability content_availability) noexcept {
    const MessageTimelineError validation = validate_timeline(timeline);
    if (validation != MessageTimelineError::none || (!payload.data && payload.count != 0) ||
        (!bulk_ids.data && bulk_ids.count != 0)) {
        if (timeline) {
            ++timeline->rejected;
        }
        return timeline_error(
            validation != MessageTimelineError::none ? validation : MessageTimelineError::invalid_input);
    }

    MessageTimelineApplyResult result{};
    const u32 old_count = timeline->list->count;
    const u64 previous_scroll = timeline->scroll_units;

    if (event.dispatch_kind == GatewayClientDispatchKind::message_create ||
        event.dispatch_kind == GatewayClientDispatchKind::message_update) {
        GatewayMessageEvent message{};
        GatewayMessageReaction reactions[kGatewayMessageReactionMaximum];
        GatewayMessageAttachment attachments[kGatewayMessageAttachmentMaximum];
        GatewayMessageEmbed embeds[kGatewayMessageEmbedMaximum];
        GatewayMessageEmbedField embed_fields[kGatewayMessageEmbedFieldMaximum];
        GatewayMessageSticker stickers[kGatewayMessageStickerMaximum];
        GatewayMessageComponent components[kGatewayMessageComponentMaximum];
        GatewayMessageAttachment snapshot_attachments[kGatewayMessageAttachmentMaximum];
        GatewayMessageEmbed snapshot_embeds[kGatewayMessageEmbedMaximum];
        GatewayMessageEmbedField snapshot_embed_fields[kGatewayMessageEmbedFieldMaximum];
        GatewayMessageSticker snapshot_stickers[kGatewayMessageStickerMaximum];
        GatewayMessageComponent snapshot_components[kGatewayMessageComponentMaximum];
        GatewayMessageNamedUser snapshot_users[kGatewayMessageMentionUserMaximum];
        const GatewayMessageRichScratch rich_scratch{
            Slice<GatewayMessageReaction>{reactions, kGatewayMessageReactionMaximum},
            Slice<GatewayMessageAttachment>{attachments, kGatewayMessageAttachmentMaximum},
            Slice<GatewayMessageEmbed>{embeds, kGatewayMessageEmbedMaximum},
            Slice<GatewayMessageEmbedField>{embed_fields, kGatewayMessageEmbedFieldMaximum},
            Slice<GatewayMessageSticker>{stickers, kGatewayMessageStickerMaximum},
            Slice<GatewayMessageComponent>{components, kGatewayMessageComponentMaximum},
            {{snapshot_attachments, kGatewayMessageAttachmentMaximum},
             {snapshot_embeds, kGatewayMessageEmbedMaximum},
             {snapshot_embed_fields, kGatewayMessageEmbedFieldMaximum},
             {snapshot_stickers, kGatewayMessageStickerMaximum},
             {snapshot_components, kGatewayMessageComponentMaximum},
             {snapshot_users, kGatewayMessageMentionUserMaximum}}};
        GatewayMessageNamedUser system_target{};
        const bool decoded = (timeline->rich_store
                                  ? gateway_client_event_as_message(event, payload, rich_scratch, &message)
                                  : gateway_client_event_as_message(event, payload, &message)) &&
                             gateway_client_event_message_system_target(event, payload, &message, &system_target);
        if (!decoded) {
            ++timeline->rejected;
            return timeline_error(MessageTimelineError::invalid_payload);
        }
        return apply_decoded_message(timeline, message,
                                     event.dispatch_kind == GatewayClientDispatchKind::message_create,
                                     content_availability);
    }

    if (event.dispatch_kind == GatewayClientDispatchKind::message_delete) {
        GatewayMessageDeleteEvent message{};
        if (!gateway_client_event_as_message_delete(event, payload, &message)) {
            ++timeline->rejected;
            return timeline_error(MessageTimelineError::invalid_payload);
        }
        UiScrollAnchor anchor = capture_anchor(*timeline);
        const MessageStoreResult found = message_store_find(*timeline->store, message.id);
        const u32 removal_index = found.order_index;
        const bool removed_anchor = found.ok() && anchor_in_range(anchor, removal_index, 1);
        result.kind = GatewayMessageApplyKind::deleted;
        result.mutation = message_reconcile_delete(timeline->store, message);
        if (!result.mutation.ok()) {
            ++timeline->rejected;
            result.error = MessageTimelineError::reconcile;
            return result;
        }
        if (timeline->rich_store && !message_rich_store_delete(timeline->rich_store, message.id)) {
            ++timeline->rejected;
            result.error = MessageTimelineError::rich_store;
            return result;
        }
        if (!ui_variable_list_remove(timeline->list, result.mutation.order_index, 1, &anchor)) {
            ++timeline->rejected;
            result.error = MessageTimelineError::ui_mutation;
            return result;
        }
        install_removed_anchor_fallback(timeline->list, &anchor, removed_anchor,
                                        result.mutation.order_index);
        clamp_or_follow(timeline, &anchor, previous_scroll);
        result.invalidation =
            structural_invalidation(result.mutation.order_index, old_count, timeline->list->count,
                                    timeline->scroll_units != previous_scroll);
        ++timeline->applied;
        return result;
    }

    if (event.dispatch_kind == GatewayClientDispatchKind::message_bulk_delete) {
        GatewayMessageBulkDeleteEvent message{};
        if (!gateway_client_event_as_message_bulk_delete(event, payload, bulk_ids, &message)) {
            ++timeline->rejected;
            return timeline_error(MessageTimelineError::invalid_payload);
        }
        result.kind = GatewayMessageApplyKind::bulk_deleted;
        UiScrollAnchor anchor = capture_anchor(*timeline);
        u32 first_changed = old_count;
        for (u32 id_index = 0; id_index < message.id_count; ++id_index) {
            const MessageStoreResult found = message_store_find(*timeline->store, bulk_ids[id_index]);
            if (found.error == MessageStoreError::not_found) {
                ++result.bulk.not_found;
                continue;
            }
            if (!found.ok()) {
                ++timeline->rejected;
                result.error = MessageTimelineError::reconcile;
                result.bulk.error = MessageReconcileError::store_failure;
                return result;
            }
            const MessageRecord* record = message_store_resolve(*timeline->store, found.handle);
            if (!record) {
                ++timeline->rejected;
                result.error = MessageTimelineError::reconcile;
                result.bulk.error = MessageReconcileError::store_failure;
                return result;
            }
            if (record->channel_id != message.channel_id) {
                ++result.bulk.channel_mismatch;
                continue;
            }
            const bool removed_anchor = anchor_in_range(anchor, found.order_index, 1);
            const MessageReconcileResult deleted = message_reconcile_delete(
                timeline->store, GatewayMessageDeleteEvent{bulk_ids[id_index], message.channel_id});
            if (!deleted.ok() ||
                (timeline->rich_store &&
                 !message_rich_store_delete(timeline->rich_store, bulk_ids[id_index])) ||
                !ui_variable_list_remove(timeline->list, deleted.order_index, 1, &anchor)) {
                ++timeline->rejected;
                result.error =
                    deleted.ok() ? MessageTimelineError::ui_mutation : MessageTimelineError::reconcile;
                result.bulk.error = deleted.error;
                return result;
            }
            install_removed_anchor_fallback(timeline->list, &anchor, removed_anchor, deleted.order_index);
            first_changed = minimum(first_changed, deleted.order_index);
            ++result.bulk.deleted;
        }
        if (result.bulk.deleted != 0) {
            clamp_or_follow(timeline, &anchor, previous_scroll);
            result.invalidation = structural_invalidation(first_changed, old_count, timeline->list->count,
                                                          timeline->scroll_units != previous_scroll);
        }
        ++timeline->applied;
        return result;
    }

    ++timeline->rejected;
    return timeline_error(MessageTimelineError::unsupported_event);
}

} // namespace maicord
