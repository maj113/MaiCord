#include "probe_present.hpp"
#include "../../src/app/application_plugin_settings_projection.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
namespace {
using namespace app_detail;
struct CatalogFixture {
    NativePluginCatalog catalog;
    NativePluginCatalogEntry entries[32];
    ApplicationPluginCatalogText visible_text[8];
    UiSettingsControl controls[kApplicationPluginSettingsMaximumControls];
    UiSettingsPageContent content;
    UiSettingsScene scene;
    UiPageRegistry registry;
    UiPage page;
    ApplicationPluginSettingsLoaded loaded[8];
};
CatalogFixture fixture{};
bool initialize(CatalogFixture& f) noexcept {
    f.catalog = {}; f.catalog.entries = {f.entries, 32}; f.catalog.count = 32;
    for (u32 i = 0; i < 32; ++i) {
        auto& entry = f.entries[i]; entry = {};
        auto& manifest = entry.manifest;
        manifest.owner_id = 700 + i; manifest.abi_version = 5;
        constexpr char prefix[] = "Local plugin ";
        u32 at = 0;
        for (; at < sizeof(prefix) - 1; ++at) manifest.name[at] = prefix[at];
        manifest.name[at++] = static_cast<char>('0' + i / 10);
        manifest.name[at++] = static_cast<char>('0' + i % 10);
        manifest.name_bytes = at;
        constexpr Utf8View version = utf8_literal("2.4.1");
        constexpr Utf8View description = utf8_literal("Native tools for your workspace. Long descriptions wrap inside the visible row without covering Enable or Unload. ");
        constexpr Utf8View dll = utf8_literal("fixture.dll");
        for (u32 j = 0; j < version.bytes; ++j) manifest.version[j] = version.data[j];
        manifest.version_bytes = version.bytes;
        for (u32 j = 0; j < description.bytes; ++j) manifest.description[j] = description.data[j];
        manifest.description_bytes = description.bytes;
        for (u32 j = 0; j < dll.bytes; ++j) manifest.dll[j] = dll.data[j];
        manifest.dll_bytes = dll.bytes;
        if (!native_plugin_manifest_valid(manifest)) return false;
    }
    auto& invalid = f.entries[2]; invalid.status = NativePluginCatalogError::manifest;
    for (u32 i = 0; i < 80; ++i) invalid.filename[i] = L'\x754C';
    invalid.filename_characters = 80;
    for (u32 i = 0; i < 8; ++i) {
        const auto& manifest = f.entries[i ? 23 + i : 0].manifest;
        f.loaded[i] = {manifest.owner_id, i, {manifest.name, manifest.name_bytes}, true};
    }
    f.content = {utf8_literal("Plugins"), kApplicationPluginSettingsDescription, {}, 1};
    return ui_page_registry_initialize(&f.registry, {&f.page, 1}) &&
        ui_page_registry_register(&f.registry, {{1, 4}, utf8_literal("Plugins"), &f.content}).ok();
}
bool project(CatalogFixture& f, u32 first, u32 remembered = 0, bool busy = false, bool retry = false, bool install_retry = false) noexcept {
    const ApplicationPluginSettingsState state{&f.catalog, first, remembered, true, busy, retry,
        retry ? utf8_literal("Plugin enabled, but couldn't be saved. Try again.") :
            utf8_literal("Only load plugins you trust. Plugins have full access to your computer."),
        NativePluginError::none, {f.loaded, 8}, install_retry};
    const u32 count = application_plugins_settings_project(state, {f.controls, kApplicationPluginSettingsMaximumControls}, {f.visible_text, 8});
    for (u32 i = 0; i < 8; ++i) if (!utf8_validate({f.visible_text[i].text, f.visible_text[i].bytes}).valid ||
        f.visible_text[i].bytes > sizeof(f.visible_text[i].text)) return false;
    f.content.controls = {f.controls, count}; ++f.content.revision;
    return count == 21 && f.controls[3].enabled == (!busy && (first != 0 || remembered != 0)) &&
        f.controls[4].enabled == (!busy && first < 24);
}
bool geometry(CatalogFixture& f, UiRect viewport, u32 target) noexcept {
    auto failed = [&](u32 line) noexcept {
        log_u64(LogLevel::error, "probe", "plugin_catalog_geometry_line", line);
        log_u64(LogLevel::error, "probe", "plugin_catalog_geometry_controls", f.scene.content.controls.count);
        log_u64(LogLevel::error, "probe", "plugin_catalog_geometry_offset_milli", static_cast<u64>(f.scene.content_offset * 1000));
        return false;
    };
    const UiSettingsTarget row{UiSettingsTargetKind::control, {1, 4}, kApplicationPluginCatalogActionFirst + target};
    if (!ui_settings_scene_update(&f.scene, &f.registry, viewport) ||
        !ui_settings_scene_focus(&f.scene, row)) return failed(__LINE__);
    UiRect anchor{};
    if (!ui_settings_scene_control_anchor(f.scene, row, &anchor) || !ui_rect_has_area(anchor)) return failed(__LINE__);
    const auto hit = ui_settings_scene_hit_test(f.scene, (anchor.left + anchor.right) * .5f, (anchor.top + anchor.bottom) * .5f);
    UiSettingsActivation action{};
    if (!ui_settings_target_equal(hit, row) || !ui_settings_scene_activation(f.scene, hit, false, &action) ||
        action.kind != UiSettingsControlKind::action) return failed(__LINE__);
    UiTextCommand commands[64]{}; UiTextCommandStream stream{};
    if (!ui_text_command_stream_initialize(&stream, {commands, 64}) || !ui_settings_scene_text(f.scene, kDefaultTheme, &stream)) return failed(__LINE__);
    bool readable = false;
    for (u32 i = 0; i < stream.count; ++i) {
        const auto& command = commands[i];
        if (utf8_equal(command.text, utf8_literal("Enable")))
            readable = readable || ui_rect_has_area(ui_rect_intersection(command.rectangle, command.clip));
        if (!utf8_validate(command.text).valid) return failed(__LINE__);
    }
    return (readable && f.scene.content_offset > 0 && f.scene.content.controls.count == 21) || failed(__LINE__);
}
} // namespace

