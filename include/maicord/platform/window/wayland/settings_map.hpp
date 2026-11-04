#pragma once

#include "maicord/platform/dbus/desktop_settings.hpp"
#include "maicord/platform/window/window.hpp"

// The desktop's settings (the settings portal, platform/dbus/desktop_settings.hpp) as the window service's
// PlatformSettings. What the desktop does not publish keeps the values the Windows defaults give.

namespace maicord::wayland {

// What GetCaretBlinkTime returns on Windows when the caret does not blink.
inline constexpr u32 kCaretNoBlink = 0xFFFFFFFFu;

inline PlatformSettings platform_settings_from_desktop(const dbus::DesktopSettings& desktop) noexcept {
    PlatformSettings settings{};
    settings.double_click_ms = desktop.double_click_ms ? desktop.double_click_ms : 500;
    settings.double_click_dx_px = 4;
    settings.double_click_dy_px = 4;
    settings.caret_blink_ms = 530;
    if (desktop.loaded) {
        // GTK's cursor-blink-time is a whole on-off cycle; Windows' blink time is one half of it.
        const u32 half = desktop.cursor_blink_ms / 2;
        settings.caret_blink_ms = !desktop.cursor_blink ? kCaretNoBlink : half < 100 ? 100 : half;
    }
    settings.wheel_lines = 3;
    settings.wheel_page = false;
    settings.client_area_animation = desktop.animations && !desktop.reduced_motion;
    settings.dark_scheme = desktop.color_scheme == dbus::ColorScheme::dark;
    settings.system_idle_ms = kSystemIdleUnknown;
    return settings;
}

} // namespace maicord::wayland
