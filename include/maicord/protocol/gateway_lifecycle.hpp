#pragma once

#include "maicord/protocol/gateway_events.hpp"

namespace maicord {

enum class GatewayLifecyclePhase : u8 {
    idle,
    awaiting_hello,
    awaiting_session,
    ready,
    reconnecting,
};

enum class GatewayLifecycleAction : u8 {
    none,
    send_identify,
    send_resume,
    send_heartbeat,
    reconnect_identify,
    reconnect_resume,
    stop,
    // Opcode 41 for the current analytics heartbeat session.
    send_time_spent_session,
};

enum class GatewayLifecycleError : u8 {
    none,
    invalid_input,
    invalid_state,
    output_too_small,
};

struct GatewayLifecycle {
    u64 last_sequence;
    u64 next_heartbeat_ms;
    u32 heartbeat_interval_ms;
    GatewayLifecyclePhase phase;
    bool has_sequence;
    bool resume_available;
    bool attempting_resume;
    bool awaiting_heartbeat_ack;
};

struct GatewayLifecycleStep {
    GatewayLifecycleAction action;
    GatewayLifecycleError error;

    constexpr bool ok() const noexcept {
        return error == GatewayLifecycleError::none;
    }
};

struct GatewayWriteResult {
    u32 bytes;
    GatewayLifecycleError error;

    constexpr bool ok() const noexcept {
        return error == GatewayLifecycleError::none;
    }
};

enum class GatewayCloseDisposition : u8 {
    stop,
    resume,
    reidentify,
};

void gateway_lifecycle_initialize(GatewayLifecycle* lifecycle) noexcept;
GatewayLifecycleStep gateway_lifecycle_connected(GatewayLifecycle* lifecycle,
                                                 bool resume_available) noexcept;
GatewayLifecycleStep gateway_lifecycle_hello(GatewayLifecycle* lifecycle, GatewayHello hello, u32 jitter,
                                             u64 now_ms) noexcept;
GatewayLifecycleStep gateway_lifecycle_tick(GatewayLifecycle* lifecycle, u64 now_ms) noexcept;
GatewayLifecycleStep gateway_lifecycle_heartbeat_requested(GatewayLifecycle* lifecycle) noexcept;
GatewayLifecycleStep gateway_lifecycle_heartbeat_acknowledged(GatewayLifecycle* lifecycle) noexcept;
GatewayLifecycleStep gateway_lifecycle_dispatch(GatewayLifecycle* lifecycle, u64 sequence) noexcept;
GatewayLifecycleStep gateway_lifecycle_session_ready(GatewayLifecycle* lifecycle,
                                                     bool resume_available) noexcept;
GatewayLifecycleStep gateway_lifecycle_reconnect_requested(GatewayLifecycle* lifecycle) noexcept;
GatewayLifecycleStep gateway_lifecycle_invalid_session(GatewayLifecycle* lifecycle,
                                                       bool resumable) noexcept;
GatewayCloseDisposition gateway_classify_close(u16 close_code) noexcept;
GatewayLifecycleStep gateway_lifecycle_closed(GatewayLifecycle* lifecycle, u16 close_code) noexcept;

// The activity a QoS heartbeat reports: reasons are gateway_qos_* bits, sent
// in name order, and the client is active while it has any.
inline constexpr u8 gateway_qos_foregrounded = 1u << 0;
inline constexpr u8 gateway_qos_rtc_connected = 1u << 1;
struct GatewayQosPayload {
    u8 reasons;
    bool active;
    constexpr bool operator==(const GatewayQosPayload&) const noexcept = default;
};
constexpr GatewayQosPayload gateway_qos_payload(u8 reasons) noexcept {
    return {reasons, reasons != 0};
}
// The desktop client's heartbeat QoS state: the next heartbeat reports
// current, which then gives way to upcoming. Becoming active merges into
// current, so a heartbeat reports activity anywhere in its interval. A
// heartbeat sent outside the interval takes its payload when it is queued
// (immediate).
struct GatewayQosState {
    GatewayQosPayload current, upcoming, immediate;
    bool has_current, has_upcoming, has_immediate;
};
// A new activity state. True when the client just became active in an
// established (or resuming) session: a heartbeat is then due at once.
bool gateway_qos_update(GatewayQosState* state, GatewayQosPayload next, bool session_established) noexcept;
// Takes the payload of a heartbeat queued now (see gateway_qos_take).
void gateway_qos_send_now(GatewayQosState* state) noexcept;
// The payload of the heartbeat being written; false when there is none.
bool gateway_qos_take(GatewayQosState* state, GatewayQosPayload* payload) noexcept;

// Opcode 40: {"op":40,"d":{"seq":S,"qos":{...}}}, with sequence 0 before any
// event and a null qos without a payload.
GatewayWriteResult gateway_write_heartbeat(const GatewayLifecycle& lifecycle, const GatewayQosPayload* qos,
                                           Slice<u8> output) noexcept;

} // namespace maicord
