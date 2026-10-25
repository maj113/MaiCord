#include "maicord/app/message_send_tracker.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
namespace {

MessageSendObserveResult observe_error(MessageSendTrackerError error) noexcept {
    return MessageSendObserveResult{MessageTimelineInvalidation{}, error, false, false};
}

MessageSendAttempt* find_attempt(MessageSendTracker* tracker, u64 nonce) noexcept {
    if (nonce == 0) {
        return nullptr;
    }
    for (u32 index = 0; index < kMessageSendTrackerCapacity; ++index) {
        MessageSendAttempt& attempt = tracker->attempts[index];
        if (attempt.active && attempt.nonce == nonce) {
            return &attempt;
        }
    }
    return nullptr;
}

MessageSendAttempt* empty_attempt(MessageSendTracker* tracker) noexcept {
    for (u32 index = 0; index < kMessageSendTrackerCapacity; ++index) {
        if (!tracker->attempts[index].active) {
            return &tracker->attempts[index];
        }
    }
    return nullptr;
}

u64 elapsed(u64 start, u64 finish) noexcept {
    return finish >= start ? finish - start : 0;
}

void push_completion(MessageSendTracker* tracker, const MessageSendAttempt& attempt,
                     MessageSendCompletionKind kind, bool history_observed = false, bool response_observed = false) noexcept {
    if (tracker->completion_count == kMessageSendTrackerCapacity) {
        tracker->completion_read = (tracker->completion_read + 1) % kMessageSendTrackerCapacity;
        --tracker->completion_count;
    }
    MessageSendCompletion& completion = tracker->completions[tracker->completion_write];
    completion = MessageSendCompletion{
        attempt.nonce,
        attempt.channel_id,
        attempt.authoritative_id,
        attempt.http_observed ? elapsed(attempt.started_at_ms, attempt.http_at_ms) : 0,
        attempt.gateway_observed ? elapsed(attempt.started_at_ms, attempt.gateway_at_ms) : 0,
        attempt.http_status,
        attempt.retries,
        kind,
        attempt.http_observed,
        attempt.gateway_observed,
        history_observed,
        response_observed,
    };
    tracker->completion_write = (tracker->completion_write + 1) % kMessageSendTrackerCapacity;
    ++tracker->completion_count;
}

void finish_attempt(MessageSendTracker* tracker, MessageSendAttempt* attempt,
                    MessageSendCompletionKind kind, bool history_observed = false, bool response_observed = false) noexcept {
    push_completion(tracker, *attempt, kind, history_observed, response_observed);
    if (tracker->active_count != 0) {
        --tracker->active_count;
    }
    reset_value<MessageSendAttempt>(attempt);
}

u64 next_nonce(MessageSendTracker* tracker, const MessageTimeline& timeline, u64 now_unix_ms) noexcept {
    const u64 epoch_ms =
        now_unix_ms > kSnowflakeEpochMilliseconds ? now_unix_ms - kSnowflakeEpochMilliseconds : now_unix_ms;
    for (u32 attempt = 0; attempt < kMessageSendTrackerCapacity + 1; ++attempt) {
        ++tracker->sequence;
        if (tracker->sequence == 0) {
            ++tracker->sequence;
        }
        u64 nonce = (epoch_ms << 22) | (tracker->sequence & 0x3FFFFFu);
        if (nonce == 0) {
            nonce = tracker->sequence;
        }
        if (!find_attempt(tracker, nonce) &&
            message_store_find(*timeline.store, nonce).error == MessageStoreError::not_found) {
            return nonce;
        }
    }
    return 0;
}

// Removes the local row of an attempt `droppable` accepts and retires the attempt.
template <class Droppable>
MessageSendObserveResult drop_attempt(MessageSendTracker* tracker, MessageTimeline* timeline, u64 nonce,
                                      Droppable droppable) noexcept {
    if (!tracker || !tracker->initialized || !timeline || !timeline->store || nonce == 0) {
        return observe_error(MessageSendTrackerError::invalid_input);
    }
    MessageSendAttempt* const attempt = find_attempt(tracker, nonce);
    if (!attempt || !droppable(*attempt)) {
        return observe_error(MessageSendTrackerError::not_found);
    }
    MessageTimelineInvalidation invalidation{};
    const MessageStoreResult local = message_store_find(*timeline->store, nonce);
    if (local.ok()) {
        const MessageTimelineApplyResult removed = message_timeline_remove_local(timeline, nonce);
        if (!removed.ok()) {
            return observe_error(MessageSendTrackerError::timeline);
        }
        invalidation = removed.invalidation;
    } else if (local.error != MessageStoreError::not_found) {
        return observe_error(MessageSendTrackerError::timeline);
    }
    if (tracker->active_count != 0) {
        --tracker->active_count;
    }
    reset_value<MessageSendAttempt>(attempt);
    return MessageSendObserveResult{invalidation, MessageSendTrackerError::none, false,
                                    invalidation.count != 0};
}

} // namespace

