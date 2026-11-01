#pragma once

#include "maicord/app/plugins/call_ledger.hpp"
#include "maicord/app/plugins/dispatch.hpp"
#include "maicord/app/plugins/loader.hpp"

// Enabled plugins: one binding per plugin owns the DLL image, the plugin's
// state, its settings block and its persisted store, and runs every call into
// the plugin under the call ledger. UI thread only.
namespace maicord {

inline constexpr u32 kPluginMaximumBindings = 32;
// The store and the settings reach disk this long after their last change, and on disable.
inline constexpr u64 kPluginFlushDelayMilliseconds = 2000;
inline constexpr u32 kPluginStagedSettings = 8;
inline constexpr u32 kPluginStagedBytes = 8192;

struct PluginRuntime;

// A persisted key-value table in one reserved region (store.cpp).
struct PluginKeyValue {
    u8* memory;
    u32 reserved_bytes, committed_bytes;
    u32 maximum_keys, maximum_key_bytes, maximum_value_bytes, maximum_total_bytes;
    u32 count, used;
    bool dirty, loaded;
    u64 dirty_since;
};

// Setting values written during a call take effect when it returns.
struct PluginStagedSetting {
    u32 index, offset, bytes;
};

// Allocated per enabled plugin and stable until disable. The first member is
// the context handed to the plugin, so the host table finds the binding from it.
struct PluginBinding {
    mc_ctx ctx;
    PluginRuntime* runtime;
    PluginImage image;
    u64 owner_id;
    // The descriptor's permissions plus those its hooks imply; all were approved.
    u64 permissions;
    // One allocation: state, the settings block, setting text and the staging area.
    u8* data;
    u32 data_bytes;
    u32 state_bytes, settings_bytes;
    // Where each text setting keeps its characters, within data.
    u32 text_offsets[MC_SETTING_MAX];
    u32 text_end;
    PluginStagedSetting staged[kPluginStagedSettings];
    u32 staged_count, staged_used;
    u8* staging;
    PluginKeyValue store;
    // Settings reach disk through a transient table at flush time.
    bool settings_dirty;
    u64 settings_dirty_since;
    bool started, hooks_published, struck_out;
    u32 calls, strikes;
};

// Zero-initialized, caller-owned, stable address. UI thread only.
struct PluginRuntime {
    PluginCallLedger* ledger;
    PluginDirectory directory;
    wchar_t data_directory[512];
    u32 data_directory_characters;
    // Where plugin toasts and log lines go (sink_context is passed back). Without a toast sink
    // mc_api.toast fails with MC_E_UNSUPPORTED; without a log sink log lines are dropped.
    void (*toast)(void* context, u64 owner_id, Utf8View text, u32 tone) noexcept;
    void (*log)(void* context, u64 owner_id, u32 level, Utf8View text) noexcept;
    void* sink_context;
    PluginBinding* bindings[kPluginMaximumBindings];
    u32 binding_count;
    u32 call_serial;
};

enum class PluginEnableError : u8 {
    none,
    invalid_argument,
    capacity,         // kPluginMaximumBindings plugins are enabled
    load,             // see PluginEnableResult::load
    duplicate_owner,  // a plugin with this owner id is enabled
    allocation,
    start_failed,     // start returned an error, or overran its budget
    release,          // the DLL could not be freed; disable again later
};

struct PluginEnableResult {
    PluginEnableError error;
    PluginLoadResult load;
    PluginBinding* binding;
    constexpr bool ok() const noexcept { return error == PluginEnableError::none; }
};

// Pins plugins_directory once for every later enable. data_directory holds
// <owner id>\store.bin and settings.bin per plugin and is created on first
// write. ledger may be null: calls are then neither timed nor attributed.
PluginLoadResult plugin_runtime_open(PluginRuntime*, ConstSlice<wchar_t> plugins_directory,
                                     ConstSlice<wchar_t> data_directory, PluginCallLedger* ledger) noexcept;
// Disables every plugin and unpins the directory.
PluginLoadResult plugin_runtime_close(PluginRuntime*) noexcept;

// Loads leaf from the pinned directory, builds the plugin's state, settings and
// hook lists, runs start and publishes its hooks. approved must contain every
// permission the plugin needs (descriptor plus hooks); nothing stays behind on failure.
PluginEnableResult plugin_runtime_enable(PluginRuntime*, ConstSlice<wchar_t> leaf, u64 approved_permissions) noexcept;
// Unpublishes the hooks, runs stop, flushes settings and store, then frees the DLL and the
// binding. False when the DLL could not be freed: the plugin is stopped and silent, and a
// later disable retries.
bool plugin_runtime_disable(PluginRuntime*, PluginBinding*) noexcept;
// Writes settings and store changes older than the flush delay, and disables plugins the
// ledger flagged for repeated overruns. Returns the owner of one disabled plugin, else 0;
// call again until it returns 0.
u64 plugin_runtime_poll(PluginRuntime*, u64 now_ms) noexcept;

// ---- Inside the runtime: dispatch.cpp and tests ---------------------------------

// What one call into a plugin did.
struct PluginCallResult {
    mc_status status;
    bool overran; // past its class budget: the call's output must be ignored
};
PluginCallResult plugin_binding_run_draft(PluginBinding*, u32 hook_kind, mc_draft_fn, mc_draft*) noexcept;

// Publishes or removes the binding's hooks in the dispatch lists (dispatch.cpp).
bool plugin_dispatch_add(PluginBinding*) noexcept;
void plugin_dispatch_remove(PluginBinding*) noexcept;

// Changes a setting from the host (a settings page). Takes effect now, or when the
// plugin's current call returns. Encodings as mc_api.set_setting.
mc_status plugin_binding_set_setting(PluginBinding*, u32 index, const void* value, u32 bytes) noexcept;

} // namespace maicord
