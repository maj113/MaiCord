#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// Automatic idle: the official desktop client goes idle after ten minutes
// without input, immediately while the session is locked, and never during a
// call. Leaving idle is immediate on input. The manual status is applied
// separately (gateway_presence_effective), so DND/invisible stay untouched.
inline constexpr u64 kPresenceIdleTimeoutMs = 10 * 60 * 1000;
inline constexpr u64 kPresenceIdlePollMs = 5'000;
struct PresenceIdle {
    u64 since_unix_ms;
    bool idle;
};
struct PresenceIdleInput {
    u64 input_idle_ms; // Time since the last keyboard or mouse input.
    u64 unix_now_ms;
    u64 timeout_ms;
    bool in_call;
    bool locked;
};
// Returns true when the idle state changed. since_unix_ms is when input
// stopped, as the official client reports it.
constexpr bool presence_idle_update(PresenceIdle* state, const PresenceIdleInput& in) noexcept {
    if (!state)
        return false;
    const bool idle = !in.in_call && (in.locked || (in.timeout_ms && in.input_idle_ms >= in.timeout_ms));
    if (idle == state->idle)
        return false;
    state->idle = idle;
    state->since_unix_ms = !idle ? 0 : in.unix_now_ms > in.input_idle_ms ? in.unix_now_ms - in.input_idle_ms : in.unix_now_ms;
    return true;
}
// Delay until the state can next change: a short poll while idle (input ends
// it), otherwise the time left before the timeout, bounded to [1 s, 60 s].
constexpr u64 presence_idle_next_check_ms(const PresenceIdle& state, const PresenceIdleInput& in) noexcept {
    if (state.idle || in.in_call)
        return kPresenceIdlePollMs;
    const u64 left = in.timeout_ms > in.input_idle_ms ? in.timeout_ms - in.input_idle_ms : 0;
    return left < 1'000 ? 1'000 : left > 60'000 ? 60'000 : left;
}
} // namespace maicord
