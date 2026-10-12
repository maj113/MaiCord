#pragma once

#include "maicord/model/message_store.hpp"
#include "maicord/protocol/gateway_message_events.hpp"

namespace maicord {

enum class MessageReconcileError : u8 {
    none,
    invalid_input,
    invalid_event,
    not_found,
    channel_mismatch,
    store_failure,
};

struct MessageReconcileResult {
    MessageHandle handle;
    u32 order_index;
    MessageStoreError store_error;
    MessageReconcileError error;
    bool inserted;

    constexpr bool ok() const noexcept {
        return error == MessageReconcileError::none;
    }
};

struct MessageBulkReconcileResult {
    u32 deleted;
    u32 not_found;
    u32 channel_mismatch;
    MessageReconcileError error;

    constexpr bool ok() const noexcept {
        return error == MessageReconcileError::none;
    }
};

MessageReconcileResult message_reconcile_create(
    MessageStore* store, const GatewayMessageEvent& event,
    MessageContentAvailability content_availability =
        MessageContentAvailability::available) noexcept;
MessageReconcileResult message_reconcile_validate_create(
    const MessageStore* store, const GatewayMessageEvent& event,
    MessageContentAvailability content_availability =
        MessageContentAvailability::available) noexcept;
MessageReconcileResult message_reconcile_update(
    MessageStore* store, const GatewayMessageEvent& event,
    MessageContentAvailability content_availability =
        MessageContentAvailability::available) noexcept;
MessageReconcileResult message_reconcile_validate_update(
    const MessageStore* store, const GatewayMessageEvent& event,
    MessageContentAvailability content_availability =
        MessageContentAvailability::available) noexcept;
MessageReconcileResult message_reconcile_delete(
    MessageStore* store, const GatewayMessageDeleteEvent& event) noexcept;
MessageBulkReconcileResult message_reconcile_bulk_delete(
    MessageStore* store, const GatewayMessageBulkDeleteEvent& event,
    ConstSlice<u64> ids) noexcept;

} // namespace maicord
