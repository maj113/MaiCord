#include "maicord/app/plugin_module_loader.hpp"
#include "maicord/app/plugin_settings_projection.hpp"
#include "fixtures/native_plugin_fixture.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/sys/crypto.hpp"

namespace maicord {
namespace {
constexpr u64 kFixtureOwner = native_plugin_fixture::owner_id;
using native_plugin_fixture::FixtureAudit;
using ConfigureFixture = native_plugin_fixture::Configure;
struct DetachContext {
    NativePluginModule* module;
    FixtureAudit* audit;
    UiSettingsScene* scene;
    const NativePluginSdkRuntime* runtime;
    bool live_page, live_state, reentry_busy, live_menu_labels, detached_scene;
};
// SDK-owned menu items of the fixture owner for target.
u32 fixture_items(const NativePluginSdkRuntime& runtime, u32 target, NativePluginSdkMenuItem* first = nullptr) noexcept {
    NativePluginSdkMenuItem items[4]{};
    const u32 count = native_plugin_sdk_menu_items(runtime, target, {items, 4});
    u32 owned = 0;
    for (u32 i = 0; i < count; ++i)
        if (items[i].owner_id == kFixtureOwner && !owned++ && first) *first = items[i];
    return owned;
}

void detach_owner(void* memory, u64 owner) noexcept {
    auto* context = static_cast<DetachContext*>(memory);
    ++context->audit->host_detaches;
    context->live_page = ui_page_registry_find(*context->audit->pages, {owner, 1}) != nullptr;
    NativePluginSdkMenuItem friends{};
    context->live_menu_labels = owner == kFixtureOwner && fixture_items(*context->runtime, MC_MENU_USER, &friends) == 1 &&
        utf8_equal(friends.label, utf8_literal("Open native fixture for friend"));
    if (context->scene && context->scene->active) {
        context->detached_scene = context->scene->selected_page.owner_id == owner;
        ui_settings_scene_close(context->scene);
    }
    MEMORY_BASIC_INFORMATION region{};
    context->live_state = context->module->module && context->module->state &&
        VirtualQuery(context->module->state, &region, sizeof(region)) == sizeof(region) && region.State == MEM_COMMIT;
    context->reentry_busy = native_plugin_module_unload(context->module).error == NativePluginModuleError::busy;
}

bool fixture_path(wchar_t* output, u32* count) noexcept {
    const DWORD length = GetModuleFileNameW(nullptr, output, kNativePluginModuleMaxPathCharacters + 1);
    if (!length || length > kNativePluginModuleMaxPathCharacters) return false;
    u32 filename = length;
    while (filename && output[filename - 1] != L'\\') --filename;
    constexpr wchar_t name[] = L"MaiCordPluginFixture.dll";
    constexpr u32 name_chars = sizeof(name) / sizeof(name[0]) - 1;
    if (!filename || filename + name_chars > kNativePluginModuleMaxPathCharacters) return false;
    for (u32 index = 0; index <= name_chars; ++index) output[filename + index] = name[index];
    *count = filename + name_chars;
    return true;
}

NativePluginModuleResult configured_load(NativePluginModule* module, NativePluginHost* host,
    ConstSlice<wchar_t> path, u32 mode, u64 owner, FixtureAudit* audit) noexcept {
    HMODULE pin = LoadLibraryExW(path.data, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!pin) return {NativePluginModuleError::load_failed, {}, GetLastError()};
    const auto configure = reinterpret_cast<ConfigureFixture>(GetProcAddress(pin, "maicord_plugin_fixture_configure"));
    if (!configure) {
        const DWORD error = GetLastError();
        FreeLibrary(pin);
        return {NativePluginModuleError::missing_export, {}, error};
    }
    configure(mode, owner, audit);
    const auto loaded = native_plugin_module_load(module, host, path, MC_PERM_ALL);
    FreeLibrary(pin);
    return loaded;
}

bool empty_module(const NativePluginModule& module) noexcept {
    return !module.module && !module.host && !module.owner_id && !module.generation && !module.state &&
        !module.state_bytes && !module.allocation_bytes && !module.enabled && !module.busy && !module.integrity && !module.sdk;
}
bool fixture_digest(const wchar_t* path, u8* output) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    u8 object[4096], bytes[4096]; DWORD needed = 0, returned = 0;
    BCRYPT_HASH_HANDLE hash{};
    bool okay = BCryptGetProperty(BCRYPT_SHA256_ALG_HANDLE, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&needed), sizeof(needed), &returned, 0) >= 0 &&
        returned == sizeof(needed) && needed && needed <= sizeof(object) &&
        BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, object, needed, nullptr, 0, 0) >= 0;
    while (okay) {
        DWORD read = 0;
        if (!ReadFile(file, bytes, sizeof(bytes), &read, nullptr)) { okay = false; break; }
        if (!read) break;
        okay = BCryptHashData(hash, bytes, read, 0) >= 0;
    }
    if (okay) okay = BCryptFinishHash(hash, output, 32, 0) >= 0;
    if (hash && BCryptDestroyHash(hash) < 0) okay = false;
    if (!CloseHandle(file)) okay = false;
    return okay;
}
// The scene reads native settings content, never plugin pages: project the plugin's
// page into host-owned controls and show a registry copy that points at them.
struct ScenePage {
    UiSettingsControl controls[8];
    UiSettingsPageContent content;
    UiPage bank[4];
    UiPageRegistry display;
};
bool open_page(UiSettingsScene* scene, const UiPageRegistry& pages, ScenePage* projected, UiPageKey key) noexcept {
    const auto* descriptor = ui_page_registry_find(pages, key);
    NativePluginSettingsSnapshot source{};
    if (!descriptor || pages.count > 4 ||
        native_plugin_settings_snapshot(descriptor->context, &source) != NativePluginSettingsError::none ||
        native_plugin_settings_project(source, {projected->controls, 8}, &projected->content) !=
            NativePluginSettingsError::none) return false;
    projected->display = pages;
    projected->display.storage = {projected->bank, 4};
    for (u32 index = 0; index < pages.count; ++index) {
        projected->bank[index] = pages.storage[index];
        if (ui_page_key_equal(pages.storage[index].key, key)) projected->bank[index].context = &projected->content;
    }
    return ui_settings_scene_update(scene, &projected->display, {0, 0, 800, 600}) &&
        ui_settings_scene_select(scene, key);
}
bool loaded_from(HMODULE module, ConstSlice<wchar_t> expected) noexcept {
    wchar_t actual[kNativePluginModuleMaxPathCharacters + 1];
    const DWORD count = GetModuleFileNameW(module, actual, kNativePluginModuleMaxPathCharacters + 1);
    if (count != expected.count) return false;
    for (u32 index = 0; index < count; ++index) if (actual[index] != expected[index]) return false;
    return true;
}
} // namespace

