#include "application_state.hpp"
#include "application_settings.hpp"
#include "application_plugins.hpp"
#include "application_plugins_runtime.hpp"
#include "application_text_form.hpp"
#include "fixtures/native_plugin_fixture.hpp"
#include "maicord/core/utf.hpp"
#include "../src/app/account_ui_runtime.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/crypto.hpp"

namespace maicord {
namespace settings_runtime_test { void measurement(const char*, u64) noexcept; }
namespace app_detail { const ApplicationTextFormRequest* settings_runtime_text_form() noexcept; }
namespace plugin_catalog_runtime {
using namespace app_detail;
struct Fixture {
    wchar_t root[1024], directory[1024], dll[1024], original_environment[1024];
    u32 directory_chars, original_chars;
    u8 digest[32];
    native_plugin_fixture::FixtureAudit audit;
    HMODULE pin;
    bool environment, directories;
};
struct Reentry {
    ApplicationPlugins* owner;
    u32 stops;
    bool closing, nested_rejected, guard_preserved, choose_rejected, owner_retained, no_picker;
} reentry{};
void stop_reentry() noexcept {
    ++reentry.stops;
    reentry.closing = g_app.plugins == reentry.owner && g_app.plugins->closing;
    reentry.nested_rejected = !application_plugins_release();
    reentry.guard_preserved = g_app.plugins == reentry.owner && g_app.plugins->closing;
    reentry.choose_rejected = !application_plugins_choose();
    reentry.owner_retained = g_app.plugins == reentry.owner;
    reentry.no_picker = g_app.plugins == reentry.owner && !g_app.plugins->selecting &&
        !g_app.plugins->picker.runtime && !application_plugins_notification();
}
void restore_abandon() noexcept {
    application_plugins_account_scope_invalidated();
    g_app.login.visible = true;
    // The real teardown path reaches Settings during a native callback. Its
    // guarded first release must retain backing until the restore attempt ends.
    application_settings_release();
}
bool join(wchar_t* out, const wchar_t* directory, const wchar_t* leaf) noexcept {
    u32 at = 0;
    while (directory[at] && at < 1023) { out[at] = directory[at]; ++at; }
    if (at == 1023) return false;
    if (at && out[at - 1] != L'\\') out[at++] = L'\\';
    for (u32 i = 0; leaf[i]; ++i) { if (at >= 1023) return false; out[at++] = leaf[i]; }
    out[at] = 0; return true;
}
bool digest(const wchar_t* path, u8* output) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    u8 object[4096], bytes[4096]; DWORD needed = 0, returned = 0;
    BCRYPT_HASH_HANDLE hash{};
    bool okay = BCryptGetProperty(BCRYPT_SHA256_ALG_HANDLE, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&needed), sizeof(needed), &returned, 0) >= 0 &&
        returned == sizeof(needed) && needed && needed <= sizeof(object) &&
        BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, object, needed, nullptr, 0, 0) >= 0;
    while (okay) {
        DWORD count = 0;
        if (!ReadFile(file, bytes, sizeof(bytes), &count, nullptr)) { okay = false; break; }
        if (!count) break;
        okay = BCryptHashData(hash, bytes, count, 0) >= 0;
    }
    if (okay) okay = BCryptFinishHash(hash, output, 32, 0) >= 0;
    if (hash && BCryptDestroyHash(hash) < 0) okay = false;
    if (!CloseHandle(file)) okay = false;
    return okay;
}
void append(char* out, u32& at, Utf8View value) noexcept {
    for (u32 i = 0; i < value.bytes; ++i) out[at++] = value.data[i];
}
void number(char* out, u32& at, u64 value) noexcept {
    char reversed[20]; u32 count = 0;
    do { reversed[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    while (count) out[at++] = reversed[--count];
}
void manifest_leaf(wchar_t* output, u32 index) noexcept {
    constexpr wchar_t pattern[] = L"00.maicord-plugin.json";
    for (u32 i = 0; i < sizeof(pattern) / sizeof(pattern[0]); ++i) output[i] = pattern[i];
    output[0] = static_cast<wchar_t>(L'0' + index / 10); output[1] = static_cast<wchar_t>(L'0' + index % 10);
}
bool write_manifest(Fixture& f, u32 index, bool correct_digest, bool changed = false) noexcept {
    wchar_t leaf[32], path[1024]; manifest_leaf(leaf, index);
    if (!join(path, f.directory, leaf)) return false;
    char json[1024]; u32 at = 0;
    append(json, at, utf8_literal("{\"schema\":3,\"owner_id\":"));
    number(json, at, index ? 1000 + index : native_plugin_fixture::owner_id);
    append(json, at, utf8_literal(",\"abi\":5,\"permissions\":[\"settings\",\"menus\"],\"name\":\"Local fixture plugin\",\"version\":\""));
    append(json, at, changed ? utf8_literal("1.1") : utf8_literal("1.0"));
    append(json, at, utf8_literal("\",\"description\":\"Native catalog runtime fixture\",\"dll\":\""));
    if (index) { append(json, at, utf8_literal("missing")); number(json, at, index); append(json, at, utf8_literal(".dll")); }
    else append(json, at, utf8_literal("MaiCordPluginFixture.dll"));
    append(json, at, utf8_literal("\",\"sha256\":\""));
    constexpr char hex[] = "0123456789abcdef";
    for (u32 i = 0; i < 32; ++i) {
        const u8 value = correct_digest ? f.digest[i] : 0;
        json[at++] = hex[value >> 4]; json[at++] = hex[value & 15];
    }
    append(json, at, utf8_literal("\"}"));
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; const bool okay = WriteFile(file, json, at, &written, nullptr) && written == at;
    return CloseHandle(file) && okay;
}
bool setup(Fixture& f) noexcept {
    if (g_app.settings || g_app.plugins || g_app.account_ui) return false;
    f.original_chars = GetEnvironmentVariableW(L"LOCALAPPDATA", f.original_environment, 1024);
    if (f.original_chars >= 1024) return false;
    wchar_t temp[1024], leaf[64];
    const DWORD count = GetTempPathW(1024, temp);
    if (!count || count >= 1024) return false;
    constexpr wchar_t prefix[] = L"MaiCordCatalogRuntime";
    u32 at = 0; for (; at < sizeof(prefix) / sizeof(prefix[0]) - 1; ++at) leaf[at] = prefix[at];
    u32 pid = GetCurrentProcessId(); wchar_t reverse[10]; u32 digits = 0;
    do { reverse[digits++] = static_cast<wchar_t>(L'0' + pid % 10); pid /= 10; } while (pid);
    while (digits) leaf[at++] = reverse[--digits]; leaf[at] = 0;
    if (!join(f.root, temp, leaf) || !CreateDirectoryW(f.root, nullptr) || !SetEnvironmentVariableW(L"LOCALAPPDATA", f.root)) return false;
    f.environment = true;
    g_app.account_ui = static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_ui) return false;
    g_app.renderer_ready = true; g_app.renderer.dpi = 96; g_app.window = nullptr; g_app.login.visible = false;
    g_app.preferences = client_preferences_defaults(); g_app.account.status.profile_id = 500;
    g_app.clock = clock_initialize(); g_app.process_start = clock_now();
    application_plugins_account_scope_invalidated();
    return application_settings_open();
}
bool prepare_files(Fixture& f) noexcept {
    wchar_t maicord[1024], source[1024];
    if (!join(maicord, f.root, L"MaiCord") || !CreateDirectoryW(maicord, nullptr) ||
        !join(f.directory, maicord, L"plugins") || !CreateDirectoryW(f.directory, nullptr)) return false;
    f.directories = true;
    while (f.directory[f.directory_chars]) ++f.directory_chars;
    const DWORD count = GetModuleFileNameW(nullptr, source, 1024);
    if (!count || count >= 1024) return false;
    u32 first = count; while (first && source[first - 1] != L'\\') --first;
    if (!first) return false; source[first] = 0;
    wchar_t path[1024];
    if (!join(path, source, L"MaiCordPluginFixture.dll") || !join(f.dll, f.directory, L"MaiCordPluginFixture.dll") ||
        !CopyFileW(path, f.dll, TRUE) || !digest(f.dll, f.digest)) return false;
    for (u32 i = 0; i < 10; ++i) if (!write_manifest(f, i, true)) return false;
    return true;
}
UiSettingsScene* scene() noexcept { return const_cast<UiSettingsScene*>(g_app.renderer.settings_scene); }
bool activate(u64 id, u32 key) noexcept {
    auto* s = scene();
    if (!s || !ui_settings_scene_select(s, {1, 4}) || !application_settings_synchronize() ||
        !ui_settings_scene_focus(s, {UiSettingsTargetKind::control, {1, 4}, id})) return false;
    return application_settings_input(UiInputKind::key_down, key, false) &&
        application_settings_input(UiInputKind::key_up, key, false);
}
// The first Enable of a plugin goes through the approval prompt (exercised once
// below). Later Enables use the remembered approval, which this seeds directly.
void approve(u32 index) noexcept {
    const auto* catalog = application_plugins_catalog_current();
    if (!g_app.plugins || !catalog || index >= catalog->count) return;
    const auto& manifest = catalog->entries[index].manifest;
    native_plugin_preferences_enable(&g_app.plugins->preferences, manifest, manifest.permissions);
}
bool enable_row(u32 index) noexcept {
    approve(index);
    return activate(kApplicationPluginCatalogActionFirst + index, VK_RETURN);
}
bool freed(void* memory) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    return memory && VirtualQuery(memory, &region, sizeof(region)) == sizeof(region) && region.State == MEM_FREE;
}
bool prepare_install(Fixture& f) noexcept {
    auto& p=*g_app.plugins;wchar_t leaf[32]{},path[1024]{};manifest_leaf(leaf,0);
    if(!join(path,f.directory,leaf))return false;
    p.install_settings=g_app.settings;p.install_lifetime=application_settings_lifetime();
    p.install_account_serial=g_app.plugin_account_scope_serial;p.install_profile=g_app.account.status.profile_id;
    p.installing=true;p.install_consumed=false;p.install_retry=false;p.install_detached=false;
    u32 count{};while(path[count])++count;
    if(native_plugin_bundle_install_start(&p.installer,{path,count},{f.directory,f.directory_chars})!=NativePluginBundleInstallError::none)return false;
    if(WaitForSingleObject(p.installer.notification,2000)!=WAIT_OBJECT_0)return false;
    return native_plugin_bundle_install_status(&p.installer).phase==NativePluginBundleInstallPhase::prepared;
}
bool finish_install() noexcept {
    for(u32 attempt=0;attempt<8&&g_app.plugins&&g_app.plugins->installing;++attempt){
        application_plugins_poll();if(!g_app.plugins->installing)return true;
        const HANDLE notification=application_plugins_notification();if(!notification||WaitForSingleObject(notification,2000)!=WAIT_OBJECT_0)return false;
    }
    application_plugins_poll();return g_app.plugins&&!g_app.plugins->installing&&!g_app.plugins->installer.runtime;
}
void cleanup(Fixture& f) noexcept {
    application_text_form_close(false, false);
    application_settings_release();
    if (f.pin) { FreeLibrary(f.pin); f.pin = nullptr; }
    if (!g_app.settings && !g_app.plugins) {
        if (g_app.account_ui) VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
        g_app.account_ui = nullptr;
    if (f.directories) {
        wchar_t preferences[1024], maicord_preferences[1024];
        if (join(maicord_preferences, f.root, L"MaiCord") && join(preferences, maicord_preferences, L"plugins.bin")) DeleteFileW(preferences);
            for (u32 i = 0; i < 10; ++i) { wchar_t leaf[32], path[1024]; manifest_leaf(leaf, i); if (join(path, f.directory, leaf)) DeleteFileW(path); }
            DeleteFileW(f.dll); RemoveDirectoryW(f.directory);
            wchar_t path[1024]; if (join(path, f.root, L"MaiCord")) RemoveDirectoryW(path);
        }
        if (f.root[0]) RemoveDirectoryW(f.root);
    }
    if (f.environment) SetEnvironmentVariableW(L"LOCALAPPDATA", f.original_chars ? f.original_environment : nullptr);
}
} // namespace plugin_catalog_runtime

