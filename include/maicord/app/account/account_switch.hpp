#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Account switching as a small state machine over host-provided effects:
//   begin:   stop_begin (quiesce + asynchronous worker stop), then reset (UI,
//            stores, caches, popups, call state; drafts were persisted by stop)
//   pump:    stop_poll until joined, then select(target) and either attach
//            (switch) or open_login (add); a failed attach reverts to source
//   login:   login_finished(true) commits the added account; false (cancel)
//            selects and re-attaches the source account.
// A stop that cannot join is retried a bounded number of times.
enum class AccountSwitchKind : u8 { none, switch_account, add_account };
enum class AccountSwitchPhase : u8 { idle, stopping, login };
enum class AccountSwitchStop : u8 { pending, stopped, failed };
enum class AccountSwitchOutcome : u8 { none, switched, added, cancelled, reverted, failed };

inline constexpr u8 kAccountSwitchStopAttempts = 3;

struct AccountSwitchIo {
    void* context;
    bool (*stop_begin)(void* context) noexcept;
    AccountSwitchStop (*stop_poll)(void* context) noexcept;
    void (*reset)(void* context) noexcept;
    bool (*select)(void* context, u32 slot) noexcept;
    bool (*attach)(void* context) noexcept;
    bool (*open_login)(void* context) noexcept;
    void (*commit)(void* context, u32 slot, AccountSwitchKind kind) noexcept;
};

struct AccountSwitch {
    AccountSwitchPhase phase;
    AccountSwitchKind kind;
    AccountSwitchOutcome outcome;
    u8 stop_attempts;
    u32 source_slot;
    u32 target_slot;
    u32 serial;
};

bool account_switch_io_valid(const AccountSwitchIo& io) noexcept;
bool account_switch_busy(const AccountSwitch& machine) noexcept;
// False leaves the current account untouched (busy, invalid or equal slots,
// or the stop could not start).
bool account_switch_begin(AccountSwitch* machine, const AccountSwitchIo& io, AccountSwitchKind kind,
                          u32 source_slot, u32 target_slot) noexcept;
// Advances a pending stop. True when the phase or outcome changed.
bool account_switch_pump(AccountSwitch* machine, const AccountSwitchIo& io) noexcept;
// Ends add mode: authenticated commits the new slot, otherwise the source returns.
bool account_switch_login_finished(AccountSwitch* machine, const AccountSwitchIo& io,
                                   bool authenticated) noexcept;

} // namespace maicord
