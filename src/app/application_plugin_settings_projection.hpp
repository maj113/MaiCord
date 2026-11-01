#pragma once
#include "application_plugins.hpp"
#include "maicord/ui/settings_scene.hpp"

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
namespace application_plugin_catalog_detail {
inline bool identity_matches(const NativePluginPreference& identity, const NativePluginManifest& manifest) noexcept {
    if (identity.owner_id != manifest.owner_id || identity.abi_version != manifest.abi_version ||
        identity.dll_bytes != manifest.dll_bytes || identity.dll_bytes > kNativePluginDllNameCapacity ||
        !bytes_equal(identity.sha256, manifest.sha256, 32)) return false;
    for (u32 i = 0; i < identity.dll_bytes; ++i) if (identity.dll[i] != ascii_lower(manifest.dll[i])) return false;
    return true;
}
inline __declspec(noinline) void append_text(ApplicationPluginCatalogText& output, Utf8View value) noexcept {
    if (output.bytes >= sizeof(output.text) - 3) return;
    for (u32 i = 0; i < value.bytes;) {
        const auto decoded = utf8_decode_one(value.data + i, value.bytes - i);
        if (!decoded.valid) return;
        const u32 width = decoded.width;
        if (width > value.bytes - i || width > sizeof(output.text) - 3 - output.bytes) {
            if (output.bytes <= sizeof(output.text) - 3) {
                output.text[output.bytes++] = '.'; output.text[output.bytes++] = '.'; output.text[output.bytes++] = '.';
            }
            return;
        }
        for (u32 j = 0; j < width; ++j) output.text[output.bytes++] = value.data[i++];
    }
}
// CP_UTF8 and WC_ERR_INVALID_CHARS (checked in application_plugins.cpp).
inline constexpr UINT kApplicationPluginCodePageUtf8 = 65001;
inline constexpr DWORD kApplicationPluginInvalidCharsError = 0x80;
inline void filename_text(ApplicationPluginCatalogText& output, const NativePluginCatalogEntry& entry) noexcept {
    reset_value(&output);
    for (u32 i = 0; i < entry.filename_characters && i < kNativePluginCatalogFilenameCapacity;) {
        const u32 width = entry.filename[i] >= 0xD800 && entry.filename[i] <= 0xDBFF &&
            i + 1 < entry.filename_characters && i + 1 < kNativePluginCatalogFilenameCapacity ? 2 : 1;
        char encoded[4];
        const int bytes = WideCharToMultiByte(kApplicationPluginCodePageUtf8, kApplicationPluginInvalidCharsError,
            entry.filename + i, static_cast<int>(width), encoded, static_cast<int>(sizeof(encoded)), nullptr,
            nullptr);
        const u32 before = output.bytes;
        append_text(output, bytes > 0 ? Utf8View{encoded, static_cast<u32>(bytes)} : utf8_literal("?"));
        if (output.bytes == sizeof(output.text) || output.bytes == before) break;
        i += width;
        if (output.bytes >= sizeof(output.text) - 3) break;
    }
    if (!output.bytes) append_text(output, utf8_literal("Unreadable plugin"));
}
inline void permissions_text(ApplicationPluginCatalogText& output, u64 permissions) noexcept {
    if (!permissions) return;
    append_text(output, utf8_literal("Permissions: "));
    bool first = true;
    for (u32 bit = 0; bit < kNativePluginPermissionCount; ++bit)
        if (permissions & (1ull << bit)) {
            if (!first) append_text(output, utf8_literal(", "));
            first = false;
            append_text(output, native_plugin_permission_name(bit));
        }
    append_text(output, utf8_literal(". "));
}
inline Utf8View entry_error(NativePluginCatalogError error) noexcept {
    if (error == NativePluginCatalogError::duplicate_owner || error == NativePluginCatalogError::duplicate_dll)
        return utf8_literal("Another plugin uses the same ID or file.");
    return utf8_literal("Couldn't read this plugin's manifest.");
}
}
inline __declspec(noinline) u32 application_plugins_catalog_project(const NativePluginCatalog* catalog, u32 first,
    bool discovered, bool busy, ConstSlice<ApplicationPluginCatalogLoaded> loaded,
    Slice<UiSettingsControl> controls, Slice<ApplicationPluginCatalogText> text, u32 remembered,
    const NativePluginPreferences* preferences) noexcept {
    using namespace application_plugin_catalog_detail;
    if (!controls.data || controls.count < 11 || !text.data || text.count < 8 ||
        loaded.count > 8 || (loaded.count && !loaded.data) || (catalog &&
         (catalog->count > 32 || catalog->count > catalog->entries.count || (catalog->count && !catalog->entries.data)))) return 0;
    const u32 total = catalog ? catalog->count : 0;
    if ((first % 8) || (total ? first >= total : first != 0)) return 0;
    controls[0] = {kApplicationPluginDiscoverAction, utf8_literal("Local plugins"),
        utf8_literal("Installed plugins"),
        UiSettingsControlKind::action, !busy, false, {}, 0, discovered ? utf8_literal("Refresh") : utf8_literal("Discover")};
    controls[1] = {kApplicationPluginPreviousAction, utf8_literal("Previous plugins"), {},
        UiSettingsControlKind::action, !busy && first != 0, false, {}, 0, utf8_literal("Previous")};
    if (!first && remembered) controls[1] = {kApplicationPluginForgetAction, utf8_literal("Saved plugin choices"),
        utf8_literal("Stop loading saved plugins at startup."),
        UiSettingsControlKind::action, !busy, false, {}, 0, utf8_literal("Forget all")};
    controls[2] = {kApplicationPluginNextAction, utf8_literal("More plugins"), {},
        UiSettingsControlKind::action, !busy && total - first > 8, false, {}, 0, utf8_literal("Next")};
    u32 count = 3;
    for (u32 i = 0; i < 8 && i < total - first; ++i) {
        const auto& entry = catalog->entries[first + i];
        bool active = false, exact = false, declared = false;
        for (u32 j = 0; j < loaded.count; ++j) if (loaded[j].owner_id && loaded[j].owner_id == entry.manifest.owner_id) {
            active = true;
            declared = loaded[j].identity != nullptr;
            exact = declared && identity_matches(*loaded[j].identity, entry.manifest);
            break;
        }
        auto& label = text[i]; reset_value(&label);
        const bool valid = entry.status == NativePluginCatalogError::none && native_plugin_manifest_valid(entry.manifest);
        Utf8View name{}, description{};
        if (valid) {
            name = {entry.manifest.name, entry.manifest.name_bytes};
            append_text(label, utf8_literal("Version "));
            append_text(label, {entry.manifest.version, entry.manifest.version_bytes});
            append_text(label, !active ? utf8_literal(" | Ready. ") : exact ? utf8_literal(" | Loaded. ") :
                declared ? utf8_literal(" · Another version is running. Unload it first.") :
                utf8_literal(" · A manually loaded plugin is running. Unload it first."));
            const auto* remembered_entry = preferences && !active ?
                native_plugin_preferences_find(*preferences, entry.manifest) : nullptr;
            if (remembered_entry && (remembered_entry->flags & native_plugin_preference_crashed))
                append_text(label, utf8_literal("Turned off because MaiCord closed while it was running. "));
            else if (remembered_entry && (remembered_entry->flags & native_plugin_preference_overran))
                append_text(label, utf8_literal("Turned off because it kept MaiCord waiting. "));
            if (!active || exact) {
                permissions_text(label, entry.manifest.permissions);
                append_text(label, {entry.manifest.description, entry.manifest.description_bytes});
            }
            description = {label.text, label.bytes};
        } else {
            filename_text(label, entry); name = {label.text, label.bytes}; description = entry_error(entry.status);
        }
        controls[count++] = {kApplicationPluginCatalogActionFirst + first + i, name, description,
            UiSettingsControlKind::action, valid && !busy && !active, false, {}, 0,
            !valid ? utf8_literal("Unavailable") : !active ? utf8_literal("Enable") : exact ? utf8_literal("Loaded") : utf8_literal("In use")};
    }
    return count;
}
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
