#include "maicord/app/bulk_read_ack_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
namespace {
bool overlap(const void* a, usize an, const void* b, usize bn) noexcept {
    const auto av = reinterpret_cast<usize>(a), bv = reinterpret_cast<usize>(b);
    return an && bn && (av > ~usize{0} - an || bv > ~usize{0} - bn || (av < bv + bn && bv < av + an));
}
bool overlaps_storage(const BulkReadAckController& c, const BulkReadAckIntent* data, u32 count) noexcept {
    const usize bytes = usize{count} * sizeof(BulkReadAckIntent);
    return overlap(data, bytes, c.pending.data, usize{c.pending.count} * sizeof(BulkReadAckIntent)) ||
           overlap(data, bytes, c.flight.data, usize{c.flight.count} * sizeof(BulkReadAckIntent));
}
bool valid(const BulkReadAckController& c) noexcept {
    return c.pending.data && c.pending.count && c.pending.count <= kBulkReadAckMaximumTargets &&
           c.flight.data && c.flight.count >= kBulkReadAckMaximumBatch && c.count <= c.pending.count &&
           c.count <= c.pending_backed_count && c.flight_count <= c.flight_backed_count &&
           c.flight_count <= kBulkReadAckMaximumBatch && c.generation;
}
void refresh_due(BulkReadAckController& c) noexcept {
    c.next_due_ms = 0;
    for (u32 i = 0; i < c.count; ++i)
        if (!c.pending[i].failed)
            c.next_due_ms = earliest_deadline(c.next_due_ms, maximum(u64{1}, c.pending[i].due_at_ms));
}
bool intent_valid(const BulkReadAckIntent& e) noexcept {
    return e.channel_id && e.message_id && e.action_serial && e.scope_serial && e.attempts <= 3;
}
u32 position(const BulkReadAckController& c, u64 id) noexcept {
    u32 first{}, last = c.count;
    while (first < last) {
        const u32 middle = first + (last - first) / 2;
        if (c.pending[middle].channel_id < id)
            first = middle + 1;
        else
            last = middle;
    }
    return first;
}
bool same(const BulkReadAckIntent& a, const BulkReadAckIntent& b) noexcept {
    return a.channel_id == b.channel_id && a.message_id == b.message_id &&
           a.action_serial == b.action_serial && a.scope_serial == b.scope_serial;
}
void erase(BulkReadAckController& c, u32 at) noexcept {
    for (u32 i = at + 1; i < c.count; ++i)
        c.pending[i - 1] = c.pending[i];
    --c.count;
}
} // namespace
bool bulk_read_ack_initialize(BulkReadAckController* c, Slice<BulkReadAckIntent> pending,
                              Slice<BulkReadAckIntent> flight, BulkReadAckReserve pending_reserve,
                              BulkReadAckReserve flight_reserve, void* context) noexcept {
    if (!c || !pending.data || !pending.count || pending.count > kBulkReadAckMaximumTargets ||
        !flight.data || flight.count < kBulkReadAckMaximumBatch)
        return false;
    reset_value(c);
    c->pending = pending;
    c->flight = flight;
    c->reserve_pending = pending_reserve;
    c->reserve_flight = flight_reserve;
    c->context = context;
    c->pending_backed_count = pending_reserve ? 0 : pending.count;
    c->flight_backed_count = flight_reserve ? 0 : kBulkReadAckMaximumBatch;
    c->generation = 1;
    return true;
}
bool bulk_read_ack_prepare(BulkReadAckController* c, ConstSlice<BulkReadAckIntent> entries) noexcept {
    if (!c || !valid(*c) || (!entries.data && entries.count) || entries.count > c->pending.count)
        return false;
    if (overlaps_storage(*c, entries.data, entries.count))
        return false;
    u32 required = c->count;
    for (u32 i = 0; i < entries.count; ++i) {
        if (!intent_valid(entries[i]))
            return false;
        for (u32 j = 0; j < i; ++j)
            if (entries[j].channel_id == entries[i].channel_id)
                return false;
        const u32 at = position(*c, entries[i].channel_id);
        if (at == c->count || c->pending[at].channel_id != entries[i].channel_id)
            ++required;
    }
    const u32 required_flight = minimum(required, kBulkReadAckMaximumBatch);
    if (required > c->pending.count ||
        (required > c->pending_backed_count &&
         (!c->reserve_pending || !c->reserve_pending(c->context, required))) ||
        (required_flight > c->flight_backed_count &&
         (!c->reserve_flight || !c->reserve_flight(c->context, required_flight))))
        return false;
    c->pending_backed_count = maximum(c->pending_backed_count, required);
    c->flight_backed_count = maximum(c->flight_backed_count, required_flight);
    return true;
}
bool bulk_read_ack_queue(BulkReadAckController* c, ConstSlice<BulkReadAckIntent> entries) noexcept {
    if (!bulk_read_ack_prepare(c, entries))
        return false;
    for (u32 i = 0; i < entries.count; ++i) {
        const u32 at = position(*c, entries[i].channel_id);
        if (at == c->count || c->pending[at].channel_id != entries[i].channel_id) {
            for (u32 j = c->count; j > at; --j)
                c->pending[j] = c->pending[j - 1];
            ++c->count;
        }
        c->pending[at] = entries[i];
    }
    if (entries.count) {
        refresh_due(*c);
        saturating_increment(c->revision);
    }
    return true;
}
u64 bulk_read_ack_next_wake(const BulkReadAckController& c) noexcept {
    if (!valid(c) || c.inflight)
        return 0;
    return c.next_due_ms ? maximum(c.next_due_ms, c.not_before_ms) : 0;
}
bool bulk_read_ack_peek(BulkReadAckController* c, u64 now, Slice<BulkReadAckIntent> output,
                        u32* count) noexcept {
    if (!c || !count || !valid(*c) || c->inflight || now < c->not_before_ms || !output.data ||
        output.count < kBulkReadAckMaximumBatch ||
        overlaps_storage(*c, output.data, output.count))
        return false;
    u32 found{};
    for (u32 i = 0; i < c->count && found < kBulkReadAckMaximumBatch; ++i)
        if (!c->pending[i].failed && c->pending[i].due_at_ms <= now)
            output[found++] = c->pending[i];
    if (!found)
        return false;
    *count = found;
    return true;
}
bool bulk_read_ack_admit(BulkReadAckController* c, ConstSlice<BulkReadAckIntent> entries, u64 id,
                         BulkReadAckTicket* ticket) noexcept {
    if (!c || !ticket || !valid(*c) || c->inflight || !id || !entries.data || !entries.count ||
        entries.count > kBulkReadAckMaximumBatch || entries.count > c->flight_backed_count ||
        overlaps_storage(*c, entries.data, entries.count))
        return false;
    for (u32 i = 0; i < entries.count; ++i) {
        const u32 at = position(*c, entries[i].channel_id);
        if (!intent_valid(entries[i]) || at == c->count || !same(c->pending[at], entries[i]) ||
            c->pending[at].failed || c->pending[at].attempts >= 3 ||
            c->pending[at].attempts != entries[i].attempts ||
            c->pending[at].due_at_ms != entries[i].due_at_ms)
            return false;
        for (u32 j = 0; j < i; ++j)
            if (entries[j].channel_id == entries[i].channel_id)
                return false;
    }
    for (u32 i = 0; i < entries.count; ++i) {
        c->flight[i] = entries[i];
        ++c->flight[i].attempts;
        ++c->pending[position(*c, entries[i].channel_id)].attempts;
    }
    c->flight_count = entries.count;
    c->request_id = id;
    c->inflight = true;
    ++c->generation;
    if (!c->generation)
        ++c->generation;
    *ticket = {id, c->generation};
    saturating_increment(c->revision);
    return true;
}
bool bulk_read_ack_complete(BulkReadAckController* c, BulkReadAckTicket ticket, bool success, bool retry,
                            u64 now, u64 retry_due) noexcept {
    if (!c || !valid(*c) || !c->inflight || !ticket.request_id || ticket.request_id != c->request_id ||
        ticket.generation != c->generation || (success && retry))
        return false;
    if (success)
        c->not_before_ms = maximum(c->not_before_ms, deadline_after(now, 1000));
    for (u32 i = 0; i < c->flight_count; ++i) {
        const u32 at = position(*c, c->flight[i].channel_id);
        if (at == c->count || !same(c->pending[at], c->flight[i]))
            continue;
        if (success)
            erase(*c, at);
        else if (retry && c->pending[at].attempts < 3)
            c->pending[at].due_at_ms = retry_due;
        else
            c->pending[at].failed = true;
    }
    c->inflight = false;
    c->request_id = 0;
    c->flight_count = 0;
    refresh_due(*c);
    saturating_increment(c->revision);
    return true;
}
void bulk_read_ack_floor(BulkReadAckController* c, u64 floor) noexcept {
    if (c && valid(*c))
        c->not_before_ms = maximum(c->not_before_ms, floor);
}
bool bulk_read_ack_cancel(BulkReadAckController* c, u64 channel, u64 serial) noexcept {
    if (!c || !valid(*c) || !channel)
        return false;
    const u32 at = position(*c, channel);
    if (at == c->count || c->pending[at].channel_id != channel ||
        (serial && c->pending[at].action_serial != serial))
        return false;
    erase(*c, at);
    refresh_due(*c);
    saturating_increment(c->revision);
    return true;
}
void bulk_read_ack_reset(BulkReadAckController* c, bool retain_floor) noexcept {
    if (!c || !valid(*c))
        return;
    c->count = c->flight_count = 0;
    c->request_id = 0;
    c->inflight = false;
    c->next_due_ms = 0;
    if (!retain_floor)
        c->not_before_ms = 0;
    ++c->generation;
    if (!c->generation)
        ++c->generation;
    saturating_increment(c->revision);
}
} // namespace maicord
