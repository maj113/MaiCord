#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/platform/desktop_integration.hpp"
#include "maicord/platform/system_tray.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "application_state.hpp"
#include "settings_catalog.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// The tray, taskbar and notification mark in the current theme's colours.
inline TrayIconPalette application_tray_palette() noexcept {
    const auto argb = [](const Color& color) noexcept {
        const auto byte = [](float v) noexcept { return static_cast<u32>((v < 0 ? 0 : v > 1 ? 1 : v) * 255.f + .5f); };
        return 0xFF000000u | (byte(color.red) << 16) | (byte(color.green) << 8) | byte(color.blue);
    };
    const auto& tokens = application_theme().tokens;
    const auto& brand = tokens.accent.brand;
    // The lower edge of the tile is the brand colour at 78% brightness.
    const Color shade{brand.red * .78f, brand.green * .78f, brand.blue * .78f, 1};
    return {argb(brand), argb(shade), argb(tokens.accent.success), argb(tokens.accent.danger),
            argb(tokens.text.on_accent)};
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
// Tray callbacks, TaskbarCreated and forwarded launches.
bool application_desktop_window_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) noexcept;
// WM_CLOSE: true when the window was hidden to the tray instead of closing.
bool application_desktop_close_to_tray() noexcept;
// Tray Quit and update restarts close for real.
void application_desktop_request_quit() noexcept;
void application_desktop_release() noexcept;
// Queues a discord:// link for navigation once the account is ready.
bool application_desktop_open_link(ConstSlice<wchar_t> url) noexcept;

// The executable that startup, protocol and notification registrations name:
// this process's MaiCord.exe, which an update replaces in place. Empty when it
// does not fit `capacity`.
inline ConstSlice<wchar_t> application_desktop_registration_executable(wchar_t* buffer, u32 capacity) noexcept {
    if (!buffer || !capacity)
        return {};
    const u32 count = GetModuleFileNameW(nullptr, buffer, capacity);
    if (!count || count >= capacity)
        return {};
    return {buffer, count};
}

// Settings → System (application_desktop_settings.cpp).
bool application_desktop_settings_project(SettingBinding, UiSettingsControl*) noexcept;
bool application_desktop_settings_activate(SettingBinding, bool checked) noexcept;
} // namespace maicord::app_detail
