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
struct PluginHttp;

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
    // The ledger's microseconds of the last call that returned (0 without a ledger): what a decorate pass charges.
    u64 last_call_us;
    // Network: bit i set = the user approved descriptor.network[i]. generation tells this enable from
    // an earlier one of the same owner, so a finished request never reaches a later enable.
    u32 approved_hosts;
    u32 generation;
    // While ctx.flags has MC_CTX_GESTURE: when the call stops counting as a user gesture. A completion
    // sets inherit_gesture_until_ms first, which keeps the deadline of the request that began it.
    u64 gesture_until_ms, inherit_gesture_until_ms;
    // Requests started in the current rate-limit window (plugin_http_submit).
    u64 http_window_ms;
    u32 http_window_count;
};

// Zero-initialized, caller-owned, stable address. UI thread only.
struct PluginRuntime {
    PluginCallLedger* ledger;
    PluginDirectory directory;
    wchar_t data_directory[512];
    u32 data_directory_characters;
    // Where plugin log lines go (sink_context is passed back); without a log sink they are dropped.
    void (*log)(void* context, u64 owner_id, u32 level, Utf8View text) noexcept;
    // A call that ran past its budget (the Developer console): strike counts toward kPluginCallOverrunLimit.
    void (*strike)(void* context, u64 owner_id, PluginCallKind kind, u32 strike, u64 elapsed_us) noexcept;
    void* sink_context;
    PluginBinding* bindings[kPluginMaximumBindings];
    u32 binding_count;
    u32 call_serial;
    // The HTTP broker; created by the first request (http.cpp).
    PluginHttp* http;
    u32 generation_serial;
    // Developer Mode reload: the state of a plugin being replaced (plugin_runtime_keep_state).
    struct {
        u64 owner_id, layout;
        u32 bytes, alignment;
        u8* state;
    } carry;
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

// What happened to the state a reload carried (plugin_runtime_keep_state).
enum class PluginStateCarry : u8 { none, kept, reset };

struct PluginEnableResult {
    PluginEnableError error;
    PluginLoadResult load;
    PluginBinding* binding;
    PluginStateCarry carried;
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
// approved_hosts: bit i = the user approved descriptor.network[i] (http.hpp). A nonnull
// sha256 must be the file's digest (plugin_image_load).
PluginEnableResult plugin_runtime_enable(PluginRuntime*, ConstSlice<wchar_t> leaf, u64 approved_permissions,
                                         u32 approved_hosts = 0, const u8* sha256 = nullptr) noexcept;
// Unpublishes the hooks, runs stop, flushes settings and store, then frees the DLL and the
// binding. False when the DLL could not be freed: the plugin is stopped and silent, and a
// later disable retries.
bool plugin_runtime_disable(PluginRuntime*, PluginBinding*) noexcept;
// Developer Mode reload: copies the state of binding aside, to be given back to the next enable of the same
// owner when its descriptor's state_layout, size and alignment are unchanged (the plugin then starts with
// MC_CTX_RELOADED); a changed layout starts it zeroed with MC_CTX_FIRST_START. The next enable of any plugin
// consumes the copy. False when the state could not be copied.
bool plugin_runtime_keep_state(PluginRuntime*, const PluginBinding*) noexcept;
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
// One timed call into the plugin for host code that owns the call's argument types (menus,
// commands, completions): invoke(&context, argument) runs under the ledger with the given
// context flags (MC_CTX_GESTURE for user-initiated calls).
using PluginInvoke = mc_status (*)(mc_ctx*, void*) noexcept;
PluginCallResult plugin_binding_call(PluginBinding*, PluginCallKind, PluginCallClass, u32 context_flags,
                                     PluginInvoke, void* argument) noexcept;

// Publishes or removes the binding's menus and commands (ui_registry.cpp).
bool plugin_ui_add(PluginBinding*) noexcept;
void plugin_ui_remove(PluginBinding*) noexcept;

// Publishes or removes the binding's decoration hooks and buttons (decorations.cpp, buttons.cpp). False when
// the host has no room for another decorating plugin.
bool plugin_surfaces_add(PluginBinding*) noexcept;
void plugin_surfaces_remove(PluginBinding*) noexcept;

// Publishes or removes the binding's hooks in the dispatch lists (dispatch.cpp).
bool plugin_dispatch_add(PluginBinding*) noexcept;
void plugin_dispatch_remove(PluginBinding*) noexcept;

// One enabled plugin's hook of a kind (the lists live in dispatch.cpp).
struct PluginSubscriber {
    PluginBinding* binding;
    mc_hook_fn function;
    u64 filter;
};
// The subscribers of hook in enable order; valid until a plugin is enabled or disabled.
ConstSlice<PluginSubscriber> plugin_dispatch_subscribers(PluginHook) noexcept;

// Publishes or removes the binding's timers and per-plugin pipeline state (pipeline.cpp).
void plugin_pipeline_add(PluginBinding*) noexcept;
void plugin_pipeline_remove(PluginBinding*) noexcept;

// Changes a setting from the host (a settings page). Takes effect now, or when the
// plugin's current call returns. Encodings as mc_api.set_setting.
mc_status plugin_binding_set_setting(PluginBinding*, u32 index, const void* value, u32 bytes) noexcept;
// A change the user made: sets the value (as plugin_binding_set_setting) and calls the plugin's
// SETTING_CHANGED hook with a gesture. MC_E_BUSY inside a call of the plugin.
mc_status plugin_binding_change_setting(PluginBinding*, u32 index, const void* value, u32 bytes) noexcept;
// Approves or revokes descriptor.network[index]. A revoked host's waiting requests fail with MC_HTTP_E_DENIED.
bool plugin_binding_set_host(PluginBinding*, u32 index, bool approved) noexcept;

} // namespace maicord
