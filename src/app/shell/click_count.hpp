#pragma once

#include "state.hpp"
#include "maicord/platform/window/window.hpp"
#include "maicord/ui/text/pointer_selection.hpp"

namespace maicord::app_detail {

// System double-click time and rectangle, in client DIPs.
inline UiClickPolicy application_click_policy() noexcept {
    const PlatformSettings settings = platform_settings_read();
    const float scale = 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
    return {settings.double_click_ms, static_cast<float>(settings.double_click_dx_px) * scale * 0.5f,
            static_cast<float>(settings.double_click_dy_px) * scale * 0.5f};
}

// Click count (1..3) for the primary-button press `event` on `owner`.
inline u8 application_click_count(const WindowEvent& event, const void* owner, float x, float y,
                                  bool double_click_message) noexcept {
    return ui_click_tracker_press(&g_app.click_tracker, owner, x, y, event.time_ms, application_click_policy(),
                                  double_click_message);
}

} // namespace maicord::app_detail
