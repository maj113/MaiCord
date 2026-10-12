#pragma once
#include "application_plugins.hpp"

namespace maicord::app_detail {
inline constexpr u32 kApplicationPluginSettingsMaximumControls = 21;
inline constexpr Utf8View kApplicationPluginSettingsDescription =
    utf8_literal("Add features to MaiCord.");
struct ApplicationPluginSettingsLoaded {
    u64 owner_id;
    u32 slot;
    Utf8View label;
    bool enabled;
    const NativePluginPreference* identity{};
};
struct ApplicationPluginSettingsState {
    const NativePluginCatalog* catalog{};
    u32 first{}, remembered{};
    bool discovered{}, busy{}, save_pending{};
    Utf8View status{};
    NativePluginError plugin_error{};
    ConstSlice<ApplicationPluginSettingsLoaded> loaded{};
    bool install_retry{};
    // Loading a DLL without a manifest is offered only in Developer Mode.
    bool developer_mode = true;
    const NativePluginPreferences* preferences{};
};
// Pure borrowed-descriptor projection. The caller owns the 21-control bank and
// eight visible text banks until the settings scene has released its views.
inline u32 application_plugins_settings_project(const ApplicationPluginSettingsState& state,
    Slice<UiSettingsControl> controls, Slice<ApplicationPluginCatalogText> text) noexcept {
    if (!controls.data || controls.count < kApplicationPluginSettingsMaximumControls ||
        !text.data || text.count < kApplicationPluginCatalogWindow || state.loaded.count > 8 ||
        (state.loaded.count && !state.loaded.data)) return 0;
    ApplicationPluginCatalogLoaded active[8]{};
    for (u32 i = 0; i < state.loaded.count; ++i) {
        const auto& loaded = state.loaded[i];
        if ((!loaded.owner_id && loaded.enabled) || loaded.slot >= 8 || !utf8_validate(loaded.label).valid) return 0;
        for (u32 j = 0; j < i; ++j)
            if (state.loaded[j].slot == loaded.slot ||
                (loaded.owner_id && state.loaded[j].owner_id == loaded.owner_id)) return 0;
        active[i] = {loaded.enabled ? loaded.owner_id : 0, loaded.identity};
    }
    const bool manual = state.developer_mode || state.save_pending;
    const u32 fixed = manual ? 2 : 1;
    const u32 catalog_count = application_plugins_catalog_project(state.catalog, state.first,
        state.discovered, state.busy, {active, state.loaded.count},
        {controls.data + fixed, 11}, text, state.remembered, state.preferences);
    if (!catalog_count) return 0;
    const auto error = state.plugin_error == NativePluginError::unsupported ?
        utf8_literal("This plugin's settings aren't supported. Update the plugin.") :
        state.plugin_error == NativePluginError::page ?
        utf8_literal("Couldn't open this plugin page. Try again.") :
        utf8_literal("The plugin couldn't complete this action. Try again.");
    const Utf8View status = state.plugin_error == NativePluginError::none ? state.status : error;
    if (manual)
        controls[0] = {1, state.save_pending ? utf8_literal("Save plugins") : utf8_literal("Load plugin"), status,
            UiSettingsControlKind::action, !state.busy, false, {}, 0,
            state.save_pending ? utf8_literal("Try again") : utf8_literal("Choose DLL")};
    controls[fixed - 1] = {kApplicationPluginInstallAction, utf8_literal("Install or update plugin"),
        state.install_retry ? utf8_literal("Finish the current installation first.") :
            manual ? utf8_literal("Choose a plugin manifest and its DLL.") : status,
        UiSettingsControlKind::action, !state.busy || state.install_retry, false, {}, 0,
        state.install_retry ? utf8_literal("Try again") : utf8_literal("Choose plugin")};
    u32 count = fixed + catalog_count;
    for (u32 i = 0; i < state.loaded.count; ++i) {
        const auto& loaded = state.loaded[i];
        controls[count++] = {100 + loaded.slot, loaded.label,
            utf8_literal("Unload this plugin and remove its settings pages."), UiSettingsControlKind::action,
            !state.busy, false, {}, 0, utf8_literal("Unload")};
    }
    return count;
}
}
