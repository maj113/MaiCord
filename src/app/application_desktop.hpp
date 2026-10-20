#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/platform/desktop_integration.hpp"
#include "maicord/platform/system_tray.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "application_state.hpp"
#include "application_update_startup.hpp"
#include "settings_catalog.hpp"
#include "maicord/core/memory.hpp"
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

// Before the update dispatcher: an ordinary launch while this profile's
// client runs hands its link (or a "show" request) to that window. True
// means the request was delivered and this process should exit.
bool application_desktop_forward_existing(ConstSlice<wchar_t> command_line) noexcept;
// After the update gate admitted this process to run the client. An update
// child takes its launch (startup flag, link) from the dispatcher's
// environment; every launch clears it so restarts never replay a link.
void application_desktop_prepare(ConstSlice<wchar_t> command_line, bool update_child) noexcept;
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

// The executable that startup, protocol and notification registrations name. A
// generation folder is deleted by a later update, so this is the dispatcher
// (the stable MaiCord.exe). Empty when it does not fit `capacity`.
inline ConstSlice<wchar_t> application_desktop_registration_executable(wchar_t* buffer, u32 capacity) noexcept {
    wchar_t module[1024], inherited[1024];
    const u32 m = GetModuleFileNameW(nullptr, module, 1024);
    if (!buffer || !m || m >= 1024)
        return {};
    u32 d = GetEnvironmentVariableW(kApplicationUpdateDispatcherVariable, inherited, 1024);
    const DWORD attributes = d && d < 1024 ? GetFileAttributesW(inherited) : INVALID_FILE_ATTRIBUTES;
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        d = 0;
    const auto path = application_update_startup_registration_path({module, m}, {inherited, d});
    if (!path.count || path.count >= capacity)
        return {};
    copy_bytes(buffer, path.data, path.count * sizeof(wchar_t));
    buffer[path.count] = 0;
    return {buffer, path.count};
}

// Settings → System (application_desktop_settings.cpp).
bool application_desktop_settings_project(SettingBinding, UiSettingsControl*) noexcept;
bool application_desktop_settings_activate(SettingBinding, bool checked) noexcept;
} // namespace maicord::app_detail
