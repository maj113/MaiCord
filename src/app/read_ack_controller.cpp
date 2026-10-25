#include "maicord/app/read_ack_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
namespace {
bool valid(const ReadAckController& controller) noexcept {
    return controller.pending.data && controller.pending.count &&
        controller.pending.count <= kReadAckMaximumIntents &&
        controller.count <= controller.backed_count && controller.backed_count <= controller.pending.count &&
        controller.generation != 0;
}
bool valid_intent(const ReadAckIntent& intent) noexcept {
    return intent.channel_id && (intent.kind == ReadAckKind::manual ||
        (intent.kind == ReadAckKind::automatic && intent.message_id &&
         !intent.mention_count_known && !intent.mention_count));
}
bool same_target(const ReadAckIntent& a, const ReadAckIntent& b) noexcept {
    return a.channel_id == b.channel_id && a.message_id == b.message_id &&
        a.last_viewed == b.last_viewed && a.flags == b.flags && a.kind == b.kind &&
        a.mention_count_known == b.mention_count_known && a.mention_count == b.mention_count;
}
u32 lower_bound(const ReadAckController& controller, u64 channel_id) noexcept {
    u32 first = 0, last = controller.count;
    while (first < last) {
        const u32 middle = first + (last - first) / 2;
        if (controller.pending[middle].channel_id < channel_id) first = middle + 1;
        else last = middle;
    }
    return first;
}
void erase(ReadAckController* controller, u32 position) noexcept {
    for (u32 index = position + 1; index < controller->count; ++index)
        controller->pending[index - 1] = controller->pending[index];
    --controller->count;
}
void clear_inflight(ReadAckController* controller) noexcept {
    reset_value(&controller->inflight); controller->request_id = 0;
    controller->inflight_active = false; controller->inflight_superseded = false;
}
bool find_due(const ReadAckController& controller, u32* index, u64* due) noexcept {
    if (!valid(controller) || controller.inflight_active || !controller.count) return false;
    u32 chosen = 0;
    for (u32 candidate = 1; candidate < controller.count; ++candidate)
        if (controller.pending[candidate].due_at_ms < controller.pending[chosen].due_at_ms) chosen = candidate;
    *index = chosen;
    *due = maximum(controller.pending[chosen].due_at_ms, controller.retry_not_before_ms);
    return true;
}
} // namespace

