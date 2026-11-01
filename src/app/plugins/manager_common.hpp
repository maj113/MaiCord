#pragma once
// Private to the plugin manager (manager_install.cpp, manager_runtime.cpp).
#include "maicord/app/plugins/manager_install.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::plugin_manager_detail {

inline constexpr u32 kInstallerTimeoutMs = 30000;

inline PluginManagerResult failure(PluginManagerError error) noexcept {
    PluginManagerResult r{};
    r.error = error;
    return r;
}
inline PluginManagerResult record_failure(AcceptanceRecordResult result) noexcept {
    PluginManagerResult r = failure(PluginManagerError::records);
    r.record_error = result.error;
    r.native_error = result.native_error;
    return r;
}
// A missing record is an empty one.
template <typename T> inline AcceptanceRecordResult load_record(AcceptanceRecordResult (*load)(ConstSlice<wchar_t>, T*) noexcept,
    const wchar_t* path, u32 count, T* value) noexcept {
    reset_value(value);
    const auto loaded = load({path, count}, value);
    return loaded.error == AcceptanceRecordError::missing ? AcceptanceRecordResult{} : loaded;
}
inline AcceptanceRecordResult load_state(const PluginManagerPaths& paths, PluginManagerState* state) noexcept {
    return load_record(plugin_manager_state_load, paths.state, paths.state_count, state);
}
inline AcceptanceRecordResult load_journal(const PluginManagerPaths& paths, PluginJournal* journal) noexcept {
    return load_record(plugin_journal_load, paths.journal, paths.journal_count, journal);
}
inline AcceptanceRecordResult load_providers(const PluginManagerPaths& paths, PluginProviders* providers) noexcept {
    return load_record(plugin_providers_load, paths.providers, paths.providers_count, providers);
}
// The local inventory of plugins\ in work (its scan handles are released). false only when the directory
// exists and cannot be read; a missing directory is an empty inventory.
inline bool scan_inventory(const PluginManagerPaths& paths, PluginManagerWork* work) noexcept {
    clear_bytes(&work->catalog, sizeof(work->catalog));
    auto scanned = native_plugin_catalog_initialize(&work->catalog, {paths.plugins, paths.plugins_count},
        {work->entries, kNativePluginCatalogMaximumEntries});
    if (scanned.ok()) scanned = native_plugin_catalog_refresh(&work->catalog);
    const bool missing = scanned.win32_error == ERROR_FILE_NOT_FOUND || scanned.win32_error == ERROR_PATH_NOT_FOUND;
    const bool released = native_plugin_catalog_release(&work->catalog).ok();
    if (!scanned.ok()) work->catalog.count = 0;
    return (scanned.ok() || missing) && released;
}
inline PluginManagerEntry* state_entry(PluginManagerState& state, u64 owner) noexcept {
    for (u32 i = 0; i < state.count && i < kPluginManagerStateMaximumEntries; ++i)
        if (state.entries[i].owner == owner) return &state.entries[i];
    return nullptr;
}
inline NativePluginPreferencesResult load_trust(const PluginManagerPaths& paths, NativePluginPreferences* trust) noexcept {
    NativePluginPreferencesIo io{};
    const auto loaded = native_plugin_preferences_load_file(&io, paths.trust, trust);
    const auto released = native_plugin_preferences_release(&io);
    return loaded.ok() ? released : loaded;
}
inline NativePluginPreferencesResult save_trust(const PluginManagerPaths& paths, const NativePluginPreferences& trust) noexcept {
    NativePluginPreferencesIo io{};
    const auto saved = native_plugin_preferences_save_file(&io, paths.trust, trust);
    const auto released = native_plugin_preferences_release(&io);
    return saved.ok() ? released : saved;
}
inline const NativePluginPreference* trusted_owner(const NativePluginPreferences& trust, u64 owner) noexcept {
    for (u32 i = 0; i < trust.count && i < kNativePluginPreferencesMaximumEntries; ++i)
        if (trust.entries[i].owner_id == owner) return &trust.entries[i];
    return nullptr;
}

} // namespace maicord::plugin_manager_detail