bool message_send_tracker_initialize(MessageSendTracker* tracker) noexcept {
    if (!tracker) {
        return false;
    }
    reset_value<MessageSendTracker>(tracker);
    tracker->initialized = true;
    return true;
}

MessageSendBeginResult message_send_tracker_begin(MessageSendTracker* tracker, MessageTimeline* timeline,
                                                  u64 channel_id, u64 author_id, Utf8View content,
                                                  u64 now_unix_ms, u64 now_monotonic_ms, bool has_attachments,
                                                  ConstSlice<GatewayMessageAttachment> local_attachments,
                                                  bool insert_local_preview) noexcept {
    if (!tracker || !tracker->initialized || !timeline || !timeline->store || channel_id == 0 ||
        author_id == 0 || now_unix_ms == 0 || now_monotonic_ms == 0 || (!content.data && content.bytes) ||
        (content.bytes == 0 && !has_attachments)) {
        return MessageSendBeginResult{0, {}, MessageSendTrackerError::invalid_input};
    }
    MessageSendAttempt* const attempt = empty_attempt(tracker);
    if (!attempt) {
        return MessageSendBeginResult{0, {}, MessageSendTrackerError::capacity};
    }
    const u64 nonce = next_nonce(tracker, *timeline, now_unix_ms);
    if (nonce == 0) {
        return MessageSendBeginResult{0, {}, MessageSendTrackerError::capacity};
    }
    const MessageTimelineApplyResult inserted = insert_local_preview ? message_timeline_insert_local(
        timeline, MessageInput{nonce, channel_id, author_id, now_unix_ms, 0, content, 0,
                               MessageContentAvailability::available, MessageDeliveryState::pending}, local_attachments) :
        MessageTimelineApplyResult{};
    if (!inserted.ok()) {
        return MessageSendBeginResult{0, inserted.invalidation, MessageSendTrackerError::timeline};
    }
    reset_value<MessageSendAttempt>(attempt);
    attempt->nonce = nonce;
    attempt->channel_id = channel_id;
    attempt->author_id = author_id;
    attempt->started_at_ms = now_monotonic_ms;
    attempt->deadline_ms = deadline_after(now_monotonic_ms, kMessageSendTrackerTimeoutMs);
    attempt->active = true;
    ++tracker->active_count;
    return MessageSendBeginResult{nonce, inserted.invalidation, MessageSendTrackerError::none};
}

MessageSendObserveResult message_send_tracker_retry(MessageSendTracker* tracker, MessageTimeline* timeline,
                                                    u64 nonce, u64 now_monotonic_ms) noexcept {
    if (!tracker || !tracker->initialized || !timeline || !timeline->store || now_monotonic_ms == 0) {
        return observe_error(MessageSendTrackerError::invalid_input);
    }
    MessageSendAttempt* const attempt = find_attempt(tracker, nonce);
    if (!attempt || !attempt->failed) {
        return observe_error(MessageSendTrackerError::not_found);
    }
    MessageTimelineInvalidation invalidation{};
    const auto resident = message_store_find(*timeline->store, nonce);
    if ((resident.ok() && !message_timeline_set_delivery(timeline, nonce, MessageDeliveryState::pending, &invalidation)) ||
        (!resident.ok() && resident.error != MessageStoreError::not_found)) {
        return observe_error(MessageSendTrackerError::timeline);
    }
    attempt->http_observed = false;
    attempt->http_success = false;
    attempt->failed = false;
    attempt->http_at_ms = 0;
    attempt->http_status = 0;
    attempt->deadline_ms = deadline_after(now_monotonic_ms, kMessageSendTrackerTimeoutMs);
    if (attempt->retries != 0xFFu) {
        ++attempt->retries;
    }
    return MessageSendObserveResult{invalidation, MessageSendTrackerError::none, false,
                                    invalidation.count != 0};
}

MessageSendObserveResult message_send_tracker_discard(MessageSendTracker* tracker,
                                                      MessageTimeline* timeline, u64 nonce) noexcept {
    return drop_attempt(tracker, timeline, nonce, [](const MessageSendAttempt& attempt) noexcept {
        return attempt.failed;
    });
}

