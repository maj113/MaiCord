#include "maicord/protocol/gateway_runtime.hpp"
#include "maicord/core/deadline.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

u32 scale_inclusive(u32 random, u32 maximum_value) noexcept {
    return static_cast<u32>((static_cast<u64>(random) * (static_cast<u64>(maximum_value) + 1)) >> 32);
}

bool has_wss_scheme(const char* value, u32 bytes) noexcept {
    constexpr char scheme[] = "wss://";
    if (bytes <= sizeof(scheme) - 1) {
        return false;
    }
    for (u32 index = 0; index < sizeof(scheme) - 1; ++index) {
        if (value[index] != scheme[index]) {
            return false;
        }
    }
    return true;
}

} // namespace

GatewayResumeCaptureError gateway_resume_capture(GatewayResumeState* state, const GatewayReady& ready,
                                                 u64 sequence) noexcept {
    if (!ready.session_id.encoded.data || ready.session_id.encoded.bytes == 0 ||
        !ready.resume_gateway_url.encoded.data || ready.resume_gateway_url.encoded.bytes == 0) {
        return GatewayResumeCaptureError::invalid_input;
    }

    const JsonStringDecodeResult session_size =
        json_string_decode_utf8(ready.session_id, Slice<char>{nullptr, 0});
    const JsonStringDecodeResult url_size =
        json_string_decode_utf8(ready.resume_gateway_url, Slice<char>{nullptr, 0});
    if (session_size.error != JsonStringDecodeError::output_too_small ||
        url_size.error != JsonStringDecodeError::output_too_small) {
        return GatewayResumeCaptureError::invalid_string;
    }
    if (session_size.bytes >= kGatewaySessionIdCapacity) {
        return GatewayResumeCaptureError::session_id_too_large;
    }
    if (url_size.bytes >= kGatewayResumeUrlCapacity) {
        return GatewayResumeCaptureError::resume_url_too_large;
    }

    GatewayResumeState captured{};
    const JsonStringDecodeResult session = json_string_decode_utf8(
        ready.session_id, Slice<char>{captured.session_id, kGatewaySessionIdCapacity - 1});
    const JsonStringDecodeResult url = json_string_decode_utf8(
        ready.resume_gateway_url, Slice<char>{captured.resume_url, kGatewayResumeUrlCapacity - 1});
    if (!session.ok() || !url.ok()) {
        return GatewayResumeCaptureError::invalid_string;
    }
    captured.session_id[session.bytes] = 0;
    captured.resume_url[url.bytes] = 0;
    captured.session_id_bytes = session.bytes;
    captured.resume_url_bytes = url.bytes;
    captured.sequence = sequence;
    captured.valid = true;
    if (!gateway_resume_validate(captured)) {
        return GatewayResumeCaptureError::invalid_url;
    }
    *state = captured;
    return GatewayResumeCaptureError::none;
}

void gateway_resume_clear(GatewayResumeState* state) noexcept {
    secure_clear_bytes(state, sizeof(*state));
}

bool gateway_resume_validate(const GatewayResumeState& state) noexcept {
    if (!state.valid || state.session_id_bytes == 0 ||
        state.session_id_bytes >= kGatewaySessionIdCapacity || state.resume_url_bytes == 0 ||
        state.resume_url_bytes >= kGatewayResumeUrlCapacity ||
        state.session_id[state.session_id_bytes] != 0 || state.resume_url[state.resume_url_bytes] != 0 ||
        !has_wss_scheme(state.resume_url, state.resume_url_bytes)) {
        return false;
    }
    return utf8_validate(Utf8View{state.session_id, state.session_id_bytes}).valid &&
           utf8_validate(Utf8View{state.resume_url, state.resume_url_bytes}).valid;
}

GatewayReconnectDelay gateway_reconnect_next(GatewayReconnectBackoff* backoff, u32 random) noexcept {
    const u32 shift = backoff->attempt < 5 ? backoff->attempt : 5;
    u32 ceiling = 1'000u << shift;
    if (ceiling > 30'000) {
        ceiling = 30'000;
    }
    saturating_increment(backoff->attempt);
    return GatewayReconnectDelay{scale_inclusive(random, ceiling)};
}

void gateway_reconnect_reset(GatewayReconnectBackoff* backoff) noexcept {
    *backoff = GatewayReconnectBackoff{};
}

u32 gateway_invalid_session_delay(u32 random) noexcept {
    return 1'000u + scale_inclusive(random, 4'000u);
}

u32 gateway_ready_failure_delay(u32 failures) noexcept {
    if (failures == 0)
        return 0;
    const u32 shift = failures - 1 < 7 ? failures - 1 : 7;
    const u32 delay = 5'000u << shift;
    return delay < 600'000u ? delay : 600'000u;
}

GatewayRateLimitStep gateway_rate_limit_acquire(GatewayRateLimiter* limiter, u64 now_ms) noexcept {
    while (limiter->count != 0) {
        const u64 oldest = limiter->timestamps[limiter->head];
        if (now_ms < oldest || now_ms - oldest < kGatewaySendWindowMs) {
            break;
        }
        limiter->head = (limiter->head + 1) % kGatewaySendLimit;
        --limiter->count;
    }
    if (limiter->count == kGatewaySendLimit) {
        return GatewayRateLimitStep{
            deadline_after(limiter->timestamps[limiter->head], kGatewaySendWindowMs), false};
    }
    const u32 tail = (limiter->head + limiter->count) % kGatewaySendLimit;
    limiter->timestamps[tail] = now_ms;
    ++limiter->count;
    return GatewayRateLimitStep{now_ms, true};
}

bool gateway_outbound_enqueue(GatewayOutboundQueue* queue, GatewayLifecycleAction action) noexcept {
    if (queue->count == kGatewayOutboundActionCapacity) {
        return false;
    }
    const u32 tail = (queue->head + queue->count) % kGatewayOutboundActionCapacity;
    queue->actions[tail] = action;
    ++queue->count;
    return true;
}

GatewayOutboundStep gateway_outbound_take(GatewayOutboundQueue* queue, u64 now_ms) noexcept {
    if (queue->count == 0) {
        return GatewayOutboundStep{0, GatewayLifecycleAction::none, GatewayOutboundState::idle};
    }
    const GatewayRateLimitStep limited = gateway_rate_limit_acquire(&queue->limiter, now_ms);
    if (!limited.acquired) {
        return GatewayOutboundStep{limited.next_allowed_ms, GatewayLifecycleAction::none,
                                   GatewayOutboundState::rate_limited};
    }
    const GatewayLifecycleAction action = queue->actions[queue->head];
    queue->head = (queue->head + 1) % kGatewayOutboundActionCapacity;
    --queue->count;
    return GatewayOutboundStep{now_ms, action, GatewayOutboundState::action};
}

void gateway_outbound_reset(GatewayOutboundQueue* queue) noexcept {
    *queue = GatewayOutboundQueue{};
}

} // namespace maicord
