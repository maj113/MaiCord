#pragma once
#include "maicord/core/base.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
enum class GatewayStateRecoveryResult : u8 { queued, pending, unavailable, exhausted };

// Application recovery policy, independent of the transport retry policy.
// A successful READY does not replenish the budget: persistent schema failures
// must not create an endless identify loop. Account teardown resets ownership.
struct GatewayStateRecovery {
    u64 due_ms;
    u8 attempts;
    bool pending;
};

inline GatewayStateRecoveryResult gateway_state_recovery_request(GatewayStateRecovery* state, bool ready) noexcept {
    if (state->pending || state->due_ms) return GatewayStateRecoveryResult::pending;
    if (!ready) return GatewayStateRecoveryResult::unavailable;
    if (state->attempts >= 3) return GatewayStateRecoveryResult::exhausted;
    ++state->attempts;
    state->pending = true;
    return GatewayStateRecoveryResult::queued;
}

inline bool gateway_state_recovery_schedule(GatewayStateRecovery* state, u64 now_ms) noexcept {
    if (!state->pending || !state->attempts || state->attempts > 3) return false;
    constexpr u32 delays[] = {1000, 30'000, 300'000};
    state->due_ms = deadline_after(now_ms, delays[state->attempts - 1]);
    state->pending = false;
    return true;
}

inline u64 gateway_state_recovery_deadline(const GatewayStateRecovery& state, bool ready) noexcept {
    return ready ? state.due_ms : 0;
}

inline bool gateway_state_recovery_due(GatewayStateRecovery* state, bool ready, u64 now_ms) noexcept {
    if (!ready || !state->due_ms || now_ms < state->due_ms) return false;
    state->due_ms = 0;
    return true;
}

inline void gateway_state_recovery_ready(GatewayStateRecovery* state) noexcept {
    state->pending = false;
    state->due_ms = 0;
}

// Only used under the same owner lock when posting the worker wake failed.
inline void gateway_state_recovery_cancel_post(GatewayStateRecovery* state) noexcept {
    if (state->pending && state->attempts) {
        state->pending = false;
        --state->attempts;
    }
}
}
