#include "maicord/app/read_ack_controller.hpp"
#include "maicord/app/read_ack_visibility.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace read_ack_probe {
ReadAckIntent all_intents[kReadAckMaximumIntents];
struct ReserveProbe { u32 calls, required, fail_above; };
bool reserve(void* context, u32 required) noexcept {
    auto& probe = *static_cast<ReserveProbe*>(context);
    ++probe.calls; probe.required = required;
    return !probe.fail_above || required <= probe.fail_above;
}
ReadAckIntent desired(u64 channel, u64 message, u32 viewed = 1, u32 flags = 1) noexcept {
    return {channel, message, viewed, flags, 0};
}
bool queue_and_mutate(ReadAckController* controller, const ReadAckIntent& intent,
    u32* local_mutations) noexcept {
    if (!read_ack_controller_queue(controller, intent, 0, true)) return false;
    ++*local_mutations; return true;
}
} // namespace read_ack_probe

void test_read_ack_controller(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace read_ack_probe;
    ReadAckIntent slots[4]{};
    ReadAckController controller{};
    ReadAckIntent intent{};
    ReadAckRequestTicket ticket{};
    u64 due = 0;
    check(!read_ack_controller_initialize(nullptr, {slots, 4}) &&
        !read_ack_controller_initialize(&controller, {}) &&
        read_ack_controller_initialize(&controller, {slots, 4}) && controller.generation &&
        controller.backed_count == 4 && !controller.count,
        "ACK controller uses caller backing and rejects absent storage");
    const auto empty = controller;
    check(!read_ack_controller_queue(&controller, desired(0, 10), 0, true) &&
        !read_ack_controller_queue(&controller, desired(1, 0), 0, true) &&
        !read_ack_controller_admit(&controller, desired(0, 0), 1, &ticket) &&
        !read_ack_controller_next_due(controller, &due) && !read_ack_controller_peek_due(controller, 0, &intent) &&
        bytes_equal(&controller, &empty, sizeof(controller)),
        "missing and zero ACK IDs cannot invent queued or admitted work");
    check(read_ack_controller_queue(&controller, desired(30, 300), 100, false) &&
        read_ack_controller_next_due(controller, &due) && due == 3100 &&
        !read_ack_controller_peek_due(controller, 3099, &intent),
        "automatic ACK has a three second coalescing deadline");
    const u32 unchanged_revision = controller.revision;
    check(read_ack_controller_queue(&controller, desired(30, 300), 2500, false) &&
        controller.revision == unchanged_revision && controller.pending[0].due_at_ms == 3100 &&
        read_ack_controller_queue(&controller, desired(30, 301, 2, 2), 3000, false) &&
        controller.count == 1 && controller.pending[0].message_id == 301 &&
        controller.pending[0].last_viewed == 2 && controller.pending[0].flags == 2 &&
        controller.pending[0].due_at_ms == 3100,
        "equal updates and latest desired cursors cannot postpone an existing timer forever");
    check(read_ack_controller_queue(&controller, desired(30, 301, 2, 2), 3050, true) &&
        read_ack_controller_next_due(controller, &due) && due == 3050 &&
        read_ack_controller_peek_due(controller, 3050, &intent),
        "mentions and explicit immediate actions accelerate automatic intent");
    const auto busy_before = controller;
    check(read_ack_controller_peek_due(controller, 9999, &intent) &&
        bytes_equal(&controller, &busy_before, sizeof(controller)) &&
        !read_ack_controller_admit(&controller, intent, 0, &ticket) &&
        bytes_equal(&controller, &busy_before, sizeof(controller)),
        "busy transport peeks and failed admission preserve the queued target");
    check(read_ack_controller_admit(&controller, intent, 71, &ticket) &&
        controller.inflight_active && controller.count == 1 &&
        controller.inflight.message_id == 301 && ticket.request_id == 71 &&
        ticket.generation == controller.generation &&
        !read_ack_controller_next_due(controller, &due) &&
        !read_ack_controller_peek_due(controller, 9999, &intent),
        "transport admission copies target but leaves desired queue until completion");
    const auto inflight_before = controller;
    ReadAckRequestTicket rejected_ticket{};
    check(!read_ack_controller_admit(&controller, controller.inflight, 72, &rejected_ticket) &&
        bytes_equal(&controller, &inflight_before, sizeof(controller)) &&
        read_ack_controller_complete(&controller, {72, ticket.generation}, ReadAckCompletion::success) ==
            ReadAckCompletionResult::ignored && bytes_equal(&controller, &inflight_before, sizeof(controller)),
        "a second admission and unrelated response cannot consume active or desired work");
    check(read_ack_controller_queue(&controller, desired(30, 302, 3, 4), 3100, false) &&
        controller.inflight.message_id == 301 && controller.pending[0].message_id == 302 &&
        read_ack_controller_complete(&controller, ticket, ReadAckCompletion::success) == ReadAckCompletionResult::completed &&
        !controller.inflight_active && controller.count == 1 && controller.pending[0].message_id == 302 &&
        read_ack_controller_peek_due(controller, 3100, &intent),
        "old successful response cannot drop a newer desired cursor queued during request");
    check(read_ack_controller_admit(&controller, intent, 72, &ticket) &&
        read_ack_controller_complete(&controller, ticket, ReadAckCompletion::success) == ReadAckCompletionResult::completed &&
        !controller.count && !controller.inflight_active,
        "exact accepted target success removes its pending intent once");
    check(read_ack_controller_complete(&controller, ticket, ReadAckCompletion::success) == ReadAckCompletionResult::ignored,
        "duplicate completion cannot complete another request");
    const auto previous_ticket = ticket;
    check(read_ack_controller_queue(&controller, desired(30, 302, 3, 4), 3200, true) &&
        read_ack_controller_peek_due(controller, 3200, &intent) &&
        read_ack_controller_admit(&controller, intent, previous_ticket.request_id, &ticket) &&
        ticket.generation != previous_ticket.generation &&
        read_ack_controller_complete(&controller, previous_ticket, ReadAckCompletion::success) == ReadAckCompletionResult::ignored &&
        controller.inflight_active && controller.count == 1 &&
        read_ack_controller_complete(&controller, ticket, ReadAckCompletion::success) == ReadAckCompletionResult::completed,
        "request ticket generation rejects old responses when transport request IDs are reused");

    check(read_ack_controller_queue(&controller, desired(30, 303), 4000, true) &&
        read_ack_controller_peek_due(controller, 4000, &intent) &&
        read_ack_controller_admit(&controller, intent, 80, &ticket) &&
        read_ack_controller_cancel(&controller, 30) && controller.inflight_superseded && !controller.count &&
        read_ack_controller_queue(&controller, desired(30, 303), 4001, true) &&
        read_ack_controller_complete(&controller, ticket, ReadAckCompletion::terminal) == ReadAckCompletionResult::superseded &&
        controller.count == 1 && controller.pending[0].message_id == 303,
        "manual cancel supersedes inflight response even when identical channel and message are queued again");
    check(read_ack_controller_peek_due(controller, 4001, &intent) &&
        read_ack_controller_admit(&controller, intent, 81, &ticket),
        "fresh desired work admits after superseded transport response drains");
    const auto stale_ticket = ticket;
    const u32 old_generation = controller.generation;
    const u32 old_backed = controller.backed_count;
    read_ack_controller_reset(&controller);
    check(controller.generation != old_generation && controller.backed_count == old_backed &&
        !controller.count && !controller.inflight_active && !controller.retry_not_before_ms &&
        read_ack_controller_queue(&controller, desired(30, 303), 4002, true) &&
        read_ack_controller_peek_due(controller, 4002, &intent) &&
        read_ack_controller_admit(&controller, intent, stale_ticket.request_id, &ticket) &&
        read_ack_controller_complete(&controller, stale_ticket, ReadAckCompletion::success) == ReadAckCompletionResult::ignored &&
        controller.inflight_active && controller.count == 1,
        "fresh READY reset rejects late completion despite reused request channel and message IDs");
    check(read_ack_controller_complete(&controller, ticket, ReadAckCompletion::terminal) == ReadAckCompletionResult::completed &&
        !controller.count, "terminal failure removes only the exact accepted target");

    check(read_ack_controller_queue(&controller, desired(3, 31), 5000, true) &&
        read_ack_controller_queue(&controller, desired(1, 11), 5000, false) &&
        read_ack_controller_queue(&controller, desired(2, 21), 5000, true) &&
        controller.count == 3 && controller.pending[0].channel_id == 1 &&
        controller.pending[1].channel_id == 2 && controller.pending[2].channel_id == 3 &&
        read_ack_controller_peek_due(controller, 5000, &intent) && intent.channel_id == 2,
        "multiple channels retain sorted independent intents and deterministic due ties");
    check(read_ack_controller_admit(&controller, intent, 90, &ticket) &&
        read_ack_controller_queue(&controller, desired(2, 22), 5001, true) &&
        read_ack_controller_complete(&controller, ticket, ReadAckCompletion::terminal) == ReadAckCompletionResult::completed &&
        controller.count == 3 && controller.pending[1].message_id == 22,
        "terminal old response leaves a newer desired target and other channels intact");
    check(read_ack_controller_peek_due(controller, 5001, &intent) &&
        read_ack_controller_admit(&controller, intent, 91, &ticket), "retry target admits without deleting desired work");
    constexpr u64 long_retry = 5000ull + 6ull * 60 * 60 * 1000;
    check(read_ack_controller_complete(&controller, ticket, ReadAckCompletion::retry, long_retry) == ReadAckCompletionResult::retry_scheduled &&
        controller.count == 3 && !controller.inflight_active &&
        read_ack_controller_next_due(controller, &due) && due == long_retry &&
        !read_ack_controller_peek_due(controller, long_retry - 1, &intent),
        "long authoritative retry remains a full numeric deadline without a short policy clamp");
    check(read_ack_controller_queue(&controller, desired(2, 23), 5002, true) &&
        read_ack_controller_queue(&controller, desired(4, 41), 5002, true) &&
        read_ack_controller_next_due(controller, &due) && due == long_retry &&
        !read_ack_controller_peek_due(controller, 5002, &intent) &&
        read_ack_controller_peek_due(controller, long_retry, &intent),
        "latest immediate work and other channels cannot shorten an authoritative retry admission floor");
    check(read_ack_controller_cancel(&controller, 2) && controller.count == 3 &&
        !read_ack_controller_cancel(&controller, 2) && !read_ack_controller_cancel(&controller, 0),
        "cancelling unsent work removes its channel exactly once");
    read_ack_controller_reset(&controller);
    check(read_ack_controller_queue(&controller, desired(1, 10), ~0ull - 10, false) &&
        read_ack_controller_next_due(controller, &due) && due == ~0ull,
        "automatic deadlines saturate rather than wrap into immediate sends");

    read_ack_controller_reset(&controller);
    check(read_ack_controller_queue(&controller, desired(1, 10), 1000, true) &&
        read_ack_controller_peek_due(controller, 1000, &intent) &&
        read_ack_controller_admit(&controller, intent, 120, &ticket) &&
        read_ack_controller_cancel(&controller, 1) &&
        read_ack_controller_queue(&controller, desired(1, 11), 1001, true) &&
        read_ack_controller_queue(&controller, desired(2, 21), 1001, true),
        "cancelled rate-limit fixture retains fresh same-channel and other-channel immediate work");
    const auto fresh_after_cancel = controller.pending[0];
    const u64 cancelled_retry = 1000ull + 6ull * 60 * 60 * 1000;
    check(read_ack_controller_complete(&controller, ticket, ReadAckCompletion::retry, cancelled_retry) ==
            ReadAckCompletionResult::superseded && controller.count == 2 &&
        bytes_equal(&fresh_after_cancel, &controller.pending[0], sizeof(fresh_after_cancel)) &&
        read_ack_controller_next_due(controller, &due) && due == cancelled_retry &&
        !read_ack_controller_peek_due(controller, cancelled_retry - 1, &intent) &&
        read_ack_controller_queue(&controller, desired(3, 31), 1002, true) &&
        read_ack_controller_next_due(controller, &due) && due == cancelled_retry &&
        read_ack_controller_peek_due(controller, cancelled_retry, &intent),
        "cancelled inflight 429 preserves six-hour admission floor without changing new desired target");
    read_ack_controller_reset(&controller);
    check(read_ack_controller_queue(&controller, desired(1, 10), 1000, true) &&
        read_ack_controller_peek_due(controller, 1000, &intent) &&
        read_ack_controller_admit(&controller, intent, 121, &ticket) &&
        read_ack_controller_cancel(&controller, 1) && !controller.count &&
        read_ack_controller_complete(&controller, ticket, ReadAckCompletion::retry, cancelled_retry) ==
            ReadAckCompletionResult::superseded && !controller.count &&
        read_ack_controller_queue(&controller, desired(2, 20), 1003, true) &&
        read_ack_controller_next_due(controller, &due) && due == cancelled_retry,
        "late 429 with no pending target still gates future immediate work by authoritative deadline");

    ReadAckIntent reserved_slots[2]{};
    ReadAckController lazy{}; ReserveProbe probe{0, 0, 1}; u32 local_mutations = 0;
    check(read_ack_controller_initialize(&lazy, {reserved_slots, 2}) &&
        read_ack_controller_set_reserve(&lazy, reserve, &probe) && !lazy.backed_count &&
        queue_and_mutate(&lazy, desired(2, 20), &local_mutations) &&
        lazy.count == 1 && lazy.backed_count == 1 && probe.calls == 1 && local_mutations == 1,
        "lazy backing commits only the required stable prefix before optimistic local mutation");
    const auto before_failed_reserve = lazy;
    check(!queue_and_mutate(&lazy, desired(1, 10), &local_mutations) && local_mutations == 1 &&
        bytes_equal(&lazy, &before_failed_reserve, sizeof(lazy)) && probe.calls == 2 && probe.required == 2 &&
        reserved_slots[0].channel_id == 2 && reserved_slots[0].message_id == 20,
        "reserve failure preserves queue and blocks the caller local-mutation hook");
    probe.fail_above = 0;
    check(queue_and_mutate(&lazy, desired(1, 10), &local_mutations) && lazy.count == 2 && lazy.backed_count == 2 &&
        reserved_slots[0].channel_id == 1 && reserved_slots[1].channel_id == 2 &&
        !queue_and_mutate(&lazy, desired(3, 30), &local_mutations) && local_mutations == 2 && lazy.count == 2 &&
        !read_ack_controller_set_reserve(&lazy, reserve, &probe),
        "successful growth keeps sorted IDs while fixed capacity fails before mutation");
    const u32 reserve_calls = probe.calls;
    read_ack_controller_reset(&lazy);
    check(queue_and_mutate(&lazy, desired(1, 10), &local_mutations) && probe.calls == reserve_calls && lazy.backed_count == 2,
        "reset reuses committed caller backing without another allocation or reserve call");
    ReadAckController maximum_queue{};
    check(read_ack_controller_initialize(&maximum_queue, {all_intents, kReadAckMaximumIntents}),
        "maximum queue needs one caller-owned intent bank");
    bool filled = true;
    for (u32 index = 1; index <= kReadAckMaximumIntents; ++index)
        filled = read_ack_controller_queue(&maximum_queue, desired(index, index + 1), 0, false) && filled;
    check(filled && maximum_queue.count == kReadAckMaximumIntents &&
        !read_ack_controller_queue(&maximum_queue, desired(kReadAckMaximumIntents + 1, 99), 0, true) &&
        read_ack_controller_queue(&maximum_queue, desired(1, 999), 1, false) && maximum_queue.count == kReadAckMaximumIntents,
        "4096 intent capacity rejects growth but still coalesces existing channels");
}
void test_read_ack_visibility(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr ReadAckVisibility visible{true, true, true, true, false, false, false, false, false, false};
    static_assert(read_ack_automatic_eligible(visible));
    check(read_ack_automatic_eligible(visible),
        "automatic ACK: foreground visible live history following bottom admits newly caught-up metadata");
    bool ReadAckVisibility::* const required[]{&ReadAckVisibility::foreground, &ReadAckVisibility::chat_visible,
        &ReadAckVisibility::following_bottom, &ReadAckVisibility::current_history};
    for (const auto gate : required) {
        auto view = visible;
        view.*gate = false;
        check(!read_ack_automatic_eligible(view),
            "automatic ACK: foreground, visible chat, bottom and current history are independently required");
    }
    bool ReadAckVisibility::* const blockers[]{&ReadAckVisibility::loading_more, &ReadAckVisibility::newer_history_required,
        &ReadAckVisibility::catchup_required, &ReadAckVisibility::resync_required,
        &ReadAckVisibility::idle, &ReadAckVisibility::manual_suppressed};
    for (const auto gate : blockers) {
        auto view = visible;
        view.*gate = true;
        check(!read_ack_automatic_eligible(view),
            "automatic ACK: loading, stale history, catchup, resync, idle and manual suppression independently block clearing");
    }
    auto history = visible;
    history.current_history = false;
    history.newer_history_required = true;
    history.catchup_required = true;
    check(!read_ack_automatic_eligible(history),
        "automatic ACK: bottom of a partial resident page is unread until live history catches up");
    auto manual = visible;
    manual.manual_suppressed = true;
    manual.idle = true;
    check(!read_ack_automatic_eligible(manual),
        "automatic ACK: idle manually unread channel stays unread while selected");
}
} // namespace maicord
