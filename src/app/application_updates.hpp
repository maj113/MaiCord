#pragma once

#include "maicord/app/update_service.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "settings_catalog.hpp"

namespace maicord::app_detail {

// Exe-only build inputs (application_update_entry.cpp): repository, publisher
// key and the installed version compiled from CMake.
UpdateServiceConfig application_update_build_config() noexcept;
// Child process: confirm startup once the account module API is admitted.
void application_updates_confirm_startup() noexcept;

enum class ApplicationUpdateState : u8 {
    disabled, idle, checking, downloading, installing, up_to_date, ready, restarting, error, rate_limited,
};
struct ApplicationUpdatesView {
    ApplicationUpdateState state;
    UpdateChannel channel;
    UpdateServiceError error;
    u64 own_version, ready_version, rollback_version;
    u64 received, total;
};
// Settings rows for one view. Text lives in the caller's scratch until the
// next projection. Returns false when the row is hidden.
struct ApplicationUpdatesText { char label[96], description[192]; };
bool application_updates_project_view(const ApplicationUpdatesView&, SettingBinding, ApplicationUpdatesText*,
    UiSettingsControl*, UiSettingsControlPresentation*) noexcept;

// UI thread. A background worker checks every 24 h (jitter, backoff,
// server retry times) and on demand; nothing here blocks on the network.
bool application_updates_initialize(const UpdateServiceConfig&) noexcept;
void application_updates_release() noexcept;
HANDLE application_updates_notification() noexcept;
// Refreshes Settings on change. True once a new version confirmed startup
// and this window should close.
bool application_updates_poll() noexcept;
bool application_updates_project(SettingBinding, UiSettingsControl*, UiSettingsControlPresentation*) noexcept;
bool application_updates_activate(SettingBinding, u32 choice) noexcept;

} // namespace maicord::app_detail
