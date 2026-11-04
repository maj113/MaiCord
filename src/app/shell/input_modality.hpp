#pragma once
#include "maicord/core/base.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window/key.hpp"

namespace maicord::app_detail {
inline bool keyboard_focus_after_message(bool visible, UINT message, WPARAM key, bool shift) noexcept {
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN ||
        message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN || message == WM_NCLBUTTONDOWN ||
        message == WM_POINTERDOWN) return false;
    if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return visible;
    return visible || key == key::tab || key == key::f6 || key == key::menu || key == key::up || key == key::down ||
        key == key::left || key == key::right || key == key::home || key == key::end || key == key::page_up ||
        key == key::page_down || key == key::enter || key == key::space || (key == key::f10 && shift);
}
}
