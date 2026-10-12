#pragma once

#include "maicord/app/browser_sign_in_receiver.hpp"
#include "maicord/app/user_auth_coordinator.hpp"

namespace maicord {

// UI-thread owner. A fresh ordinary coordinator belongs exclusively to this
// attempt; neither password/reset nor QR requests may share it.
struct BrowserLogin {
    BrowserSignInReceiver receiver;
    BrowserSignInReceiverStatus status;
    bool authenticated;
};
struct BrowserLoginPumpResult {
    UserAuthCoordinatorResult coordinator;
    UserAuthCoordinatorEvent event;
    bool accepted;
    bool failed;
};

BrowserSignInReceiverStatus browser_login_start(BrowserLogin* owner, u64 generation,
                                                u32 timeout_ms) noexcept;
BrowserSignInReceiverStatus browser_login_status(BrowserLogin* owner) noexcept;
BrowserLoginPumpResult browser_login_pump(BrowserLogin* owner, UserAuthCoordinator* coordinator) noexcept;
HANDLE browser_login_notification_handle(const BrowserLogin* owner,
                                         const UserAuthCoordinator* coordinator) noexcept;
// Stop invalidates receiver admission before transport/profile teardown. Keep
// the whole owner alive when its worker cannot join.
BrowserSignInReceiverStatus browser_login_stop(BrowserLogin* owner, u32 timeout_ms) noexcept;

} // namespace maicord
