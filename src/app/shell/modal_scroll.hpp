#pragma once
#include "overlay_input.hpp"
#include "state.hpp"
#include "maicord/ui/layout/scroll_host.hpp"

#include <limits.h>

namespace maicord::app_detail {
// Modal controllers consume wheel input themselves, but share the ordinary
// scroll host's timing, wheel preferences and fractional-offset policy. The
// point is the wheel's content position from application_wheel_point.
inline UiScrollDispatch application_modal_scroll_wheel(UiScrollBinding binding, UiPoint point, WPARAM wparam) noexcept {
    const u32 lines = application_wheel_scroll_lines();
    return ui_scroll_host_wheel(&g_app.scroll_host, binding, point, static_cast<short>(HIWORD(wparam)), lines,
                                lines == WHEEL_PAGESCROLL, application_now_us(), 96.0 / maximum(g_app.renderer.dpi, 1u));
}
} // namespace maicord::app_detail
