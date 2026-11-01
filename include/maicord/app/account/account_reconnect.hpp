#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

enum class AccountRequirement : u8;

// Restart schedule for an account session that stopped while its cached shell
// stays up. Restarts back off exponentially with jitter, stop after a fixed
// number of attempts and never start on their own after authentication or
// account-requirement stops; those wait for the manual Reconnect action.
inline constexpr u32 kAccountReconnectMaximumAttempts = 8;
inline constexpr u32 kAccountReconnectBaseMs = 2'000;
inline constexpr u32 kAccountReconnectCapMs = 120'000;
// A session that stays up this long after READY starts the next schedule over.
inline constexpr u32 kAccountReconnectStableMs = 60'000;

enum class AccountReconnectPhase : u8 {
    idle,       // connected, or never stopped
    waiting,    // a restart is scheduled at retry_at_ms
    connecting, // a restarted session waits for READY
    offline,    // retries exhausted or blocked: only Reconnect restarts
};
enum class AccountStopReason : u8 { failure, authentication, requirement };
enum class AccountConnectionBanner : u8 { none, reconnecting, waiting, offline };

struct AccountReconnect {
    u64 retry_at_ms;     // monotonic; nonzero only while waiting
    u64 connected_at_ms; // last READY/RESUMED
    u64 jitter;          // xorshift state; zero is reseeded
    u32 attempt;         // restarts since the schedule began
    AccountReconnectPhase phase;
    AccountStopReason reason;
    bool gateway_down; // a running session lost its Gateway since READY/RESUMED
};

// A stop's reason from the coordinator error and pending requirement.
AccountStopReason account_reconnect_classify(u8 coordinator_error, AccountRequirement required) noexcept;
// The jittered delay before restart `attempt`: uniform in [d/2, d] where d
// doubles from kAccountReconnectBaseMs up to kAccountReconnectCapMs.
u32 account_reconnect_delay(u32 attempt, u64* jitter) noexcept;
// The session stopped: schedules the next restart, or goes offline.
void account_reconnect_stopped(AccountReconnect*, AccountStopReason, u64 now_ms) noexcept;
bool account_reconnect_due(const AccountReconnect&, u64 now_ms) noexcept;
// A due restart begins; start_failed schedules the one after it.
void account_reconnect_starting(AccountReconnect*) noexcept;
void account_reconnect_start_failed(AccountReconnect*, u64 now_ms) noexcept;
// READY or RESUMED.
void account_reconnect_connected(AccountReconnect*, u64 now_ms) noexcept;
// The running session's Gateway closed or began reconnecting.
void account_reconnect_gateway_lost(AccountReconnect*) noexcept;
// The user's Reconnect: a fresh schedule whose first restart is due now.
void account_reconnect_manual(AccountReconnect*, u64 now_ms) noexcept;
// Forgets the schedule (logout, account switch, sign-in).
void account_reconnect_reset(AccountReconnect*) noexcept;
// The only deadline this schedule wakes for; zero otherwise.
u64 account_reconnect_next_wake(const AccountReconnect&) noexcept;
AccountConnectionBanner account_reconnect_banner(const AccountReconnect&) noexcept;

} // namespace maicord
