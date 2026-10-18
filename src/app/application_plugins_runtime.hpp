#pragma once
#include "maicord/app/plugin_module_loader.hpp"
#include "maicord/app/plugin_preferences.hpp"
#include "maicord/platform/file_picker.hpp"
#include "maicord/app/plugin_bundle_install.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
enum class RelationshipType : u8;
}
namespace maicord::app_detail {
struct ApplicationPluginCatalogOwner;
// ABI-5 runtime, call accounting and the notices they produce. Allocated with
// the plugins owner; never at application startup.
struct ApplicationPluginsSdk {
    NativePluginSdkRuntime runtime;
    PluginCallLedger ledger;
    bool ledger_open;
    bool events_pending;
    u64 slot_revision, rendered_slot_revision, next_timer;
    char notice[192];
    char composer_notice[256];
    u32 composer_notice_bytes;
    u64 composer_notice_until;
    u8 composer_notice_tone; // MC_TONE_* of an mc.actions notice; host notices are neutral.
    bool (*composer_refresh)() noexcept;
    char approval_text[2048];
    u64 approval_revision, approval_serial;
    u32 approval_index;
    UiPageKey edit_page;
    u64 edit_control, edit_serial;
};
struct ApplicationPlugins {
    NativePluginModule modules[8];
    NativePluginPreference loaded_identities[8];
    u64 loaded_identity_generations[8];
    char labels[8][1024];
    u32 label_bytes[8];
    NativeFilePicker picker;
    Utf8View status;
    ApplicationPluginCatalogOwner* catalog;
    u64 catalog_revision;
    u32 catalog_first;
    bool selecting, closing, picker_consumed, reopen_settings;
    bool activating;
    NativePluginPreferences preferences;
    NativePluginPreferencesIo preferences_io;
    bool preferences_dirty;
    NativePluginBundleInstall installer;
    void* install_settings;
    u64 install_lifetime, install_account_serial, install_profile;
    bool installing, install_consumed, install_retry, install_detached;
    ApplicationPluginsSdk* sdk;
};
static_assert(sizeof(ApplicationPlugins) <= 12288);

// The ABI-5 runtime, null while the plugins owner or its SDK is not allocated.
NativePluginSdkRuntime* application_plugins_runtime() noexcept;
// MC_RELATIONSHIP_* of a relationship type (application_plugins_read.cpp).
u32 application_plugins_relationship_kind(RelationshipType) noexcept;
// Shared between application_plugins.cpp and application_plugins_sdk.cpp.
bool application_plugins_save_choices(ApplicationPlugins&, const NativePluginPreferences&) noexcept;
// Creates the ABI-5 runtime and opens the call marker; reports a plugin that
// was running when MaiCord last exited.
bool application_plugins_sdk_open(ApplicationPlugins&, PluginCallMarker* leftover) noexcept;
void application_plugins_sdk_close(ApplicationPlugins&) noexcept;
// Points the host at the runtime and ledger before any module load.
void application_plugins_sdk_bind_host(ApplicationPlugins&, NativePluginHost*) noexcept;
void application_plugins_sdk_poll(ApplicationPlugins&) noexcept;
// Effect registrations changed: repaint, and re-list the effect picker
// (application_plugins_fx.cpp).
void application_plugins_fx_poll(ApplicationPluginsSdk&) noexcept;
// Fill the runtime's host sources (application_plugins_read.cpp, _actions.cpp).
void application_plugins_read_install(NativePluginSdkRuntime&) noexcept;
void application_plugins_actions_install(NativePluginSdkRuntime&) noexcept;
void application_plugins_set_notice(ApplicationPlugins&, Utf8View first, Utf8View second = {},
                                    Utf8View third = {}) noexcept;
} // namespace maicord::app_detail
