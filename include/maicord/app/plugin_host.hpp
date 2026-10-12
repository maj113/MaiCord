#pragma once

#include "maicord/ui/settings_scene.hpp"
#include "maicord/app/plugin_settings.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {

inline constexpr u32 native_plugin_capability_pages = 1u << 0;
inline constexpr u32 native_plugin_known_capabilities = native_plugin_capability_pages;

// Monotonic application lifetime token. Zero/saturated values never capture a
// target; teardown can keep invalidating an exhausted scope without wrapping.
inline bool plugin_ui_scope_current(u64 current, u64 captured) noexcept {
    return current && current != ~u64{0} && current == captured;
}
inline void plugin_ui_scope_invalidate(u64* serial) noexcept {
    if (serial && *serial != ~u64{0}) ++*serial;
}

enum class NativePluginError : u8 {
    none, invalid_argument, capability_denied, budget, duplicate_owner,
    capacity, start_failed, not_found, inactive, busy, page, unsupported, stale, activation_failed,
};

struct NativePluginResult {
    NativePluginError error;
    UiPageError page_error;
    bool changed;
    constexpr bool ok() const noexcept { return error == NativePluginError::none; }
};

using NativePluginRegisterPage = NativePluginResult(WINAPI*)(
    void* binding, u64 generation, u64 page_id, Utf8View label, void* context) noexcept;
using NativePluginUnregisterPage = NativePluginResult(WINAPI*)(
    void* binding, u64 generation, u64 page_id) noexcept;

// Passed by value and retained by value by the plugin, never as a borrowed
// pointer. Every service checks a slot generation and supplies the owner itself.
struct NativePluginServices {
    void* binding;
    u64 generation;
    NativePluginRegisterPage register_page;
    NativePluginUnregisterPage unregister_page;
};
static_assert(sizeof(NativePluginServices) == 32);

using NativePluginStart = bool(WINAPI*)(NativePluginServices services, void* state,
                                       u32 state_bytes) noexcept;
using NativePluginStop = void(WINAPI*)(void* state, u32 state_bytes) noexcept;

struct NativePluginActivationResult {
    bool accepted;
    bool changed;
};
// Semantic values only: no scene, renderer, widget or borrowed descriptor pointer.
// Accepted callbacks update their owned content and bump its revision when changed.
// Content revisions must saturate instead of wrapping; exhaustion blocks activation.
// Services and lifecycle changes are unavailable throughout this callback.
using NativePluginActivateSettings = NativePluginActivationResult(WINAPI*)(
    void* state, u32 state_bytes, NativePluginSettingsActivation activation) noexcept;

// Host-internal slot descriptor (the ABI-5 adapter fills it; plugins never see
// it). The module and every borrowed label/context must survive until disable
// returns. Stop runs after any attempted start, including a failed start, so it
// must accept zeroed and partially initialized state. State is host-owned.
struct NativePluginDefinition {
    u64 owner_id;
    u32 required_capabilities;
    u32 page_limit;
    u32 state_bytes;
    u32 state_alignment;
    NativePluginStart start;
    NativePluginStop stop;
    NativePluginActivateSettings activate_settings;
};
static_assert(sizeof(NativePluginDefinition) == 48);

// Close/unbind every scene and popup that borrows this owner's data. Called
// before registry removal and stop, also during failed-start rollback.
using NativePluginDetachOwner = void(*)(void* context, u64 owner_id) noexcept;

struct NativePluginPolicy {
    u32 granted_capabilities;
    u32 max_pages_per_plugin;
    u32 max_state_bytes_per_plugin;
    u32 max_total_state_bytes;
    NativePluginDetachOwner detach_owner;
    void* detach_context;
    // Zero reserves no namespace. Applications reserve their native settings owner
    // even while its built-in pages are not registered.
    u64 reserved_owner_id;
};

enum class NativePluginPhase : u8 { empty, starting, enabled, stopping };
struct NativePluginHost;
struct NativePluginSdkRuntime;
struct PluginCallLedger;

struct NativePluginSlot {
    NativePluginDefinition definition;
    NativePluginHost* host;
    void* state;
    u64 generation;
    u32 page_count;
    NativePluginPhase phase;
    bool service_busy;
};

// UI-thread only and noncopyable by contract: host and slots have stable
// addresses until every plugin is disabled. Storage is fixed and caller-owned.
// Policies constrain cooperative plugins, not hostile in-process native code.
// No allocation, polling, worker, dynamic loader, or runtime is owned here.
struct NativePluginHost {
    Slice<NativePluginSlot> slots;
    UiPageRegistry* pages;
    NativePluginPolicy policy;
    u64 generation;
    u32 active_count;
    u32 state_bytes;
    bool busy;
    // Optional; set by the owner after initialize. sdk enables ABI-5 modules,
    // ledger times every plugin entry made through this host.
    NativePluginSdkRuntime* sdk;
    PluginCallLedger* ledger;
};

// Initialize only a zero-initialized host or one whose plugins have all been
// disabled. Reinitialization preserves generation to reject stale service copies.
// A null detach callback means the caller has no bound plugin scenes/popups.
bool native_plugin_host_initialize(NativePluginHost* host, UiPageRegistry* pages,
    Slice<NativePluginSlot> slots, NativePluginPolicy policy) noexcept;
NativePluginResult native_plugin_host_enable(NativePluginHost* host,
    const NativePluginDefinition& definition, Slice<u8> state_storage) noexcept;
NativePluginResult native_plugin_host_disable(NativePluginHost* host, u64 owner_id) noexcept;
NativePluginResult native_plugin_host_disable_all(NativePluginHost* host) noexcept;
struct NativePluginUiToken {
    UiPageKey page;
    u64 generation;
    u32 registry_revision;
    u32 content_revision;
};
// Capture on pointer press or keyboard activation. Tokens contain no borrowed
// module pointers and expire on slot reuse or descriptor/registry revision change.
// Saturated registry/content revisions refuse capture and activation with stale;
// page removal and plugin teardown remain available.
NativePluginResult native_plugin_host_settings_token(const NativePluginHost* host,
    UiPageKey page, NativePluginUiToken* token) noexcept;
// Resolves the current registered page and control before invoking module code.
// Removed IDs, disabled controls, changed kinds and invalid choices are rejected.
NativePluginResult native_plugin_host_activate_settings(NativePluginHost* host,
    NativePluginUiToken token, UiSettingsActivation activation) noexcept;
const NativePluginSlot* native_plugin_host_find(const NativePluginHost& host, u64 owner_id) noexcept;

} // namespace maicord
