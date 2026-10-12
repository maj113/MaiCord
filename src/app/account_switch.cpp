#include "maicord/app/account_switch.hpp"
#include "maicord/app/account_paths.hpp"

namespace maicord {
namespace {

void finish(AccountSwitch* machine, AccountSwitchOutcome outcome) noexcept {
    machine->phase = AccountSwitchPhase::idle;
    machine->outcome = outcome;
    machine->stop_attempts = 0;
}

// Returns to the account the switch started from. Its profile is unchanged,
// so the ordinary restore path brings it back.
void revert(AccountSwitch* machine, const AccountSwitchIo& io, AccountSwitchOutcome outcome) noexcept {
    const bool restored = io.select(io.context, machine->source_slot) && io.attach(io.context);
    finish(machine, restored ? outcome : AccountSwitchOutcome::failed);
}

} // namespace

bool account_switch_io_valid(const AccountSwitchIo& io) noexcept {
    return io.stop_begin && io.stop_poll && io.reset && io.select && io.attach && io.open_login && io.commit;
}

bool account_switch_busy(const AccountSwitch& machine) noexcept {
    return machine.phase != AccountSwitchPhase::idle;
}

bool account_switch_begin(AccountSwitch* machine, const AccountSwitchIo& io, AccountSwitchKind kind,
                          u32 source_slot, u32 target_slot) noexcept {
    if (!machine || !account_switch_io_valid(io) || account_switch_busy(*machine) ||
        (kind != AccountSwitchKind::switch_account && kind != AccountSwitchKind::add_account) ||
        source_slot >= kAccountPathsSlotCount || target_slot >= kAccountPathsSlotCount ||
        source_slot == target_slot)
        return false;
    if (!io.stop_begin(io.context)) {
        machine->outcome = AccountSwitchOutcome::failed;
        return false;
    }
    ++machine->serial;
    machine->kind = kind;
    machine->source_slot = source_slot;
    machine->target_slot = target_slot;
    machine->stop_attempts = 1;
    machine->outcome = AccountSwitchOutcome::none;
    machine->phase = AccountSwitchPhase::stopping;
    // Account-scoped UI goes before the next account's state can arrive; the
    // old worker no longer reaches it because stop owns the host.
    io.reset(io.context);
    return true;
}

bool account_switch_pump(AccountSwitch* machine, const AccountSwitchIo& io) noexcept {
    if (!machine || !account_switch_io_valid(io) || machine->phase != AccountSwitchPhase::stopping)
        return false;
    const AccountSwitchStop stop = io.stop_poll(io.context);
    if (stop == AccountSwitchStop::pending)
        return false;
    if (stop == AccountSwitchStop::failed) {
        if (machine->stop_attempts < kAccountSwitchStopAttempts && io.stop_begin(io.context)) {
            ++machine->stop_attempts;
            return true;
        }
        finish(machine, AccountSwitchOutcome::failed);
        return true;
    }
    if (!io.select(io.context, machine->target_slot)) {
        revert(machine, io, AccountSwitchOutcome::reverted);
        return true;
    }
    if (machine->kind == AccountSwitchKind::add_account) {
        if (io.open_login(io.context)) {
            machine->phase = AccountSwitchPhase::login;
            return true;
        }
        revert(machine, io, AccountSwitchOutcome::reverted);
        return true;
    }
    if (io.attach(io.context)) {
        io.commit(io.context, machine->target_slot, machine->kind);
        finish(machine, AccountSwitchOutcome::switched);
        return true;
    }
    revert(machine, io, AccountSwitchOutcome::reverted);
    return true;
}

bool account_switch_login_finished(AccountSwitch* machine, const AccountSwitchIo& io,
                                   bool authenticated) noexcept {
    if (!machine || !account_switch_io_valid(io) || machine->phase != AccountSwitchPhase::login)
        return false;
    if (authenticated) {
        io.commit(io.context, machine->target_slot, machine->kind);
        finish(machine, AccountSwitchOutcome::added);
        return true;
    }
    revert(machine, io, AccountSwitchOutcome::cancelled);
    return true;
}

} // namespace maicord
