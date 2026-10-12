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
    u64 next_deadline_ms;
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
GatewayWriteResult gateway_write_heartbeat(const GatewayLifecycle& lifecycle, Slice<u8> output) noexcept;

} // namespace maicord
