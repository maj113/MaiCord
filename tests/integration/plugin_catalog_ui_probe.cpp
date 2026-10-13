#include "probe_present.hpp"
#include "../../src/app/application_plugin_settings_projection.hpp"
#include "../../src/app/application_plugin_browser_projection.hpp"
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
// The catalog tabs over a small synthetic snapshot: an official and an
// unofficial provider, one installed plugin with an update, one whose update
// needs review, and a key change.
struct BrowserFixture {
    PluginCatalogSnapshot snapshot;
    PluginCatalogEntryView entries[12];
    PluginCatalogVersionView versions[12];
    char documents[8192];
    PluginBrowserBank bank;
};
BrowserFixture browser{};
UiTextCommand browser_commands[1024];
PluginIndexText browser_text(u32 provider, const char* value) noexcept {
    auto& p = browser.snapshot.providers[provider];
    u32 n = 0;
    while (value[n]) { browser.documents[p.document_offset + p.document_bytes + n] = value[n]; ++n; }
    const PluginIndexText text{p.document_bytes, n};
    p.document_bytes += n;
    return text;
}
void browser_name(char* out, u32* bytes, const char* value) noexcept {
    u32 n = 0;
    while (value[n]) { out[n] = value[n]; ++n; }
    *bytes = n;
}
void build_browser() noexcept {
    auto& s = browser.snapshot;
    s.entries = browser.entries; s.versions = browser.versions; s.documents = browser.documents;
    s.configured = true; s.provider_count = 2;
    auto& official = s.providers[0];
    official.id = kPluginProviderOfficial; official.kind = PluginProviderKind::official;
    official.status = PluginCatalogStatus::ok; official.installable = official.enabled = official.check_daily = true;
    browser_name(official.host, &official.host_bytes, "maj113.github.io");
    browser_name(official.name, &official.name_bytes, "MaiCord plugins");
    auto& custom = s.providers[1];
    custom.id = 0x77; custom.kind = PluginProviderKind::custom_signed; custom.status = PluginCatalogStatus::key_changed;
    custom.installable = custom.enabled = custom.check_daily = true; custom.document_offset = 4096;
    browser_name(custom.host, &custom.host_bytes, "plugins.example.com");
    browser_name(custom.path, &custom.path_bytes, "/catalog/");
    browser_name(custom.name, &custom.name_bytes, "Example plugins");
    custom.fingerprint[0] = 0xAB; custom.fingerprint[1] = 0x12;
    const char* names[12]{"Showcase", "Message log", "Quick reply", "Gradient background", "Username suffixes", "Read aloud",
        "Compact mode", "Timestamps", "Pinned notes", "Night shift", "Mention alerts", "Emoji tools"};
    for (u32 i = 0; i < 12; ++i) {
        const u32 provider = i < 9 ? 0 : 1;
        auto& e = browser.entries[i];
        e.owner = 500 + i; e.provider = provider; e.installed_index = ~0u;
        e.name = browser_text(provider, names[i]);
        e.summary = browser_text(provider, "SDK sample with a longer summary that wraps inside the row without covering the View button.");
        e.author = browser_text(provider, "maj113"); e.author_count = 1;
        e.license = browser_text(provider, "MPL-2.0");
        e.homepage = browser_text(provider, "https://github.com/maj113/MaiCord-plugins");
        e.categories = 1u << (i % 10); e.first_version = i; e.version_count = 1;
        auto& v = browser.versions[i];
        v.code = 1000000999; v.released = 1760200000; v.bytes = 48640; v.permissions = 0b1011;
        v.compatibility = PluginCompatibility::compatible;
        v.changelog = browser_text(provider, "First catalog release.");
        v.sha256[0] = 0x4d; v.sha256[1] = 0x43;
    }
    browser.entries[0].disclosures = 0b1;
    browser.versions[5].compatibility = PluginCompatibility::needs_newer_host;
    browser.entries[5].newest = kPluginIndexNoVersion;
    s.entry_count = s.version_count = 12;
    s.installed_count = 2;
    auto& a = s.installed[0];
    a.owner = 501; a.provider = kPluginProviderOfficial; a.has_state = a.trusted = true; a.installed_code = 1000000999; a.entry = 1;
    browser_name(a.version, &a.version_bytes, "1.0.0");
    browser.versions[1].code = 1001000999;
    browser.entries[1].installed_index = 0; browser.entries[1].bound = true;
    browser.entries[1].update.outcome = PluginUpdateOutcome::available;
    auto& b = s.installed[1];
    b.owner = 509; b.provider = 0x77; b.has_state = b.trusted = true; b.installed_code = 1000000999; b.entry = 9;
    browser_name(b.version, &b.version_bytes, "0.9.0");
    browser.entries[9].installed_index = 1; browser.entries[9].bound = true;
    browser.entries[9].update.outcome = PluginUpdateOutcome::review;
    browser.entries[9].update.permissions = {0b100, 0, 0b1};
}
// Projects one tab into the bank and checks a label is drawn unclipped.
bool browser_frame(CatalogFixture& f, D3D11Renderer& renderer, const PluginBrowserUiState& ui, Utf8View readable, u64 focus,
    bool (*capture)(const wchar_t*) noexcept, const wchar_t* name) noexcept {
    renderer_bind_settings_scene(&renderer, nullptr); ui_settings_scene_close(&f.scene);
    PluginBrowserFacts facts{};
    facts.snapshot = &browser.snapshot; facts.configured = true;
    facts.now_unix = 1760400000; facts.checked_unix = 1760400000 - 7200;
    const auto failed = [](u32 step) noexcept {
        log_u64(LogLevel::error, "probe", "plugin_browser_frame_step", step);
        return false;
    };
    const u32 count = plugin_browser_project(ui, facts, &browser.bank);
    for (u32 i = 0; i < count; ++i) {
        const auto& c = browser.bank.controls[i];
        if (!utf8_validate(c.label).valid || !utf8_validate(c.description).valid) return failed(1);
    }
    f.content.controls = {browser.bank.controls, count}; f.content.presentation = browser.bank.presentation; ++f.content.revision;
    const float pixels = static_cast<float>(renderer.dpi) / 96.f;
    const UiRect viewport{0, 0, renderer.width / pixels, renderer.height / pixels};
    if (!count || !ui_settings_scene_update(&f.scene, &f.registry, viewport)) return failed(2);
    if (focus) {
        const UiSettingsTarget target{UiSettingsTargetKind::control, {1, 4}, focus};
        ui_settings_scene_reveal(&f.scene, target);
        if (!ui_settings_scene_update(&f.scene, &f.registry, viewport)) return failed(3);
    }
    UiTextCommandStream stream{};
    if (!ui_text_command_stream_initialize(&stream, {browser_commands, 1024}) ||
        !ui_settings_scene_text(f.scene, kDefaultTheme, &stream)) return failed(4);
    bool drawn = false;
    for (u32 i = 0; i < stream.count; ++i)
        if (utf8_equal(browser_commands[i].text, readable))
            drawn = drawn || ui_rect_has_area(ui_rect_intersection(browser_commands[i].rectangle, browser_commands[i].clip));
    if (!drawn) return failed(5);
    renderer_bind_settings_scene(&renderer, &f.scene);
    if (!renderer_rebuild_content_text(&renderer, kDefaultTheme)) return failed(6);
    if (!probe_present(&renderer, kDefaultTheme)) return failed(7);
    return capture(name) || failed(8);
}
bool browser_frames(CatalogFixture& f, D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept) noexcept {
    build_browser();
    // Official plugins come first: page one shows them, Community the rest.
    PluginBrowserUiState ui{};
    bool valid = browser_frame(f, renderer, ui, utf8_literal("Needs a newer MaiCord"), kPluginBrowserRowFirst + 5, capture,
        L"plugin-browser-list.bmp");
    ui.source = PluginBrowserSource::community;
    valid = valid && browser_frame(f, renderer, ui, utf8_literal("Unofficial \xC2\xB7 plugins.example.com"), 0, capture,
        L"plugin-browser-community.bmp");
    ui = {};
    ui.detail = true; ui.detail_provider = 0x77; ui.detail_owner = 509;
    valid = valid && browser_frame(f, renderer, ui, utf8_literal("Review update"), 0, capture, L"plugin-browser-detail-unofficial.bmp");
    ui.detail_provider = kPluginProviderOfficial; ui.detail_owner = 501;
    valid = valid && browser_frame(f, renderer, ui, utf8_literal("Homepage"), kPluginBrowserHomepageId, capture,
        L"plugin-browser-detail-official.bmp");
    ui = {}; ui.tab = PluginBrowserTab::updates;
    valid = valid && browser_frame(f, renderer, ui, utf8_literal("New permissions"), 0, capture, L"plugin-browser-updates.bmp");
    ui.tab = PluginBrowserTab::providers;
    valid = valid && browser_frame(f, renderer, ui, utf8_literal("Trust new key"), 0, capture, L"plugin-browser-providers.bmp");
    if (!valid) log_write(LogLevel::error, "probe", "plugin_browser_frames_failed");
    return valid;
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
    if (valid) {
        stage = __LINE__;
        valid = renderer_resize(&renderer, width, height) && browser_frames(f, renderer, capture);
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
