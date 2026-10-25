#pragma once
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/spsc_mailbox.hpp"
#include "maicord/core/win32_types.hpp"
namespace maicord {
struct GatewayClientMailbox
    : SpscMailbox<GatewayClientEvent, kGatewayClientEventCapacity, kGatewayClientDispatchPayloadCapacity> {
    // Set by the producer before it re-checks for room and blocks; cleared by whoever wakes or ends that
    // wait. The consumer's interlocked clear follows its ownership store, so either the producer's
    // re-check sees the freed room or the consumer sees the flag: a wake cannot be lost.
    volatile LONG space_waiting;
};
// Production producer waits only at full-mailbox boundaries. Consumer signals
// space after advancing ownership (gateway_mailbox_signal_space); cancellation
// outranks simultaneous space.
// With `deferred`, a successful push only sets *deferred: the producer signals
// `notification` once for its whole batch (before it blocks) instead of
// waking the consumer per record. A full mailbox still signals before waiting.
bool gateway_mailbox_publish(GatewayClientMailbox* mailbox, HANDLE space, HANDLE cancel,
    HANDLE notification, u64 deadline_ms, const GatewayClientEvent& event, ConstSlice<u8> payload,
    bool* deferred = nullptr) noexcept;
// An oversized record (beyond the ring) stays in the producer's large storage.
// `lease` becomes 1 before the descriptor is visible; the consumer copies the
// payload out and clears it. Same waits and cancellation as above.
bool gateway_mailbox_publish_external(GatewayClientMailbox* mailbox, HANDLE space, HANDLE cancel,
    HANDLE notification, u64 deadline_ms, const GatewayClientEvent& event, u32 payload_bytes,
    volatile LONG* lease, bool* deferred = nullptr) noexcept;
// Waits until the consumer has released the previous oversized record.
bool gateway_mailbox_wait_lease(GatewayClientMailbox* mailbox, HANDLE space, HANDLE cancel, HANDLE notification,
    u64 deadline_ms, const volatile LONG* lease) noexcept;
// Consumer side, after it advanced read ownership or released the lease: signals `space` only when
// the producer announced a wait, so a drain costs no kernel call per record.
void gateway_mailbox_signal_space(GatewayClientMailbox* mailbox, HANDLE space) noexcept;
GatewayClientDispatchRouteResult gateway_route_relationship(const GatewayEnvelope& envelope,
    Slice<u32> scratch, Slice<char> output, GatewayClientEvent* event,
    ConstSlice<u8>* payload, bool* handled) noexcept;
GatewayClientDispatchRouteResult gateway_relationship_merged_presences(JsonValue data,
    Slice<u32> scratch, JsonValue* merged) noexcept;
} // namespace maicord
