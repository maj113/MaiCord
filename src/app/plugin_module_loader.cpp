#include "maicord/app/plugin_module_loader.hpp"
#include "maicord/core/ascii.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

NativePluginModuleResult result(NativePluginModuleError error, DWORD win32_error = 0) noexcept {
    return {error, {}, win32_error};
}

NativePluginModuleResult host_result(NativePluginResult host) noexcept {
    return {host.ok() ? NativePluginModuleError::none : NativePluginModuleError::host, host, 0};
}

bool valid_path(ConstSlice<wchar_t> path) noexcept {
    if (!path.data || path.count < 7 || path.count > kNativePluginModuleMaxPathCharacters) return false;
    for (u32 index = 0; index < path.count; ++index) {
        if (!path[index] || path[index] == L'/' || path[index] < 32) return false;
    }
    if (path[path.count - 4] != L'.' || ascii_lower(path[path.count - 3]) != L'd' ||
        ascii_lower(path[path.count - 2]) != L'l' || ascii_lower(path[path.count - 1]) != L'l') return false;
    const wchar_t drive = ascii_lower(path[0]);
    if (drive >= L'a' && drive <= L'z' && path[1] == L':' && path[2] == L'\\') {
        for (u32 index = 3; index < path.count; ++index) if (path[index] == L':') return false;
        return true;
    }
    // Ordinary UNC only. Device/extended namespaces are outside
    // this loader's contract, as are incomplete server/share paths.
    if (path[0] != L'\\' || path[1] != L'\\' || path[2] == L'?' || path[2] == L'.' || path[2] == L'\\')
        return false;
    u32 server_end = 2;
    while (server_end < path.count && path[server_end] != L'\\') ++server_end;
    const u32 share_begin = server_end + 1;
    if (share_begin >= path.count || path[share_begin] == L'\\') return false;
    u32 share_end = share_begin;
    while (share_end < path.count && path[share_end] != L'\\') ++share_end;
    if (share_end + 1 >= path.count) return false;
    for (u32 index = 2; index < path.count; ++index) if (path[index] == L':') return false;
    return true;
}

NativePluginResult validate_definition(const NativePluginHost& host,
                                      const NativePluginDefinition& definition) noexcept {
    if (!definition.owner_id || !definition.start || !definition.stop || !definition.state_alignment ||
        (definition.state_alignment & (definition.state_alignment - 1)) || definition.state_alignment > 64)
        return {NativePluginError::invalid_argument};
    if (definition.owner_id == host.policy.reserved_owner_id || native_plugin_host_find(host, definition.owner_id))
        return {NativePluginError::duplicate_owner};
    if (definition.required_capabilities & (~host.policy.granted_capabilities | ~native_plugin_known_capabilities))
        return {NativePluginError::capability_denied};
    if (definition.page_limit > host.policy.max_pages_per_plugin ||
        (definition.page_limit && !(definition.required_capabilities & native_plugin_capability_pages)) ||
        definition.state_bytes > kNativePluginModuleMaxStateBytes ||
        definition.state_bytes > host.policy.max_state_bytes_per_plugin ||
        host.state_bytes > host.policy.max_total_state_bytes ||
        definition.state_bytes > host.policy.max_total_state_bytes - host.state_bytes)
        return {NativePluginError::budget};
    for (u32 index = 0; index < host.pages->count; ++index) {
        if (ui_page_registry_at(*host.pages, index)->key.owner_id == definition.owner_id)
            return {NativePluginError::duplicate_owner};
    }
    bool free_slot = false;
    for (u32 index = 0; index < host.slots.count; ++index) {
        const auto& slot = host.slots[index];
        if (slot.phase == NativePluginPhase::empty) free_slot = true;
        else if (slot.definition.owner_id == definition.owner_id)
            return {NativePluginError::duplicate_owner};
    }
    return {free_slot && host.generation != ~u64{0} ? NativePluginError::none : NativePluginError::capacity};
}

