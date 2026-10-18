#pragma once
#include "maicord/app/plugin_host.hpp"
#include "maicord/app/plugin_manager_runtime.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

// ---- compiled configuration (application_plugin_browser_entry.cpp) ----------------------------
// MAICORD_PLUGIN_CATALOG_REPOSITORY and the two official keys (current and
// next). No key means the official catalog is not configured for this build.
struct PluginBrowserConfig {
    u32 repository_bytes, key_count;
    char repository[kUpdateRepositoryMaximumBytes];
    u8 keys[2][65];
};
PluginBrowserConfig application_plugin_browser_build_config() noexcept;

// ---- the catalog worker (application_plugin_browser_worker.cpp) -------------------------------
// One background thread modeled on the updater's: checks on a 24 h schedule
// with backoff only when a catalog plugin is installed or the user browsed or
// asked; network buffers and index work are allocated per operation. All
// catalog files are touched here, never on the UI thread (startup recovery
// excepted). Results publish through a revisioned view and a notify event.
enum class PluginBrowserCommandKind : u8 {
    none,
    operation,       // install, update or rollback: prepare (download and stage)
    uninstall,       // journal the uninstall and remove its trust
    finish,          // settle the last prepared operation after the installer
    probe,           // add provider: parse the address and fetch without admitting
    probe_key,       // "Trust new key": fetch the provider's current key
    add_provider,    // confirmed probe: save, admit and cache
    trust_key,       // confirmed key change
    cancel_probe,
    remove_provider,
    official_flags,
    provider_flags,
    plugin_flags,
};
struct PluginBrowserCommand {
    PluginBrowserCommandKind kind;
    PluginJournalOperation operation;
    bool enable_after, delete_data, allow_unsigned;
    u32 set, clear;
    u64 provider, owner, version_code, approved, held_code;
    u32 text_bytes;
    char text[kPluginProviderAddressMaximumBytes];
};
enum class PluginBrowserPhase : u8 { idle, checking, working };
struct PluginBrowserWorkerView {
    u64 revision;
    PluginBrowserPhase phase;
    u64 checked_unix;                 // the last check that reached a provider
    u32 offers, staged;               // staged: automatic updates waiting for the next start
    u32 key_changed_count;
    u64 key_changed[kPluginProviderMaximum];
    u64 revoked_serial;               // changes when plugins must be turned off now
    u32 revoked_count;
    PluginRevokedPlugin revoked[kPluginManagerStateMaximumEntries];
    u64 snapshot_serial;              // a new snapshot waits in the worker
    u64 command_serial;               // the completed command
    PluginManagerResult manager;
    PluginManagerPrepared prepared;
    PluginManagerSettled settled;
    AcceptanceRecordResult record;
    PluginProviderAddressError address_error;
    PluginProviderAddress address;
    PluginProviderProbe probe;
    PluginProviderAddResult add;
};
struct PluginBrowserWorkerOptions {
    UpdateTransport transport;    // tests; null get: HTTPS per operation
    const wchar_t* base;          // tests; null: %LOCALAPPDATA%\MaiCord
    u32 base_count;
    u64 first_check_delay, first_check_jitter; // seconds after start, when the privacy rule allows
    u64 (*clock)() noexcept;      // tests; null: wall clock
};
bool plugin_browser_worker_start(const PluginBrowserConfig&, const PluginBrowserWorkerOptions&) noexcept;
void plugin_browser_worker_stop() noexcept;
bool plugin_browser_worker_running() noexcept;
HANDLE plugin_browser_worker_notification() noexcept;
// Copies the view when its revision differs from seen; false otherwise.
bool plugin_browser_worker_view(PluginBrowserWorkerView* view, u64 seen) noexcept;
// manual: the user asked (Check now, Refresh); provider 0 checks every enabled provider.
bool plugin_browser_worker_check(bool manual, u64 provider = 0) noexcept;
// Browse was opened: checks may run from now on, and once per hour on open.
void plugin_browser_worker_browse() noexcept;
// A snapshot is wanted (after every change) until cleared.
void plugin_browser_worker_want_snapshot(bool want) noexcept;
// Ownership passes to the caller (release with plugin_catalog_snapshot_release).
PluginCatalogSnapshot* plugin_browser_worker_take_snapshot() noexcept;
// One command at a time; false while another is pending or running.
bool plugin_browser_worker_command(const PluginBrowserCommand&, u64* serial) noexcept;
bool plugin_browser_worker_busy() noexcept;

// ---- UI (application_plugin_browser.cpp) ------------------------------------------------------
void application_plugin_browser_initialize() noexcept;
void application_plugin_browser_release() noexcept;
HANDLE application_plugin_browser_notification() noexcept;
void application_plugin_browser_poll() noexcept;
// Inside application_plugins_restore, before anything loads: journal
// recovery, the roll-forward of a staged automatic update, then the gate.
void application_plugin_browser_startup() noexcept;
// kPluginGate* reasons restore and Enable honor.
u8 application_plugin_browser_gate(u64 owner) noexcept;
// The bundle installer finished (any operation); catalog operations settle.
void application_plugin_browser_installer_complete(const NativePluginBundleInstallResult&) noexcept;
// Settings > Plugins: projects the page into the browser bank; 0 when the
// bank cannot be allocated (the caller keeps the local page).
u32 application_plugin_browser_project(UiSettingsControl** controls, UiSettingsControlPresentation** presentation,
    NativePluginError plugin_error, bool* changed) noexcept;
bool application_plugin_browser_activate(u64 control_id, const UiSettingsActivation& request) noexcept;
void application_plugin_browser_page_visible(bool visible) noexcept;
// The settings owner released its scene: the bank is no longer borrowed.
void application_plugin_browser_settings_released() noexcept;

} // namespace maicord::app_detail
