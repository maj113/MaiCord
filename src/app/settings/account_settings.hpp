#pragma once
#include "maicord/app/account/account_module_api.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "catalog.hpp"

namespace maicord::app_detail {
// My Account, Profiles, Devices, Connected Apps, Data & Privacy's account rows and Messaging Permissions' blocked and
// ignored accounts. Only the open page's reads
// load, each time it opens; every change runs as a flow of
// dialogs (text forms) whose requests go on the DETAILS lane, verifying with MFA or the password first when Discord
// asks (Userdoccers: authentication#mfa-verification). Texts borrowed by projected controls live until release.
void application_account_settings_visible(SettingsPanel) noexcept;
bool application_account_settings_project(SettingBinding, UiSettingsControl*, UiSettingsControlPresentation*) noexcept;
// Expands a list binding (devices_list, apps_list, connections_list, security_keys_list, blocked_list, ignored_list)
// into rows after its category; returns the number written.
u32 application_account_settings_list(SettingBinding, Slice<UiSettingsControl>, Slice<UiSettingsControlPresentation>) noexcept;
// Connected Apps' rows (every connection's display options and every authorized app) and Messaging Permissions' rows
// (every blocked and ignored account) project into this bank.
inline constexpr u32 kAccountSettingsBankRows = 192;
bool application_account_settings_bank(UiSettingsControl** controls, UiSettingsControlPresentation** presentation) noexcept;
// checked is a toggle's new state; choice the pressed button of a button row.
bool application_account_settings_activate(SettingBinding, bool checked, u32 choice) noexcept;
bool application_account_settings_activate_row(u64 control_id, bool checked, u32 choice) noexcept;
bool application_account_settings_pending(u64 now_ms) noexcept;
u64 application_account_settings_next_wake() noexcept;
bool application_account_settings_submit(u64 request_id, u64 now_ms) noexcept;
bool application_account_settings_response(const RestClientResponse&, Utf8View body) noexcept;
void application_account_settings_release() noexcept;
#if !defined(_WIN32)
// The security-key ceremony's wakes on Linux (platform/desktop/webauthn.hpp) for a flow of these pages (a registration or
// a verification): the key wants a PIN, or the ceremony ended. False when the token is not one of these pages'.
bool application_account_settings_webauthn_progress(u64 token, u64 kind) noexcept;
#endif

// What the pages can do with the account right now; reads load only when ready.
enum class AccountSettingsConnection : u8 { offline, connecting, action_required, ready };
AccountSettingsConnection application_account_settings_connection() noexcept;
// True once per connection change, and once whenever the blocked or ignored accounts change while Messaging
// Permissions shows them; the settings host re-projects on it.
bool application_account_settings_changed() noexcept;
// Test builds only (fake-server UI probes, tools/test_user_settings_wayland.sh): opens Settings once at the page or
// setting key MAICORD_TEST_USER_SETTINGS names, as soon as the account is ready.
void application_account_settings_test_open() noexcept;
} // namespace maicord::app_detail
