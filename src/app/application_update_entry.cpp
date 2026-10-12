#include "application_updates.hpp"
#include "application_update_startup.hpp"
#include "maicord/app/account_host.hpp"
#include "maicord/app/update_log.hpp"

#include <Windows.h>

// Build inputs from CMake (MAICORD_UPDATE_REPOSITORY, MAICORD_UPDATE_PUBLIC_KEY,
// MAICORD_UPDATE_PREVIEW and the project version). Only this translation unit
// sees them, so changing the publisher configuration rebuilds one file.
#if !defined(MAICORD_UPDATE_REPOSITORY)
#define MAICORD_UPDATE_REPOSITORY ""
#endif
#if !defined(MAICORD_UPDATE_PUBLIC_KEY)
#define MAICORD_UPDATE_PUBLIC_KEY ""
#endif
#if !defined(MAICORD_UPDATE_PREVIEW)
#define MAICORD_UPDATE_PREVIEW 0
#endif
#if !defined(MAICORD_VERSION_MAJOR)
#define MAICORD_VERSION_MAJOR 0
#define MAICORD_VERSION_MINOR 0
#define MAICORD_VERSION_PATCH 0
#endif

namespace maicord::app_detail {
namespace {
constexpr char repository[] = MAICORD_UPDATE_REPOSITORY;
constexpr char public_key[] = MAICORD_UPDATE_PUBLIC_KEY;
static_assert((sizeof(repository) == 1) == (sizeof(public_key) == 1),
    "Set both MAICORD_UPDATE_REPOSITORY and MAICORD_UPDATE_PUBLIC_KEY, or neither.");
static_assert(sizeof(public_key) == 1 || sizeof(public_key) == 131, "MAICORD_UPDATE_PUBLIC_KEY is 130 hex digits.");
static_assert(update_version_components_valid(MAICORD_VERSION_MAJOR, MAICORD_VERSION_MINOR, MAICORD_VERSION_PATCH, MAICORD_UPDATE_PREVIEW));
constexpr u64 installed_version =
    update_version_number(MAICORD_VERSION_MAJOR, MAICORD_VERSION_MINOR, MAICORD_VERSION_PATCH, MAICORD_UPDATE_PREVIEW);
void write_update_log(LogLevel level, const char* category, const char* line) noexcept { log_write(level, category, line); }
void update_log_session(bool open) noexcept { if (open) log_open(); else log_close(); }
} // namespace

UpdateServiceConfig application_update_build_config() noexcept {
    // Every updater path starts here, so this routes its diagnostics.
    g_update_log_sink = write_update_log;
    g_update_log_session = update_log_session;
    UpdateServiceConfig config{};
    if (!update_service_config({repository, sizeof(repository) - 1}, {public_key, sizeof(public_key) - 1}, installed_version, &config))
        update_service_config({}, {}, installed_version, &config);
    return config;
}

void application_updates_confirm_startup() noexcept {
    UpdateHandoffChild* child = application_update_startup_child();
    if (!child) return;
    // Independent local admission of the sibling account module's API; no
    // account start, login or network is involved.
    AccountHost host{};
    const bool admitted = account_host_load(&host).ok() && account_host_api_valid(host.api) && !host.instance && !host.login_instance;
    const bool confirmed = admitted && update_handoff_child_acknowledge(child, true).ok();
    account_host_release(&host);
    update_handoff_child_release(child);
    // An unconfirmed child exits promptly so the coordinator rolls back.
    if (!confirmed) ExitProcess(kApplicationUpdateStartupRefusalExitCode);
}

} // namespace maicord::app_detail
