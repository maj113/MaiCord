#pragma once

#include "maicord/model/message_reconcile.hpp"
#include "maicord/ui/virtual_list.hpp"

namespace maicord {

struct GatewayMessageAttachment;
struct MessageRichStore;

struct GatewayClientEvent;

enum class GatewayMessageApplyKind : u8 {
    none,
    created,
    updated,
    deleted,
    bulk_deleted,
    deferred,
};

enum class MessageTimelineError : u8 {
    none,
    invalid_input,
    count_mismatch,
    ui_capacity,
    invalid_payload,
    unsupported_event,
    reconcile,
    ui_mutation,
    rich_store,
};

struct MessageTimelineInvalidation {
    u32 first;
    u32 count;
    bool structural;
    bool scroll_changed;
};

struct MessageTimelineApplyResult {
    MessageReconcileResult mutation;
    MessageBulkReconcileResult bulk;
    MessageTimelineInvalidation invalidation;
    u32 evicted;
    GatewayMessageApplyKind kind;
    MessageTimelineError error;
    bool compacted;

    constexpr bool ok() const noexcept {
        return error == MessageTimelineError::none;
    }
};

struct MessageTimeline {
    MessageStore* store;
    MessageRichStore* rich_store;
    UiVariableList* list;
    u64 scroll_units;
    // Cumulative coordinate translation caused by layout/history anchoring,
    // excluding deliberate scrolling. Animations follow this translation.
    double scroll_origin_units;
    u64 viewport_units;
    u32 default_item_units;
    u32 applied;
    u32 rejected;
    u32 evicted;
    u32 deferred;
    u32 compactions;
    u32 history_pages;
    u32 history_inserted;
    u32 reset_generation;
    bool follow_bottom;
    bool newer_refetch_required;
};

bool message_timeline_initialize(MessageTimeline* timeline, MessageStore* store, UiVariableList* list,
                                 u32 default_item_units, u64 viewport_units, bool follow_bottom) noexcept;
bool message_timeline_bind_rich_store(MessageTimeline* timeline,
                                      MessageRichStore* rich_store) noexcept;
bool message_timeline_reset(MessageTimeline* timeline) noexcept;
bool message_timeline_set_viewport(MessageTimeline* timeline, u64 viewport_units) noexcept;
bool message_timeline_set_scroll(MessageTimeline* timeline, u64 scroll_units, bool follow_bottom) noexcept;
bool message_timeline_restore_scroll(MessageTimeline* timeline, u64 scroll_units, bool follow_bottom) noexcept;
// The bottom scroll offset: content height beyond the viewport, or 0.
u64 message_timeline_maximum_scroll(const MessageTimeline& timeline) noexcept;
bool message_timeline_scroll_by(MessageTimeline* timeline, i64 delta_units, u64 bottom_tolerance_units,
                                bool* changed) noexcept;
bool message_timeline_scroll_to_start(MessageTimeline* timeline, bool* changed) noexcept;
bool message_timeline_scroll_to_bottom(MessageTimeline* timeline, bool* changed) noexcept;
bool message_timeline_is_at_bottom(const MessageTimeline& timeline, u64 tolerance_units) noexcept;
bool message_timeline_set_item_height(MessageTimeline* timeline, u32 index, u32 item_units,
                                      MessageTimelineInvalidation* invalidation) noexcept;
MessageTimelineApplyResult message_timeline_insert_local(
    MessageTimeline* timeline, const MessageInput& input,
    ConstSlice<GatewayMessageAttachment> attachments = {}) noexcept;
MessageTimelineApplyResult message_timeline_remove_local(
    MessageTimeline* timeline, u64 message_id) noexcept;
bool message_timeline_set_delivery(MessageTimeline* timeline, u64 message_id,
                                   MessageDeliveryState delivery,
                                   MessageTimelineInvalidation* invalidation) noexcept;
// Gives a local preview the reply quote its echo will carry, so it groups and
// measures like the confirmed message. A non-resident target is left unquoted.
// False only for invalid input, a missing or confirmed record, or a rich-store failure.
bool message_timeline_set_local_reply(MessageTimeline* timeline, u64 message_id, u64 reply_message_id) noexcept;
// Applies a fully decoded authoritative create without a Gateway envelope or
// sequence. Shares resident eviction, rich content and layout with live events.
MessageTimelineApplyResult message_timeline_apply_created(
    MessageTimeline* timeline, const GatewayMessageEvent& message,
    MessageContentAvailability content_availability = MessageContentAvailability::available) noexcept;
MessageTimelineApplyResult message_timeline_apply_updated(
    MessageTimeline* timeline, const GatewayMessageEvent& message,
    MessageContentAvailability content_availability = MessageContentAvailability::available) noexcept;
MessageTimelineApplyResult message_timeline_apply(
    MessageTimeline* timeline, const GatewayClientEvent& event, ConstSlice<u8> payload, Slice<u64> bulk_ids,
    MessageContentAvailability content_availability = MessageContentAvailability::available) noexcept;

} // namespace maicord
