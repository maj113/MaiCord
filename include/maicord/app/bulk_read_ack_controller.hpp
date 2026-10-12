#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
inline constexpr u32 kBulkReadAckMaximumTargets = 4096;
inline constexpr u32 kBulkReadAckMaximumBatch = 100;
struct BulkReadAckIntent {
    u64 channel_id, message_id, action_serial, scope_serial, due_at_ms;
    u32 attempts;
    bool failed;
};
static_assert(sizeof(BulkReadAckIntent) == 48);
struct BulkReadAckTicket {
    u64 request_id;
    u32 generation;
};
using BulkReadAckReserve = bool (*)(void*, u32) noexcept;
struct BulkReadAckController {
    Slice<BulkReadAckIntent> pending, flight;
    BulkReadAckReserve reserve_pending, reserve_flight;
    void* context;
    u64 request_id, not_before_ms, next_due_ms;
    u32 count, flight_count, pending_backed_count, flight_backed_count, generation, revision;
    bool inflight;
};
bool bulk_read_ack_initialize(BulkReadAckController*, Slice<BulkReadAckIntent> pending,
                              Slice<BulkReadAckIntent> flight, BulkReadAckReserve pending_reserve,
                              BulkReadAckReserve flight_reserve, void* context) noexcept;
// Reserves the entire merged queue before changing any logical state.
bool bulk_read_ack_prepare(BulkReadAckController*, ConstSlice<BulkReadAckIntent>) noexcept;
bool bulk_read_ack_queue(BulkReadAckController*, ConstSlice<BulkReadAckIntent>) noexcept;
u64 bulk_read_ack_next_wake(const BulkReadAckController&) noexcept;
bool bulk_read_ack_peek(BulkReadAckController*, u64 now_ms, Slice<BulkReadAckIntent> output,
                        u32* count) noexcept;
bool bulk_read_ack_admit(BulkReadAckController*, ConstSlice<BulkReadAckIntent>, u64 request_id,
                         BulkReadAckTicket*) noexcept;
// Accepted flight storage stays immutable until exact completion. New intents
// replace only pending state; same-cursor different serials remain distinct.
bool bulk_read_ack_complete(BulkReadAckController*, BulkReadAckTicket, bool success, bool retry, u64 now_ms,
                            u64 retry_due_ms) noexcept;
void bulk_read_ack_floor(BulkReadAckController*, u64 not_before_ms) noexcept;
bool bulk_read_ack_cancel(BulkReadAckController*, u64 channel_id, u64 action_serial = 0) noexcept;
void bulk_read_ack_reset(BulkReadAckController*, bool retain_floor = false) noexcept;
} // namespace maicord