void test_application_plugin_catalog_runtime(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace plugin_catalog_runtime;
    // A failed cooperative unload can retain code until process exit. Its audit
    // backing must remain live even when this bounded fixture returns early.
    static Fixture f{};
    struct Restore { Fixture& f; ~Restore() noexcept { cleanup(f); } } restore{f};
    check(setup(f), "catalog UI: isolated environment and actual Settings owner initialize");
    if (!scene()) return;
    check(!g_app.plugins && !application_plugins_catalog_current() && application_settings_owner_bytes() <= 3 * 4096,
        "catalog UI: Settings projection allocates no plugin/catalog owner and keeps three-page budget");
    check(application_plugins_restore() && !g_app.plugins,
        "catalog preferences: missing startup choices retain no Plugins owner and execute no DLL");
    check(activate(kApplicationPluginDiscoverAction, VK_SPACE) && application_plugins_catalog_current() &&
        !application_plugins_catalog_current()->count && !application_plugins_notification(),
        "catalog UI: explicit Space Discover owns metadata only and shows missing-folder inventory");
    auto* catalog_memory = g_app.plugins ? g_app.plugins->catalog : nullptr;
    MEMORY_BASIC_INFORMATION catalog_region{}, plugins_region{};
    check(catalog_memory && VirtualQuery(catalog_memory, &catalog_region, sizeof(catalog_region)) == sizeof(catalog_region) &&
        catalog_region.State == MEM_COMMIT &&
        VirtualQuery(g_app.plugins, &plugins_region, sizeof(plugins_region)) == sizeof(plugins_region) && plugins_region.State == MEM_COMMIT,
        "catalog UI: lazy catalog and plugin runtime expose committed VM ownership");
    settings_runtime_test::measurement("MEASURE plugin_catalog_committed_bytes=", catalog_region.RegionSize);
    settings_runtime_test::measurement("MEASURE plugins_runtime_committed_bytes=", plugins_region.RegionSize);
    const bool prepared = catalog_memory && prepare_files(f);
    check(prepared, "catalog UI: bounded ten-manifest fixture and known compiled DLL created");
    if (!prepared) return;
    check(activate(kApplicationPluginDiscoverAction, VK_RETURN) && application_plugins_catalog_current()->count == 10 &&
        g_app.plugins->catalog == catalog_memory && !GetModuleHandleW(L"MaiCordPluginFixture.dll") && !application_plugins_present(0),
        "catalog UI: Enter Refresh reuses lazy owner and executes no DLL entry/query/start");
    const auto revision = application_plugins_catalog_revision();
    check(activate(kApplicationPluginNextAction, VK_SPACE) && g_app.plugins->catalog_first == 8 &&
        application_plugins_catalog_revision() > revision && scene()->content.controls.count == 7 &&
        scene()->content.controls[0].kind == UiSettingsControlKind::tabs &&
        !application_plugins_catalog_page(true, revision), "catalog UI: page window is eight and stale page revision is rejected");
    check(activate(kApplicationPluginPreviousAction, VK_RETURN) && g_app.plugins->catalog_first == 0,
        "catalog UI: Enter Previous restores first window through shared semantic dispatcher");
    // A click picks the tab under the pointer; stepping to the next tab would
    // land on Updates and then Browse.
    const UiSettingsTarget tabs{UiSettingsTargetKind::control, {1, 4}, scene()->content.controls[0].id};
    const auto selected_tab = [&tabs]() noexcept {
        const auto* control = scene() ? ui_settings_scene_control(*scene(), tabs) : nullptr;
        return control ? control->selected_choice : ~0u;
    };
    const auto click_tab = [&tabs](u32 choice) noexcept {
        UiRect tab{};
        if (!scene() || !ui_settings_scene_choice_rectangle(*scene(), tabs, choice, &tab)) return false;
        const LPARAM at = MAKELPARAM(static_cast<u16>((tab.left + tab.right) * .5f), static_cast<u16>((tab.top + tab.bottom) * .5f));
        return application_settings_window_message(WM_LBUTTONDOWN, MK_LBUTTON, at) &&
            application_settings_window_message(WM_LBUTTONUP, 0, at);
    };
    check(selected_tab() == 1 && click_tab(3) && selected_tab() == 3 && click_tab(1) && selected_tab() == 1,
        "catalog UI: clicking a tab selects that tab, not the next one");
    // Press an existing Enable row, change the same manifest, then release. The
    // row ID/name/count are unchanged, so only content/catalog admission rejects it.
    const UiSettingsTarget enable{UiSettingsTargetKind::control, {1, 4}, kApplicationPluginCatalogActionFirst};
    UiRect anchor{};
    check(ui_settings_scene_focus(scene(), enable) && ui_settings_scene_control_anchor(*scene(), enable, &anchor),
        "catalog UI: actual native Enable row exposes mouse anchor");
    const LPARAM point = MAKELPARAM(static_cast<u16>((anchor.left + anchor.right) * .5f), static_cast<u16>((anchor.top + anchor.bottom) * .5f));
    const u64 old_revision = application_plugins_catalog_revision();
    application_settings_window_message(WM_LBUTTONDOWN, 0, point);
    check(write_manifest(f, 0, true, true) && application_plugins_discover() && application_plugins_catalog_revision() > old_revision,
        "catalog UI: same-path manifest Refresh unbinds and advances revision despite stable name/count");
    application_settings_window_message(WM_LBUTTONUP, 0, point);
    check(!application_plugins_present(0) && !GetModuleHandleW(L"MaiCordPluginFixture.dll") &&
        !application_plugins_catalog_enable(0, old_revision), "catalog UI: stale mouse release and old Enable token cannot load changed metadata");
    f.pin = LoadLibraryExW(f.dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto configure = f.pin ? reinterpret_cast<native_plugin_fixture::Configure>(GetProcAddress(f.pin, "maicord_plugin_fixture_configure")) : nullptr;
    check(configure != nullptr, "catalog UI: explicit known fixture audit can be configured");
    if (!configure) return;
    f.audit.pages = application_settings_pages(); f.audit.popup = &g_app.popup_scene;
    configure(0, native_plugin_fixture::owner_id, &f.audit);
    check(write_manifest(f, 0, false) && application_plugins_discover() && (approve(0), true) &&
        !application_plugins_catalog_enable(0, application_plugins_catalog_revision()) && !f.audit.queries && !f.audit.starts &&
        !application_plugins_present(0), "catalog UI: declared digest mismatch rejects before fixture query/start and retains no module");
    check(write_manifest(f, 0, true) && application_plugins_discover() &&
        activate(kApplicationPluginCatalogActionFirst, VK_RETURN) && settings_runtime_text_form() &&
        settings_runtime_text_form()->confirmation_only && !f.audit.queries && !application_plugins_present(0),
        "catalog UI: first Enter Enable asks the user to approve the manifest's permissions before any code runs");
    const auto* approval = settings_runtime_text_form();
    check(approval && approval->valid(approval->owner) && approval->save(approval->owner, {}) &&
        f.audit.queries == 1 && f.audit.starts == 1 && g_app.plugins->modules[0].enabled &&
        g_app.plugins->preferences.count == 1 &&
        g_app.plugins->preferences.entries[0].permissions == (MC_PERM_SETTINGS | MC_PERM_MENUS),
        "catalog UI: approving enables through the verified declared loader and remembers exactly the approved permissions");
    application_text_form_close(false, false);
    const auto* loaded_control = scene() ? ui_settings_scene_control(*scene(), enable) : nullptr;
    check(loaded_control && !loaded_control->enabled && utf8_equal(loaded_control->action_label, utf8_literal("Loaded")),
        "catalog UI: only actually enabled module projects Loaded and suppresses duplicate Enable");
    const u32 queries_before_refresh = f.audit.queries, starts_before_refresh = f.audit.starts;
    check(write_manifest(f, 0, false, true) && application_plugins_discover(),
        "catalog identity: changed local manifest can be refreshed while previous code stays active");
    loaded_control = scene() ? ui_settings_scene_control(*scene(), enable) : nullptr;
    check(loaded_control && !loaded_control->enabled && utf8_equal(loaded_control->action_label, utf8_literal("In use")) &&
        utf8_equal(loaded_control->description,
            utf8_literal("Version 1.1 · Another version is running. Unload it first.")) &&
        (approve(0), true) && !application_plugins_catalog_enable(0, application_plugins_catalog_revision()) &&
        f.audit.queries == queries_before_refresh && f.audit.starts == starts_before_refresh,
        "catalog identity: refreshed bundle version does not claim old code is loaded or enter a second module");
    check(write_manifest(f, 0, true, true) && application_plugins_discover(),
        "catalog identity: presentation-only version change restores exact code identity");
    loaded_control = scene() ? ui_settings_scene_control(*scene(), enable) : nullptr;
    check(loaded_control && utf8_equal(loaded_control->action_label, utf8_literal("Loaded")),
        "catalog identity: unchanged digest and DLL remain Loaded despite changed display metadata");
    const u64 captured_generation = g_app.plugins->loaded_identity_generations[0];
    g_app.plugins->loaded_identity_generations[0] = 0;
    application_settings_refresh_pages();
    loaded_control = scene() ? ui_settings_scene_control(*scene(), enable) : nullptr;
    check(loaded_control && !loaded_control->enabled && utf8_equal(loaded_control->action_label, utf8_literal("In use")) &&
        utf8_equal(loaded_control->description,
            utf8_literal("Version 1.1 · A manually loaded plugin is running. Unload it first.")),
        "catalog identity: an identity from another module generation cannot certify active code");
    g_app.plugins->loaded_identity_generations[0] = captured_generation;
    check(write_manifest(f, 0, true) && application_plugins_discover(),
        "catalog identity: known original manifest remains refreshable after active-owner feedback");
    check(activate(100, VK_SPACE) && f.audit.stops == 1 && !application_plugins_present(0),
        "catalog UI: native Space Unload detaches pages before releasing known DLL/state");
    check(ui_settings_scene_focus(scene(), enable) && ui_settings_scene_control_anchor(*scene(), enable, &anchor),
        "catalog UI: native Enable row returns after Unload");
    approve(0);
    const LPARAM enable_point = MAKELPARAM(static_cast<u16>((anchor.left + anchor.right) * .5f), static_cast<u16>((anchor.top + anchor.bottom) * .5f));
    check(application_settings_window_message(WM_LBUTTONDOWN, 0, enable_point) &&
        application_settings_window_message(WM_LBUTTONUP, 0, enable_point) &&
        f.audit.queries == 2 && f.audit.starts == 2 && g_app.plugins->modules[0].enabled &&
        activate(100, VK_RETURN) && f.audit.stops == 2,
        "catalog UI: current mouse Enable and Enter Unload match keyboard semantic actions");
    auto* plugins_memory = g_app.plugins; auto* settings_memory = g_app.settings;
    check(enable_row(0) && g_app.plugins->modules[0].enabled,
        "catalog UI: known native fixture enables before release callback reentry");
    check(g_app.plugins->preferences.count == 1 && !application_plugins_save_pending(),
        "catalog preferences: successful explicit trusted Enable durably remembers one bounded identity");
    const u32 starts_before_restore = f.audit.starts, queries_before_restore = f.audit.queries;
    check(application_plugins_release() && !g_app.plugins && application_plugins_restore() &&
        g_app.plugins && g_app.plugins->modules[0].enabled && g_app.plugins->preferences.count == 1 &&
        f.audit.starts == starts_before_restore + 1 && f.audit.queries == queries_before_restore + 1 &&
        !application_settings_active(),
        "catalog preferences: teardown preserves authority and startup freshly verifies known DLL without opening Settings");
    check(application_settings_plugin_catalog_reopen() && activate(100, VK_RETURN) && !g_app.plugins->preferences.count &&
        !application_plugins_present(0), "catalog preferences: explicit Unload removes restart authority before stopping code");
    check(enable_row(0) && application_plugins_release() &&
        write_manifest(f, 0, false) && application_plugins_restore() && !application_plugins_present(0) &&
        f.audit.starts == starts_before_restore + 2 && f.audit.queries == queries_before_restore + 2 &&
        g_app.plugins->preferences.count == 1,
        "catalog preferences: changed declared digest never queries or starts remembered code");
    check(application_settings_plugin_catalog_reopen(), "catalog preferences: changed or missing choice can open its native recovery page");
    const u64 forget_revision = application_plugins_catalog_revision();
    check(!application_plugins_catalog_forget(forget_revision + 1), "catalog preferences: stale Forget revision rejects");
    check(activate(kApplicationPluginForgetAction, VK_SPACE), "catalog preferences: current native Forget consumes Space");
    check(!g_app.plugins->preferences.count, "catalog preferences: current Forget clears missing restart choices");
    check(write_manifest(f, 0, true), "catalog preferences: known fixture metadata restores after changed-digest rejection");
    check(application_plugins_discover(), "catalog preferences: Refresh accepts restored known fixture metadata");
    check(enable_row(0) && g_app.plugins->modules[0].enabled,
        "catalog preferences: explicit trusted Enable resumes after Forget");
    wchar_t second_dll[1024]{};
    check(join(second_dll, f.directory, L"missing1.dll") && CopyFileW(f.dll, second_dll, TRUE),
        "catalog preferences: second explicitly known compiled module is copied into the isolated fixture");
    const HMODULE second_pin = LoadLibraryExW(second_dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto configure_second = second_pin ? reinterpret_cast<native_plugin_fixture::Configure>(
        GetProcAddress(second_pin, "maicord_plugin_fixture_configure")) : nullptr;
    static native_plugin_fixture::FixtureAudit second_audit{};
    if (configure_second) {
        second_audit.pages = application_settings_pages();
        second_audit.popup = &g_app.popup_scene;
        configure_second(0, 1001, &second_audit);
        check(application_plugins_discover() && enable_row(1) &&
            g_app.plugins->preferences.count == 2 && second_audit.starts == 1,
            "catalog preferences: second trusted module records separate identity before restore reentry");
        check(application_plugins_release() && !g_app.plugins, "catalog preferences: both known modules release before restore fault");
        const u32 first_stops = f.audit.stops, second_stops = second_audit.stops;
        second_audit.start_callback_hook = restore_abandon;
        check(!application_plugins_restore() && !g_app.plugins && !g_app.settings &&
            f.audit.stops == first_stops + 1 && second_audit.stops == second_stops + 1,
            "catalog preferences: account abandonment during later start unwinds every admitted module and retries deferred Settings teardown");
        second_audit.start_callback_hook = nullptr;
        second_audit.pages = nullptr;
        g_app.login.visible = false;
        check(application_settings_plugin_catalog_reopen() && application_plugins_discover() &&
            activate(kApplicationPluginForgetAction, VK_RETURN), "catalog preferences: remembered choices remain explicitly clearable after abandoned restoration");
        f.audit.pages = application_settings_pages();
    } else check(false, "catalog preferences: second known fixture export resolves");
    if (second_pin) check(FreeLibrary(second_pin) != 0, "catalog preferences: explicit known second fixture pin releases");
    check(DeleteFileW(second_dll) != 0, "catalog preferences: owned second fixture file is removed after module release");
    check(enable_row(0) && g_app.plugins->modules[0].enabled,
        "catalog preferences: normal explicit Enable resumes after reentrant restore teardown");
    const u32 stops_before_install=f.audit.stops;
    check(prepare_install(f),"bundle controller: direct offline worker prepares a verified local update without invoking the picker");
    if(g_app.plugins->installer.runtime){
        ++g_app.plugins->install_lifetime;
        check(finish_install()&&g_app.plugins->modules[0].enabled&&f.audit.stops==stops_before_install,
            "bundle controller: mismatched monotonic Settings lifetime cancels before unload/publication");
    }
    check(prepare_install(f),"bundle controller: second offline request captures the actual current Settings lifetime");
    if(g_app.plugins->installer.runtime){
        application_plugins_account_scope_invalidated();
        check(finish_install()&&g_app.plugins->modules[0].enabled&&f.audit.stops==stops_before_install,
            "bundle controller: account-scope invalidation after preparation cancels without stopping trusted loaded code");
    }
    check(prepare_install(f),"bundle controller: verified update reaches explicit old-owner unload barrier");
    if(g_app.plugins->installer.runtime){
        g_app.plugins->modules[0].busy=true;
        check(finish_install()&&g_app.plugins->modules[0].enabled&&f.audit.stops==stops_before_install&&
            application_plugins_catalog_current()->entries[0].manifest.dll_bytes==sizeof("MaiCordPluginFixture.dll")-1,
            "bundle controller: failed old-owner unload cancels staging and retains authoritative prior inventory and module");
        g_app.plugins->modules[0].busy=false;
    }
    plugins_memory = g_app.plugins; catalog_memory = g_app.plugins->catalog;
    settings_memory = g_app.settings;
    reentry = {}; reentry.owner = g_app.plugins;
    f.audit.stop_callback_hook = stop_reentry;
    // Slot zero stops through genuine DLL code; a later busy slot then forces
    // the outer release to retain ownership for a subsequent explicit retry.
    g_app.plugins->modules[1].busy = true;
    application_settings_release();
    check(g_app.plugins == plugins_memory && g_app.settings == settings_memory && g_app.plugins->catalog == catalog_memory,
        "catalog UI: busy release retains catalog/module/Settings backing for retry");
    check(reentry.stops == 1 && reentry.closing && reentry.nested_rejected && reentry.guard_preserved &&
        reentry.choose_rejected && reentry.owner_retained && reentry.no_picker && !g_app.plugins->closing,
        "catalog UI: native stop reentry cannot clear outer release guard or create picker; failed outer release permits retry");
    f.audit.stop_callback_hook = nullptr;
    g_app.plugins->modules[1].busy = false;
    application_settings_release();
    check(!g_app.plugins && !g_app.settings && freed(catalog_memory) && freed(plugins_memory) && freed(settings_memory),
        "catalog UI: completed release frees lazy catalog and app owners after descriptions unbind");
}
} // namespace maicord
