#include "maicord/app/bulk_read_ack_controller.hpp"
#include "maicord/core/memory.hpp"
namespace maicord {
namespace {
BulkReadAckIntent bulk_slots[4096], bulk_flight[100], bulk_batch[100], bulk_entries[201];
struct BulkReserveProbe {
    u32 pending_calls, flight_calls, fail_above;
};
bool bulk_pending_reserve(void* opaque, u32 count) noexcept {
    auto& p = *static_cast<BulkReserveProbe*>(opaque);
    ++p.pending_calls;
    return !p.fail_above || count <= p.fail_above;
}
bool bulk_flight_reserve(void* opaque, u32) noexcept {
    ++static_cast<BulkReserveProbe*>(opaque)->flight_calls;
    return true;
}
BulkReadAckIntent bulk_entry(u64 id, u64 serial = 1, u64 cursor = 100) noexcept {
    return {id, cursor, serial, 7, 0, 0, false};
}
} // namespace
void test_bulk_read_ack_controller(void (*check)(bool, const char*) noexcept) noexcept {
    BulkReadAckController c{};
    BulkReadAckTicket ticket{};
    u32 count{};
    check(!bulk_read_ack_initialize(nullptr, {bulk_slots, 4096}, {bulk_flight, 100}, nullptr, nullptr,
                                    nullptr) &&
              bulk_read_ack_initialize(&c, {bulk_slots, 4096}, {bulk_flight, 100}, nullptr, nullptr,
                                       nullptr) &&
              !bulk_read_ack_next_wake(c),
          "bulk read: caller-owned empty controller has no wake");
    for (u32 i = 0; i < 201; ++i)
        bulk_entries[i] = bulk_entry(1000 + i);
    check(bulk_read_ack_queue(&c, {bulk_entries, 201}) && c.count == 201 &&
              bulk_read_ack_next_wake(c) == 1 && bulk_read_ack_peek(&c, 1, {bulk_batch, 100}, &count) &&
              count == 100 && bulk_read_ack_admit(&c, {bulk_batch, count}, 42, &ticket) &&
              !bulk_read_ack_next_wake(c),
          "bulk read: 201 targets admit one immutable batch of exactly 100");
    const auto immutable = bulk_flight[0];
    auto newer = bulk_entry(1000, 2, 100);
    check(bulk_read_ack_queue(&c, {&newer, 1}) &&
              bytes_equal(&immutable, &bulk_flight[0], sizeof(immutable)) &&
              !bulk_read_ack_complete(&c, {41, ticket.generation}, true, false, 50, 0) && c.inflight &&
              bulk_read_ack_complete(&c, ticket, true, false, 50, 0) && c.count == 102 &&
              c.pending[0].action_serial == 2 && bulk_read_ack_next_wake(c) == 1050,
          "bulk read: same cursor with newer serial survives old completion and success starts 1s floor");
    check(!bulk_read_ack_peek(&c, 1049, {bulk_batch, 100}, &count) &&
              bulk_read_ack_peek(&c, 1050, {bulk_batch, 100}, &count) && count == 100 &&
              bulk_read_ack_admit(&c, {bulk_batch, count}, 43, &ticket),
          "bulk read: exact success spacing admits the second batch");
    bulk_read_ack_floor(&c, 10000);
    check(bulk_read_ack_complete(&c, ticket, false, true, 1100, 3100) &&
              bulk_read_ack_next_wake(c) == 10000,
          "bulk read: retained 429 floor dominates transient retry and all remaining batches");
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        check(bulk_read_ack_peek(&c, 10000 + attempt * 4000, {bulk_batch, 100}, &count) &&
                  bulk_read_ack_admit(&c, {bulk_batch, count}, 44 + attempt, &ticket) &&
                  bulk_read_ack_complete(&c, ticket, false, true, 10000 + attempt * 4000,
                                         14000 + attempt * 4000),
              "bulk read: admitted retries retain original boundaries");
    }
    u32 failed{};
    for (u32 i = 0; i < c.count; ++i)
        if (c.pending[i].failed)
            ++failed;
    check(failed == 100 && c.count == 102,
          "bulk read: third failure keeps only current failed targets for explicit retry");
    bulk_read_ack_reset(&c);
    BulkReserveProbe reserve{0, 0, 100};
    check(bulk_read_ack_initialize(&c, {bulk_slots, 4096}, {bulk_flight, 100}, bulk_pending_reserve,
                                   bulk_flight_reserve, &reserve),
          "bulk read: demand backing initializes without commits");
    const u32 old_revision = c.revision;
    check(!bulk_read_ack_queue(&c, {bulk_entries, 101}) && c.count == 0 && c.revision == old_revision &&
              !c.inflight && !bulk_read_ack_next_wake(c),
          "bulk read: failed 101-prefix reserve has no logical partial plan");
    reserve.fail_above = 0;
    check(bulk_read_ack_queue(&c, {bulk_entries, 101}) && c.count == 101 &&
              bulk_read_ack_peek(&c, 1, {bulk_batch, 100}, &count) &&
              bulk_read_ack_admit(&c, {bulk_batch, count}, 70, &ticket),
          "bulk read: 101 reserves whole queue and immutable 100 prefix before admission");
    const auto saved = bulk_flight[0];
    check(bulk_read_ack_cancel(&c, 1000, 1) && bytes_equal(&saved, &bulk_flight[0], sizeof(saved)) &&
              c.count == 100 && bulk_read_ack_complete(&c, ticket, true, false, 1, 0) && c.count == 1 &&
              c.pending[0].channel_id == 1100,
          "bulk read: newer unread cancellation preserves admitted evidence and the final one target");
    auto invalid = bulk_entry(9999);
    invalid.message_id = 0;
    const auto before = c;
    check(!bulk_read_ack_queue(&c, {&invalid, 1}) && c.count == before.count &&
              c.revision == before.revision,
          "bulk read: zero cursor never becomes outgoing bulk acknowledgement");
    const u32 calls = reserve.pending_calls + reserve.flight_calls, revision = c.revision;
    for (u32 warm = 0; warm < 64; ++warm)
        check(bulk_read_ack_next_wake(c) == 1001, "bulk read: warm wake stays stable");
    check(c.revision == revision && reserve.pending_calls + reserve.flight_calls == calls,
          "bulk read: 64 warm deadline reads perform no reserve or publication");
    count = 777;
    check(!bulk_read_ack_queue(&c, {c.pending.data, c.count}) &&
              !bulk_read_ack_peek(&c, 2000, {c.pending.data, 100}, &count) && count == 777,
          "bulk read: overlapping input/output aliases fail before queue or output mutation");
    bulk_read_ack_reset(&c, true);
    check(!c.count && !c.inflight && !bulk_read_ack_next_wake(c) && c.not_before_ms == 1001,
          "bulk read: READY reset retires logical work while optionally retaining actor cooldown");
}
} // namespace maicord
