#pragma once
// Private to the plugin manager (plugin_manager_install.cpp, plugin_manager_runtime.cpp).
#include "maicord/app/plugin_manager_install.hpp"
#include "maicord/core/memory.hpp"

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
// A missing state.bin is an empty state.
inline AcceptanceRecordResult load_state(const PluginManagerPaths& paths, PluginManagerState* state) noexcept {
    reset_value(state);
    const auto loaded = plugin_manager_state_load({paths.state, paths.state_count}, state);
    return loaded.error == AcceptanceRecordError::missing ? AcceptanceRecordResult{} : loaded;
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