// Called only after enable rollback or successful disable has removed every
// borrowed descriptor. FreeLibrary failures retain all memory for retry.
NativePluginModuleResult release_integrity(NativePluginModule* module) noexcept {
    if (!module->integrity) return result(NativePluginModuleError::none);
    const auto released = native_plugin_verified_file_release(module->integrity);
    if (!released.ok()) return result(NativePluginModuleError::release_failed, released.win32_error);
    if (!VirtualFree(module->integrity, 0, MEM_RELEASE)) return result(NativePluginModuleError::release_failed, GetLastError());
    module->integrity = nullptr;
    return result(NativePluginModuleError::none);
}
NativePluginModuleResult release_resources(NativePluginModule* module) noexcept {
    if (module->sdk) {
        if (!native_plugin_sdk_destroy(module->sdk)) return result(NativePluginModuleError::release_failed);
        module->sdk = nullptr;
    }
    if (module->module) {
        if (!FreeLibrary(module->module)) return result(NativePluginModuleError::release_failed, GetLastError());
        module->module = nullptr;
    }
    if (module->state) {
        if (!VirtualFree(module->state, 0, MEM_RELEASE))
            return result(NativePluginModuleError::release_failed, GetLastError());
        module->state = nullptr;
    }
    const auto integrity = release_integrity(module);
    if (!integrity.ok()) return integrity;
    // Retain the operation guard through DLL_PROCESS_DETACH reentry.
    *module = {};
    return result(NativePluginModuleError::none);
}

NativePluginModuleResult failed_load(NativePluginModule* module, NativePluginModuleResult failure) noexcept {
    const auto released = release_resources(module);
    module->busy = false;
    return released.ok() ? failure : released;
}

} // namespace

static NativePluginModuleResult allocate_state(NativePluginModule* module, u32 state_bytes) noexcept {
    module->state_bytes = state_bytes;
    if (!state_bytes) return result(NativePluginModuleError::none);
    Win32SystemInfo system{};
    GetSystemInfo(system.native());
    module->allocation_bytes = (static_cast<usize>(state_bytes) + system.dwPageSize - 1) /
        system.dwPageSize * system.dwPageSize;
    module->state = VirtualAlloc(nullptr, static_cast<SIZE_T>(module->allocation_bytes), MEM_RESERVE | MEM_COMMIT,
                                PAGE_READWRITE);
    return module->state ? result(NativePluginModuleError::none) :
        result(NativePluginModuleError::allocation_failed, GetLastError());
}

static NativePluginModuleResult query_error(NativePluginSdkQueryError error) noexcept {
    switch (error) {
    case NativePluginSdkQueryError::permissions: return result(NativePluginModuleError::permissions);
    case NativePluginSdkQueryError::owner:
    case NativePluginSdkQueryError::abi: return result(NativePluginModuleError::metadata_mismatch);
    case NativePluginSdkQueryError::budget:
        return {NativePluginModuleError::host, {NativePluginError::budget}, 0};
    default: return result(NativePluginModuleError::query_failed);
    }
}

static NativePluginModuleResult load_module(NativePluginModule* module,
    NativePluginHost* host, ConstSlice<wchar_t> absolute_path, u64 expected_owner, u64 approved) noexcept {
    if (!module || !host || !host->slots.data || !host->slots.count || !host->pages || !host->pages->storage.data)
        return result(NativePluginModuleError::invalid_argument);
    if (module->busy || host->busy) return result(NativePluginModuleError::busy);
    if (module->module || module->state || module->host || module->enabled)
        return result(NativePluginModuleError::already_loaded);
    if (!valid_path(absolute_path)) return result(NativePluginModuleError::invalid_path);
    wchar_t terminated[kNativePluginModuleMaxPathCharacters + 1];
    for (u32 index = 0; index < absolute_path.count; ++index) terminated[index] = absolute_path[index];
    terminated[absolute_path.count] = 0;
    module->busy = true;
    module->host = host;
    module->module = LoadLibraryExW(terminated, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module->module) return failed_load(module, result(NativePluginModuleError::load_failed, GetLastError()));
    const auto query = reinterpret_cast<mc_query_fn>(GetProcAddress(module->module, kNativePluginQueryExport));
    if (!query) return failed_load(module, result(NativePluginModuleError::missing_export, GetLastError()));
    if (!host->sdk) return failed_load(module, result(NativePluginModuleError::unsupported));
    module->sdk = native_plugin_sdk_create(host->sdk, approved);
    if (!module->sdk) return failed_load(module, result(NativePluginModuleError::allocation_failed));
    NativePluginDefinition adapter{};
    const auto queried = native_plugin_sdk_query(module->sdk, query, expected_owner, &adapter);
    if (queried != NativePluginSdkQueryError::none) return failed_load(module, query_error(queried));
    const auto validation = validate_definition(*host, adapter);
    if (!validation.ok()) return failed_load(module, host_result(validation));
    module->owner_id = adapter.owner_id;
    const auto allocated = allocate_state(module, adapter.state_bytes);
    if (!allocated.ok()) return failed_load(module, allocated);
    const auto enabled = native_plugin_sdk_enable(module->sdk, host,
        adapter, {static_cast<u8*>(module->state), module->state_bytes});
    if (!enabled.ok()) return failed_load(module, host_result(enabled));
    module->enabled = true;
    module->generation = native_plugin_host_find(*host, adapter.owner_id)->generation;
    const auto integrity = release_integrity(module);
    module->busy = false;
    if (!integrity.ok()) return integrity;
    return host_result(enabled);
}

