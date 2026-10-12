#include "maicord/model/message_reconcile.hpp"

namespace maicord {
namespace {

static_assert(kGatewayMessageContentMaximumBytes == kMaximumMessageContentBytes);

MessageReconcileResult reconcile_error(MessageReconcileError error,
                                       MessageStoreError store_error =
                                           MessageStoreError::none) noexcept {
    return MessageReconcileResult{MessageHandle{~0u, 0}, ~0u, store_error, error, false};
}

MessageReconcileResult store_result(MessageStoreResult result) noexcept {
    return result.ok()
               ? MessageReconcileResult{result.handle, result.order_index, MessageStoreError::none,
                                        MessageReconcileError::none, result.inserted}
               : reconcile_error(MessageReconcileError::store_failure, result.error);
}

constexpr u32 create_required_fields = gateway_message_field_id |
                                       gateway_message_field_channel_id |
                                       gateway_message_field_author_id |
                                       gateway_message_field_content |
                                       gateway_message_field_timestamp;
constexpr u32 update_required_fields = gateway_message_field_id |
                                       gateway_message_field_channel_id;
constexpr u32 known_fields = create_required_fields |
                             gateway_message_field_edited_timestamp |
                             gateway_message_field_flags |
                             gateway_message_field_mention_everyone |
                             gateway_message_field_mentions |
                             gateway_message_field_mention_roles |
                             gateway_message_field_member |
                             gateway_message_field_message_reference |
                             gateway_message_field_referenced_message |
                             gateway_message_field_reactions |
                             gateway_message_field_attachments |
                              gateway_message_field_embeds |
                              gateway_message_field_sticker_items |
                              gateway_message_field_components |
                              gateway_message_field_interaction |
                              gateway_message_field_nonce |
                              gateway_message_field_type |
                              gateway_message_field_author_policy |
                              gateway_message_field_activity_instance |
                              gateway_message_field_application_id |
                              gateway_message_field_changelog_id |
                              gateway_message_field_interaction_metadata |
                              gateway_message_field_pinned | gateway_message_field_message_snapshots |
                              gateway_message_field_poll | gateway_message_field_call | gateway_message_field_activity |
                              // The started-thread object is rich-store data only (thread starter card).
                              // It must be admitted here or every page carrying it is rejected whole.
                              gateway_message_field_thread;

u32 merge_message_presentation(const GatewayMessageEvent& event, u32 previous) noexcept {
    if (event.fields & gateway_message_field_pinned)
        previous = (previous & ~message_presentation_pinned) |
            (event.pinned ? message_presentation_pinned : 0u);
    const u32 fields[] = {gateway_message_field_poll, gateway_message_field_call, gateway_message_field_activity};
    const u8 features[] = {gateway_message_forwarding_poll, gateway_message_forwarding_call,
        gateway_message_forwarding_activity};
    const u32 flags[] = {message_presentation_has_poll, message_presentation_has_call,
        message_presentation_has_activity};
    for (u32 index = 0; index < 3; ++index)
        if (event.fields & fields[index])
            previous = (previous & ~flags[index]) | ((event.forwarding_features & features[index]) ? flags[index] : 0u);
    return previous;
}

u32 create_presentation(const MessageStore& store, const GatewayMessageEvent& event) noexcept {
    const auto found = message_store_find(store, event.id);
    const auto* previous = found.ok() ? message_store_resolve(store, found.handle) : nullptr;
    return merge_message_presentation(event, previous ? previous->presentation_flags : 0u);
}

u16 create_type(const MessageStore& store, const GatewayMessageEvent& event) noexcept {
    if (event.fields & gateway_message_field_type) return event.type;
    const auto found = message_store_find(store, event.id);
    const auto* previous = found.ok() ? message_store_resolve(store, found.handle) : nullptr;
    return previous ? previous->type : u16{0};
}

MessageReconcileResult prepare_update(
    const MessageStore* store, const GatewayMessageEvent& event,
    MessageContentAvailability content_availability,
    MessageInput* merged) noexcept {
    if (!store || !merged) {
        return reconcile_error(MessageReconcileError::invalid_input);
    }
    if ((event.fields & update_required_fields) != update_required_fields ||
        (event.fields & ~known_fields) != 0) {
        return reconcile_error(MessageReconcileError::invalid_event);
    }
    const MessageStoreResult found = message_store_find(*store, event.id);
    if (!found.ok()) {
        return reconcile_error(found.error == MessageStoreError::not_found
                                   ? MessageReconcileError::not_found
                                   : MessageReconcileError::store_failure,
                               found.error);
    }
    const MessageRecord* existing = message_store_resolve(*store, found.handle);
    if (!existing) {
        return reconcile_error(MessageReconcileError::store_failure,
                               MessageStoreError::not_found);
    }
    if (existing->channel_id != event.channel_id) {
        return reconcile_error(MessageReconcileError::channel_mismatch);
    }
    const Utf8View existing_content = message_store_content(*store, found.handle);
    *merged = MessageInput{
        event.id,
        event.channel_id,
        (event.fields & gateway_message_field_author_id) != 0 ? event.author_id
                                                              : existing->author_id,
        (event.fields & gateway_message_field_timestamp) != 0 ? event.timestamp_ms
                                                               : existing->timestamp_ms,
        (event.fields & gateway_message_field_edited_timestamp) != 0
            ? event.edited_timestamp_ms
            : existing->edited_timestamp_ms,
        (event.fields & gateway_message_field_content) != 0 ? event.content
                                                             : existing_content,
        (event.fields & gateway_message_field_flags) != 0 ? event.flags
                                                           : existing->flags,
        (event.fields & gateway_message_field_content) != 0
            ? content_availability
            : existing->content_availability,
        existing->delivery,
        merge_message_presentation(event, existing->presentation_flags),
        (event.fields & gateway_message_field_type) ? event.type : existing->type,
    };
    const MessageStoreError validation = message_store_validate_input(store, *merged);
    return validation == MessageStoreError::none
               ? MessageReconcileResult{found.handle, found.order_index,
                                        MessageStoreError::none,
                                        MessageReconcileError::none, false}
               : reconcile_error(MessageReconcileError::store_failure,
                                 validation);
}

} // namespace

MessageReconcileResult message_reconcile_create(
    MessageStore* store, const GatewayMessageEvent& event,
    MessageContentAvailability content_availability) noexcept {
    if (!store) {
        return reconcile_error(MessageReconcileError::invalid_input);
    }
    if ((event.fields & create_required_fields) != create_required_fields ||
        (event.fields & ~known_fields) != 0) {
        return reconcile_error(MessageReconcileError::invalid_event);
    }
    return store_result(message_store_upsert(
        store, MessageInput{event.id, event.channel_id, event.author_id,
                            event.timestamp_ms, event.edited_timestamp_ms,
                            event.content, event.flags, content_availability,
                            MessageDeliveryState::authoritative, create_presentation(*store, event),
                            create_type(*store, event)}));
}

MessageReconcileResult message_reconcile_validate_create(
    const MessageStore* store, const GatewayMessageEvent& event,
    MessageContentAvailability content_availability) noexcept {
    if (!store) {
        return reconcile_error(MessageReconcileError::invalid_input);
    }
    if ((event.fields & create_required_fields) != create_required_fields ||
        (event.fields & ~known_fields) != 0) {
        return reconcile_error(MessageReconcileError::invalid_event);
    }
    const MessageStoreError validation = message_store_validate_input(
        store, MessageInput{event.id, event.channel_id, event.author_id,
                            event.timestamp_ms, event.edited_timestamp_ms,
                            event.content, event.flags, content_availability,
                            MessageDeliveryState::authoritative, create_presentation(*store, event),
                            create_type(*store, event)});
    return validation == MessageStoreError::none
               ? MessageReconcileResult{MessageHandle{~0u, 0}, ~0u,
                                        MessageStoreError::none,
                                        MessageReconcileError::none, false}
               : reconcile_error(MessageReconcileError::store_failure,
                                 validation);
}

MessageReconcileResult message_reconcile_update(
    MessageStore* store, const GatewayMessageEvent& event,
    MessageContentAvailability content_availability) noexcept {
    if (!store) {
        return reconcile_error(MessageReconcileError::invalid_input);
    }
    if ((event.fields & update_required_fields) != update_required_fields ||
        (event.fields & ~known_fields) != 0) {
        return reconcile_error(MessageReconcileError::invalid_event);
    }
    const MessageStoreResult found = message_store_find(*store, event.id);
    if (!found.ok()) {
        return reconcile_error(found.error == MessageStoreError::not_found
                                   ? MessageReconcileError::not_found
                                   : MessageReconcileError::store_failure,
                               found.error);
    }
    const MessageRecord* existing = message_store_resolve(*store, found.handle);
    if (!existing) {
        return reconcile_error(MessageReconcileError::store_failure,
                               MessageStoreError::not_found);
    }
    if (existing->channel_id != event.channel_id) {
        return reconcile_error(MessageReconcileError::channel_mismatch);
    }
    const Utf8View existing_content = message_store_content(*store, found.handle);
    const MessageInput merged{
        event.id,
        event.channel_id,
        (event.fields & gateway_message_field_author_id) != 0 ? event.author_id
                                                              : existing->author_id,
        (event.fields & gateway_message_field_timestamp) != 0 ? event.timestamp_ms
                                                               : existing->timestamp_ms,
        (event.fields & gateway_message_field_edited_timestamp) != 0
            ? event.edited_timestamp_ms
            : existing->edited_timestamp_ms,
        (event.fields & gateway_message_field_content) != 0 ? event.content
                                                             : existing_content,
        (event.fields & gateway_message_field_flags) != 0 ? event.flags
                                                           : existing->flags,
        (event.fields & gateway_message_field_content) != 0
            ? content_availability
            : existing->content_availability,
        existing->delivery,
        merge_message_presentation(event, existing->presentation_flags),
        (event.fields & gateway_message_field_type) ? event.type : existing->type,
    };
    return store_result(message_store_upsert(store, merged));
}

MessageReconcileResult message_reconcile_validate_update(
    const MessageStore* store, const GatewayMessageEvent& event,
    MessageContentAvailability content_availability) noexcept {
    MessageInput merged{};
    return prepare_update(store, event, content_availability, &merged);
}

MessageReconcileResult message_reconcile_delete(
    MessageStore* store, const GatewayMessageDeleteEvent& event) noexcept {
    if (!store || event.id == 0 || event.channel_id == 0) {
        return reconcile_error(MessageReconcileError::invalid_input);
    }
    const MessageStoreResult found = message_store_find(*store, event.id);
    if (!found.ok()) {
        return reconcile_error(found.error == MessageStoreError::not_found
                                   ? MessageReconcileError::not_found
                                   : MessageReconcileError::store_failure,
                               found.error);
    }
    const MessageRecord* record = message_store_resolve(*store, found.handle);
    if (!record) {
        return reconcile_error(MessageReconcileError::store_failure,
                               MessageStoreError::not_found);
    }
    if (record->channel_id != event.channel_id) {
        return reconcile_error(MessageReconcileError::channel_mismatch);
    }
    return message_store_delete(store, event.id)
               ? MessageReconcileResult{found.handle, found.order_index, MessageStoreError::none,
                                        MessageReconcileError::none, false}
               : reconcile_error(MessageReconcileError::store_failure,
                                 MessageStoreError::not_found);
}

MessageBulkReconcileResult message_reconcile_bulk_delete(
    MessageStore* store, const GatewayMessageBulkDeleteEvent& event,
    ConstSlice<u64> ids) noexcept {
    if (!store || event.channel_id == 0 || event.id_count == 0 ||
        event.id_count > kGatewayBulkDeleteMaximumIds || event.id_count != ids.count ||
        !ids.data) {
        return MessageBulkReconcileResult{0, 0, 0,
                                          MessageReconcileError::invalid_input};
    }
    MessageBulkReconcileResult result{};
    for (u32 index = 0; index < ids.count; ++index) {
        const MessageStoreResult found = message_store_find(*store, ids[index]);
        if (found.error == MessageStoreError::not_found) {
            ++result.not_found;
            continue;
        }
        if (!found.ok()) {
            result.error = MessageReconcileError::store_failure;
            return result;
        }
        const MessageRecord* record = message_store_resolve(*store, found.handle);
        if (!record) {
            result.error = MessageReconcileError::store_failure;
            return result;
        }
        if (record->channel_id != event.channel_id) {
            ++result.channel_mismatch;
            continue;
        }
        if (!message_store_delete(store, ids[index])) {
            result.error = MessageReconcileError::store_failure;
            return result;
        }
        ++result.deleted;
    }
    return result;
}

} // namespace maicord

