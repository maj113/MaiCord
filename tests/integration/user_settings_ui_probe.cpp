#include "probe_present.hpp"
#include "settings_catalog.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/render/text_form_surface.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/text_form.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
namespace {
// The real catalog pages and sidebar with fixture values, projected the way
// the settings host shows them before live values arrive, plus a gallery of
// control states. Only production scene, layout, paint and text paths run;
// no host action is dispatched and no account is involved.
constexpr u32 kRows = 32;
constexpr UiPageKey kGalleryPage{2, 1};
struct Fixture {
    UiPage pages[kSettingsPanelCount + 1];
    UiSettingsPageContent contents[kSettingsPanelCount + 1];
    UiSettingsControl controls[kSettingsPanelCount + 1][kRows];
    UiSettingsControlPresentation looks[kSettingsPanelCount + 1][kRows];
    UiSettingsEditor editors[2];
    UiEditGeometry geometries[2];
    UiPageRegistry registry;
    UiSettingsScene scene;
    TextFormScene form;
    TextFormSurface surface;
};
Fixture fixture;

constexpr Utf8View kDevices[]{utf8_literal("Default"), utf8_literal("Headset (Synthetic Audio)")};
constexpr Utf8View kGames[]{utf8_literal("Example Game")};
constexpr Utf8View kThemes[]{utf8_literal("Light"), utf8_literal("Dark"), utf8_literal("AMOLED")};
constexpr Utf8View kSpacing[]{utf8_literal("0px"), utf8_literal("4px"), utf8_literal("8px"), utf8_literal("16px"),
                              utf8_literal("24px")};
constexpr Utf8View kDisplay[]{utf8_literal("Default"), utf8_literal("Compact")};
constexpr Utf8View kSpoilers[]{utf8_literal("On click"), utf8_literal("Always"), utf8_literal("On servers I moderate")};
constexpr Utf8View kAccess[]{utf8_literal("Deny"), utf8_literal("Inherit"), utf8_literal("Allow")};
constexpr Utf8View kRoleColors[]{utf8_literal("Blue"), utf8_literal("Green"), utf8_literal("Gold"),
                                 utf8_literal("Red"), utf8_literal("Pink"), utf8_literal("Gray")};
constexpr u32 kRoleRgb[]{0x3498DB, 0x2ECC71, 0xF1C40F, 0xE74C3C, 0xE91E63, 0x95A5A6};

// Theme swatches resolve as the settings host does, under default
// preferences (the dark theme the probe renders); the gallery's Theme row is
// the base theme palette and its colour grid shows role colours.
constexpr ClientPreferences kPreferences = client_preferences_defaults();
SettingBinding binding_of(u64 id) noexcept {
    UiSettingsSchemaLocation location{};
    const auto* node = settings_control_location(id, &location) ? settings_catalog_node(location) : nullptr;
    return node ? static_cast<SettingBinding>(node->binding) : SettingBinding::none;
}
bool palette(void*, u64 id, u32 choice, BackgroundTheme* output) noexcept {
    if (id == 1000 + 17) {
        if (choice >= 6)
            return false;
        *output = background_theme_defaults();
        output->color_count = 2;
        output->colors[0] = output->colors[1] = kRoleRgb[choice];
        return true;
    }
    return settings_theme_swatch(kPreferences, id == 1000 + 15 ? SettingBinding::theme_base : binding_of(id), choice,
                                 output);
}

// One catalog panel with every searchable row at a representative value.
u32 project_panel(u32 panel, UiSettingsControl* out, UiSettingsControlPresentation* looks) noexcept {
    const auto& nodes = settings_catalog_panel(static_cast<SettingsPanel>(panel)).nodes;
    u32 count = 0;
    for (u32 i = 0; i < nodes.count && count < kRows; ++i) {
        const auto& n = nodes[i];
        if ((n.flags & ui_settings_schema_search_hidden) ||
            (n.kind == UiSettingsControlKind::category && (n.flags & ui_settings_schema_dynamic)))
            continue;
        auto& c = out[count];
        c = {settings_control_id(panel, i), n.label, n.description, n.kind, true, (i % 3) != 1, n.choices,
             n.default_choice < n.choices.count ? n.default_choice : 0, n.action_label, nullptr};
        if (n.kind == UiSettingsControlKind::choice && !c.choices.count)
            c.choices = n.binding == static_cast<u32>(SettingBinding::add_game) ? ConstSlice<Utf8View>{kGames, 1}
                                                                                  : ConstSlice<Utf8View>{kDevices, 2};
        if (n.kind == UiSettingsControlKind::palette && !c.choices.count)
            c.choices = {kThemes, 3};
        if (n.kind == UiSettingsControlKind::slider && c.choices.count > 120)
            c.selected_choice = c.choices.count * 2 / 3;
        if (n.kind == UiSettingsControlKind::category && n.binding)
            continue; // list headings are followed by host rows only
        looks[count] = {static_cast<UiSettingsVariant>(n.variant),
                        static_cast<u8>(n.flags & ui_settings_schema_marks ? ui_settings_presentation_marks : 0),
                        n.default_choice < kUiSettingsNoDefault ? static_cast<u16>(n.default_choice)
                                                                : kUiSettingsNoDefault};
        if (n.binding == static_cast<u32>(SettingBinding::group_spacing))
            c.selected_choice = 2, looks[count].default_choice = 3;
        SettingValue value{};
        if (n.kind == UiSettingsControlKind::palette &&
            settings_store_read(kPreferences, static_cast<SettingBinding>(n.binding), &value))
            c.selected_choice = value.choice;
        ++count;
    }
    return count;
}

// Control states: focus, disabled, error, value labels, marks and swatches.
u32 build_gallery(Fixture& f) noexcept {
    auto* c = f.controls[kSettingsPanelCount];
    auto* p = f.looks[kSettingsPanelCount];
    using K = UiSettingsControlKind;
    f.editors[0] = {{utf8_literal("A display name well past the limit"), 2, 9, true, true, true}, nullptr,
                    utf8_literal("Display Name")};
    f.editors[1] = {{{}, 0, 0, true, false, false}, nullptr, utf8_literal("Tell everyone about yourself")};
    u32 n = 0;
    const auto add = [&](UiSettingsControl control, UiSettingsControlPresentation look = {}) noexcept {
        control.id = 1000 + n;
        c[n] = control;
        p[n++] = look.default_choice || look.flags || look.variant != UiSettingsVariant::standard
                     ? look
                     : UiSettingsControlPresentation{UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    };
    add({0, utf8_literal("Switches"), {}, K::category, true, false});
    add({0, utf8_literal("Focused switch"), utf8_literal("Keyboard focus rings the track concentrically."), K::toggle,
         true, true});
    add({0, utf8_literal("Off"), {}, K::toggle, true, false});
    add({0, utf8_literal("Disabled"), utf8_literal("Disabled controls are drawn at half opacity."), K::toggle, false,
         true});
    add({0, utf8_literal("Sliders"), {}, K::category, true, false});
    add({0, utf8_literal("Speaker Volume"), {}, K::slider, true, false,
         settings_catalog_panel(SettingsPanel::voice).nodes[4].choices, 140});
    add({0, utf8_literal("Space Between Message Groups"), utf8_literal("The default mark is highlighted."), K::slider,
         true, false, {kSpacing, 5}, 2},
        {UiSettingsVariant::standard, ui_settings_presentation_marks, 3});
    add({0, utf8_literal("Fields"), {}, K::category, true, false});
    add({0, utf8_literal("Display Name"), {}, K::edit, true, false, {}, 0, {}, &f.editors[0]});
    add({0, utf8_literal("Display names can be at most 32 characters."), {}, K::error, true, false});
    add({0, utf8_literal("Show spoiler content"), {}, K::choice, true, false, {kSpoilers, 3}, 0});
    add({0, utf8_literal("Bio"), {}, K::multiline, true, false, {}, 0, {}, &f.editors[1]});
    add({0, utf8_literal("Choices"), {}, K::category, true, false});
    add({0, utf8_literal("Chat Message Display"), {}, K::radio, true, false, {kDisplay, 2}, 0});
    add({0, utf8_literal("Send Messages"), {}, K::segments, true, false, {kAccess, 3}, 2},
        {UiSettingsVariant::standard, ui_settings_presentation_tristate, kUiSettingsNoDefault});
    add({0, utf8_literal("Theme"), {}, K::palette, true, false, {kThemes, 3}, 1});
    add({0, utf8_literal("Colors"), {}, K::category, true, false});
    add({0, utf8_literal("Role color"), {}, K::swatches, true, false, {kRoleColors, 6}, 2});
    add({0, utf8_literal("Couldn't save settings"), utf8_literal("Your latest changes are applied but not saved yet."),
         K::notice, true, false, {}, 0, utf8_literal("Try again")},
        {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    return n;
}

bool show(Fixture& f, D3D11Renderer& renderer, UiPageKey page, UiRect viewport, const wchar_t* name,
          bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!ui_page_registry_select(&f.registry, page).ok() || !ui_settings_scene_update(&f.scene, &f.registry, viewport) ||
        !f.scene.content.controls.count)
        return false;
    // Every materialized control has visible geometry inside the panel.
    u32 placed = 0;
    for (u32 i = 0; i < f.scene.content.controls.count; ++i) {
        const UiSettingsTarget target{UiSettingsTargetKind::control, page, f.scene.content.controls[i].id};
        UiSettingsControlGeometry geometry{};
        if (ui_settings_scene_control_geometry(f.scene, target, &geometry))
            placed += ui_rect_has_area(ui_rect_intersection(geometry.clip, f.scene.layout.panel)) ? 1 : 0;
    }
    return placed && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
           probe_present(&renderer, kDefaultTheme) && capture(name);
}
} // namespace

bool user_settings_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache& cache,
                            bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!capture || renderer.login_visible || !renderer.dpi)
        return false;
    auto& f = fixture;
    const UiSettingsScene* old_scene = renderer.settings_scene;
    const u32 width = renderer.width, height = renderer.height;
    struct Restore {
        D3D11Renderer& renderer;
        Fixture& f;
        const UiSettingsScene* old_scene;
        u32 width, height;
        ~Restore() noexcept {
            renderer_bind_text_form(&renderer, nullptr);
            text_form_surface_release(&f.surface);
            text_form_release(&f.form);
            renderer_bind_settings_scene(&renderer, old_scene);
            ui_settings_scene_release(&f.scene);
            ui_page_registry_clear(&f.registry);
            renderer_resize(&renderer, width, height);
        }
    } restore{renderer, f, old_scene, width, height};
    u32 stage = __LINE__;
    bool valid = ui_page_registry_initialize(&f.registry, {f.pages, kSettingsPanelCount + 1});
    for (u32 panel = 0; valid && panel < kSettingsPanelCount; ++panel) {
        const auto& source = settings_catalog_panel(static_cast<SettingsPanel>(panel));
        const u32 count = project_panel(panel, f.controls[panel], f.looks[panel]);
        f.contents[panel] = {source.title, {}, {f.controls[panel], count}, 1};
        f.contents[panel].presentation = f.looks[panel];
        valid = ui_page_registry_register(&f.registry,
                                          {{kSettingsNativeOwner, panel + 1u}, source.title, &f.contents[panel]})
                    .ok();
    }
    const u32 gallery = build_gallery(f);
    f.contents[kSettingsPanelCount] = {utf8_literal("Control states"),
                                       utf8_literal("Synthetic rows for every settings control state."),
                                       {f.controls[kSettingsPanelCount], gallery}, 1};
    f.contents[kSettingsPanelCount].presentation = f.looks[kSettingsPanelCount];
    if (valid)
        stage = __LINE__;
    valid = valid &&
            ui_page_registry_register(&f.registry,
                                      {kGalleryPage, utf8_literal("Control states"), &f.contents[kSettingsPanelCount]})
                .ok();
    const auto navigation = settings_catalog_navigation(true);
    if (valid)
        stage = __LINE__;
    valid = valid && renderer_resize(&renderer, 1360, 1000);
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    const UiRect viewport{0, 0, 1360 / scale, 1000 / scale};
    valid = valid && ui_settings_scene_set_presentation(&f.scene, UiSettingsPresentation::modal) &&
            ui_settings_scene_bind_navigation(&f.scene, navigation, 1) &&
            ui_settings_scene_bind_palette(&f.scene, {&f, palette, 1});
    if (valid)
        renderer_bind_settings_scene(&renderer, &f.scene);
    const auto page = [](SettingsPanel panel) noexcept {
        return UiPageKey{kSettingsNativeOwner, static_cast<u64>(panel) + 1};
    };
    if (valid)
        stage = __LINE__;
    valid = valid && show(f, renderer, page(SettingsPanel::content_social), viewport, L"user-settings-messaging.bmp", capture);
    if (valid)
        stage = __LINE__;
    valid = valid && show(f, renderer, page(SettingsPanel::appearance), viewport, L"user-settings-appearance.bmp", capture);
    if (valid)
        stage = __LINE__;
    valid = valid &&
            show(f, renderer, page(SettingsPanel::accessibility), viewport, L"user-settings-accessibility.bmp", capture);
    if (valid)
        stage = __LINE__;
    valid = valid &&
            show(f, renderer, page(SettingsPanel::activity_privacy), viewport, L"user-settings-activity.bmp", capture);
    if (valid)
        stage = __LINE__;
    valid = valid && show(f, renderer, page(SettingsPanel::voice), viewport, L"user-settings-voice.bmp", capture);
    // The gallery with keyboard focus on its first switch, then scrolled to
    // the swatches with focus on them.
    if (valid)
        stage = __LINE__;
    const UiSettingsTarget field_target{UiSettingsTargetKind::control, kGalleryPage, 1008};
    valid = valid && ui_page_registry_select(&f.registry, kGalleryPage).ok() &&
            ui_settings_scene_update(&f.scene, &f.registry, viewport) &&
            ui_settings_scene_focus(&f.scene, {UiSettingsTargetKind::control, kGalleryPage, 1001});
    ui_settings_scene_set_focus_visible(&f.scene, true);
    UiSettingsControlGeometry field{};
    if (valid)
        stage = __LINE__;
    valid = valid && (ui_settings_scene_reveal(&f.scene, field_target), true) &&
            ui_settings_scene_control_geometry(f.scene, field_target, &field);
    if (valid) {
        // A measured line with a selection over "display" and the caret after it.
        auto& g = f.geometries[0];
        const float middle = (field.text.top + field.text.bottom) * .5f;
        g = {};
        g.text = {field.text.left, middle - 10, field.text.left + 240, middle + 10};
        g.selections[0] = {field.text.left + 14, middle - 10, field.text.left + 64, middle + 10};
        g.selection_count = 1;
        g.caret = {field.text.left + 64, middle - 10, field.text.left + 65, middle + 10};
        f.editors[0].geometry = &g;
        ++f.contents[kSettingsPanelCount].revision;
    }
    if (valid)
        stage = __LINE__;
    valid = valid && show(f, renderer, kGalleryPage, viewport, L"user-settings-controls.bmp", capture);
    const UiSettingsTarget swatch_target{UiSettingsTargetKind::control, kGalleryPage, 1017};
    if (valid)
        stage = __LINE__;
    valid = valid && ui_settings_scene_focus(&f.scene, swatch_target) &&
            (ui_settings_scene_reveal(&f.scene, {UiSettingsTargetKind::control, kGalleryPage, 1018}), true) &&
            show(f, renderer, kGalleryPage, viewport, L"user-settings-controls-swatch-focus.bmp", capture);
    // A hex colour form after Apply with invalid text: reverted to the last
    // accepted colour, previewed in its swatch, with the error outline.
    TextFormContent content{utf8_literal("Pick a Color"),
                            utf8_literal("Enter a hex colour such as #5865F2."),
                            utf8_literal("Primary"),
                            utf8_literal("That isn't a hex colour. Use #RGB or #RRGGBB."),
                            utf8_literal("Cancel"),
                            utf8_literal("Apply"),
                            utf8_literal("#58F"),
                            4,
                            4,
                            true,
                            true};
    content.single_line = true;
    content.placeholder = utf8_literal("#RRGGBB");
    content.error = true;
    content.swatch = true;
    content.swatch_rgba = 0x5588FFFFu;
    if (valid)
        stage = __LINE__;
    valid = valid && text_form_update(&f.form, content, viewport, &cache) &&
            ui_rect_has_area(text_form_swatch_box(f.form));
    if (valid) {
        f.surface.scene = &f.form;
        renderer_bind_text_form(&renderer, &f.surface);
        if (valid)
        stage = __LINE__;
        valid = renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
                capture(L"user-settings-hex-error.bmp");
    }
    if (!valid)
        log_u64(LogLevel::error, "probe", "user_settings_failure_line", stage);
    log_write(valid ? LogLevel::info : LogLevel::error, "probe",
              valid ? "user_settings_catalog_pages_and_control_states_passed" : "user_settings_ui_failed");
    return valid;
}
} // namespace maicord
