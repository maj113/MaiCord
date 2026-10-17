#include "maicord/protocol/gateway_lifecycle.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
namespace {

GatewayLifecycleStep step(GatewayLifecycleAction action,
                          GatewayLifecycleError error = GatewayLifecycleError::none) noexcept {
    return GatewayLifecycleStep{action, error};
}

GatewayLifecycleStep invalid_state(const GatewayLifecycle* lifecycle) noexcept {
    return step(GatewayLifecycleAction::none,
                lifecycle ? GatewayLifecycleError::invalid_state : GatewayLifecycleError::invalid_input);
}

// Heartbeats, dispatches and session invalidation apply once HELLO was answered.
bool session_phase(const GatewayLifecycle& lifecycle) noexcept {
    return lifecycle.phase == GatewayLifecyclePhase::awaiting_session ||
           lifecycle.phase == GatewayLifecyclePhase::ready;
}

GatewayLifecycleAction reconnect_action(GatewayLifecycle* lifecycle, bool can_resume) noexcept {
    lifecycle->phase = GatewayLifecyclePhase::reconnecting;
    lifecycle->awaiting_heartbeat_ack = false;
    lifecycle->attempting_resume = can_resume;
    if (!can_resume) {
        lifecycle->resume_available = false;
        lifecycle->has_sequence = false;
    }
    return can_resume ? GatewayLifecycleAction::reconnect_resume
                      : GatewayLifecycleAction::reconnect_identify;
}

} // namespace

void gateway_lifecycle_initialize(GatewayLifecycle* lifecycle) noexcept {
    if (lifecycle) {
        *lifecycle = GatewayLifecycle{};
    }
}

GatewayLifecycleStep gateway_lifecycle_connected(GatewayLifecycle* lifecycle,
                                                 bool resume_available) noexcept {
    if (!lifecycle || (lifecycle->phase != GatewayLifecyclePhase::idle &&
                       lifecycle->phase != GatewayLifecyclePhase::reconnecting)) {
        return invalid_state(lifecycle);
    }
    lifecycle->heartbeat_interval_ms = 0;
    lifecycle->next_heartbeat_ms = 0;
    lifecycle->awaiting_heartbeat_ack = false;
    lifecycle->resume_available = resume_available && lifecycle->has_sequence;
    lifecycle->attempting_resume = lifecycle->resume_available;
    lifecycle->phase = GatewayLifecyclePhase::awaiting_hello;
    return step(GatewayLifecycleAction::none);
}

GatewayLifecycleStep gateway_lifecycle_hello(GatewayLifecycle* lifecycle, GatewayHello hello, u32 jitter,
                                             u64 now_ms) noexcept {
    if (!lifecycle || hello.heartbeat_interval_ms == 0) {
        return lifecycle
                   ? step(GatewayLifecycleAction::none, GatewayLifecycleError::invalid_input)
                   : invalid_state(nullptr);
    }
    if (lifecycle->phase != GatewayLifecyclePhase::awaiting_hello) {
        return invalid_state(lifecycle);
    }
    lifecycle->heartbeat_interval_ms = hello.heartbeat_interval_ms;
    const u64 jitter_ms = (static_cast<u64>(hello.heartbeat_interval_ms) * static_cast<u64>(jitter)) >> 32;
    lifecycle->next_heartbeat_ms = deadline_after(now_ms, jitter_ms);
    lifecycle->awaiting_heartbeat_ack = false;
    lifecycle->phase = GatewayLifecyclePhase::awaiting_session;
    return step(lifecycle->attempting_resume ? GatewayLifecycleAction::send_resume
                                                         : GatewayLifecycleAction::send_identify);
}

GatewayLifecycleStep gateway_lifecycle_tick(GatewayLifecycle* lifecycle, u64 now_ms) noexcept {
    if (!lifecycle) {
        return invalid_state(nullptr);
    }
    if (!session_phase(*lifecycle)) {
        return step(GatewayLifecycleAction::none);
    }
    if (now_ms < lifecycle->next_heartbeat_ms) {
        return step(GatewayLifecycleAction::none);
    }
    if (lifecycle->awaiting_heartbeat_ack) {
        return step(reconnect_action(lifecycle, lifecycle->resume_available && lifecycle->has_sequence));
    }
    lifecycle->awaiting_heartbeat_ack = true;
    lifecycle->next_heartbeat_ms = deadline_after(now_ms, lifecycle->heartbeat_interval_ms);
    return step(GatewayLifecycleAction::send_heartbeat);
}

GatewayLifecycleStep gateway_lifecycle_heartbeat_requested(GatewayLifecycle* lifecycle) noexcept {
    if (!lifecycle || !session_phase(*lifecycle)) {
        return invalid_state(lifecycle);
    }
    lifecycle->awaiting_heartbeat_ack = true;
    return step(GatewayLifecycleAction::send_heartbeat);
}

