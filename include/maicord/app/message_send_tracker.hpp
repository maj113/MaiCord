#pragma once

#include "maicord/app/message_timeline.hpp"

namespace maicord {

inline constexpr u32 kMessageSendTrackerCapacity = 16;
inline constexpr u64 kMessageSendTrackerTimeoutMs = 30'000;

enum class MessageSendTrackerError : u8 {
    none,
    invalid_input,
    capacity,
    timeline,
    not_found,
    channel_mismatch,
    author_mismatch,
    message_mismatch,
};

enum class MessageSendHttpDisposition : u8 {
    success,
    retry_wait,
    terminal_failure,
};

enum class MessageSendCompletionKind : u8 {
    accepted,
    expired,
};

enum class MessageSendConfirmationSource : u8 {
    history,
    response,
};

struct MessageSendAttempt {
    u64 nonce;
    u64 channel_id;
    u64 author_id;
    u64 authoritative_id;
    u64 started_at_ms;
    u64 http_at_ms;
    u64 gateway_at_ms;
    u64 deadline_ms;
    u16 http_status;
    u8 retries;
    bool active;
    bool http_observed;
    bool http_success;
    bool gateway_observed;
    bool failed;
};

struct MessageSendCompletion {
    u64 nonce;
    u64 channel_id;
    u64 authoritative_id;
    u64 api_elapsed_ms;
    u64 gateway_elapsed_ms;
    u16 http_status;
    u8 retries;
    MessageSendCompletionKind kind;
    bool http_observed;
    bool gateway_observed;
    bool history_observed;
    bool response_observed;
};

struct MessageSendBeginResult {
    u64 nonce;
    MessageTimelineInvalidation invalidation;
    MessageSendTrackerError error;

    constexpr bool ok() const noexcept {
        return error == MessageSendTrackerError::none;
    }
};

struct MessageSendObserveResult {
    MessageTimelineInvalidation invalidation;
    MessageSendTrackerError error;
    bool completed;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == MessageSendTrackerError::none;
    }
};

struct MessageSendTracker {
    MessageSendAttempt attempts[kMessageSendTrackerCapacity];
    MessageSendCompletion completions[kMessageSendTrackerCapacity];
    u32 sequence;
    u32 completion_read;
    u32 completion_write;
    u32 completion_count;
    u32 active_count;
    u32 peak_active;
    u32 begun;
    u32 completed;
    u32 expired;
    u32 retries;
    u32 unmatched_gateway;
    u32 rejected;
    u64 total_api_elapsed_ms;
    u64 total_gateway_elapsed_ms;
    bool initialized;
};

bool message_send_tracker_initialize(MessageSendTracker* tracker) noexcept;
MessageSendBeginResult message_send_tracker_begin(
    MessageSendTracker* tracker, MessageTimeline* timeline, u64 channel_id,
    u64 author_id, Utf8View content, u64 now_unix_ms,
    u64 now_monotonic_ms, bool has_attachments = false,
    ConstSlice<GatewayMessageAttachment> local_attachments = {}, bool insert_local_preview = true) noexcept;
MessageSendObserveResult message_send_tracker_retry(
    MessageSendTracker* tracker, MessageTimeline* timeline, u64 nonce,
    u64 now_monotonic_ms) noexcept;
MessageSendObserveResult message_send_tracker_discard(
    MessageSendTracker* tracker, MessageTimeline* timeline, u64 nonce) noexcept;
MessageSendObserveResult message_send_tracker_cancel_unsent(
    MessageSendTracker* tracker, MessageTimeline* timeline, u64 nonce) noexcept;
MessageSendObserveResult message_send_tracker_observe_http(
    MessageSendTracker* tracker, MessageTimeline* timeline, u64 nonce,
    u16 http_status, MessageSendHttpDisposition disposition,
    u64 now_monotonic_ms, u64 retry_at_ms = 0) noexcept;
MessageSendObserveResult message_send_tracker_observe_gateway(
    MessageSendTracker* tracker, MessageTimeline* timeline, u64 nonce,
    u64 channel_id, u64 author_id, u64 authoritative_id, u64 now_monotonic_ms) noexcept;
// REST confirms delivery immediately without inventing a Gateway timestamp.
MessageSendObserveResult message_send_tracker_confirm(
    MessageSendTracker* tracker, MessageTimeline* timeline, u64 nonce,
    u64 channel_id, u64 author_id, u64 authoritative_id, MessageSendConfirmationSource source) noexcept;
bool message_send_tracker_tick(MessageSendTracker* tracker,
                               MessageTimeline* timeline,
                               u64 now_monotonic_ms, bool* changed) noexcept;
bool message_send_tracker_take_completion(MessageSendTracker* tracker,
                                          MessageSendCompletion* completion) noexcept;

} // namespace maicord
