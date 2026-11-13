#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/platform/desktop/integration.hpp"
#include "maicord/platform/desktop/system_tray.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "../shell/state.hpp"
#include "../settings/catalog.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// The tray, taskbar and notification mark: MaiCord's brand violet in the built-in flat themes (Theme::brand_marks),
// else the theme's accent, which a gradient theme derives from its colours (its emphasis fill and label).
inline TrayIconPalette application_tray_palette() noexcept {
    const auto argb = [](const Color& color) noexcept {
        const auto byte = [](float v) noexcept { return static_cast<u32>((v < 0 ? 0 : v > 1 ? 1 : v) * 255.f + .5f); };
        return 0xFF000000u | (byte(color.red) << 16) | (byte(color.green) << 8) | byte(color.blue);
    };
    const Theme& theme = application_theme();
    const auto& tokens = theme.tokens;
    const bool gradient = theme.background.color_count != 0;
    const auto& brand = gradient ? tokens.emphasis.fill : tokens.accent.brand;
    // The lower edge of the tile is the accent at 78% brightness.
    const Color shade{brand.red * .78f, brand.green * .78f, brand.blue * .78f, 1};
    const TrayIconPalette themed{argb(brand), argb(shade), argb(tokens.accent.success), argb(tokens.accent.danger),
                                 argb(gradient ? tokens.emphasis.text : tokens.text.on_accent)};
    return theme.brand_marks ? tray_icon_brand_palette(themed) : themed;
}

// Desktop integration: single instance, discord:// links, the notification
// area icon, close-to-tray and the System settings page.

// An ordinary launch while this profile's client runs hands its link (or a
// "show" request) to that window. True means the request was delivered and
// this process should exit.
bool application_desktop_forward_existing(ConstSlice<wchar_t> command_line) noexcept;
// Before the client starts: its launch (startup flag, link) and the profile's
// single-instance claim.
void application_desktop_prepare(ConstSlice<wchar_t> command_line) noexcept;
// SW_* for the first ShowWindow (Start minimized).
int application_desktop_initial_show() noexcept;
// Publishes the window for later launches and creates the tray icon.
void application_desktop_window_created() noexcept;
// Frame tick: tray state and pending link navigation.
void application_desktop_tick(u64 now_ms) noexcept;
#if defined(_WIN32)
// Tray callbacks, TaskbarCreated and forwarded launches.
bool application_desktop_window_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) noexcept;
#endif
// WM_CLOSE: true when the window was hidden to the tray instead of closing.
bool application_desktop_close_to_tray() noexcept;
// Tray Quit and update restarts close for real.
void application_desktop_request_quit() noexcept;
void application_desktop_release() noexcept;
// Queues a discord:// link for navigation once the account is ready.
bool application_desktop_open_link(const ConstSlice<wchar_t>& url) noexcept;

// The executable that startup, protocol and notification registrations name:
// this process's MaiCord.exe, which an update replaces in place (on Linux, the
// AppImage file when it runs from one). Empty when it does not fit `capacity`.
inline ConstSlice<wchar_t> application_desktop_registration_executable(wchar_t* buffer, u32 capacity) noexcept {
    if (!buffer || !capacity)
        return {};
#if !defined(_WIN32)
    // An AppImage runs from a mount that changes with every launch: the registration names the file the user started.
    if (const u32 image = desktop_appimage_path(buffer, capacity))
        return {buffer, image};
#endif
    const u32 count = executable_path(buffer, capacity);
    if (!count || count >= capacity)
        return {};
    return {buffer, count};
}

// Settings → System (desktop_settings.cpp).
bool application_desktop_settings_project(SettingBinding, UiSettingsControl*) noexcept;
bool application_desktop_settings_activate(SettingBinding, bool checked) noexcept;
} // namespace maicord::app_detail