GatewayLifecycleStep gateway_lifecycle_heartbeat_acknowledged(GatewayLifecycle* lifecycle) noexcept {
    if (!lifecycle || !session_phase(*lifecycle)) {
        return invalid_state(lifecycle);
    }
    lifecycle->awaiting_heartbeat_ack = false;
    return step(GatewayLifecycleAction::none);
}

GatewayLifecycleStep gateway_lifecycle_dispatch(GatewayLifecycle* lifecycle, u64 sequence) noexcept {
    if (!lifecycle || !session_phase(*lifecycle)) {
        return invalid_state(lifecycle);
    }
    lifecycle->last_sequence = sequence;
    lifecycle->has_sequence = true;
    return step(GatewayLifecycleAction::none);
}

GatewayLifecycleStep gateway_lifecycle_session_ready(GatewayLifecycle* lifecycle,
                                                     bool resume_available) noexcept {
    if (!lifecycle || lifecycle->phase != GatewayLifecyclePhase::awaiting_session) {
        return invalid_state(lifecycle);
    }
    lifecycle->phase = GatewayLifecyclePhase::ready;
    lifecycle->resume_available = resume_available && lifecycle->has_sequence;
    lifecycle->attempting_resume = false;
    return step(GatewayLifecycleAction::none);
}

GatewayLifecycleStep gateway_lifecycle_reconnect_requested(GatewayLifecycle* lifecycle) noexcept {
    if (!lifecycle || (lifecycle->phase != GatewayLifecyclePhase::awaiting_hello &&
                       lifecycle->phase != GatewayLifecyclePhase::awaiting_session &&
                       lifecycle->phase != GatewayLifecyclePhase::ready)) {
        return invalid_state(lifecycle);
    }
    return step(reconnect_action(lifecycle, lifecycle->resume_available && lifecycle->has_sequence));
}

GatewayLifecycleStep gateway_lifecycle_invalid_session(GatewayLifecycle* lifecycle,
                                                       bool resumable) noexcept {
    if (!lifecycle || !session_phase(*lifecycle)) {
        return invalid_state(lifecycle);
    }
    const bool can_resume = resumable && lifecycle->resume_available && lifecycle->has_sequence;
    return step(reconnect_action(lifecycle, can_resume));
}

GatewayCloseDisposition gateway_classify_close(u16 close_code) noexcept {
    switch (close_code) {
    case 1000:
    case 1001:
    case 4004:
    case 4010:
    case 4011:
    case 4012:
    case 4013:
    case 4014:
        return GatewayCloseDisposition::stop;
    case 4007:
    case 4009:
        return GatewayCloseDisposition::reidentify;
    default:
        return GatewayCloseDisposition::resume;
    }
}

GatewayLifecycleStep gateway_lifecycle_closed(GatewayLifecycle* lifecycle, u16 close_code) noexcept {
    if (!lifecycle || lifecycle->phase == GatewayLifecyclePhase::idle) {
        return invalid_state(lifecycle);
    }
    const GatewayCloseDisposition disposition = gateway_classify_close(close_code);
    if (disposition == GatewayCloseDisposition::stop) {
        lifecycle->phase = GatewayLifecyclePhase::idle;
        lifecycle->awaiting_heartbeat_ack = false;
        lifecycle->resume_available = false;
        lifecycle->has_sequence = false;
        return step(GatewayLifecycleAction::stop);
    }
    const bool can_resume = disposition == GatewayCloseDisposition::resume && lifecycle->resume_available &&
                            lifecycle->has_sequence;
    return step(reconnect_action(lifecycle, can_resume));
}

GatewayWriteResult gateway_write_heartbeat(const GatewayLifecycle& lifecycle, Slice<u8> output) noexcept {
    if (!output.data) {
        return GatewayWriteResult{0, GatewayLifecycleError::invalid_input};
    }
    constexpr u8 prefix[] = {'{', '"', 'o', 'p', '"', ':', '1', ',', '"', 'd', '"', ':'};
    constexpr u8 suffix[] = {'}'};
    constexpr u8 null_value[] = {'n', 'u', 'l', 'l'};
    const u32 value_bytes = lifecycle.has_sequence ? decimal_digits(lifecycle.last_sequence)
                                                   : static_cast<u32>(sizeof(null_value));
    const u32 required = static_cast<u32>(sizeof(prefix)) + value_bytes + static_cast<u32>(sizeof(suffix));
    if (output.count < required) {
        return GatewayWriteResult{0, GatewayLifecycleError::output_too_small};
    }
    u32 offset = 0;
    for (u32 index = 0; index < static_cast<u32>(sizeof(prefix)); ++index) {
        output[offset++] = prefix[index];
    }
    if (lifecycle.has_sequence) {
        decimal_write(output.data + offset, lifecycle.last_sequence);
        offset += value_bytes;
    } else {
        for (u32 index = 0; index < static_cast<u32>(sizeof(null_value)); ++index) {
            output[offset++] = null_value[index];
        }
    }
    output[offset++] = suffix[0];
    return GatewayWriteResult{offset, GatewayLifecycleError::none};
}

} // namespace maicord
