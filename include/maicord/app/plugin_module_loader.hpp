#pragma once

#include "maicord/app/plugin_host.hpp"
#include "maicord/app/plugin_catalog.hpp"
#include "maicord/app/plugin_sdk.hpp"

namespace maicord {

inline constexpr u32 kNativePluginModuleMaxPathCharacters = 1023;
inline constexpr char kNativePluginQueryExport[] = MC_QUERY_EXPORT;

enum class NativePluginModuleError : u8 {
    none, invalid_argument, invalid_path, busy, already_loaded, load_failed,
    missing_export, query_failed, host, allocation_failed, release_failed,
    manifest, integrity, metadata_mismatch,
    // ABI 5: the definition asked for permissions the user did not approve,
    // or the host has no ABI-5 runtime.
    permissions, unsupported,
};

struct NativePluginModuleResult {
    NativePluginModuleError error;
    NativePluginResult host_result;
    DWORD win32_error;
    NativePluginCatalogError integrity_error;
    constexpr bool ok() const noexcept { return error == NativePluginModuleError::none; }
};

// Zero-initialized, caller-owned, stable-address and noncopyable by contract.
// UI-thread only. Load/unload manages one DLL reference and one lazily allocated
// state. No destructor, discovery, auto-enable, worker or timer exists.
// Host, slots and registry must outlive this object until unload succeeds.
struct NativePluginModule {
    HMODULE module;
    NativePluginHost* host;
    u64 owner_id;
    u64 generation;
    void* state;
    u32 state_bytes;
    usize allocation_bytes;
    bool enabled;
    bool busy;
    NativePluginVerifiedFile* integrity;
    // Host-side binding of the loaded ABI-5 plugin; null once released.
    NativePluginSdkBinding* sdk;
};

// Count excludes NUL. Accepts drive-rooted or UNC .dll paths, bounded above.
// DLL entry/query run trusted in-process code before ABI/policy validation;
// cooperative capabilities and state budgets are not a security boundary.
// The DLL must export maicord_plugin_query_v5. approved bounds the definition's
// permissions.
NativePluginModuleResult native_plugin_module_load(NativePluginModule* module,
    NativePluginHost* host, ConstSlice<wchar_t> absolute_path, u64 approved_permissions = 0) noexcept;

// Only a user Enable or a previously saved Enable/trust gate. Hash verification pins the declared
// same-directory DLL and all ancestors through load/query/start. Query owner/ABI
// must match the manifest before host enable. Hash matching is not publisher trust.
// Permissions are limited to both the manifest list and approved_permissions.
NativePluginModuleResult native_plugin_module_load_declared(NativePluginModule* module,
    NativePluginHost* host, ConstSlice<wchar_t> absolute_directory,
    const NativePluginManifest& manifest, u64 approved_permissions = 0) noexcept;

// Disable first: host closes borrowing scenes/popups, removes pages and calls
// stop before unloading code or releasing state. Busy failures keep the state
// for a retry. A host already disabled by disable_all is supported.
NativePluginModuleResult native_plugin_module_unload(NativePluginModule* module) noexcept;

} // namespace maicord
