#pragma once
#include "maicord/app/account_module_api.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "settings_catalog.hpp"

namespace maicord::app_detail {
// My account, Profiles, Devices and Authorized apps. Reads load when their
// page opens; every mutation is confirmed in a dialog first and runs on the
// DETAILS lane. Texts borrowed by projected controls live until release.
void application_account_settings_visible(SettingsPanel) noexcept;
bool application_account_settings_project(SettingBinding, UiSettingsControl*, UiSettingsControlPresentation*) noexcept;
// Expands a list binding (devices_list, apps_list) into rows after its
// category; returns the number written.
u32 application_account_settings_list(SettingBinding, Slice<UiSettingsControl>, Slice<UiSettingsControlPresentation>) noexcept;
bool application_account_settings_activate(SettingBinding) noexcept;
bool application_account_settings_activate_row(u64 control_id) noexcept;
bool application_account_settings_pending(u64 now_ms) noexcept;
u64 application_account_settings_next_wake() noexcept;
bool application_account_settings_submit(u64 request_id, u64 now_ms) noexcept;
bool application_account_settings_response(const RestClientResponse&, Utf8View body) noexcept;
void application_account_settings_release() noexcept;
} // namespace maicord::app_detail