bool read_ack_controller_initialize(ReadAckController* controller,
    Slice<ReadAckIntent> pending) noexcept {
    if (!controller || !pending.data || !pending.count || pending.count > kReadAckMaximumIntents) return false;
    reset_value(controller); controller->pending = pending; controller->backed_count = pending.count;
    controller->generation = 1; return true;
}
bool read_ack_controller_set_reserve(ReadAckController* controller,
    ReadAckReserve reserve, void* context) noexcept {
    if (!controller || !valid(*controller) || controller->count || controller->inflight_active) return false;
    controller->reserve = reserve; controller->reserve_context = context;
    controller->backed_count = reserve ? 0 : controller->pending.count;
    return true;
}
bool read_ack_controller_queue(ReadAckController* controller,
    const ReadAckIntent& desired, u64 now_ms, bool immediate) noexcept {
    if (!controller || !valid(*controller) || !valid_intent(desired)) return false;
    ReadAckIntent queued = desired;
    queued.due_at_ms = deadline_after(now_ms, immediate || desired.kind == ReadAckKind::manual ? 0u : kReadAckAutomaticDelayMs);
    const bool supersede = desired.kind == ReadAckKind::manual && controller->inflight_active &&
        controller->inflight.channel_id == desired.channel_id && !controller->inflight_superseded;
    const u32 position = lower_bound(*controller, desired.channel_id);
    if (position < controller->count && controller->pending[position].channel_id == desired.channel_id) {
        const auto old = controller->pending[position];
        queued.due_at_ms = minimum(old.due_at_ms, queued.due_at_ms);
        if (same_target(old, queued) && old.due_at_ms == queued.due_at_ms) {
            if (supersede) { controller->inflight_superseded = true; saturating_increment(controller->revision); }
            return true;
        }
        controller->pending[position] = queued;
    } else {
        const u32 required = controller->count + 1;
        if (required > controller->pending.count || (required > controller->backed_count &&
            (!controller->reserve || !controller->reserve(controller->reserve_context, required)))) return false;
        controller->backed_count = maximum(controller->backed_count, required);
        for (u32 index = controller->count; index > position; --index)
            controller->pending[index] = controller->pending[index - 1];
        controller->pending[position] = queued; ++controller->count;
    }
    if (supersede) controller->inflight_superseded = true;
    saturating_increment(controller->revision); return true;
}
bool read_ack_controller_next_due(const ReadAckController& controller, u64* due_at_ms) noexcept {
    if (!due_at_ms) return false;
    u32 index = 0; u64 due = 0;
    if (!find_due(controller, &index, &due)) return false;
    *due_at_ms = due; return true;
}
bool read_ack_controller_peek_due(const ReadAckController& controller,
    u64 now_ms, ReadAckIntent* intent) noexcept {
    if (!intent) return false;
    u32 index = 0; u64 due = 0;
    if (!find_due(controller, &index, &due) || now_ms < due) return false;
    *intent = controller.pending[index]; return true;
}
bool read_ack_controller_admit(ReadAckController* controller,
    const ReadAckIntent& intent, u64 request_id, ReadAckRequestTicket* ticket) noexcept {
    if (!controller || !ticket || !valid(*controller) || !valid_intent(intent) || !request_id ||
        controller->inflight_active) return false;
    const u32 position = lower_bound(*controller, intent.channel_id);
    if (position == controller->count || !same_target(controller->pending[position], intent) ||
        controller->pending[position].due_at_ms != intent.due_at_ms) return false;
    controller->inflight = intent; controller->request_id = request_id;
    controller->inflight_active = true; controller->inflight_superseded = false;
    ++controller->generation; if (!controller->generation) ++controller->generation;
    *ticket = {request_id, controller->generation};
    saturating_increment(controller->revision); return true;
}
ReadAckCompletionResult read_ack_controller_complete(ReadAckController* controller,
    ReadAckRequestTicket ticket, ReadAckCompletion completion, u64 retry_due_at_ms) noexcept {
    if (!controller || !valid(*controller) || !controller->inflight_active || !ticket.request_id ||
        ticket.request_id != controller->request_id || ticket.generation != controller->generation ||
        (completion != ReadAckCompletion::success && completion != ReadAckCompletion::retry &&
         completion != ReadAckCompletion::terminal)) return ReadAckCompletionResult::ignored;
    const bool superseded = controller->inflight_superseded;
    const u32 position = lower_bound(*controller, controller->inflight.channel_id);
    const bool channel_pending = position < controller->count &&
        controller->pending[position].channel_id == controller->inflight.channel_id;
    // A transport admission floor belongs to the request/rate bucket, even
    // when its target was cancelled or replaced while the request was active.
    if (completion == ReadAckCompletion::retry)
        controller->retry_not_before_ms = maximum(controller->retry_not_before_ms, retry_due_at_ms);
    if (!superseded && channel_pending) {
        if (completion == ReadAckCompletion::retry) {
            // The transport-supplied admission floor also protects a newer
            // desired cursor from repeatedly bypassing an authoritative retry.
            controller->pending[position].due_at_ms = maximum(controller->pending[position].due_at_ms, retry_due_at_ms);
        } else if (same_target(controller->pending[position], controller->inflight)) erase(controller, position);
    }
    clear_inflight(controller); saturating_increment(controller->revision);
    if (superseded) return ReadAckCompletionResult::superseded;
    return completion == ReadAckCompletion::retry ? ReadAckCompletionResult::retry_scheduled : ReadAckCompletionResult::completed;
}
bool read_ack_controller_cancel(ReadAckController* controller, u64 channel_id) noexcept {
    if (!controller || !valid(*controller) || !channel_id) return false;
    bool changed = false;
    const u32 position = lower_bound(*controller, channel_id);
    if (position < controller->count && controller->pending[position].channel_id == channel_id) {
        erase(controller, position); changed = true;
    }
    if (controller->inflight_active && controller->inflight.channel_id == channel_id && !controller->inflight_superseded) {
        controller->inflight_superseded = true; changed = true;
    }
    if (changed) saturating_increment(controller->revision);
    return changed;
}
void read_ack_controller_reset(ReadAckController* controller) noexcept {
    if (!controller || !valid(*controller)) return;
    controller->count = 0; controller->retry_not_before_ms = 0; clear_inflight(controller);
    ++controller->generation; if (!controller->generation) ++controller->generation;
    saturating_increment(controller->revision);
}
} // namespace maicord
