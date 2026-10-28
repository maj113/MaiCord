#include "application_plugins.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/text_bank.hpp"
#include "application_plugin_browser.hpp"
#include "application_plugins_runtime.hpp"
#include "application_plugin_settings_projection.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "maicord/app/plugin_module_loader.hpp"
#include "maicord/platform/file_picker.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/log.hpp"
#include "application_log_fields.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
static_assert(application_plugin_catalog_detail::kApplicationPluginCodePageUtf8 == CP_UTF8 &&
              application_plugin_catalog_detail::kApplicationPluginInvalidCharsError ==
                  WC_ERR_INVALID_CHARS);
struct ApplicationPluginCatalogOwner {
    NativePluginCatalog catalog;
    NativePluginCatalogEntry entries[kNativePluginCatalogMaximumEntries];
};
namespace {
bool local_catalog_directory(wchar_t*,u32*) noexcept;
bool advance_catalog(ApplicationPlugins&) noexcept;
void poll_install(ApplicationPlugins&) noexcept;
const NativePluginPreference* loaded_identity(const ApplicationPlugins& plugins, u32 slot) noexcept {
    const auto& module = plugins.modules[slot];
    const auto& identity = plugins.loaded_identities[slot];
    return module.enabled && module.generation && module.generation == plugins.loaded_identity_generations[slot] &&
        identity.owner_id == module.owner_id ? &identity : nullptr;
}
void capture_loaded_identity(ApplicationPlugins& plugins, u32 slot, const NativePluginManifest& manifest) noexcept {
    auto& identity = plugins.loaded_identities[slot];
    reset_value(&identity);
    identity.owner_id = manifest.owner_id;
    identity.abi_version = manifest.abi_version;
    identity.dll_bytes = manifest.dll_bytes;
    copy_bytes(identity.sha256, manifest.sha256, sizeof(identity.sha256));
    for (u32 i = 0; i < manifest.dll_bytes; ++i) identity.dll[i] = ascii_lower(manifest.dll[i]);
    plugins.loaded_identity_generations[slot] = plugins.modules[slot].generation;
}
// Settings > Plugins is the page on screen.
bool plugins_page_selected() noexcept {
    const auto* scene = g_app.renderer.settings_scene;
    return scene && scene->selected_page.owner_id == 1 && scene->selected_page.page_id == 4;
}
// The Settings window, account scope and profile a plugin operation started under.
struct ScopeStamp {
    const void* settings;
    u64 serial, profile;
};
ScopeStamp scope_stamp() noexcept {
    return {g_app.settings, g_app.plugin_account_scope_serial, g_app.account.status.profile_id};
}
bool scope_unchanged(const ScopeStamp& stamp) noexcept {
    return static_cast<const void*>(g_app.settings) == stamp.settings &&
        g_app.plugin_account_scope_serial == stamp.serial && g_app.account.status.profile_id == stamp.profile;
}
bool install_context(const ApplicationPlugins& plugins,bool detached=false) noexcept {
    if(g_app.plugins!=&plugins)return false;
    return static_cast<const void*>(g_app.settings)==plugins.install_settings&&
        application_settings_lifetime()==plugins.install_lifetime&&
        g_app.plugin_account_scope_serial==plugins.install_account_serial&&
        g_app.account.status.profile_id==plugins.install_profile&&g_app.account_ui&&!g_app.login.visible&&
        ((application_settings_active()&&plugins_page_selected())||(detached&&!application_settings_active()));
}
void preferences_error(NativePluginPreferencesResult result) noexcept {
    log_u64_pair(LogLevel::warning, "plugins", "preferences_error\0preferences_native_error",
                 static_cast<u64>(result.error), result.win32_error);
}
ApplicationPlugins* ensure() noexcept {
    if (g_app.plugins) return g_app.plugins;
    g_app.plugins = static_cast<ApplicationPlugins*>(VirtualAlloc(nullptr, sizeof(ApplicationPlugins),
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (g_app.plugins) {
        auto& plugins = *g_app.plugins;
        const auto loaded = native_plugin_preferences_load(&plugins.preferences_io, &plugins.preferences);
        if (!loaded.ok()) {
            preferences_error(loaded);
            plugins.status = utf8_literal("Couldn't load your saved plugins. Enable them again.");
        }
        // A marker left inside a plugin call means MaiCord exited during it.
        // Keep that plugin off until the user turns it on again.
        PluginCallMarker leftover{};
        if (application_plugins_sdk_open(plugins, &leftover) && leftover.present) {
            log_u64(LogLevel::warning, "plugins", "exited_during_call_owner", leftover.owner_id);
            NativePluginPreferences flagged = plugins.preferences;
            if (native_plugin_preferences_set_flags(&flagged, leftover.owner_id, native_plugin_preference_crashed)) {
                if (!application_plugins_save_choices(plugins, flagged)) {
                    plugins.preferences = flagged;
                    plugins.preferences_dirty = true;
                }
                application_plugins_set_notice(plugins,
                    utf8_literal("A plugin was turned off because MaiCord closed while it was running."));
            }
        }
    }
    return g_app.plugins;
}
u32 free_slot(const ApplicationPlugins& plugins) noexcept {
    for (u32 i = 0; i < 8; ++i)
        if (!plugins.modules[i].module && !plugins.modules[i].state && !plugins.modules[i].busy && !plugins.modules[i].integrity) return i;
    return 8;
}
Utf8View error_text(NativePluginModuleResult result) noexcept {
    if (result.error == NativePluginModuleError::invalid_path) return utf8_literal("Couldn't open this file. Choose a plugin DLL.");
    if (result.error == NativePluginModuleError::missing_export || result.error == NativePluginModuleError::query_failed)
        return utf8_literal("This isn't a compatible MaiCord plugin.");
    if (result.error == NativePluginModuleError::host)
        return utf8_literal("Couldn't enable this plugin. It may be incompatible or already enabled.");
    if (result.error == NativePluginModuleError::busy) return utf8_literal("The plugin is busy. Try again in a moment.");
    if (result.error == NativePluginModuleError::release_failed) return utf8_literal("Couldn't unload the plugin. Try again.");
    if (result.error == NativePluginModuleError::integrity) return utf8_literal("Couldn't verify the plugin. Refresh the list and try again.");
    if (result.error == NativePluginModuleError::metadata_mismatch || result.error == NativePluginModuleError::manifest)
        return utf8_literal("The plugin files don't match its manifest.");
    if (result.error == NativePluginModuleError::permissions)
        return utf8_literal("The plugin asked for permissions you didn't approve.");
    if (result.error == NativePluginModuleError::unsupported)
        return utf8_literal("This plugin needs a newer version of MaiCord.");
    return utf8_literal("Couldn't load the plugin. It may be missing dependencies.");
}
void report_error(NativePluginModuleResult result) noexcept {
    log_u64_pair(LogLevel::warning, "plugins", "module_error\0module_native_error", static_cast<u64>(result.error),
                 result.win32_error);
}
void module_failed(ApplicationPlugins& plugins, NativePluginModuleResult result) noexcept {
    plugins.status = error_text(result);
    report_error(result);
}
void set_label(ApplicationPlugins& plugins, u32 slot, const NativePluginManifest& manifest) noexcept {
    for (u32 i = 0; i < manifest.name_bytes; ++i) plugins.labels[slot][i] = manifest.name[i];
    plugins.label_bytes[slot] = manifest.name_bytes;
}
void copy_label(ApplicationPlugins& plugins, u32 index, ConstSlice<wchar_t> path) noexcept {
    u32 first = 0;
    for (u32 i = 0; i < path.count; ++i) if (path.data[i] == L'/' || path.data[i] == L'\\') first = i + 1;
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data + first,
        static_cast<int>(path.count - first), plugins.labels[index], 1024, nullptr, nullptr);
    plugins.label_bytes[index] = bytes > 0 ? static_cast<u32>(bytes) : 0;
}
} // namespace
bool application_plugins_save_choices(ApplicationPlugins& plugins, const NativePluginPreferences& candidate) noexcept {
    const auto released = native_plugin_preferences_release(&plugins.preferences_io);
    if (!released.ok()) { preferences_error(released); return false; }
    const auto saved = native_plugin_preferences_save(&plugins.preferences_io, candidate);
    if (saved.ok() || saved.committed) plugins.preferences = candidate;
    if (!saved.ok()) preferences_error(saved);
    return saved.ok() || saved.committed;
}
bool application_plugins_choose() noexcept {
    // A DLL without a manifest has no declared identity or permissions.
    if (!g_app.preferences.developer_mode) return false;
    auto* plugins = ensure();
    if (!plugins || application_plugins_busy()) return false;
    if (free_slot(*plugins) == 8) {
        plugins->status = utf8_literal("Plugin limit reached. Unload one first.");
        return false;
    }
    const auto result = native_file_picker_start(&plugins->picker, g_app.window, 1, utf8_literal("Load plugin"));
    plugins->selecting = result == NativeFilePickerError::none;
    plugins->picker_consumed = false;
    plugins->reopen_settings = false;
    plugins->status = plugins->selecting ? Utf8View{} :
        utf8_literal("Couldn't open the file picker. Try again.");
    return plugins->selecting;
}
HANDLE application_plugins_notification() noexcept {
    const auto* plugins=g_app.plugins;if(!plugins||plugins->install_retry)return nullptr;
    // The Developer Mode DLL picker keeps its completion event until release. Bundle
    // selection acknowledges that event and waits for the worker's exit instead.
    if(plugins->selecting)return plugins->installing&&plugins->picker_consumed?
        plugins->picker.thread:plugins->picker.notification;
    if(plugins->installing&&plugins->installer.runtime)
        return plugins->install_consumed?plugins->installer.thread:plugins->installer.notification;
    return nullptr;
}
void application_plugins_poll() noexcept {
    auto* plugins = g_app.plugins;
    if (plugins && !plugins->closing) application_plugins_sdk_poll(*plugins);
    if (plugins && !plugins->closing) application_plugins_render_poll();
    plugins = g_app.plugins;
    if(plugins&&plugins->installing&&!plugins->closing){
        if(!plugins->selecting)poll_install(*plugins);
        else {
            const auto selected=native_file_picker_status(&plugins->picker);
            if(selected.phase!=NativeFilePickerPhase::complete)return;
            if(!plugins->picker_consumed){
                if(selected.ok()&&selected.count==1&&install_context(*plugins)){
                    wchar_t directory[kNativePluginCatalogPathCapacity];u32 characters{};
                    const auto path=native_file_picker_path(&plugins->picker,0);
                    const auto started=local_catalog_directory(directory,&characters)?
                        native_plugin_bundle_install_start(&plugins->installer,path,{directory,characters}):NativePluginBundleInstallError::path;
                    plugins->status=started==NativePluginBundleInstallError::none?
                        utf8_literal("Installing plugin…"):
                        utf8_literal("Couldn't install the plugin. Check its manifest.");
                }else reset_value<Utf8View>(&plugins->status);
                plugins->picker_consumed=true;
            }
            if(!native_file_picker_release(&plugins->picker,0)){
                // The worker can exit between release's wait and this check;
                // only a release that still fails after its exit is a failure.
                if(plugins->picker.thread&&WaitForSingleObject(plugins->picker.thread,0)!=WAIT_OBJECT_0)return;
                if(!native_file_picker_release(&plugins->picker,0)){
                    plugins->install_retry=true;plugins->status=utf8_literal("Plugin installation didn't finish. Try again.");
                    application_settings_refresh_pages();return;
                }
            }
            plugins->selecting=false;
            if(!plugins->installer.runtime){plugins->installing=false;application_settings_refresh_pages();}
            else poll_install(*plugins);
        }
        if(g_app.plugins==plugins&&!plugins->installing)application_settings_refresh_pages();
        return;
    }
    if (!plugins || !plugins->selecting || plugins->closing) return;
    const auto result = native_file_picker_status(&plugins->picker);
    if (result.phase != NativeFilePickerPhase::complete) return;
    if (!plugins->picker_consumed) plugins->reopen_settings = application_settings_active();
    if (!plugins->picker_consumed && result.ok() && result.count == 1) {
        const u32 index = free_slot(*plugins);
        const auto path = native_file_picker_path(&plugins->picker, 0);
        auto* host = application_settings_plugin_host();
        if (index < 8 && host) {
            copy_label(*plugins, index, path);
            const auto stamp = scope_stamp();
            plugins->activating = true;
            application_plugins_sdk_bind_host(*plugins, host);
            // Developer Mode load: the user picked this DLL directly.
            const auto loaded = native_plugin_module_load(&plugins->modules[index], host, path, MC_PERM_ALL);
            if (!scope_unchanged(stamp)) {
                const auto rollback = native_plugin_module_unload(&plugins->modules[index]);
                plugins->reopen_settings = false;
                plugins->status = rollback.ok() ? Utf8View{} : error_text(rollback);
                if (!rollback.ok()) report_error(rollback);
                if (!loaded.ok()) report_error(loaded);
            } else {
                plugins->status = !loaded.ok() ? error_text(loaded) : plugins->modules[index].enabled ?
                    utf8_literal("Plugin enabled.") :
                    utf8_literal("Couldn't enable the plugin.");
                if (!loaded.ok()) report_error(loaded);
            }
            plugins->activating = false;
        } else plugins->status = utf8_literal("Plugin limit reached. Unload one first.");
    } else if (!plugins->picker_consumed) {
        plugins->status = result.error == NativeFilePickerError::cancelled ? Utf8View{} :
            utf8_literal("Couldn't read the plugin file.");
    }
    plugins->picker_consumed = true;
    // Completion keeps its path unchanged until the worker has exited and release
    // succeeds. Keep its signaled handle on a rare timeout so the next pump retries.
    if (!native_file_picker_release(&plugins->picker, 0)) return;
    plugins->selecting = false;
    const bool reopen = plugins->reopen_settings;
    plugins->reopen_settings = false;
    application_settings_refresh_pages();
    if (reopen && !application_settings_active()) application_settings_open();
}
bool application_plugins_unload(u32 index) noexcept {
    auto* plugins = g_app.plugins;
    if (!plugins || application_plugins_busy() || index >= 8 || !application_plugins_present(index)) return false;
    plugins->activating = true;
    NativePluginPreferences disabled = plugins->preferences;
    if (native_plugin_preferences_remove_owner(&disabled, plugins->modules[index].owner_id)) {
        if (!application_plugins_save_choices(*plugins, disabled)) {
            plugins->status = utf8_literal("Couldn't unload the plugin. Try again.");
            plugins->activating = false; application_settings_refresh_pages(); return false;
        }
        plugins->preferences_dirty = false;
    }
    const bool reopen = application_settings_active();
    const bool plugin_page = plugins_page_selected();
    const auto stamp = scope_stamp();
    const auto result = native_plugin_module_unload(&plugins->modules[index]);
    plugins->activating = false;
    plugins->status = result.ok() ? utf8_literal("Plugin unloaded.") : error_text(result);
    if (result.ok()) {
        plugins->label_bytes[index] = 0;
        reset_value(&plugins->loaded_identities[index]);
        plugins->loaded_identity_generations[index] = 0;
    }
    else report_error(result);
    application_settings_refresh_pages();
    if (reopen && scope_unchanged(stamp) && !application_settings_active()) {
        if (plugin_page) application_settings_plugin_catalog_reopen();
        else application_settings_open();
    }
    return result.ok();
}
bool application_plugins_release() noexcept {
    auto* plugins = g_app.plugins;
    if (!plugins) return true;
    // Stop/DLL detach can run nested native code. Only the outer release
    // may clear this flag; nested attempts must neither clear it nor start a picker.
    if (plugins->activating || plugins->closing) return false;
    bool borrowing_backing = plugins->catalog != nullptr;
    for (const auto& module : plugins->modules)
        borrowing_backing = borrowing_backing || module.module || module.state || module.integrity;
    plugins->closing = true;
    if(plugins->installer.runtime){
        native_plugin_bundle_install_cancel(&plugins->installer);
        native_plugin_bundle_install_acknowledge(&plugins->installer);
        if(!native_plugin_bundle_install_release(&plugins->installer,kAccountStopTimeoutMs)){
            const auto pending=native_plugin_bundle_install_status(&plugins->installer);
            plugins->install_retry=pending.phase==NativePluginBundleInstallPhase::complete;
            plugins->status=utf8_literal("Plugin installation is still finishing.");
            plugins->closing=false;return false;
        }
        plugins->installing=false;plugins->install_retry=false;
    }
    if (plugins->picker.runtime) {
        // A committed selection may outlive cancellation while its worker exits.
        // Shutdown abandons it even when the join needs another pump.
        plugins->picker_consumed = true;
        plugins->reopen_settings = false;
        native_file_picker_cancel(&plugins->picker);
        if (!native_file_picker_release(&plugins->picker, kAccountStopTimeoutMs)) { plugins->closing = false; return false; }
        plugins->selecting = false;
        plugins->picker_consumed = false;
        plugins->reopen_settings = false;
    }
    for (auto& module : plugins->modules) {
        const auto result = native_plugin_module_unload(&module);
        if (!result.ok()) { plugins->closing = false; module_failed(*plugins, result); return false; }
    }
    const auto preferences_released = native_plugin_preferences_release(&plugins->preferences_io);
    if (!preferences_released.ok()) {
        preferences_error(preferences_released);
        plugins->status = utf8_literal("Couldn't save plugin settings. Try again.");
        plugins->closing = false; return false;
    }
    application_plugins_sdk_close(*plugins);
    if (borrowing_backing) application_settings_plugin_catalog_unbind();
    if (plugins->catalog) {
        const auto released = native_plugin_catalog_release(&plugins->catalog->catalog);
        if (!released.ok()) {
            plugins->status = utf8_literal("Couldn't save plugin settings. Try again.");
            plugins->closing = false; return false;
        }
        if (!VirtualFree(plugins->catalog, 0, MEM_RELEASE)) { plugins->closing = false; return false; }
        plugins->catalog = nullptr;
    }
    if (!VirtualFree(plugins, 0, MEM_RELEASE)) { plugins->closing = false; return false; }
    g_app.plugins = nullptr;
    return true;
}
bool application_plugins_busy() noexcept { return g_app.plugins && (g_app.plugins->selecting || g_app.plugins->closing || g_app.plugins->activating || g_app.plugins->installing || g_app.plugins->installer.runtime); }
bool application_plugins_save_pending() noexcept { return g_app.plugins && g_app.plugins->preferences_dirty; }
bool application_plugins_retry_save() noexcept {
    auto* plugins = g_app.plugins;
    if (!plugins || application_plugins_busy() || !plugins->preferences_dirty) return false;
    plugins->activating = true;
    const bool saved = application_plugins_save_choices(*plugins, plugins->preferences);
    plugins->preferences_dirty = !saved;
    plugins->status = saved ? utf8_literal("Plugins saved.") :
        utf8_literal("Plugins enabled, but couldn't be saved. Try again.");
    plugins->activating = false; application_settings_refresh_pages(); return saved;
}
bool application_plugins_present(u32 index) noexcept {
    return g_app.plugins && index < 8 && (g_app.plugins->modules[index].module || g_app.plugins->modules[index].state || g_app.plugins->modules[index].integrity);
}
Utf8View application_plugins_label(u32 index) noexcept {
    if (!g_app.plugins || index >= 8 || !g_app.plugins->label_bytes[index]) return utf8_literal("Native plugin");
    return {g_app.plugins->labels[index], g_app.plugins->label_bytes[index]};
}
Utf8View application_plugins_status() noexcept {
    return g_app.plugins && g_app.plugins->status.bytes ? g_app.plugins->status :
        utf8_literal("Only load plugins you trust. Plugins have full access to your computer.");
}

namespace {
bool catalog_row_visible(const ApplicationPlugins& plugins, u32 index) noexcept {
    return index < plugins.catalog->catalog.count && index >= plugins.catalog_first &&
        index - plugins.catalog_first < 8;
}
bool catalog_context(ApplicationPlugins* plugins, u64 expected_revision, bool require_catalog = true) noexcept {
    return plugins && plugins == g_app.plugins && !application_plugins_busy() && (!require_catalog || plugins->catalog) &&
        plugins->catalog_revision == expected_revision && g_app.account_ui && !g_app.login.visible &&
        application_settings_active() && plugins_page_selected();
}
bool advance_catalog(ApplicationPlugins& plugins) noexcept {
    if (plugins.catalog_revision == ~u64{0}) return false;
    ++plugins.catalog_revision;
    return true;
}
bool local_catalog_directory(wchar_t* output, u32* characters) noexcept {
    const DataPathResult built = data_path(output, kNativePluginCatalogPathCapacity, L"plugins");
    *characters = built.length;
    return built.ok();
}
void inventory_result(ApplicationPlugins& plugins, NativePluginCatalogResult result) noexcept {
    if (result.ok()) plugins.status = plugins.catalog->catalog.count ?
        utf8_literal("Only enable plugins you trust. Plugins have full access to your computer.") :
        utf8_literal("No plugins installed.");
    else if (result.error == NativePluginCatalogError::directory &&
        (result.win32_error == ERROR_FILE_NOT_FOUND || result.win32_error == ERROR_PATH_NOT_FOUND))
        plugins.status = utf8_literal("No plugins installed.");
    else if (result.error == NativePluginCatalogError::capacity)
        plugins.status = utf8_literal("Too many plugins. Keep up to 32 in the Plugins folder, then refresh.");
    else plugins.status = utf8_literal("Couldn't read the Plugins folder. Try refreshing.");
}
Utf8View install_result(NativePluginBundleInstallResult result) noexcept {
    using Error=NativePluginBundleInstallError;
    if(result.cleanup_pending)return utf8_literal("Finish the current installation first.");
    if(result.committed&&result.uninstall)return utf8_literal("Plugin uninstalled.");
    if(result.committed)return result.idempotent?utf8_literal("This plugin is already installed."):
        utf8_literal("Plugin installed. Enable it to use it.");
    if(result.error==Error::unsupported)return utf8_literal("Couldn't update the plugin. The previous version is still installed.");
    if(result.error==Error::retained_limit)return utf8_literal("Couldn't update the plugin. Restart MaiCord and try again.");
    if(result.error==Error::cancelled)return utf8_literal("Installation cancelled.");
    if(result.error==Error::busy||result.error==Error::conflict)return utf8_literal("Couldn't install the plugin because of a conflicting file.");
    return utf8_literal("Couldn't install the plugin. Check its manifest and DLL.");
}
void poll_install(ApplicationPlugins& plugins) noexcept {
    using Phase=NativePluginBundleInstallPhase;
    if(!plugins.installer.runtime||plugins.install_retry||plugins.activating)return;
    // Native stop/Settings callbacks may pump nested messages. Keep the app
    // alive through every detach, copied view model and reopen in this turn.
    plugins.activating=true;
    struct Guard{ApplicationPlugins& owner;~Guard() noexcept{owner.activating=false;}} guard{plugins};
    auto result=native_plugin_bundle_install_status(&plugins.installer);
    if(result.phase!=Phase::prepared&&result.phase!=Phase::complete){
        native_plugin_bundle_install_acknowledge(&plugins.installer);
        result=native_plugin_bundle_install_status(&plugins.installer);
    }
    if(result.phase==Phase::prepared){
        if(!install_context(plugins))native_plugin_bundle_install_cancel(&plugins.installer);
        else {
            bool authorized=true;
            if(!result.idempotent){
                application_settings_plugin_catalog_unbind();plugins.install_detached=true;
                authorized=install_context(plugins,true);
                for(u32 i=0;authorized&&i<8;++i)if(plugins.modules[i].owner_id==result.owner_id&&application_plugins_present(i)){
                    const auto unloaded=native_plugin_module_unload(&plugins.modules[i]);
                    authorized=unloaded.ok()&&install_context(plugins,true);
                    if(unloaded.ok())plugins.label_bytes[i]=0;else report_error(unloaded);
                }
            }
            if(authorized&&install_context(plugins,plugins.install_detached)){
                if(native_plugin_bundle_install_commit(&plugins.installer)!=NativePluginBundleInstallError::none)
                    native_plugin_bundle_install_cancel(&plugins.installer);
                else plugins.status=utf8_literal("Updating plugin…");
            }else native_plugin_bundle_install_cancel(&plugins.installer);
            if(plugins.install_detached&&install_context(plugins,true))application_settings_plugin_catalog_reopen();
            plugins.install_detached=false;
        }
        return;
    }
    if(result.phase!=Phase::complete)return;
    if(plugins.install_consumed&&!result.cleanup_pending)plugins.status=install_result(result);
    if(!plugins.install_consumed){
        const bool current=install_context(plugins);
        plugins.status=install_result(result);
        if(result.committed&&current&&!result.idempotent){
            application_settings_plugin_catalog_unbind();plugins.install_detached=true;
            if(install_context(plugins,true)&&advance_catalog(plugins)){
                if(!plugins.catalog)plugins.catalog=static_cast<ApplicationPluginCatalogOwner*>(VirtualAlloc(nullptr,
                    sizeof(ApplicationPluginCatalogOwner),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
                plugins.catalog_first=0;
                if(!(plugins.catalog&&native_plugin_bundle_install_catalog(&plugins.installer,&plugins.catalog->catalog,{plugins.catalog->entries,32}))){
                    if(plugins.catalog)plugins.catalog->catalog.count=0;
                    plugins.status=utf8_literal("Plugin installed. Refresh the list to enable it.");
                }
            }
            if(install_context(plugins,true))application_settings_plugin_catalog_reopen();
            plugins.install_detached=false;
        }
        plugins.install_consumed=true;
        native_plugin_bundle_install_acknowledge(&plugins.installer);
        if(current)application_settings_refresh_pages();
    }
    if(result.cleanup_pending){plugins.install_retry=true;application_settings_refresh_pages();return;}
    if(!native_plugin_bundle_install_release(&plugins.installer,0)){
        if(plugins.installer.thread&&WaitForSingleObject(plugins.installer.thread,0)!=WAIT_OBJECT_0)return;
        // The worker signals completion just before it exits, so it can exit
        // between release's zero wait and the check above. Retry once before
        // reporting a genuine release failure.
        if(!native_plugin_bundle_install_release(&plugins.installer,0)){
            plugins.install_retry=true;plugins.status=utf8_literal("Plugin installation didn't finish. Try again.");
            application_settings_refresh_pages();return;
        }
    }
    plugins.installing=false;plugins.install_consumed=false;plugins.install_retry=false;
    // Catalog operations settle their records once the installer is released.
    application_plugin_browser_installer_complete(result);
    if(install_context(plugins))application_settings_refresh_pages();
}
} // namespace

bool application_plugins_install() noexcept {
    auto* plugins=g_app.plugins;
    if(plugins&&plugins->install_retry&&!plugins->closing&&!plugins->activating){
        if(plugins->picker.runtime&&!native_file_picker_release(&plugins->picker,0))return false;
        plugins->selecting=false;
        if(plugins->installer.runtime){
            const auto result=native_plugin_bundle_install_status(&plugins->installer);
            if(result.phase==NativePluginBundleInstallPhase::complete&&result.cleanup_pending){
                if(native_plugin_bundle_install_retry_cleanup(&plugins->installer)!=NativePluginBundleInstallError::none)return false;
            }
        }
        plugins->install_retry=false;
        if(plugins->installer.runtime)poll_install(*plugins);else plugins->installing=false;
        application_settings_refresh_pages();return true;
    }
    if(application_plugins_busy()||!g_app.account_ui||g_app.login.visible||!application_settings_active()||
        !plugins_page_selected())return false;
    plugins=ensure();if(!plugins||(plugins->catalog&&plugins->catalog->catalog.cleanup))return false;
    // Capture before the dialog starts, including the monotonic lifetime that
    // prevents a released/reallocated Settings pointer from passing this guard.
    plugins->install_settings=g_app.settings;plugins->install_lifetime=application_settings_lifetime();
    plugins->install_account_serial=g_app.plugin_account_scope_serial;plugins->install_profile=g_app.account.status.profile_id;
    const auto started=native_file_picker_start(&plugins->picker,g_app.window,1,utf8_literal("Install plugin"));
    plugins->selecting=started==NativeFilePickerError::none;plugins->installing=plugins->selecting||plugins->picker.runtime;
    plugins->picker_consumed=false;plugins->install_consumed=false;plugins->install_detached=false;
    plugins->install_retry=!plugins->selecting&&plugins->picker.runtime;
    plugins->status=plugins->selecting?utf8_literal("Choose a .maicord-plugin.json file."):
        utf8_literal("Couldn't open the file picker. Try again.");
    application_settings_refresh_pages();return plugins->selecting;
}

u32 application_plugins_settings_controls(Slice<UiSettingsControl> controls,Slice<ApplicationPluginCatalogText> text,NativePluginError error) noexcept {
    const auto* plugins=g_app.plugins;ApplicationPluginSettingsLoaded loaded[8]{};u32 count{};
    if(plugins)for(u32 i=0;i<8;++i)if(application_plugins_present(i))loaded[count++]={plugins->modules[i].owner_id,i,
        application_plugins_label(i),plugins->modules[i].enabled,loaded_identity(*plugins,i)};
    ApplicationPluginSettingsState state{};state.catalog=plugins&&plugins->catalog?&plugins->catalog->catalog:nullptr;
    state.first=plugins?plugins->catalog_first:0;state.remembered=plugins?plugins->preferences.count:0;
    state.discovered=plugins&&plugins->catalog;state.busy=application_plugins_busy();state.save_pending=application_plugins_save_pending();
    state.status=application_plugins_status();state.plugin_error=error;state.loaded={loaded,count};state.install_retry=plugins&&plugins->install_retry;
    state.developer_mode=g_app.preferences.developer_mode;state.preferences=plugins?&plugins->preferences:nullptr;
    return application_plugins_settings_project(state,controls,text);
}

u64 application_plugins_catalog_revision() noexcept { return g_app.plugins ? g_app.plugins->catalog_revision : 0; }
const NativePluginCatalog* application_plugins_catalog_current() noexcept {
    return g_app.plugins && g_app.plugins->catalog ? &g_app.plugins->catalog->catalog : nullptr;
}

bool application_plugins_discover() noexcept {
    if (application_plugins_busy() || !g_app.account_ui || g_app.login.visible || !application_settings_active()) return false;
    auto* plugins = ensure();
    if (!plugins) return false;
    const auto stamp = scope_stamp();
    if (!plugins_page_selected()) return false;
    // No live Settings or choice descriptor borrows a bank being overwritten.
    application_settings_plugin_catalog_unbind();
    if (g_app.plugins != plugins || !scope_unchanged(stamp) || !g_app.account_ui || g_app.login.visible ||
        !advance_catalog(*plugins)) return false;
    NativePluginCatalogResult result{NativePluginCatalogError::invalid_input};
    if (!plugins->catalog)
        plugins->catalog = static_cast<ApplicationPluginCatalogOwner*>(VirtualAlloc(nullptr,
            sizeof(ApplicationPluginCatalogOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (plugins->catalog) {
        result = native_plugin_catalog_release(&plugins->catalog->catalog);
        plugins->catalog->catalog.count = 0;
        if (result.ok()) {
            wchar_t directory[kNativePluginCatalogPathCapacity]; u32 characters = 0;
            result = {NativePluginCatalogError::invalid_input};
            if (local_catalog_directory(directory, &characters))
                result = native_plugin_catalog_initialize(&plugins->catalog->catalog, {directory, characters}, {plugins->catalog->entries, 32});
        }
    }
    if (result.ok()) result = native_plugin_catalog_refresh(&plugins->catalog->catalog);
    plugins->catalog_first = 0;
    inventory_result(*plugins, result);
    application_settings_refresh_pages();
    return application_settings_plugin_catalog_reopen();
}
bool application_plugins_catalog_page(bool previous, u64 revision) noexcept {
    auto* plugins = g_app.plugins;
    if (!catalog_context(plugins, revision)) return false;
    const u32 count = plugins->catalog->catalog.count, first = plugins->catalog_first;
    if (previous ? first < 8 : count - first <= 8) return false;
    const auto stamp = scope_stamp();
    application_settings_plugin_catalog_unbind();
    if (g_app.plugins != plugins || !scope_unchanged(stamp) || !advance_catalog(*plugins)) return false;
    plugins->catalog_first = previous ? first - 8 : first + 8;
    application_settings_refresh_pages();
    return application_settings_plugin_catalog_reopen();
}
namespace {
bool enable_entry(u32 index, u64 revision, u64 approved) noexcept;
bool approval_valid(TextFormOwnerKey key) noexcept {
    auto* plugins = g_app.plugins;
    return plugins && plugins->sdk && key.scope == plugins->sdk->approval_serial &&
        key.collection == plugins->sdk->approval_revision && key.item == plugins->sdk->approval_index &&
        g_app.plugin_account_scope_serial == plugins->sdk->approval_serial;
}
bool approval_save(TextFormOwnerKey key, Utf8View) noexcept {
    if (!approval_valid(key)) return false;
    auto* plugins = g_app.plugins;
    if (!plugins->catalog || key.item >= plugins->catalog->catalog.count) return false;
    const u64 permissions = plugins->catalog->catalog.entries[static_cast<u32>(key.item)].manifest.permissions;
    enable_entry(static_cast<u32>(key.item), key.collection, permissions);
    return true;
}
// Lists what an ABI-5 plugin asks for and enables it only after the user agrees.
bool request_approval(ApplicationPlugins& plugins, u32 index, u64 revision) noexcept {
    auto* sdk = plugins.sdk;
    if (!sdk) return false;
    const auto& manifest = plugins.catalog->catalog.entries[index].manifest;
    TextBank text{sdk->approval_text, sizeof(sdk->approval_text), 0};
    if (manifest.publisher_bytes) {
        text.add("Published by ");
        text.add({manifest.publisher, manifest.publisher_bytes});
        text.add(".\n");
    }
    text.add("This plugin will be able to:");
    for (u32 bit = 0; bit < kNativePluginPermissionCount; ++bit)
        if (manifest.permissions & (1ull << bit)) {
            text.add("\n\xE2\x80\xA2 ");
            text.add(native_plugin_permission_label(bit));
        }
    if (!manifest.permissions) text.add("\nNothing beyond running its own code.");
    text.add("\nPlugins run with full access to your computer. Only enable plugins you trust.");
    sdk->approval_serial = g_app.plugin_account_scope_serial;
    sdk->approval_revision = revision;
    sdk->approval_index = index;
    char title[kNativePluginNameCapacity + 16];
    TextBank heading{title, sizeof(title), 0};
    heading.add("Enable ");
    heading.add({manifest.name, manifest.name_bytes});
    heading.add("?");
    ApplicationTextFormRequest request{};
    request.title = heading.view();
    request.context = text.view();
    request.accept = utf8_literal("Allow and enable");
    request.owner = {sdk->approval_serial, revision, index};
    request.valid = approval_valid;
    request.save = approval_save;
    request.confirmation_only = true;
    return application_text_form_open(request);
}
} // namespace

bool application_plugins_catalog_enable(u32 index, u64 revision) noexcept {
    auto* plugins = g_app.plugins;
    if (!catalog_context(plugins, revision) || !catalog_row_visible(*plugins, index)) return false;
    const auto& entry = plugins->catalog->catalog.entries[index];
    if (entry.status != NativePluginCatalogError::none || !native_plugin_manifest_valid(entry.manifest)) return false;
    // A plugin revoked as harmful stays off.
    if (application_plugin_browser_gate(entry.manifest.owner_id) & kPluginGateRevoked) return false;
    // Permissions need the user's approval unless this exact plugin was
    // already approved for at least the same set.
    const auto* remembered = native_plugin_preferences_find(plugins->preferences, entry.manifest);
    if (!remembered || (entry.manifest.permissions & ~static_cast<u64>(remembered->permissions)))
        return request_approval(*plugins, index, revision);
    return enable_entry(index, revision, remembered->permissions);
}

namespace {
bool enable_entry(u32 index, u64 revision, u64 approved) noexcept {
    auto* plugins = g_app.plugins;
    if (!catalog_context(plugins, revision) || !catalog_row_visible(*plugins, index)) return false;
    const auto& entry = plugins->catalog->catalog.entries[index];
    if (entry.status != NativePluginCatalogError::none || !native_plugin_manifest_valid(entry.manifest)) return false;
    const u32 slot = free_slot(*plugins);
    if (slot == 8) { plugins->status = utf8_literal("Unload a plugin before enabling another."); application_settings_refresh_pages(); return false; }
    for (const auto& module : plugins->modules) if (module.enabled && module.owner_id == entry.manifest.owner_id) return false;
    const NativePluginManifest manifest = entry.manifest;
    const auto& catalog = plugins->catalog->catalog;
    wchar_t directory[kNativePluginCatalogPathCapacity];
    const u32 characters = catalog.directory_characters;
    if (!characters || characters >= kNativePluginCatalogPathCapacity) return false;
    copy_bytes(directory, catalog.directory, (characters + 1) * static_cast<u32>(sizeof(wchar_t)));
    const auto stamp = scope_stamp();
    const u32 first = plugins->catalog_first;
    const u32 scene_revision = g_app.renderer.settings_scene->revision;
    auto* host = application_settings_plugin_host_current();
    if (!host || !catalog_context(plugins, revision)) return false;
    plugins->activating = true;
    application_plugins_sdk_bind_host(*plugins, host);
    const auto result = native_plugin_module_load_declared(&plugins->modules[slot], host, {directory, characters},
        manifest, approved);
    const bool current = g_app.plugins == plugins && scope_unchanged(stamp) &&
        g_app.account_ui && !g_app.login.visible && plugins->catalog_revision == revision && plugins->catalog_first == first &&
        application_settings_active() && plugins_page_selected() &&
        g_app.renderer.settings_scene->revision == scene_revision;
    if (!current) {
        const auto rollback = native_plugin_module_unload(&plugins->modules[slot]);
        plugins->activating = false;
        if (!rollback.ok()) module_failed(*plugins, rollback);
        else {
            plugins->status = result.ok() ? Utf8View{} : error_text(result);
            if (!result.ok()) report_error(result);
        }
        return false;
    }
    plugins->activating = false;
    if (plugins->modules[slot].enabled) set_label(*plugins, slot, manifest);
    if (result.ok()) plugins->status = utf8_literal("Plugin enabled.");
    else module_failed(*plugins, result);
    if (result.ok() && plugins->modules[slot].enabled) {
        capture_loaded_identity(*plugins, slot, manifest);
        NativePluginPreferences remembered = plugins->preferences;
        const auto admitted = native_plugin_preferences_enable(&remembered, manifest,
            approved & manifest.permissions);
        if (admitted.ok()) {
            plugins->activating = true;
            const bool saved = application_plugins_save_choices(*plugins, remembered);
            plugins->preferences = remembered;
            plugins->preferences_dirty = !saved;
            plugins->status = saved ? utf8_literal("Plugin enabled.") :
                utf8_literal("Plugin enabled, but couldn't be saved. Try again.");
            plugins->activating = false;
        } else {
            preferences_error(admitted);
            plugins->status = utf8_literal("Plugin enabled for this session only. Remove a saved plugin to keep it enabled.");
        }
    }
    advance_catalog(*plugins);
    application_settings_refresh_pages();
    return result.ok();
}
} // namespace

bool application_plugins_catalog_forget(u64 revision) noexcept {
    auto* plugins = g_app.plugins;
    if (!catalog_context(plugins, revision, false) || plugins->catalog_first || !plugins->preferences.count) return false;
    plugins->activating = true;
    const NativePluginPreferences empty{};
    const bool saved = application_plugins_save_choices(*plugins, empty);
    if (saved) plugins->preferences_dirty = false;
    plugins->status = saved ? utf8_literal("Saved plugins cleared.") :
        utf8_literal("Couldn't clear saved plugins. Try again.");
    plugins->activating = false;
    advance_catalog(*plugins);
    application_settings_refresh_pages();
    return saved;
}

bool application_plugins_restore() noexcept {
    if (application_plugins_busy() || !g_app.account_ui || g_app.login.visible) return false;
    if (g_app.plugins) return true;
    // Before plugins.bin is read: recovery may settle trust.
    application_plugin_browser_startup();
    auto* plugins = ensure();
    if (!plugins) return false;
    // Empty/malformed records grant nothing and leave the ordinary shell
    // without a Plugins or Settings allocation.
    if (!plugins->preferences.count) {
        const bool released = application_plugins_release();
        application_settings_refresh_pages(); return released;
    }
    if (plugins->preferences_io.cleanup && !native_plugin_preferences_release(&plugins->preferences_io).ok()) {
        application_settings_refresh_pages(); return false;
    }
    plugins->catalog = static_cast<ApplicationPluginCatalogOwner*>(VirtualAlloc(nullptr,
        sizeof(ApplicationPluginCatalogOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!plugins->catalog) { application_settings_refresh_pages(); return false; }
    wchar_t directory[kNativePluginCatalogPathCapacity]; u32 characters = 0;
    NativePluginCatalogResult scanned{NativePluginCatalogError::path};
    if (local_catalog_directory(directory, &characters)) {
        scanned = native_plugin_catalog_initialize(&plugins->catalog->catalog, {directory, characters}, {plugins->catalog->entries, 32});
        if (scanned.ok()) scanned = native_plugin_catalog_refresh(&plugins->catalog->catalog);
    }
    advance_catalog(*plugins);
    if (!scanned.ok()) { inventory_result(*plugins, scanned); application_settings_refresh_pages(); return false; }
    bool matching = false;
    for (u32 i = 0; i < plugins->catalog->catalog.count; ++i) {
        const auto& entry = plugins->catalog->catalog.entries[i];
        matching = matching || (entry.status == NativePluginCatalogError::none &&
            !application_plugin_browser_gate(entry.manifest.owner_id) &&
            native_plugin_preferences_restorable(plugins->preferences, entry.manifest));
    }
    if (!matching) {
        if (!plugins->sdk || plugins->status.data != plugins->sdk->notice)
            plugins->status = utf8_literal("Some saved plugins changed or are missing. Enable them again.");
        application_settings_refresh_pages();
        return true;
    }
    auto* host = application_settings_plugin_host();
    if (!host) return false;
    const auto stamp = scope_stamp();
    u32 restored = 0, attempted = 0; bool failed = false, scope_abandoned = false;
    plugins->activating = true;
    for (u32 index = 0; index < plugins->catalog->catalog.count; ++index) {
        const auto& entry = plugins->catalog->catalog.entries[index];
        if (entry.status != NativePluginCatalogError::none ||
            !native_plugin_preferences_restorable(plugins->preferences, entry.manifest)) continue;
        // Revoked as harmful, or a build the catalog says this host can't run.
        if (application_plugin_browser_gate(entry.manifest.owner_id)) continue;
        const u32 slot = free_slot(*plugins);
        if (slot == 8) { failed = true; break; }
        const NativePluginManifest manifest = entry.manifest;
        const auto* remembered = native_plugin_preferences_find(plugins->preferences, manifest);
        const u64 approved = remembered ? remembered->permissions : 0;
        attempted |= 1u << slot;
        application_plugins_sdk_bind_host(*plugins, host);
        const auto loaded = native_plugin_module_load_declared(&plugins->modules[slot], host, {directory, characters},
            manifest, approved);
        if (g_app.plugins != plugins || !scope_unchanged(stamp) || !g_app.account_ui || g_app.login.visible) {
            failed = scope_abandoned = true; break;
        }
        if (plugins->modules[slot].enabled) {
            if (loaded.ok()) capture_loaded_identity(*plugins, slot, manifest);
            set_label(*plugins, slot, manifest);
            ++restored;
        }
        if (!loaded.ok()) { report_error(loaded); failed = true; }
    }
    if (scope_abandoned) {
        // A later native callback can abandon the account after earlier modules
        // started. Unwind the entire attempt while nested lifetime changes remain
        // blocked; failed unloads keep their real module/state owner for retry.
        for (u32 slot = 0; slot < 8; ++slot) if (attempted & (1u << slot)) {
            const auto rollback = native_plugin_module_unload(&plugins->modules[slot]);
            if (!rollback.ok()) report_error(rollback);
        }
    }
    plugins->activating = false;
    if (scope_abandoned && (!g_app.account_ui || g_app.login.visible)) {
        application_settings_release();
        return false;
    }
    u32 restorable = 0;
    for (u32 i = 0; i < plugins->preferences.count; ++i) {
        const auto& remembered = plugins->preferences.entries[i];
        restorable += remembered.flags || application_plugin_browser_gate(remembered.owner_id) ? 0u : 1u;
    }
    failed = failed || restored != restorable;
    if (!plugins->sdk || plugins->status.data != plugins->sdk->notice)
        plugins->status = failed ? utf8_literal("Some saved plugins couldn't be loaded.") :
            restored ? Utf8View{} :
            utf8_literal("Some saved plugins changed or are missing. Enable them again.");
    application_settings_refresh_pages();
    return !failed;
}

bool application_plugins_install_prepared(ConstSlice<wchar_t> manifest, NativePluginBundleInstallOptions options, u64 uninstall_owner,
    ConstSlice<wchar_t> directory) noexcept {
    if (application_plugins_busy() || !g_app.account_ui || g_app.login.visible || !application_settings_active() ||
        !plugins_page_selected()) return false;
    auto* plugins = ensure();
    if (!plugins || (plugins->catalog && plugins->catalog->catalog.cleanup)) return false;
    plugins->install_settings = g_app.settings; plugins->install_lifetime = application_settings_lifetime();
    plugins->install_account_serial = g_app.plugin_account_scope_serial; plugins->install_profile = g_app.account.status.profile_id;
    const auto started = uninstall_owner ? native_plugin_bundle_uninstall_start(&plugins->installer, uninstall_owner, directory)
                                         : native_plugin_bundle_install_start(&plugins->installer, manifest, directory, options);
    if (started != NativePluginBundleInstallError::none) return false;
    plugins->installing = true;
    plugins->selecting = plugins->picker_consumed = plugins->install_consumed = plugins->install_detached = plugins->install_retry = false;
    plugins->status = uninstall_owner ? utf8_literal("Uninstalling plugin\xE2\x80\xA6") : utf8_literal("Installing plugin\xE2\x80\xA6");
    application_settings_refresh_pages();
    return true;
}
bool application_plugins_reload_trust() noexcept {
    auto* plugins = g_app.plugins;
    if (!plugins) return true;
    if (plugins->activating || plugins->closing || plugins->installer.runtime) return false;
    const auto released = native_plugin_preferences_release(&plugins->preferences_io);
    if (!released.ok()) { preferences_error(released); return false; }
    NativePluginPreferences loaded{};
    const auto result = native_plugin_preferences_load(&plugins->preferences_io, &loaded);
    if (!result.ok()) { preferences_error(result); return false; }
    plugins->preferences = loaded;
    plugins->preferences_dirty = false;
    return true;
}
bool application_plugins_enable_owner(u64 owner) noexcept {
    auto* plugins = g_app.plugins;
    if (!plugins || !plugins->catalog || !catalog_context(plugins, plugins->catalog_revision)) return false;
    const auto& catalog = plugins->catalog->catalog;
    for (u32 i = 0; i < catalog.count; ++i) {
        const auto& entry = catalog.entries[i];
        if (entry.status != NativePluginCatalogError::none || entry.manifest.owner_id != owner) continue;
        if (i < plugins->catalog_first || i - plugins->catalog_first >= 8) {
            // Enable admits rows of the visible window only.
            auto* settings = g_app.settings;
            application_settings_plugin_catalog_unbind();
            if (g_app.plugins != plugins || g_app.settings != settings || !advance_catalog(*plugins)) return false;
            plugins->catalog_first = i / 8 * 8;
            application_settings_refresh_pages();
            if (!application_settings_plugin_catalog_reopen()) return false;
        }
        return application_plugins_catalog_enable(i, plugins->catalog_revision);
    }
    return false;
}
} // namespace maicord::app_detail
