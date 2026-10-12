#include "application_desktop.hpp"
#include "application_state.hpp"
#include "application_update_startup.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
struct Registered {
    u64 checked_ms;
    DesktopRegistration startup, protocol;
    bool known, startup_failed, protocol_failed;
} g_registered;

DesktopRegistryScope scope() noexcept {
    return desktop_registry_user_scope();
}
// Refreshes the registry view at most once a second while settings render.
void observe(bool force) noexcept {
    const u64 now = GetTickCount64();
    if (!force && g_registered.known && now - g_registered.checked_ms < 1000)
        return;
    wchar_t executable[1024];
    const auto path = application_desktop_registration_executable(executable, 1024);
    const auto s = scope();
    g_registered.startup = desktop_startup_query(s, path);
    g_registered.protocol = desktop_protocol_query(s, path);
    g_registered.checked_ms = now;
    g_registered.known = true;
}
} // namespace

ConstSlice<wchar_t> application_desktop_registration_executable(wchar_t* buffer, u32 capacity) noexcept {
    // A generation folder is deleted by a later update; register the
    // dispatcher (the stable MaiCord.exe), as the notification identity does.
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

bool application_desktop_settings_project(SettingBinding binding, UiSettingsControl* control) noexcept {
    if (!control)
        return false;
    switch (binding) {
    case SettingBinding::open_on_startup:
        observe(false);
        control->checked = g_registered.startup == DesktopRegistration::ours;
        if (g_registered.startup_failed)
            control->description = utf8_literal("Windows didn't save this change. Try again.");
        return true;
    case SettingBinding::start_minimized:
        observe(false);
        control->checked = g_app.preferences.start_minimized;
        // Only a launch from the startup entry starts minimized.
        control->enabled = g_registered.startup == DesktopRegistration::ours;
        return true;
    case SettingBinding::minimize_to_tray:
        control->checked = g_app.preferences.minimize_to_tray;
        return true;
    case SettingBinding::protocol_handler:
        observe(false);
        control->checked = g_registered.protocol == DesktopRegistration::ours;
        if (g_registered.protocol_failed)
            control->description = utf8_literal("Windows didn't save this change. Try again.");
        else if (g_registered.protocol == DesktopRegistration::other)
            control->description = utf8_literal("Another app opens discord:// links. Turn this on to use MaiCord.");
        return true;
    default:
        return false;
    }
}

bool application_desktop_settings_activate(SettingBinding binding, bool checked) noexcept {
    if (binding != SettingBinding::open_on_startup && binding != SettingBinding::protocol_handler)
        return false;
    wchar_t executable[1024];
    const auto path = application_desktop_registration_executable(executable, 1024);
    const auto s = scope();
    const bool startup = binding == SettingBinding::open_on_startup;
    const bool saved = path.count && (startup ? desktop_startup_set(s, path, checked) : desktop_protocol_set(s, path, checked));
    (startup ? g_registered.startup_failed : g_registered.protocol_failed) = !saved;
    if (!saved)
        log_write(LogLevel::warning, "desktop", startup ? "startup_registration_failed" : "protocol_registration_failed");
    observe(true);
    return saved;
}

} // namespace maicord::app_detail
