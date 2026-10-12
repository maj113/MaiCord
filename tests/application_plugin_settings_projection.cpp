#include "../src/app/application_plugin_settings_projection.hpp"

namespace maicord {
void test_application_plugin_settings_projection(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace app_detail;
    static NativePluginCatalogEntry entries[8];
    static UiSettingsControl controls[kApplicationPluginSettingsMaximumControls];
    static ApplicationPluginCatalogText text[8];
    static ApplicationPluginSettingsLoaded loaded[8];
    static NativePluginPreference identities[8];
    NativePluginCatalog catalog{}; catalog.entries = {entries, 8}; catalog.count = 8;
    for (u32 i = 0; i < 8; ++i) {
        entries[i] = {};
        auto& manifest = entries[i].manifest;
        manifest.owner_id = 700 + i; manifest.abi_version = 5;
        constexpr auto name = utf8_literal("Trusted local plugin");
        constexpr auto version = utf8_literal("1.0");
        constexpr auto dll = utf8_literal("fixture.dll");
        copy_bytes(manifest.name, name.data, name.bytes); manifest.name_bytes = name.bytes;
        copy_bytes(manifest.version, version.data, version.bytes); manifest.version_bytes = version.bytes;
        copy_bytes(manifest.dll, dll.data, dll.bytes); manifest.dll_bytes = dll.bytes;
        identities[i] = {};
        identities[i].owner_id = manifest.owner_id; identities[i].abi_version = manifest.abi_version;
        identities[i].dll_bytes = manifest.dll_bytes;
        copy_bytes(identities[i].dll, manifest.dll, manifest.dll_bytes);
        copy_bytes(identities[i].sha256, manifest.sha256, 32);
        loaded[i] = {manifest.owner_id, i, {manifest.name, manifest.name_bytes}, i != 7, &identities[i]};
    }
    ApplicationPluginSettingsState state{};
    state.status = utf8_literal("Choose a trusted DLL.");
    auto project = [&]() noexcept { return application_plugins_settings_project(state, {controls, 21}, {text, 8}); };
    check(project() == 5 && controls[0].id == 1 && controls[1].id == kApplicationPluginInstallAction &&
        utf8_equal(controls[1].label, utf8_literal("Install or update plugin")) &&
        utf8_equal(controls[1].action_label, utf8_literal("Choose plugin")) &&
        controls[2].id == kApplicationPluginDiscoverAction && controls[0].enabled && controls[1].enabled,
        "plugin settings projection: undiscovered fixed actions use production IDs and labels without inventory allocation");
    state.catalog = &catalog; state.discovered = true; state.remembered = 8; state.loaded = {loaded, 8};
    check(project() == 21 && controls[3].id == kApplicationPluginForgetAction &&
        controls[5].id == kApplicationPluginCatalogActionFirst && !controls[5].enabled &&
        controls[12].enabled && controls[13].id == 100 && controls[20].id == 107 && controls[20].enabled &&
        controls[13].label.data == loaded[0].label.data,
        "plugin settings projection: maximum21 rows preserve active ownership and allow retained disabled module cleanup");
    check(utf8_equal(controls[5].action_label, utf8_literal("Loaded")) &&
        utf8_equal(controls[5].description, utf8_literal("Version 1.0 | Loaded. ")),
        "plugin catalog identity: exact declared identity identifies the active bundle");
    entries[0].manifest.sha256[0] = 1;
    check(project() == 21 && !controls[5].enabled &&
        utf8_equal(controls[5].action_label, utf8_literal("In use")) &&
        utf8_equal(controls[5].description, utf8_literal("Version 1.0 · Another version is running. Unload it first.")),
        "plugin catalog identity: refreshed digest cannot mislabel old code as the displayed loaded bundle");
    entries[0].manifest.sha256[0] = 0;
    identities[0].dll[0] = 'x';
    check(project() == 21 && utf8_equal(controls[5].action_label, utf8_literal("In use")),
        "plugin catalog identity: same owner and digest with a different DLL leaf still requires explicit reload");
    identities[0].dll[0] = 'f';
    ++identities[0].abi_version;
    check(project() == 21 && utf8_equal(controls[5].action_label, utf8_literal("In use")),
        "plugin catalog identity: ABI participates in exact running-code identity");
    --identities[0].abi_version;
    entries[0].manifest.dll[0] = 'F'; entries[0].manifest.version[2] = '1';
    state.remembered = 0;
    check(project() == 21 && utf8_equal(controls[5].action_label, utf8_literal("Loaded")) &&
        utf8_equal(controls[5].description, utf8_literal("Version 1.1 | Loaded. ")),
        "plugin catalog identity: canonical filename case and presentation version do not change code identity or depend on saved authority");
    entries[0].manifest.dll[0] = 'f'; entries[0].manifest.version[2] = '0'; state.remembered = 8;
    loaded[0].identity = nullptr;
    check(project() == 21 && !controls[5].enabled && utf8_equal(controls[5].action_label, utf8_literal("In use")) &&
        utf8_equal(controls[5].description, utf8_literal("Version 1.0 · A manually loaded plugin is running. Unload it first.")),
        "plugin catalog identity: manual same-owner code blocks duplicates without inheriting declared bundle identity");
    loaded[0].identity = &identities[0];
    state.busy = true; state.save_pending = true;
    bool disabled = project() == 21;
    for (u32 i = 0; i < 21; ++i) disabled = disabled && !controls[i].enabled;
    check(disabled && utf8_equal(controls[0].label, utf8_literal("Save plugins")) &&
        utf8_equal(controls[0].action_label, utf8_literal("Try again")),
        "plugin settings projection: busy disables every action while failed-save retry labels stay distinct");
    state.install_retry = true;
    bool finish_only = project() == 21 && controls[1].enabled &&
        utf8_equal(controls[1].action_label, utf8_literal("Try again")) &&
        utf8_equal(controls[1].description, utf8_literal("Finish the current installation first."));
    for (u32 i = 0; i < 21; ++i) if (i != 1) finish_only = finish_only && !controls[i].enabled;
    check(finish_only, "plugin settings projection: retained cleanup enables only explicit Try again while ordinary actions stay busy");
    state.install_retry = false;
    state.busy = false; state.plugin_error = NativePluginError::page;
    check(project() == 21 && controls[0].description.data != state.status.data &&
        utf8_equal(controls[0].description, utf8_literal("Couldn't open this plugin page. Try again.")),
        "plugin settings projection: native page failure uses the shared production status");
    state.plugin_error = NativePluginError::activation_failed;
    check(project() == 21 && utf8_equal(controls[0].description,
        utf8_literal("The plugin couldn't complete this action. Try again.")),
        "plugin settings projection: native action failure has separate production feedback");
    const auto before = controls[0];
    loaded[7].slot = 0;
    check(!project() && controls[0].id == before.id && controls[0].label.data == before.label.data,
        "plugin settings projection: duplicate loaded slots reject before projection");
    loaded[7].slot = 7;
    const auto original0 = loaded[0], original1 = loaded[1];
    loaded[0].owner_id = loaded[1].owner_id = 0;
    loaded[0].enabled = loaded[1].enabled = false;
    check(project() == 21 && controls[0].enabled && controls[1].enabled &&
        controls[5].enabled && controls[6].enabled && controls[13].enabled && controls[14].enabled &&
        controls[13].id == 100 && controls[14].id == 101,
        "plugin settings projection: two unidentified retained modules preserve fixed actions and cleanup without catalog Loaded authority");
    loaded[0].enabled = true;
    check(!project(), "plugin settings projection: enabled module requires a nonzero validated owner identity");
    loaded[0] = original0; loaded[1] = original1;
    check(!application_plugins_settings_project(state, {controls, 20}, {text, 8}) &&
        !application_plugins_settings_project(state, {controls, 21}, {text, 7}),
        "plugin settings projection: full descriptor and shared text budgets are required");
    state.plugin_error = NativePluginError::unsupported;
    check(project() == 21 && utf8_equal(controls[0].description,
        utf8_literal("This plugin's settings aren't supported. Update the plugin.")),
        "plugin settings projection: unsupported descriptor format has actionable visible feedback");
    state.catalog = nullptr; state.loaded = {}; state.remembered = 0; state.plugin_error = NativePluginError::none;
    const u32 count = project();
    UiPage page{}; UiPageRegistry registry{}; UiSettingsScene scene{};
    UiSettingsPageContent content{utf8_literal("Plugins"), kApplicationPluginSettingsDescription, {controls, count}, 1};
    const UiSettingsTarget target{UiSettingsTargetKind::control, {1, 4}, kApplicationPluginInstallAction};
    bool geometry = ui_page_registry_initialize(&registry, {&page, 1}) &&
        ui_page_registry_register(&registry, {{1, 4}, utf8_literal("Plugins"), &content}).ok() &&
        ui_settings_scene_update(&scene, &registry, {0, 0, 640, 480}) && ui_settings_scene_focus(&scene, target);
    UiRect anchor{}; UiSettingsActivation activation{};
    geometry = geometry && ui_settings_scene_control_anchor(scene, target, &anchor) && ui_rect_has_area(anchor);
    if (geometry) {
        const auto hit = ui_settings_scene_hit_test(scene, (anchor.left + anchor.right) * .5f, (anchor.top + anchor.bottom) * .5f);
        geometry = ui_settings_target_equal(hit, target) && ui_settings_scene_activation(scene, hit, false, &activation) &&
            activation.kind == UiSettingsControlKind::action && activation.target.control_id == kApplicationPluginInstallAction;
    }
    check(geometry, "plugin settings projection: compact retained native focus/hit activation preserves install semantic identity");
    ui_settings_scene_release(&scene);
}
}
