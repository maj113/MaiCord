#pragma once
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/spsc_mailbox.hpp"
#include "maicord/core/win32_types.hpp"
namespace maicord {
using GatewayClientMailbox = SpscMailbox<GatewayClientEvent, kGatewayClientEventCapacity, kGatewayClientDispatchPayloadCapacity>;
// Production producer waits only at full-mailbox boundaries. Consumer signals
// space after advancing ownership; cancellation outranks simultaneous space.
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
bool gateway_mailbox_wait_lease(HANDLE space, HANDLE cancel, HANDLE notification, u64 deadline_ms,
    const volatile LONG* lease) noexcept;
GatewayClientDispatchRouteResult gateway_route_relationship(const GatewayEnvelope& envelope,
    Slice<u32> scratch, Slice<char> output, GatewayClientEvent* event,
    ConstSlice<u8>* payload, bool* handled) noexcept;
GatewayClientDispatchRouteResult gateway_relationship_merged_presences(JsonValue data,
    Slice<u32> scratch, JsonValue* merged) noexcept;
} // namespace maicord