bool plugin_catalog_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!capture || !renderer.dpi || renderer.login_visible) return false;
    auto& f = fixture;
    const auto* previous = renderer.settings_scene;
    const u32 width = renderer.width, height = renderer.height;
    struct Restore {
        D3D11Renderer& renderer; CatalogFixture& f; const UiSettingsScene* previous; u32 width, height;
        ~Restore() noexcept {
            renderer_bind_settings_scene(&renderer, previous); ui_settings_scene_release(&f.scene);
            renderer_resize(&renderer, width, height);
        }
    } restore{renderer, f, previous, width, height};
    bool valid = initialize(f); u32 stage = __LINE__;
    const u32 first[] = {0, 8, 24}, target[] = {3, 11, 31};
    const wchar_t* names[] = {L"plugin-catalog-first.bmp", L"plugin-catalog-compact.bmp", L"plugin-catalog-last.bmp"};
    const float pixels = static_cast<float>(renderer.dpi) / 96.f;
    for (u32 i = 0; valid && i < 3; ++i) {
        stage = __LINE__;
        // Release borrowing widgets before rewriting the visible row text bank.
        renderer_bind_settings_scene(&renderer, nullptr); ui_settings_scene_close(&f.scene);
        valid = renderer_resize(&renderer, i == 1 ? static_cast<u32>(640 * pixels) : width,
            i == 1 ? static_cast<u32>(480 * pixels) : height);
        stage = __LINE__;
        if (valid) valid = project(f, first[i]);
        const UiRect viewport{0, 0, renderer.width / pixels, renderer.height / pixels};
        if (valid) { stage = __LINE__; valid = geometry(f, viewport, target[i]); }
        renderer_bind_settings_scene(&renderer, &f.scene);
        valid = valid && renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) && capture(names[i]);
        if (valid) {
            const u32 builds = renderer.paint_stream_builds;
            valid = probe_present(&renderer, kDefaultTheme) && renderer.paint_stream_builds == builds;
        }
    }
    for (u32 i = 0; valid && i < 2; ++i) {
        stage = __LINE__;
        renderer_bind_settings_scene(&renderer, nullptr); ui_settings_scene_close(&f.scene);
        valid = renderer_resize(&renderer, static_cast<u32>(640 * pixels), static_cast<u32>(480 * pixels)) && project(f, 0, 8, false, i != 0);
        const UiRect viewport{0, 0, renderer.width / pixels, renderer.height / pixels};
        valid = valid && ui_settings_scene_update(&f.scene, &f.registry, viewport) &&
            ui_settings_scene_focus(&f.scene, {UiSettingsTargetKind::control, {1, 4}, i ? 1 : kApplicationPluginForgetAction});
        renderer_bind_settings_scene(&renderer, &f.scene);
        valid = valid && renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
            capture(i ? L"plugin-choices-save-retry.bmp" : L"plugin-choices-forget.bmp");
        if (valid) {
            const u32 builds = renderer.paint_stream_builds;
            valid = probe_present(&renderer, kDefaultTheme) && renderer.paint_stream_builds == builds;
        }
    }
    const wchar_t* install_names[] = {L"plugin-install-normal.bmp", L"plugin-install-compact.bmp", L"plugin-install-retry-finish.bmp"};
    for (u32 i = 0; valid && i < 3; ++i) {
        stage = __LINE__;
        renderer_bind_settings_scene(&renderer, nullptr); ui_settings_scene_close(&f.scene);
        valid = renderer_resize(&renderer, i ? static_cast<u32>(640 * pixels) : width,
            i ? static_cast<u32>(480 * pixels) : height) && project(f, 0, 0, i == 2, false, i == 2);
        const UiRect viewport{0, 0, renderer.width / pixels, renderer.height / pixels};
        const UiSettingsTarget install_target{UiSettingsTargetKind::control, {1, 4}, kApplicationPluginInstallAction};
        valid = valid && ui_settings_scene_update(&f.scene, &f.registry, viewport);
        if (valid) valid = ui_settings_scene_focus(&f.scene, install_target);
        if (valid) ui_settings_scene_set_focus_visible(&f.scene, true);
        UiRect anchor{};
        UiSettingsActivation activation{};
        if (valid) {
            valid = ui_settings_scene_control_anchor(f.scene, install_target, &anchor) && ui_rect_has_area(anchor);
            const auto hit = ui_settings_scene_hit_test(f.scene, (anchor.left + anchor.right) * .5f, (anchor.top + anchor.bottom) * .5f);
            valid = valid && ui_settings_target_equal(hit, install_target) &&
                ui_settings_scene_activation(f.scene, hit, false, &activation) && activation.target.control_id == kApplicationPluginInstallAction;
        }
        UiTextCommand commands[64]{}; UiTextCommandStream text{};
        valid = valid && ui_text_command_stream_initialize(&text, {commands, 64}) &&
            ui_settings_scene_text(f.scene, kDefaultTheme, &text);
        bool readable = false;
        for (u32 j = 0; valid && j < text.count; ++j)
            if (utf8_equal(commands[j].text, i == 2 ? utf8_literal("Try again") : utf8_literal("Choose plugin")))
                readable = readable || ui_rect_has_area(ui_rect_intersection(commands[j].rectangle, commands[j].clip));
        valid = valid && readable;
        renderer_bind_settings_scene(&renderer, &f.scene);
        valid = valid && renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) && capture(install_names[i]);
    }
    renderer_bind_settings_scene(&renderer, previous);
    valid = renderer_resize(&renderer, width, height) && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
        probe_present(&renderer, kDefaultTheme) && valid;
    if (!valid) log_u64(LogLevel::error, "probe", "plugin_catalog_failure_line", stage);
    log_write(valid ? LogLevel::info : LogLevel::error, "probe", valid ?
        "plugin_catalog_pages_compact_text_scroll_and_retention_passed" : "plugin_catalog_hardware_failed");
    return valid;
}
} // namespace maicord
