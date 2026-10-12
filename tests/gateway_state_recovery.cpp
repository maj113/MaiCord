#include "maicord/app/gateway_client.hpp"

namespace maicord {
void test_gateway_state_recovery(void (*check)(bool, const char*) noexcept) noexcept {
    GatewayStateRecovery state{};
    check(gateway_client_request_fresh_state(nullptr) == GatewayStateRecoveryResult::unavailable,
        "null gateway cannot queue state recovery");
    GatewayClient dormant{};
    check(gateway_client_request_fresh_state(&dormant) == GatewayStateRecoveryResult::unavailable &&
        gateway_state_recovery_request(&state, false) == GatewayStateRecoveryResult::unavailable &&
        !state.attempts && !gateway_state_recovery_schedule(&state, 10) && !state.due_ms,
        "dormant state recovery neither spends attempts nor invents a deadline");
    check(gateway_state_recovery_request(&state, true) == GatewayStateRecoveryResult::queued &&
        gateway_state_recovery_request(&state, true) == GatewayStateRecoveryResult::pending && state.attempts == 1 &&
        gateway_state_recovery_request(&state, false) == GatewayStateRecoveryResult::pending,
        "duplicate recovery requests coalesce across transport phase changes");
    gateway_state_recovery_cancel_post(&state);
    check(!state.pending && !state.attempts, "failed worker wake refunds the unsent recovery attempt");
    const u32 delays[] = {1000, 30'000, 300'000};
    for (u32 attempt = 0; attempt < 3; ++attempt) {
        const u64 now = 1000 + attempt * 1'000'000ull;
        check(gateway_state_recovery_request(&state, true) == GatewayStateRecoveryResult::queued &&
            gateway_state_recovery_schedule(&state, now) && state.due_ms == now + delays[attempt] && !state.pending &&
            !gateway_state_recovery_schedule(&state, now) && state.attempts == attempt + 1,
            "worker consumes each bounded recovery once with increasing delay");
        check(gateway_state_recovery_request(&state, true) == GatewayStateRecoveryResult::pending &&
            gateway_state_recovery_deadline(state, true) == state.due_ms &&
            !gateway_state_recovery_deadline(state, false) &&
            !gateway_state_recovery_due(&state, true, now + delays[attempt] - 1) &&
            !gateway_state_recovery_due(&state, false, now + delays[attempt]),
            "recovery keeps transport active until due and cannot busy-wake an unready session");
        check(gateway_state_recovery_due(&state, true, now + delays[attempt]) &&
            !gateway_state_recovery_due(&state, true, now + delays[attempt]),
            "recovery deadline triggers one fresh identify");
        gateway_state_recovery_cancel_post(&state);
        check(state.attempts == attempt + 1, "completed requests cannot refund their retry budget");
    }
    check(gateway_state_recovery_request(&state, true) == GatewayStateRecoveryResult::exhausted &&
        !state.pending && !gateway_state_recovery_schedule(&state, 0) && state.attempts == 3,
        "persistent projection failure cannot cause an unbounded identify loop");
    state = {};
    check(gateway_state_recovery_request(&state, true) == GatewayStateRecoveryResult::queued &&
        gateway_state_recovery_schedule(&state, ~0ull - 10) && state.due_ms == ~0ull,
        "recovery deadlines saturate instead of wrapping into immediate retries");
    gateway_state_recovery_ready(&state);
    check(!state.due_ms && !state.pending && state.attempts == 1,
        "fresh READY cancels recovery but retains the lifetime retry budget");
}
}
