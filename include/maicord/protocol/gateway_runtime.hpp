#pragma once

#include "maicord/protocol/gateway_lifecycle.hpp"

namespace maicord {

inline constexpr u32 kGatewaySessionIdCapacity = 256;
inline constexpr u32 kGatewayResumeUrlCapacity = 2048;
inline constexpr u32 kGatewaySendLimit = 120;
inline constexpr u64 kGatewaySendWindowMs = 60'000;
inline constexpr u32 kGatewayOutboundActionCapacity = 8;

struct GatewayResumeState {
    char session_id[kGatewaySessionIdCapacity];
    char resume_url[kGatewayResumeUrlCapacity];
    u64 sequence;
    u32 session_id_bytes;
    u32 resume_url_bytes;
    bool valid;
};

enum class GatewayResumeCaptureError : u8 {
    none,
    invalid_input,
    session_id_too_large,
    resume_url_too_large,
    invalid_string,
    invalid_url,
};

GatewayResumeCaptureError gateway_resume_capture(GatewayResumeState* state, const GatewayReady& ready,
                                                 u64 sequence) noexcept;
void gateway_resume_clear(GatewayResumeState* state) noexcept;
bool gateway_resume_validate(const GatewayResumeState& state) noexcept;

struct GatewayReconnectBackoff {
    u32 attempt;
};

struct GatewayReconnectDelay {
    u32 delay_ms;
};

// Full-jitter exponential backoff: random value in [0, ceiling], with a 1 s
// base and 30 s cap. The state is reset after READY/RESUMED.
GatewayReconnectDelay gateway_reconnect_next(GatewayReconnectBackoff* backoff, u32 random) noexcept;
void gateway_reconnect_reset(GatewayReconnectBackoff* backoff) noexcept;
u32 gateway_invalid_session_delay(u32 random) noexcept;
// Floor before re-identifying after the n-th consecutive READY that could not
// be projected: 5 s doubling to a 10 min cap (at most ~150 IDENTIFYs a day,
// well inside the server's 1000/day session-start budget). Zero for n == 0.
u32 gateway_ready_failure_delay(u32 failures) noexcept;

struct GatewayRateLimiter {
    u64 timestamps[kGatewaySendLimit];
    u32 head;
    u32 count;
};

struct GatewayRateLimitStep {
    u64 next_allowed_ms;
    bool acquired;
};

GatewayRateLimitStep gateway_rate_limit_acquire(GatewayRateLimiter* limiter, u64 now_ms) noexcept;

struct GatewayOutboundQueue {
    GatewayRateLimiter limiter;
    GatewayLifecycleAction actions[kGatewayOutboundActionCapacity];
    u32 head;
    u32 count;
};

enum class GatewayOutboundState : u8 {
    idle,
    action,
    rate_limited,
    invalid,
};

struct GatewayOutboundStep {
    u64 next_allowed_ms;
    GatewayLifecycleAction action;
    GatewayOutboundState state;
};

bool gateway_outbound_enqueue(GatewayOutboundQueue* queue,
                              GatewayLifecycleAction action) noexcept;
GatewayOutboundStep gateway_outbound_take(GatewayOutboundQueue* queue, u64 now_ms) noexcept;
void gateway_outbound_reset(GatewayOutboundQueue* queue) noexcept;

} // namespace maicord
