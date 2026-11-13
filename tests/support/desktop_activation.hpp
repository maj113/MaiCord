#pragma once
// Whether a WM_ACTIVATE may run activation work that makes a cross-apartment COM call (the Windows notification
// setting). The taskbar restores a minimized window by activating it while still minimized, then posting SC_RESTORE;
// an outgoing COM call from the STA in that WM_ACTIVATE lets the COM modal loop discard the queued WM_SYSCOMMAND with
// a beep. The restore's own (non-minimized) activation follows, so the work waits for that one.
#include "maicord/core/win32_types.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {

constexpr bool desktop_activation_refresh_allowed(WPARAM activation) noexcept {
    // LOWORD(activation) is the state; a nonzero HIWORD means minimized.
    return static_cast<WORD>(activation & 0xffff) != WA_INACTIVE && static_cast<WORD>((activation >> 16) & 0xffff) == 0;
}

} // namespace maicord
