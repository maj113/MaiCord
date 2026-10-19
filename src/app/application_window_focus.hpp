#pragma once
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
// Window focus as the official client sees it (its WINDOW_FOCUS state): a
// background, minimized or tray-hidden window is unfocused. Queried at each
// decision, so it needs no message hooks or polling.
inline bool application_window_focused(HWND window) noexcept {
    return window && GetForegroundWindow() == window && IsWindowVisible(window) && !IsIconic(window);
}
} // namespace maicord::app_detail