MessageSendObserveResult message_send_tracker_cancel_unsent(MessageSendTracker* tracker,
    MessageTimeline* timeline, u64 nonce) noexcept {
    return drop_attempt(tracker, timeline, nonce, [](const MessageSendAttempt& attempt) noexcept {
        return !attempt.gateway_observed && !attempt.http_success;
    });
}

MessageSendObserveResult message_send_tracker_observe_http(MessageSendTracker* tracker,
                                                           MessageTimeline* timeline, u64 nonce,
                                                           u16 http_status,
                                                           MessageSendHttpDisposition disposition,
                                                           u64 now_monotonic_ms, u64 retry_at_ms) noexcept {
    if (!tracker || !tracker->initialized || !timeline || nonce == 0 || now_monotonic_ms == 0) {
        return observe_error(MessageSendTrackerError::invalid_input);
    }
    MessageSendAttempt* const attempt = find_attempt(tracker, nonce);
    if (!attempt) {
        return observe_error(MessageSendTrackerError::not_found);
    }
    MessageTimelineInvalidation invalidation{};
    const bool was_failed = attempt->failed;
    if (disposition == MessageSendHttpDisposition::retry_wait) {
        if (retry_at_ms != 0 && retry_at_ms < now_monotonic_ms) {
            return observe_error(MessageSendTrackerError::invalid_input);
        }
        if (was_failed && !attempt->gateway_observed && message_store_find(*timeline->store, nonce).ok() &&
            !message_timeline_set_delivery(timeline, nonce, MessageDeliveryState::pending, &invalidation))
            return observe_error(MessageSendTrackerError::timeline);
        attempt->failed = false;
        if (attempt->retries != 0xFFu) {
            ++attempt->retries;
        }
        attempt->http_status = http_status;
        const u64 retry_base = retry_at_ms != 0 ? retry_at_ms : now_monotonic_ms;
        attempt->deadline_ms = deadline_after(retry_base, kMessageSendTrackerTimeoutMs);
        return MessageSendObserveResult{invalidation, MessageSendTrackerError::none, false, invalidation.count != 0};
    }
    attempt->http_observed = true;
    attempt->http_success = disposition == MessageSendHttpDisposition::success;
    attempt->failed = disposition == MessageSendHttpDisposition::terminal_failure;
    attempt->http_at_ms = now_monotonic_ms;
    attempt->http_status = http_status;
    if (attempt->failed) {
        // A terminal response is already user-actionable. Keep the failed
        // attempt and its exact payload for retry/discard, but do not schedule
        // a second synthetic timeout completion for the same failure.
        attempt->deadline_ms = 0;
    } else if (attempt->deadline_ms == 0) {
        attempt->deadline_ms = deadline_after(now_monotonic_ms, kMessageSendTrackerTimeoutMs);
    }
    if ((was_failed || attempt->failed) && !attempt->gateway_observed && message_store_find(*timeline->store, nonce).ok() &&
        !message_timeline_set_delivery(timeline, nonce,
            attempt->failed ? MessageDeliveryState::failed : MessageDeliveryState::pending, &invalidation)) {
        return observe_error(MessageSendTrackerError::timeline);
    }
    const bool complete = attempt->gateway_observed;
    if (complete) {
        finish_attempt(tracker, attempt, MessageSendCompletionKind::accepted);
    }
    return MessageSendObserveResult{invalidation, MessageSendTrackerError::none, complete,
                                    invalidation.count != 0};
}

MessageSendObserveResult message_send_tracker_observe_gateway(MessageSendTracker* tracker,
                                                              MessageTimeline* timeline, u64 nonce,
                                                              u64 channel_id, u64 author_id, u64 authoritative_id,
                                                              u64 now_monotonic_ms) noexcept {
    if (!tracker || !tracker->initialized || !timeline || nonce == 0 || channel_id == 0 || author_id == 0 ||
        authoritative_id == 0 || now_monotonic_ms == 0) {
        return observe_error(MessageSendTrackerError::invalid_input);
    }
    MessageSendAttempt* const attempt = find_attempt(tracker, nonce);
    if (!attempt) {
        ++tracker->unmatched_gateway;
        return MessageSendObserveResult{{}, MessageSendTrackerError::none, false, false};
    }
    if (attempt->channel_id != channel_id) {
        return observe_error(MessageSendTrackerError::channel_mismatch);
    }
    if (attempt->author_id != author_id) return observe_error(MessageSendTrackerError::author_mismatch);
    if (attempt->authoritative_id && attempt->authoritative_id != authoritative_id)
        return observe_error(MessageSendTrackerError::message_mismatch);
    // Gateway replay must not replace the original observation timestamp.
    if (attempt->gateway_observed) return MessageSendObserveResult{{}, MessageSendTrackerError::none, false, false};
    const MessageTimelineApplyResult removed = message_timeline_remove_local(timeline, nonce);
    if (!removed.ok()) {
        return observe_error(MessageSendTrackerError::timeline);
    }
    attempt->gateway_observed = true;
    attempt->gateway_at_ms = now_monotonic_ms;
    attempt->authoritative_id = authoritative_id;
    const bool complete = attempt->http_observed;
    if (complete) {
        finish_attempt(tracker, attempt, MessageSendCompletionKind::accepted);
    }
    return MessageSendObserveResult{removed.invalidation, MessageSendTrackerError::none, complete,
                                    removed.invalidation.count != 0};
}