void test_plugin_module_loader(void (*check)(bool, const char*) noexcept) noexcept {
    UiPage storage[4]{};
    UiPageRegistry pages{};
    UiSettingsPageContent builtin_content{utf8_literal("Built-in"), {}, {}, 1, kUiSettingsPageFormat};
    UiSettingsScene scene{};
    static ScenePage projected{};
    NativePluginSlot slots[2]{};
    NativePluginHost host{};
    NativePluginModule module{};
    FixtureAudit audit{};
    audit.pages = &pages;
    static NativePluginSdkRuntime runtime{};
    DetachContext detach{&module, &audit, &scene, &runtime};
    NativePluginPolicy policy{native_plugin_capability_pages, 4, 4096, 8192, detach_owner, &detach, 10};
    check(ui_page_registry_initialize(&pages, {storage, 4}) &&
        native_plugin_host_initialize(&host, &pages, {slots, 2}, policy) &&
        ui_page_registry_register(&pages, {{10, 1}, utf8_literal("Built-in"), &builtin_content}).ok(),
        "DLL loader initializes caller-owned host and preserves independent native pages");
    host.sdk = &runtime;
    wchar_t path[kNativePluginModuleMaxPathCharacters + 1];
    u32 path_chars = 0;
    const bool located = fixture_path(path, &path_chars);
    check(located, "DLL fixture path resolves beside the external test executable");
    if (!located) return;
    const ConstSlice<wchar_t> fixture{path, path_chars};
    constexpr wchar_t relative[] = L"MaiCordPluginFixture.dll";
    constexpr wchar_t drive_relative[] = L"D:MaiCordPluginFixture.dll";
    constexpr wchar_t device[] = L"\\\\.\\MaiCordPluginFixture.dll";
    constexpr wchar_t executable[] = L"D:\\MaiCordPluginFixture.exe";
    constexpr wchar_t embedded[] = L"D:\\Mai\0CordPluginFixture.dll";
    check(native_plugin_module_load(&module, &host, {relative, sizeof(relative) / sizeof(relative[0]) - 1}).error == NativePluginModuleError::invalid_path &&
        native_plugin_module_load(&module, &host, {drive_relative, sizeof(drive_relative) / sizeof(drive_relative[0]) - 1}).error == NativePluginModuleError::invalid_path &&
        native_plugin_module_load(&module, &host, {device, sizeof(device) / sizeof(device[0]) - 1}).error == NativePluginModuleError::invalid_path &&
        native_plugin_module_load(&module, &host, {executable, sizeof(executable) / sizeof(executable[0]) - 1}).error == NativePluginModuleError::invalid_path &&
        native_plugin_module_load(&module, &host, {embedded, sizeof(embedded) / sizeof(embedded[0]) - 1}).error ==
            NativePluginModuleError::invalid_path &&
        native_plugin_module_load(&module, &host, {path, kNativePluginModuleMaxPathCharacters + 1}).error ==
            NativePluginModuleError::invalid_path && empty_module(module) && !host.active_count,
        "DLL loader rejects relative, device, executable, embedded-NUL and overlong paths before loading code");
    check(native_plugin_module_unload(&module).ok() && empty_module(module),
        "DLL loader validates caller objects and empty unload is idempotent");

    HMODULE pin = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(pin != nullptr, "real no-CRT DLL fixture loads with module-folder and System32 dependency flags");
    if (!pin) return;
    const auto query = reinterpret_cast<mc_query_fn>(GetProcAddress(pin, kNativePluginQueryExport));
    const mc_host_info info{sizeof(mc_host_info), kPluginSdkHostAbi};
    mc_plugin_definition queried{};
    queried.size = sizeof(queried);
    check(query && query(&info, &queried) == MC_OK && queried.abi_version == MC_ABI_VERSION &&
        queried.owner_id == kFixtureOwner && queried.start && queried.stop &&
        queried.permissions == (MC_PERM_SETTINGS | MC_PERM_MENUS) &&
        !GetProcAddress(pin, "maicord_plugin_query"),
        "named C export fills an ABI-4 definition and no older query export exists");
    FreeLibrary(pin);

    host.busy = true;
    check(native_plugin_module_load(&module, &host, fixture, MC_PERM_ALL).error == NativePluginModuleError::busy && empty_module(module),
        "busy host refuses DLL load before module entry or query runs");
    host.busy = false;
    const auto loaded = configured_load(&module, &host, fixture, 0, kFixtureOwner, &audit);
    check(loaded.ok() && module.module && loaded_from(module.module, fixture) &&
        module.host == &host && module.owner_id == kFixtureOwner &&
        module.generation && module.enabled && module.state && module.allocation_bytes >= module.state_bytes &&
        module.state_bytes == host.state_bytes && audit.starts == 1 && audit.state_zero && audit.state_aligned &&
        audit.dll_detaches == 0 && pages.count == 2 && fixture_items(runtime, MC_MENU_USER) == 1 &&
        fixture_items(runtime, MC_MENU_MESSAGE) == 1,
        "explicit DLL load owns aligned zeroed Win32 state and registers settings page plus user and message items");
    if (!loaded.ok()) return;
    NativePluginModule duplicate{};
    const auto duplicate_result = configured_load(&duplicate, &host, fixture, 0, kFixtureOwner, &audit);
    check(duplicate_result.error == NativePluginModuleError::host &&
        duplicate_result.host_result.error == NativePluginError::duplicate_owner && empty_module(duplicate) &&
        audit.starts == 1 && host.active_count == 1 && module.enabled,
        "duplicate DLL owner is rejected before state allocation or a second start");
    check(native_plugin_module_load(&module, &host, fixture, MC_PERM_ALL).error == NativePluginModuleError::already_loaded,
        "already-owned module cannot overwrite a live lifecycle");
    NativePluginUiToken token{};
    const UiSettingsActivation activation{{UiSettingsTargetKind::control, {kFixtureOwner, 1}, 1},
        UiSettingsControlKind::toggle, true, 0};
    check(native_plugin_host_settings_token(&host, {kFixtureOwner, 1}, &token).ok(),
        "loaded module settings page captures a generation and descriptor token");
    const auto activated = native_plugin_host_activate_settings(&host, token, activation);
    const auto* page = ui_page_registry_find(pages, {kFixtureOwner, 1});
    const auto* content = page ? static_cast<const NativePluginSettingsPage*>(page->context) : nullptr;
    check(activated.ok() && activated.changed && content && content->controls[0].checked &&
        native_plugin_host_activate_settings(&host, token, activation).error == NativePluginError::stale,
        "loaded DLL toggle dispatch updates its borrowed projection and expires the old revision token");
    NativePluginSdkMenuContext friend_action{};
    friend_action.target = MC_MENU_USER;
    friend_action.user_id = 1234;
    NativePluginSdkMenuContext chat_action{};
    chat_action.target = MC_MENU_MESSAGE;
    chat_action.user_id = 5678;
    chat_action.channel_id = 9012;
    chat_action.message_id = 3456;
    chat_action.flags = MC_MENU_OWN | MC_MENU_PRIVATE | 0x80u;
    check(native_plugin_sdk_menu_activate(&runtime, kFixtureOwner, 1, friend_action) &&
        audit.menu_activations == 1 && audit.last_menu_activation.item_id == 1 &&
        audit.last_menu_activation.target == MC_MENU_USER && audit.last_menu_activation.user_id == 1234 &&
        !audit.last_menu_activation.channel_id && !audit.last_menu_activation.message_id,
        "actual DLL user menu handler receives the typed context with only the user ID");
    check(native_plugin_sdk_menu_activate(&runtime, kFixtureOwner, 2, chat_action) &&
        audit.menu_activations == 2 && audit.last_menu_activation.item_id == 2 &&
        audit.last_menu_activation.target == MC_MENU_MESSAGE && audit.last_menu_activation.user_id == 5678 &&
        audit.last_menu_activation.channel_id == 9012 && audit.last_menu_activation.message_id == 3456 &&
        audit.last_menu_activation.flags == (MC_MENU_OWN | MC_MENU_PRIVATE),
        "actual DLL message handler receives copied IDs and only defined context flags");
    const u32 callback_count = audit.menu_activations;
    auto wrong_target = chat_action;
    wrong_target.target = MC_MENU_USER;
    check(!native_plugin_sdk_menu_activate(&runtime, kFixtureOwner, 2, wrong_target) &&
        !native_plugin_sdk_menu_activate(&runtime, kFixtureOwner + 1, 2, chat_action) &&
        audit.menu_activations == callback_count,
        "an item activated for another target or owner never enters the DLL");
    const u64 revision = runtime.menu_revision;
    check(audit.remove_menu && audit.remove_menu(2) && runtime.menu_revision != revision &&
        !fixture_items(runtime, MC_MENU_MESSAGE) && fixture_items(runtime, MC_MENU_USER) == 1 &&
        !native_plugin_sdk_menu_activate(&runtime, kFixtureOwner, 2, chat_action) &&
        audit.menu_activations == callback_count,
        "actual DLL menu removal bumps the menu revision and its item can no longer be activated");
    check(open_page(&scene, pages, &projected, {kFixtureOwner, 1}), "borrowed DLL page is reopened before unload");
    check(native_plugin_host_settings_token(&host, {kFixtureOwner, 1}, &token).ok(), "DLL activation token refreshes explicitly");
    const u32 detaches_before_unload = audit.host_detaches;
    detach.live_page = detach.live_state = detach.reentry_busy = false;
    detach.live_menu_labels = detach.detached_scene = false;
    HMODULE owned_handle = module.module;
    void* owned_state = module.state;
    host.busy = true;
    check(native_plugin_module_unload(&module).error == NativePluginModuleError::busy &&
        module.module == owned_handle && module.state == owned_state && module.enabled && pages.count == 2,
        "busy unload preserves DLL, state, host accounting and borrowed pages for retry");
    host.busy = false;
    check(native_plugin_module_unload(&module).ok() && empty_module(module) && !host.active_count &&
        !host.state_bytes && pages.count == 1 && !fixture_items(runtime, MC_MENU_USER) && detach.live_page &&
        detach.live_state && detach.reentry_busy && detach.live_menu_labels && detach.detached_scene && !scene.active &&
        audit.host_detaches == detaches_before_unload + 1 &&
        audit.stops == 1 && audit.stop_after_detach && audit.stop_without_pages && audit.stop_services_inactive &&
        audit.stop_menu_services_inactive &&
        audit.dll_detaches == 1 && audit.dll_after_stop && GetModuleHandleW(L"MaiCordPluginFixture.dll") == nullptr &&
        native_plugin_host_activate_settings(&host, token, activation).error == NativePluginError::stale &&
        !native_plugin_sdk_menu_activate(&runtime, kFixtureOwner, 1, friend_action) &&
        audit.menu_activations == callback_count,
        "unload detaches the live page and items, drops them, stops with services inactive and frees actual DLL");
    check(configured_load(&module, &host, fixture, 0, kFixtureOwner, &audit).ok() &&
        native_plugin_host_activate_settings(&host, token, activation).error == NativePluginError::stale &&
        runtime.menu_revision != revision && native_plugin_module_unload(&module).ok(),
        "DLL reload cannot revive prior UI tokens");

    audit = {};
    audit.pages = &pages;
    detach.live_page = detach.live_state = detach.reentry_busy = false;
    detach.live_menu_labels = detach.detached_scene = false;
    const auto failed_start = configured_load(&module, &host, fixture, 3, kFixtureOwner, &audit);
    check(failed_start.error == NativePluginModuleError::host && failed_start.host_result.error == NativePluginError::start_failed &&
        empty_module(module) && pages.count == 1 && !fixture_items(runtime, MC_MENU_USER) && !host.active_count &&
        !host.state_bytes && audit.starts == 1 &&
        audit.stops == 1 && audit.host_detaches == 1 && detach.live_page && detach.live_state &&
        detach.reentry_busy && audit.stop_after_detach &&
        audit.stop_without_pages && audit.stop_services_inactive &&
        audit.stop_menu_services_inactive && audit.dll_detaches == 1 && audit.dll_after_stop,
        "failed DLL start rolls back its menu items and settings page before stop, state and module release");
    // Query, ABI, permission, owner, layout and hard state-budget rejections.
    constexpr u32 rejected_modes[]{1, 2, 4, 5, 6, 7};
    for (const u32 mode : rejected_modes) {
        audit = {};
        audit.pages = &pages;
        const auto rejected = configured_load(&module, &host, fixture, mode, kFixtureOwner, &audit);
        const NativePluginModuleError expected = mode == 2 ? NativePluginModuleError::metadata_mismatch :
            mode == 4 ? NativePluginModuleError::permissions : mode == 7 ? NativePluginModuleError::host :
            NativePluginModuleError::query_failed;
        check(rejected.error == expected && (mode != 7 || rejected.host_result.error == NativePluginError::budget) &&
            empty_module(module) && audit.queries == 1 && !audit.starts && !audit.stops && audit.dll_detaches == 1 &&
            pages.count == 1 && !fixture_items(runtime, MC_MENU_USER),
            "query, ABI, permission, owner, layout and hard state-budget rejections release DLL without starting it");
    }
    audit = {};
    audit.pages = &pages;
    // Settings permission only: the menu interface is denied and start fails after it began.
    const auto denied_menus = configured_load(&module, &host, fixture, 8, kFixtureOwner, &audit);
    check(denied_menus.error == NativePluginModuleError::host &&
        denied_menus.host_result.error == NativePluginError::start_failed && empty_module(module) &&
        audit.starts == 1 && audit.stops == 1 && pages.count == 1 && !fixture_items(runtime, MC_MENU_USER),
        "an unapproved interface fails start and rolls the plugin back");
    audit = {};
    audit.pages = &pages;
    check(configured_load(&module, &host, fixture, 0, 10, &audit).host_result.error == NativePluginError::duplicate_owner &&
        empty_module(module) && !audit.starts, "reserved native owner is rejected before DLL start");
    host.policy.max_state_bytes_per_plugin = 1;
    check(configured_load(&module, &host, fixture, 0, kFixtureOwner, &audit).host_result.error == NativePluginError::budget &&
        empty_module(module) && !audit.starts, "DLL state declaration must fit host policy before allocation");
    host.policy.max_state_bytes_per_plugin = policy.max_state_bytes_per_plugin;
    host.policy.max_total_state_bytes = 1;
    check(configured_load(&module, &host, fixture, 0, kFixtureOwner, &audit).host_result.error == NativePluginError::budget &&
        empty_module(module) && !audit.starts, "DLL aggregate state declaration is prevalidated");
    host.policy.max_total_state_bytes = policy.max_total_state_bytes;
    check(configured_load(&module, &host, fixture, 0, kFixtureOwner, &audit).ok() &&
        native_plugin_host_disable_all(&host).ok() && native_plugin_module_unload(&module).ok() &&
        empty_module(module) && pages.count == 1 && !fixture_items(runtime, MC_MENU_USER),
        "explicit host disable_all permits later module unload without a repeated stop");

    wchar_t system_path[MAX_PATH];
    const UINT system_chars = GetSystemDirectoryW(system_path, MAX_PATH);
    constexpr wchar_t system_dll[] = L"\\kernel32.dll";
    constexpr u32 system_suffix = sizeof(system_dll) / sizeof(system_dll[0]) - 1;
    if (system_chars && system_chars + system_suffix < MAX_PATH) {
        for (u32 index = 0; index <= system_suffix; ++index) system_path[system_chars + index] = system_dll[index];
        check(native_plugin_module_load(&module, &host, {system_path, system_chars + system_suffix}).error ==
            NativePluginModuleError::missing_export && empty_module(module) && pages.count == 1,
            "DLL without the exact query export releases its owned module reference");
    } else check(false, "System32 path for missing-export validation fits the fixed test buffer");
    constexpr wchar_t missing[] = L"C:\\MaiCord-absent-plugin-fixture-2026-10-01.dll";
    const auto absent = native_plugin_module_load(&module, &host, {missing, sizeof(missing) / sizeof(missing[0]) - 1});
    check(absent.error == NativePluginModuleError::load_failed && absent.win32_error && empty_module(module) &&
        pages.count == 1 && native_plugin_module_unload(&module).ok(),
        "absent absolute DLL reports a Win32 failure and leaves no lifecycle resources");

    NativePluginManifest declared{};
    declared.owner_id = kFixtureOwner; declared.abi_version = 5;
    declared.permissions = MC_PERM_SETTINGS | MC_PERM_MENUS;
    constexpr auto declared_name = utf8_literal("Known offline fixture");
    constexpr auto declared_version = utf8_literal("1");
    constexpr auto declared_dll = utf8_literal("MaiCordPluginFixture.dll");
    copy_bytes(declared.name, declared_name.data, declared_name.bytes); declared.name_bytes = declared_name.bytes;
    copy_bytes(declared.version, declared_version.data, declared_version.bytes); declared.version_bytes = declared_version.bytes;
    copy_bytes(declared.dll, declared_dll.data, declared_dll.bytes); declared.dll_bytes = declared_dll.bytes;
    u32 directory_chars = path_chars;
    while (directory_chars && path[directory_chars - 1] != L'\\') --directory_chars;
    if (directory_chars > 3) --directory_chars;
    const bool hashed = fixture_digest(path, declared.sha256);
    check(hashed && directory_chars, "declared loader test hashes only the known built offline fixture");
    if (hashed && directory_chars) {
        audit = {}; audit.pages = &pages;
        pin = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto configure = pin ? reinterpret_cast<ConfigureFixture>(GetProcAddress(pin, "maicord_plugin_fixture_configure")) : nullptr;
        check(configure != nullptr, "declared fixture configures observations without loading arbitrary code");
        if (configure) {
            configure(0, kFixtureOwner, &audit);
            const ConstSlice<wchar_t> directory{path, directory_chars};
            NativePluginManifest altered = declared; altered.sha256[0] ^= 1;
            const auto rejected = native_plugin_module_load_declared(&module, &host, directory, altered);
            check(rejected.error == NativePluginModuleError::integrity && rejected.integrity_error == NativePluginCatalogError::digest &&
                !audit.queries && !audit.starts && empty_module(module) && !host.active_count,
                "declared digest mismatch blocks query/start and releases transient VM and file pins");
            altered = declared; ++altered.owner_id;
            const auto owner_mismatch = native_plugin_module_load_declared(&module, &host, directory, altered);
            check(owner_mismatch.error == NativePluginModuleError::metadata_mismatch && audit.queries == 1 && !audit.starts && empty_module(module),
                "declared owner mismatch is rejected after query and before native host start");
            configure(2, kFixtureOwner, &audit);
            const auto abi_mismatch = native_plugin_module_load_declared(&module, &host, directory, declared, MC_PERM_ALL);
            check(abi_mismatch.error == NativePluginModuleError::metadata_mismatch && audit.queries == 2 && !audit.starts && empty_module(module),
                "declared ABI mismatch cannot publish native descriptors or enter start");
            configure(0, kFixtureOwner, &audit);
            const auto enabled_declared = native_plugin_module_load_declared(&module, &host, directory, declared, MC_PERM_ALL);
            check(enabled_declared.ok() && module.enabled && !module.integrity && audit.starts == 1 && host.active_count == 1 &&
                native_plugin_module_unload(&module).ok() && empty_module(module) && audit.stops == 1,
                "explicit declared fixture enables ABI 4 then releases its one lazy verification page and unloads cleanly");
        }
        if (pin) FreeLibrary(pin);
    }
    ui_settings_scene_release(&scene);
}
} // namespace maicord
