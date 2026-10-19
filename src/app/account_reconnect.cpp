#include "maicord/app/account_reconnect.hpp"
#include "maicord/app/user_account_coordinator.hpp"
#include "maicord/protocol/account_requirement.hpp"

namespace maicord {
namespace {
u64 next_random(u64* state) noexcept {
    u64 x = *state ? *state : 0x9E3779B97F4A7C15ull;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}
void schedule(AccountReconnect* r, u64 now_ms) noexcept {
    if (r->attempt >= kAccountReconnectMaximumAttempts) {
        r->phase = AccountReconnectPhase::offline;
        r->retry_at_ms = 0;
        return;
    }
    r->phase = AccountReconnectPhase::waiting;
    r->retry_at_ms = now_ms + account_reconnect_delay(r->attempt, &r->jitter);
}
} // namespace

AccountStopReason account_reconnect_classify(u8 coordinator_error, AccountRequirement required) noexcept {
    if (required != AccountRequirement::none)
        return AccountStopReason::requirement;
    const auto error = static_cast<UserAccountCoordinatorError>(coordinator_error);
    return error == UserAccountCoordinatorError::authentication_rejected ||
                   error == UserAccountCoordinatorError::credential
               ? AccountStopReason::authentication
               : AccountStopReason::failure;
}

u32 account_reconnect_delay(u32 attempt, u64* jitter) noexcept {
    u32 delay = kAccountReconnectBaseMs;
    for (u32 i = 0; i < attempt && delay < kAccountReconnectCapMs; ++i)
        delay *= 2;
    if (delay > kAccountReconnectCapMs)
        delay = kAccountReconnectCapMs;
    const u32 half = delay / 2;
    return half + static_cast<u32>(next_random(jitter) % (u64{delay - half} + 1));
}

void account_reconnect_stopped(AccountReconnect* r, AccountStopReason reason, u64 now_ms) noexcept {
    r->reason = reason;
    r->gateway_down = false;
    // Only a session that stayed up starts the schedule over; READY followed
    // by another quick stop keeps backing off.
    if (r->connected_at_ms && now_ms - r->connected_at_ms >= kAccountReconnectStableMs)
        r->attempt = 0;
    r->connected_at_ms = 0;
    if (reason != AccountStopReason::failure) {
        r->phase = AccountReconnectPhase::offline;
        r->retry_at_ms = 0;
        return;
    }
    schedule(r, now_ms);
}

bool account_reconnect_due(const AccountReconnect& r, u64 now_ms) noexcept {
    return r.phase == AccountReconnectPhase::waiting && now_ms >= r.retry_at_ms;
}

void account_reconnect_starting(AccountReconnect* r) noexcept {
    r->phase = AccountReconnectPhase::connecting;
    r->retry_at_ms = 0;
    ++r->attempt;
}

void account_reconnect_start_failed(AccountReconnect* r, u64 now_ms) noexcept {
    schedule(r, now_ms);
}

void account_reconnect_connected(AccountReconnect* r, u64 now_ms) noexcept {
    r->phase = AccountReconnectPhase::idle;
    r->retry_at_ms = 0;
    r->gateway_down = false;
    r->connected_at_ms = now_ms ? now_ms : 1;
}

void account_reconnect_gateway_lost(AccountReconnect* r) noexcept {
    if (r->phase == AccountReconnectPhase::idle)
        r->gateway_down = true;
}

void account_reconnect_manual(AccountReconnect* r, u64 now_ms) noexcept {
    r->attempt = 0;
    r->reason = AccountStopReason::failure;
    r->phase = AccountReconnectPhase::waiting;
    r->retry_at_ms = now_ms ? now_ms : 1;
}

void account_reconnect_reset(AccountReconnect* r) noexcept {
    const u64 jitter = r->jitter;
    *r = {};
    r->jitter = jitter;
}

u64 account_reconnect_next_wake(const AccountReconnect& r) noexcept {
    return r.phase == AccountReconnectPhase::waiting ? r.retry_at_ms : 0;
}

AccountConnectionBanner account_reconnect_banner(const AccountReconnect& r) noexcept {
    switch (r.phase) {
    case AccountReconnectPhase::waiting:
        return AccountConnectionBanner::waiting;
    case AccountReconnectPhase::connecting:
        return AccountConnectionBanner::reconnecting;
    case AccountReconnectPhase::offline:
        return AccountConnectionBanner::offline;
    case AccountReconnectPhase::idle:
        break;
    }
    return r.gateway_down ? AccountConnectionBanner::reconnecting : AccountConnectionBanner::none;
}

} // namespace maicord
