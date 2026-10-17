#include "maicord/model/message_reconcile.hpp"
#include "maicord/protocol/gateway_message_events.hpp"

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

// Finds the resident record a Gateway event names and checks its channel.
MessageReconcileResult find_resident(const MessageStore& store, u64 message_id, u64 channel_id,
                                     const MessageRecord** record) noexcept {
    const MessageStoreResult found = message_store_find(store, message_id);
    if (!found.ok()) {
        return reconcile_error(found.error == MessageStoreError::not_found
                                   ? MessageReconcileError::not_found
                                   : MessageReconcileError::store_failure,
                               found.error);
    }
    *record = message_store_resolve(store, found.handle);
    if (!*record) {
        return reconcile_error(MessageReconcileError::store_failure,
                               MessageStoreError::not_found);
    }
    if ((*record)->channel_id != channel_id) {
        return reconcile_error(MessageReconcileError::channel_mismatch);
    }
    return MessageReconcileResult{found.handle, found.order_index, MessageStoreError::none,
                                  MessageReconcileError::none, false};
}

// Merges an update over the resident record; the result carries its handle.
MessageReconcileResult merge_update(
    const MessageStore* store, const GatewayMessageEvent& event,
    MessageContentAvailability content_availability,
    MessageInput* merged) noexcept {
    if (!store) {
        return reconcile_error(MessageReconcileError::invalid_input);
    }
    if ((event.fields & update_required_fields) != update_required_fields ||
        (event.fields & ~known_fields) != 0) {
        return reconcile_error(MessageReconcileError::invalid_event);
    }
    const MessageRecord* existing = nullptr;
    const MessageReconcileResult found = find_resident(*store, event.id, event.channel_id, &existing);
    if (!found.ok()) {
        return found;
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
    return found;
}

// Admits a create event and builds its record input.
MessageReconcileError prepare_create(const MessageStore* store, const GatewayMessageEvent& event,
                                     MessageContentAvailability content_availability,
                                     MessageInput* input) noexcept {
    if (!store) {
        return MessageReconcileError::invalid_input;
    }
    if ((event.fields & create_required_fields) != create_required_fields ||
        (event.fields & ~known_fields) != 0) {
        return MessageReconcileError::invalid_event;
    }
    *input = MessageInput{event.id, event.channel_id, event.author_id,
                          event.timestamp_ms, event.edited_timestamp_ms,
                          event.content, event.flags, content_availability,
                          MessageDeliveryState::authoritative, create_presentation(*store, event),
                          create_type(*store, event)};
    return MessageReconcileError::none;
}

} // namespace

MessageReconcileResult message_reconcile_create(
    MessageStore* store, const GatewayMessageEvent& event,
    MessageContentAvailability content_availability) noexcept {
    MessageInput input{};
    const MessageReconcileError error = prepare_create(store, event, content_availability, &input);
    if (error != MessageReconcileError::none) {
        return reconcile_error(error);
    }
    return store_result(message_store_upsert(store, input));
}

MessageReconcileResult message_reconcile_validate_create(
    const MessageStore* store, const GatewayMessageEvent& event,
    MessageContentAvailability content_availability) noexcept {
    MessageInput input{};
    const MessageReconcileError error = prepare_create(store, event, content_availability, &input);
    if (error != MessageReconcileError::none) {
        return reconcile_error(error);
    }
    const MessageStoreError validation = message_store_validate_input(store, input);
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
    MessageInput merged{};
    const MessageReconcileResult prepared = merge_update(store, event, content_availability, &merged);
    if (!prepared.ok())
        return prepared;
    return store_result(message_store_upsert(store, merged));
}

MessageReconcileResult message_reconcile_validate_update(
    const MessageStore* store, const GatewayMessageEvent& event,
    MessageContentAvailability content_availability) noexcept {
    MessageInput merged{};
    const MessageReconcileResult prepared = merge_update(store, event, content_availability, &merged);
    if (!prepared.ok())
        return prepared;
    const MessageStoreError validation = message_store_validate_input(store, merged);
    return validation == MessageStoreError::none
               ? prepared
               : reconcile_error(MessageReconcileError::store_failure, validation);
}

MessageReconcileResult message_reconcile_delete(
    MessageStore* store, const GatewayMessageDeleteEvent& event) noexcept {
    if (!store || event.id == 0 || event.channel_id == 0) {
        return reconcile_error(MessageReconcileError::invalid_input);
    }
    const MessageRecord* record = nullptr;
    const MessageReconcileResult found = find_resident(*store, event.id, event.channel_id, &record);
    if (!found.ok()) {
        return found;
    }
    return message_store_delete(store, event.id)
               ? found
               : reconcile_error(MessageReconcileError::store_failure,
                                 MessageStoreError::not_found);
}

} // namespace maicord

