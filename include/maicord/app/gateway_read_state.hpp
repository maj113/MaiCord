#pragma once
#include "maicord/protocol/gateway_payload.hpp"

namespace maicord {
inline constexpr u32 kGatewayReadStateMaximumEntries = 4096;
enum GatewayClientReadStateField : u8 {
    gateway_read_state_cursor = 1u << 0,
    gateway_read_state_mentions = 1u << 1,
    gateway_read_state_last_viewed = 1u << 2,
    gateway_read_state_flags = 1u << 3,
    gateway_read_state_manual = 1u << 4,
};
// Pointer-free fixed record; snapshots pack ascending channel IDs. Unknown
// channel IDs remain meaningful and must reach the typed model consumer.
struct GatewayClientReadStateMutation {
    u64 channel_id;
    u64 message_id;
    u32 mention_count;
    u32 last_viewed;
    u32 flags;
    u8 fields;
    u8 reserved[3];
};
static_assert(sizeof(GatewayClientReadStateMutation) == 32);
struct GatewayClientReadStateEvent {
    GatewayClientReadStateMutation entry; // MESSAGE_ACK only; snapshots use payload records.
    u64 version;
    u32 record_count;
    bool partial;
    bool version_known;
};
static_assert(sizeof(GatewayClientReadStateEvent) <= 128);
struct GatewayClientEvent;
struct GatewayClientDispatchRouteResult;
// Metadata validates the complete root envelope, including duplicate/type
// guards, without decoding entries. Used before READY removes old channels.
GatewayClientDispatchRouteResult gateway_client_read_state_metadata(JsonValue data,
    Slice<u32> scratch, u32 maximum_depth, GatewayClientReadStateEvent* output) noexcept;
// No descriptor/payload is published on error. Caller-owned storage is staging
// scratch and may be touched; one complete validated snapshot is the output.
GatewayClientDispatchRouteResult gateway_client_route_read_state_snapshot(JsonValue data,
    Slice<u32> scratch, u32 maximum_depth, Slice<char> storage,
    GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept;
GatewayClientDispatchRouteResult gateway_client_route_message_ack(JsonValue data,
    Slice<u32> scratch, u32 maximum_depth, GatewayClientEvent* event,
    ConstSlice<u8>* payload) noexcept;
bool gateway_client_read_state_record(ConstSlice<u8> payload, u32 index,
    GatewayClientReadStateMutation* output) noexcept;
} // namespace maicord