NativePluginModuleResult native_plugin_module_load(NativePluginModule* module,
    NativePluginHost* host, ConstSlice<wchar_t> absolute_path, u64 approved) noexcept {
    if (module && module->busy) return result(NativePluginModuleError::busy);
    if (module && (module->integrity || module->sdk))
        return result(NativePluginModuleError::already_loaded);
    return load_module(module, host, absolute_path, 0, approved);
}

NativePluginModuleResult native_plugin_module_load_declared(NativePluginModule* module,
    NativePluginHost* host, ConstSlice<wchar_t> directory, const NativePluginManifest& manifest, u64 approved) noexcept {
    if (!module || !host || !host->slots.data || !host->slots.count || !host->pages || !host->pages->storage.data)
        return result(NativePluginModuleError::invalid_argument);
    if (module->busy || host->busy) return result(NativePluginModuleError::busy);
    if (module->module || module->state || module->host || module->enabled || module->integrity || module->sdk)
        return result(NativePluginModuleError::already_loaded);
    if (!native_plugin_manifest_valid(manifest)) return result(NativePluginModuleError::manifest);
    if (manifest.min_host_abi > kPluginSdkHostAbi) return result(NativePluginModuleError::unsupported);
    const u64 expected_owner = manifest.owner_id;
    if (expected_owner == host->policy.reserved_owner_id || native_plugin_host_find(*host, expected_owner))
        return host_result({NativePluginError::duplicate_owner});
    // Allocate the module record before any filesystem handle is acquired; even a rare
    // failed CloseHandle remains visible and retryable through unload.
    module->integrity = static_cast<NativePluginVerifiedFile*>(VirtualAlloc(nullptr,
        sizeof(NativePluginVerifiedFile), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!module->integrity) return result(NativePluginModuleError::allocation_failed, GetLastError());
    const auto checked = native_plugin_declared_file_verify(directory, manifest, module->integrity);
    if (!checked.ok()) {
        auto failed = result(NativePluginModuleError::integrity, checked.win32_error);
        failed.integrity_error = checked.error;
        if (checked.error == NativePluginCatalogError::release) {
            failed.error = NativePluginModuleError::release_failed;
            return failed;
        }
        const auto released = release_integrity(module);
        return released.ok() ? failed : released;
    }
    return load_module(module, host, {module->integrity->path, module->integrity->path_characters}, expected_owner,
        approved & manifest.permissions);
}

NativePluginModuleResult native_plugin_module_unload(NativePluginModule* module) noexcept {
    if (!module) return result(NativePluginModuleError::invalid_argument);
    if (module->busy || (module->host && module->host->busy)) return result(NativePluginModuleError::busy);
    module->busy = true;
    if (module->enabled) {
        if (!module->host || !module->owner_id) {
            module->busy = false;
            return result(NativePluginModuleError::invalid_argument);
        }
        const auto* slot = native_plugin_host_find(*module->host, module->owner_id);
        // A caller can call disable_all before unloading its modules.
        // Never disable a replacement owner that now belongs to other storage.
        if (slot && (slot->state != module->state || slot->generation != module->generation)) {
            module->busy = false;
            return result(NativePluginModuleError::invalid_argument);
        }
        if (slot) {
            const auto disabled = native_plugin_host_disable(module->host, module->owner_id);
            if (!disabled.ok()) {
                module->busy = false;
                return host_result(disabled);
            }
        }
        module->enabled = false;
    }
    const auto released = release_resources(module);
    module->busy = false;
    return released;
}

} // namespace maicord
