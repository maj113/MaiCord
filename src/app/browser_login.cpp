#include "maicord/app/browser_login.hpp"

namespace maicord {

BrowserSignInReceiverStatus browser_login_start(BrowserLogin* owner, u64 generation,
                                                u32 timeout_ms) noexcept {
    if (!owner || owner->receiver.runtime || owner->status.generation || owner->authenticated)
        return {
            0, 0, 0, 0, 0, BrowserSignInReceiverPhase::empty, BrowserSignInReceiverError::invalid_input};
    owner->status = browser_sign_in_receiver_start(&owner->receiver, generation, timeout_ms);
    return owner->status;
}

BrowserSignInReceiverStatus browser_login_status(BrowserLogin* owner) noexcept {
    if (!owner)
        return {};
    if (owner->receiver.runtime)
        owner->status = browser_sign_in_receiver_status(&owner->receiver);
    return owner->status;
}

BrowserLoginPumpResult browser_login_pump(BrowserLogin* owner, UserAuthCoordinator* coordinator) noexcept {
    BrowserLoginPumpResult result{};
    if (!owner || !coordinator || owner->authenticated)
        return result;
    auto status = browser_login_status(owner);
    if (!status.ok() || status.phase == BrowserSignInReceiverPhase::expired ||
        status.phase == BrowserSignInReceiverPhase::cancelled ||
        status.phase == BrowserSignInReceiverPhase::failed ||
        status.phase == BrowserSignInReceiverPhase::stopping)
        return result;
    if (status.phase == BrowserSignInReceiverPhase::candidate_ready) {
        SecureBuffer exchange{};
        const auto initialized = secure_buffer_initialize(&exchange, kBrowserSignInMaximumBodyBytes);
        const auto copied =
            initialized.ok() ? browser_sign_in_receiver_take_exchange(&owner->receiver, status.generation,
                                                                      status.candidate_id, &exchange)
                             : initialized;
        if (copied.ok())
            result.coordinator =
                user_auth_coordinator_submit_browser_handoff_exchange(coordinator, &exchange);
        secure_buffer_release(&exchange);
        if (!copied.ok() || !result.coordinator.ok()) {
            owner->status = browser_sign_in_receiver_finish_exchange(&owner->receiver, status.generation,
                                                                     status.candidate_id, false);
            result.failed = !initialized.ok() || (copied.ok() && !result.coordinator.ok());
            return result;
        }
        owner->status = browser_sign_in_receiver_status(&owner->receiver);
        return result;
    }
    if (status.phase != BrowserSignInReceiverPhase::exchange_pending)
        return result;
    result.coordinator = user_auth_coordinator_pump(coordinator, &result.event);
    if (result.event.kind == UserAuthCoordinatorEventKind::none && result.coordinator.ok())
        return result;
    if (result.event.kind == UserAuthCoordinatorEventKind::authenticated && result.coordinator.ok()) {
        // This is the final generation/candidate/deadline gate. A late response
        // cannot enter the ordinary protected profile transaction.
        owner->status = browser_sign_in_receiver_finish_exchange(&owner->receiver, status.generation,
                                                                 status.candidate_id, true);
        result.accepted = owner->status.ok() && owner->status.phase == BrowserSignInReceiverPhase::complete;
        owner->authenticated = result.accepted;
        result.failed = !result.accepted;
        return result;
    }
    owner->status = browser_sign_in_receiver_finish_exchange(&owner->receiver, status.generation,
                                                             status.candidate_id, false);
    result.failed = !result.coordinator.ok();
    return result;
}

HANDLE browser_login_notification_handle(const BrowserLogin* owner,
                                         const UserAuthCoordinator* coordinator) noexcept {
    if (!owner || !owner->receiver.runtime)
        return nullptr;
    const auto status = browser_sign_in_receiver_status(&owner->receiver);
    return status.phase == BrowserSignInReceiverPhase::exchange_pending
               ? user_auth_coordinator_notification_handle(coordinator)
               : browser_sign_in_receiver_notification_handle(&owner->receiver);
}

BrowserSignInReceiverStatus browser_login_stop(BrowserLogin* owner, u32 timeout_ms) noexcept {
    if (!owner)
        return {};
    owner->status = browser_sign_in_receiver_stop(&owner->receiver, timeout_ms);
    return owner->status;
}

} // namespace maicord
