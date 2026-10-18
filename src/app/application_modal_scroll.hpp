#pragma once
#include "maicord/core/win32_records.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/ui/scroll_host.hpp"

#include <limits.h>

namespace maicord::app_detail {
// Modal controllers consume wheel input themselves, but share the ordinary
// scroll host's timing, wheel preferences and fractional-offset policy.
inline UiScrollDispatch application_modal_scroll_wheel(UiScrollHost* host, UiScrollBinding binding,
                                                       HWND window, u32 dpi, WPARAM wparam, LPARAM lparam,
                                                       u64 now_us) noexcept {
    Win32Point point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    if (!ScreenToClient(window, point.native()))
        return UiScrollDispatch::unhandled;
    const double scale = 96.0 / maximum(dpi, 1u);
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    return ui_scroll_host_wheel(
        host, binding, {static_cast<float>(point.x * scale), static_cast<float>(point.y * scale)},
        static_cast<short>(HIWORD(wparam)), lines, lines == WHEEL_PAGESCROLL, now_us, scale);
}
} // namespace maicord::app_detail
