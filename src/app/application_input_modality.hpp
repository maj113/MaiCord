#pragma once
#include "maicord/core/base.hpp"

#include <Windows.h>
namespace maicord::app_detail {
inline bool keyboard_focus_after_message(bool visible, UINT message, WPARAM key, bool shift) noexcept {
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN ||
        message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN || message == WM_NCLBUTTONDOWN ||
        message == WM_POINTERDOWN) return false;
    if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return visible;
    return visible || key == VK_TAB || key == VK_F6 || key == VK_APPS || key == VK_UP || key == VK_DOWN ||
        key == VK_LEFT || key == VK_RIGHT || key == VK_HOME || key == VK_END || key == VK_PRIOR ||
        key == VK_NEXT || key == VK_RETURN || key == VK_SPACE || (key == VK_F10 && shift);
}
}
