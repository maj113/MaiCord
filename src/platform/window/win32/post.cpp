// window_post for Windows: the cross-thread wake of window.hpp as a private window message.
#include "maicord/core/win32_types.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window/window.hpp"
#include "internal.hpp"

namespace maicord {
namespace {

// The message of each WakeCause: the ones win32_translate turns into wake events (internal.hpp), and the ones the
// application's window procedure handles itself (kApplicationAccessibilityMessage and the tray message).
UINT wake_message(u32 cause) noexcept {
    switch (cause) {
    case wake_poke:
        return WM_NULL;
    case wake_begin_webauthn:
        return win32_detail::kBeginWebAuthnMessage;
    case wake_phone_verify:
        return win32_detail::kPhoneVerifyMessage;
    case wake_notifications:
        return win32_detail::kNotificationMessage;
    case wake_accessibility:
        return kWindowMessageApp + 0x3A;
    case wake_tray:
        return kWindowMessageApp + 0x3C;
    default:
        return 0;
    }
}

} // namespace

// Fails (false) without a window or for an unknown cause; on a PostMessageW failure the thread's last error is
// the system's.
bool window_post(const Waker& waker, u64 a, u64 b) noexcept {
    const UINT message = wake_message(waker.cause);
    if (!waker.window || (!message && waker.cause != wake_poke))
        return false;
    return PostMessageW(static_cast<HWND>(waker.window), message, static_cast<WPARAM>(a), static_cast<LPARAM>(b)) != 0;
}

} // namespace maicord