MessageSendObserveResult message_send_tracker_confirm(MessageSendTracker* tracker,
    MessageTimeline* timeline, u64 nonce, u64 channel_id, u64 author_id, u64 authoritative_id, MessageSendConfirmationSource source) noexcept {
    if (!tracker || !tracker->initialized || !timeline || !timeline->store ||
        !nonce || !channel_id || !author_id || !authoritative_id ||
        (source != MessageSendConfirmationSource::history && source != MessageSendConfirmationSource::response)) {
        return observe_error(MessageSendTrackerError::invalid_input);
    }
    MessageSendAttempt* const attempt = find_attempt(tracker, nonce);
    if (!attempt) return MessageSendObserveResult{{}, MessageSendTrackerError::none, false, false};
    if (attempt->channel_id != channel_id) return observe_error(MessageSendTrackerError::channel_mismatch);
    if (attempt->author_id != author_id) return observe_error(MessageSendTrackerError::author_mismatch);
    if (attempt->authoritative_id && attempt->authoritative_id != authoritative_id)
        return observe_error(MessageSendTrackerError::message_mismatch);
    const auto removed = message_timeline_remove_local(timeline, nonce);
    if (!removed.ok()) return observe_error(MessageSendTrackerError::timeline);
    attempt->authoritative_id = authoritative_id;
    finish_attempt(tracker, attempt, MessageSendCompletionKind::accepted,
        source == MessageSendConfirmationSource::history, source == MessageSendConfirmationSource::response);
    return MessageSendObserveResult{removed.invalidation, MessageSendTrackerError::none, true,
        removed.invalidation.count != 0};
}

bool message_send_tracker_tick(MessageSendTracker* tracker, MessageTimeline* timeline, u64 now_monotonic_ms,
                               bool* changed) noexcept {
    if (changed) {
        *changed = false;
    }
    if (!tracker || !tracker->initialized || !timeline || now_monotonic_ms == 0 || !changed) {
        return false;
    }
    for (u32 index = 0; index < kMessageSendTrackerCapacity; ++index) {
        MessageSendAttempt& attempt = tracker->attempts[index];
        if (!attempt.active || attempt.deadline_ms == 0 || now_monotonic_ms < attempt.deadline_ms) {
            continue;
        }
        if (attempt.gateway_observed) {
            finish_attempt(tracker, &attempt, MessageSendCompletionKind::accepted);
            continue;
        }
        MessageTimelineInvalidation invalidation{};
        if (message_store_find(*timeline->store, attempt.nonce).ok() &&
            !message_timeline_set_delivery(timeline, attempt.nonce, MessageDeliveryState::failed,
                                           &invalidation)) {
            return false;
        }
        *changed = *changed || invalidation.count != 0;
        // Expiry makes the row user-actionable; it must not destroy the
        // attempt required by message_send_tracker_retry(). Publish the
        // one-shot completion to the controller and park the retained attempt
        // without a deadline until the user retries or discards it.
        attempt.failed = true;
        attempt.deadline_ms = 0;
        push_completion(tracker, attempt, MessageSendCompletionKind::expired);
    }
    return true;
}

bool message_send_tracker_take_completion(MessageSendTracker* tracker,
                                          MessageSendCompletion* completion) noexcept {
    if (!tracker || !tracker->initialized || !completion || tracker->completion_count == 0) {
        return false;
    }
    *completion = tracker->completions[tracker->completion_read];
    reset_value<MessageSendCompletion>(&tracker->completions[tracker->completion_read]);
    tracker->completion_read = (tracker->completion_read + 1) % kMessageSendTrackerCapacity;
    --tracker->completion_count;
    return true;
}

} // namespace maicord
