#include "maicord/app/account/account_switch.hpp"
#include "maicord/app/account/account_paths.hpp"

namespace maicord {
namespace {

void finish(AccountSwitch* machine, AccountSwitchOutcome outcome) noexcept {
    machine->phase = AccountSwitchPhase::idle;
    machine->outcome = outcome;
    machine->stop_attempts = 0;
}

// Returns to the account the switch started from. Its profile is unchanged,
// so the ordinary restore path brings it back.
void revert(AccountSwitch* machine, AccountSwitchOutcome outcome) noexcept {
    const bool restored = account_switch_select(machine->source_slot) && account_switch_attach();
    finish(machine, restored ? outcome : AccountSwitchOutcome::failed);
}

} // namespace

bool account_switch_busy(const AccountSwitch& machine) noexcept {
    return machine.phase != AccountSwitchPhase::idle;
}

bool account_switch_begin(AccountSwitch* machine, AccountSwitchKind kind, u32 source_slot, u32 target_slot) noexcept {
    if (account_switch_busy(*machine) ||
        (kind != AccountSwitchKind::switch_account && kind != AccountSwitchKind::add_account) ||
        source_slot >= kAccountPathsSlotCount || target_slot >= kAccountPathsSlotCount ||
        source_slot == target_slot)
        return false;
    if (!account_switch_stop_begin()) {
        machine->outcome = AccountSwitchOutcome::failed;
        return false;
    }
    machine->kind = kind;
    machine->source_slot = source_slot;
    machine->target_slot = target_slot;
    machine->stop_attempts = 1;
    machine->outcome = AccountSwitchOutcome::none;
    machine->phase = AccountSwitchPhase::stopping;
    // Account-scoped UI goes before the next account's state can arrive; the
    // old worker no longer reaches it because stop owns the host.
    account_switch_reset();
    return true;
}

bool account_switch_pump(AccountSwitch* machine) noexcept {
    if (machine->phase != AccountSwitchPhase::stopping)
        return false;
    const AccountSwitchStop stop = account_switch_stop_poll();
    if (stop == AccountSwitchStop::pending)
        return false;
    if (stop == AccountSwitchStop::failed) {
        if (machine->stop_attempts < kAccountSwitchStopAttempts && account_switch_stop_begin()) {
            ++machine->stop_attempts;
            return true;
        }
        finish(machine, AccountSwitchOutcome::failed);
        return true;
    }
    if (!account_switch_select(machine->target_slot)) {
        revert(machine, AccountSwitchOutcome::reverted);
        return true;
    }
    if (machine->kind == AccountSwitchKind::add_account) {
        if (account_switch_open_login()) {
            machine->phase = AccountSwitchPhase::login;
            return true;
        }
        revert(machine, AccountSwitchOutcome::reverted);
        return true;
    }
    if (account_switch_attach()) {
        account_switch_commit(machine->target_slot);
        finish(machine, AccountSwitchOutcome::switched);
        return true;
    }
    revert(machine, AccountSwitchOutcome::reverted);
    return true;
}

bool account_switch_login_finished(AccountSwitch* machine, bool authenticated) noexcept {
    if (machine->phase != AccountSwitchPhase::login)
        return false;
    if (authenticated) {
        account_switch_commit(machine->target_slot);
        finish(machine, AccountSwitchOutcome::added);
        return true;
    }
    revert(machine, AccountSwitchOutcome::cancelled);
    return true;
}

} // namespace maicord
