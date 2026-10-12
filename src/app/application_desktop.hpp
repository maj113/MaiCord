#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/platform/desktop_integration.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "settings_catalog.hpp"

namespace maicord::app_detail {
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

// Settings → System (application_desktop_settings.cpp).
ConstSlice<wchar_t> application_desktop_registration_executable(wchar_t* buffer, u32 capacity) noexcept;
bool application_desktop_settings_project(SettingBinding, UiSettingsControl*) noexcept;
bool application_desktop_settings_activate(SettingBinding, bool checked) noexcept;
} // namespace maicord::app_detail
