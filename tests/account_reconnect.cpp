// Account restart schedule: jittered exponential backoff, the attempt cap and
// manual Reconnect, no automatic restart after sign-in or requirement stops,
// banner states, and wakes only at retry deadlines. Synthetic clock values.
#include "maicord/app/account_reconnect.hpp"
#include "maicord/app/user_account_coordinator.hpp"
#include "maicord/protocol/account_requirement.hpp"
#include "test_output.hpp"

namespace maicord::account_reconnect_test {
namespace {
u32 failures{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
constexpr u8 error(UserAccountCoordinatorError value) noexcept {
    return static_cast<u8>(value);
}
u32 ceiling(u32 attempt) noexcept {
    u64 delay = u64{kAccountReconnectBaseMs} << (attempt > 20 ? 20 : attempt);
    return delay > kAccountReconnectCapMs ? kAccountReconnectCapMs : static_cast<u32>(delay);
}

void test_delay() noexcept {
    bool bounded = true, varied = false;
    for (u64 seed = 1; seed <= 64; ++seed) {
        u64 jitter = seed * 0x9E37'79B9'7F4A'7C15ull;
        u32 first = 0;
        for (u32 attempt = 0; attempt < 12; ++attempt) {
            const u32 delay = account_reconnect_delay(attempt, &jitter);
            bounded = bounded && delay >= ceiling(attempt) / 2 && delay <= ceiling(attempt);
            if (!attempt) {
                varied = varied || (seed > 1 && delay != first);
                first = delay;
            }
        }
    }
    u64 a = 7, b = 7;
    check(bounded, "reconnect delay stays within half and all of the doubling capped ceiling");
    check(varied || account_reconnect_delay(0, &a) == account_reconnect_delay(0, &b),
          "reconnect delay is jittered and deterministic for a seed");
    check(ceiling(20) == kAccountReconnectCapMs && ceiling(0) == kAccountReconnectBaseMs,
          "reconnect ceiling doubles from 2 s to the 2 min cap");
}

// A session that never reaches READY: every deadline restarts, every restart
// stops again. The scheduler wakes only at those deadlines, then goes offline.
void test_schedule_and_wakes() noexcept {
    AccountReconnect r{};
    r.jitter = 42;
    u64 now = 1'000'000;
    check(!account_reconnect_next_wake(r) && account_reconnect_banner(r) == AccountConnectionBanner::none,
          "a connected account has no banner and no wake");
    account_reconnect_stopped(&r, AccountStopReason::failure, now);
    u32 restarts = 0, wakes = 0;
    bool only_deadlines = true, banners = true;
    for (u32 turn = 0; turn < 64 && account_reconnect_next_wake(r); ++turn) {
        const u64 deadline = account_reconnect_next_wake(r);
        banners = banners && account_reconnect_banner(r) == AccountConnectionBanner::waiting;
        only_deadlines = only_deadlines && deadline > now && !account_reconnect_due(r, deadline - 1) &&
                         deadline - now <= kAccountReconnectCapMs;
        now = deadline;
        ++wakes;
        if (!account_reconnect_due(r, now))
            break;
        account_reconnect_starting(&r);
        ++restarts;
        banners = banners && account_reconnect_banner(r) == AccountConnectionBanner::reconnecting;
        only_deadlines = only_deadlines && !account_reconnect_next_wake(r);
        now += 500;
        account_reconnect_stopped(&r, AccountStopReason::failure, now);
    }
    check(restarts == kAccountReconnectMaximumAttempts && wakes == kAccountReconnectMaximumAttempts,
          "restarts stop after the attempt cap, one wake per retry deadline");
    check(only_deadlines && banners, "waiting wakes only at its deadline; connecting and offline never wake");
    check(r.phase == AccountReconnectPhase::offline && !account_reconnect_next_wake(r) &&
              account_reconnect_banner(r) == AccountConnectionBanner::offline &&
              !account_reconnect_due(r, now + 3'600'000),
          "an exhausted schedule goes offline and never restarts on its own");
    account_reconnect_manual(&r, now);
    check(r.attempt == 0 && account_reconnect_due(r, now) && account_reconnect_next_wake(r) == now,
          "Reconnect starts a fresh schedule due now");
    account_reconnect_starting(&r);
    account_reconnect_start_failed(&r, now);
    check(r.phase == AccountReconnectPhase::waiting && r.attempt == 1 && r.retry_at_ms >= now + ceiling(1) / 2 &&
              r.retry_at_ms <= now + ceiling(1),
          "a failed start schedules the next attempt with the next delay");
}

void test_user_stops() noexcept {
    check(account_reconnect_classify(error(UserAccountCoordinatorError::authentication_rejected),
                                     AccountRequirement::none) == AccountStopReason::authentication &&
              account_reconnect_classify(error(UserAccountCoordinatorError::credential), AccountRequirement::none) ==
                  AccountStopReason::authentication &&
              account_reconnect_classify(error(UserAccountCoordinatorError::none), AccountRequirement::verify_phone) ==
                  AccountStopReason::requirement &&
              account_reconnect_classify(error(UserAccountCoordinatorError::gateway), AccountRequirement::none) ==
                  AccountStopReason::failure &&
              account_reconnect_classify(error(UserAccountCoordinatorError::none), AccountRequirement::none) ==
                  AccountStopReason::failure,
          "sign-in and credential errors and pending requirements are the user's stops");
    constexpr AccountStopReason user_reasons[]{AccountStopReason::authentication, AccountStopReason::requirement};
    for (const auto reason : user_reasons) {
        AccountReconnect r{};
        account_reconnect_stopped(&r, reason, 5'000);
        check(r.phase == AccountReconnectPhase::offline && !account_reconnect_next_wake(r) &&
                  !account_reconnect_due(r, ~u64{0}) && account_reconnect_banner(r) == AccountConnectionBanner::offline &&
                  r.reason == reason,
              "a user stop never schedules a restart and offers only Reconnect");
    }
}

void test_connection() noexcept {
    AccountReconnect r{};
    r.jitter = 9;
    account_reconnect_stopped(&r, AccountStopReason::failure, 10'000);
    account_reconnect_starting(&r);
    account_reconnect_connected(&r, 20'000);
    check(r.phase == AccountReconnectPhase::idle && account_reconnect_banner(r) == AccountConnectionBanner::none &&
              !account_reconnect_next_wake(r),
          "READY clears the banner and the wake");
    account_reconnect_stopped(&r, AccountStopReason::failure, 20'000 + kAccountReconnectStableMs - 1);
    check(r.attempt == 1 && r.retry_at_ms >= 20'000 + kAccountReconnectStableMs - 1 + ceiling(1) / 2,
          "a stop soon after READY keeps backing off instead of looping at the base delay");
    account_reconnect_starting(&r);
    account_reconnect_connected(&r, 100'000);
    account_reconnect_stopped(&r, AccountStopReason::failure, 100'000 + kAccountReconnectStableMs);
    check(r.attempt == 0, "a session that stayed up starts the schedule over");

    AccountReconnect live{};
    account_reconnect_connected(&live, 1);
    account_reconnect_gateway_lost(&live);
    check(account_reconnect_banner(live) == AccountConnectionBanner::reconnecting &&
              !account_reconnect_next_wake(live),
          "a running session that lost its Gateway shows reconnecting without its own wake");
    account_reconnect_connected(&live, 2);
    check(account_reconnect_banner(live) == AccountConnectionBanner::none, "RESUMED clears the banner");
    account_reconnect_stopped(&live, AccountStopReason::failure, 3);
    account_reconnect_gateway_lost(&live);
    check(account_reconnect_banner(live) == AccountConnectionBanner::waiting,
          "a scheduled restart keeps its waiting banner");
    const u64 jitter = live.jitter;
    account_reconnect_reset(&live);
    check(live.phase == AccountReconnectPhase::idle && !live.attempt && !account_reconnect_next_wake(live) &&
              live.jitter == jitter && account_reconnect_banner(live) == AccountConnectionBanner::none,
          "reset forgets the schedule");
}
} // namespace
} // namespace maicord::account_reconnect_test

extern "C" __declspec(noreturn) void WINAPI maicord_account_reconnect_tests_entry() noexcept {
    using namespace maicord::account_reconnect_test;
    test_delay();
    test_schedule_and_wakes();
    test_user_stops();
    test_connection();
    const bool failed = failures != 0;
    constexpr char pass[] = "PASS account_reconnect\n", fail[] = "FAIL account_reconnect\n";
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
