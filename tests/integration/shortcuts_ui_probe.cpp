#include "probe_present.hpp"
#include "maicord/app/shortcut_registry.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/shortcuts_scene.hpp"
#include "maicord/core/log.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord {
namespace {
// Fixture projections copied from src/app/application_shortcuts.cpp (overlay)
// and src/app/application_keybinds_settings.cpp (settings page): the probe does
// not link the application owners. Rendering uses the production scenes.
constexpr u32 kRows = kUiShortcutsMaximumRows;
constexpr u8 kModifiers[]{shortcut_ctrl, shortcut_alt, shortcut_shift};
struct Overlay {
    UiShortcutsScene scene;
    UiShortcutsContent content;
    UiShortcutsRow rows[kRows];
    ShortcutListingRow listing[kRows];
    char keys[kRows][2][24];
};
struct Page {
    UiPage pages[1];
    UiSettingsPageContent content;
    UiSettingsControl controls[56];
    UiSettingsControlPresentation presentation[56];
    char chips[56][2][40];
    Utf8View chip_views[56][2];
    ShortcutListingRow listing[kRows];
    UiPageRegistry registry;
    UiSettingsScene scene;
};
Overlay overlay;
Page page;

void project_overlay(Utf8View query) noexcept {
    const u32 listed = shortcut_listing(query, {overlay.listing, kRows});
    for (u32 i = 0; i < listed; ++i) {
        const auto& source = overlay.listing[i];
        auto& row = overlay.rows[i];
        row = {};
        row.label = source.label;
        row.header = source.header;
        const bool shared = source.chords[0].key && source.chords[1].modifiers &&
                            source.chords[1].modifiers == source.chords[0].modifiers;
        for (u32 chord = 0; chord < 2 && !source.header; ++chord) {
            const auto c = source.chords[chord];
            if (!c.key)
                continue;
            u32 count = 0;
            for (const u8 modifier : kModifiers)
                if ((c.modifiers & modifier) && !(chord == 1 && shared))
                    row.keys[chord][count++] = shortcut_modifier_label(modifier);
            char* text = overlay.keys[i][chord];
            row.keys[chord][count++] = {text, shortcut_key_label(c.key, {text, 24})};
            row.key_counts[chord] = static_cast<u8>(count);
        }
    }
    overlay.content.title = utf8_literal("Keyboard shortcuts");
    overlay.content.placeholder = utf8_literal("Search shortcuts");
    overlay.content.empty = utf8_literal("No shortcuts match your search.");
    overlay.content.query = query;
    overlay.content.rows = {overlay.rows, listed};
    ++overlay.content.revision;
}

Utf8View chord_chip(u32 row, u32 slot, ShortcutChord chord) noexcept {
    const u32 bytes = shortcut_chord_label(chord, {page.chips[row][slot], 40});
    return page.chip_views[row][slot] = {page.chips[row][slot], bytes};
}
constexpr Utf8View kActions[]{utf8_literal("Push to talk"), utf8_literal("Toggle mute"), utf8_literal("Toggle deafen")};
u32 project_page(bool draft) noexcept {
    u32 n = 0;
    const auto add = [&](UiSettingsControl control, UiSettingsVariant variant = UiSettingsVariant::standard,
                         u8 flags = 0) noexcept {
        control.id = n + 1;
        page.controls[n] = control;
        page.presentation[n++] = {variant, flags, kUiSettingsNoDefault};
    };
    add({0, utf8_literal("Custom keybinds"),
         utf8_literal("Voice keybinds also work while MaiCord is in the background during a call."),
         UiSettingsControlKind::category, true});
    chord_chip(n, 0, {VK_F8, 0});
    add({0, kActions[0], {}, UiSettingsControlKind::action, true, false, {page.chip_views[n], 1}, 0, utf8_literal("Delete")},
        UiSettingsVariant::critical);
    chord_chip(n, 0, {'M', shortcut_ctrl | shortcut_alt});
    add({0, kActions[1], {}, UiSettingsControlKind::action, true, false, {page.chip_views[n], 1}, 0, utf8_literal("Delete")},
        UiSettingsVariant::critical);
    if (draft) {
        add({0, utf8_literal("Action"), {}, UiSettingsControlKind::radio, true, false, {kActions, 3}, 2});
        add({0, utf8_literal("Keys"), utf8_literal("Press the keys now. Esc cancels."), UiSettingsControlKind::action, true,
             false, {}, 0, utf8_literal("Press keys\xE2\x80\xA6")});
        add({0, utf8_literal("New keybind"), utf8_literal("Nothing is saved until you record keys."),
             UiSettingsControlKind::action, true, false, {}, 0, utf8_literal("Cancel")});
    } else
        add({0, utf8_literal("Add a keybind"), utf8_literal("Choose a voice action, then press the keys to use."),
             UiSettingsControlKind::action, true, false, {}, 0, utf8_literal("Add keybind")},
            UiSettingsVariant::primary);
    add({0, utf8_literal("Default shortcuts"),
         utf8_literal("Built-in shortcuts can't be changed. Press Ctrl+/ anywhere to see them."),
         UiSettingsControlKind::notice, true});
    const u32 listed = shortcut_listing({}, {page.listing, kRows});
    for (u32 i = 0; i < listed && n < 56; ++i) {
        const auto& source = page.listing[i];
        if (source.header) {
            add({0, source.label, {}, UiSettingsControlKind::category, true});
            continue;
        }
        u32 chips = 0;
        const u32 row = n;
        for (u32 chord = 0; chord < 2; ++chord)
            if (source.chords[chord].key)
                chord_chip(row, chips++, source.chords[chord]);
        add({0, source.label, {}, UiSettingsControlKind::action, true, false, {page.chip_views[row], chips}, 0, {}},
            UiSettingsVariant::standard, ui_settings_presentation_compact);
    }
    return n;
}
} // namespace

bool shortcuts_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    if (!renderer.content_text || !renderer.dpi)
        return false;
    struct Restore {
        D3D11Renderer& renderer;
        const UiShortcutsScene* old_shortcuts;
        const UiSettingsScene* old_settings;
        const UiWidgetManager* focus;
        ~Restore() noexcept {
            renderer.shortcuts_scene = old_shortcuts;
            renderer.focus_owner = focus;
            renderer_bind_settings_scene(&renderer, old_settings);
            renderer.retained_presentation.valid = false;
            ui_shortcuts_scene_release(&overlay.scene);
            ui_settings_scene_release(&page.scene);
            ui_page_registry_unregister_owner(&page.registry, 1);
            ui_page_registry_clear(&page.registry);
        }
    } restore{renderer, renderer.shortcuts_scene, renderer.settings_scene, renderer.focus_owner};
    Theme theme = kDefaultTheme;
    const UiRect viewport{0, 0, renderer.width * 96.f / renderer.dpi, renderer.height * 96.f / renderer.dpi};
    const auto show_overlay = [&](Utf8View query, const wchar_t* name) noexcept {
        project_overlay(query);
        if (!ui_shortcuts_scene_update(&overlay.scene, &overlay.content, viewport))
            return false;
        renderer.shortcuts_scene = &overlay.scene;
        renderer.focus_owner = &overlay.scene.widgets;
        return renderer_rebuild_content_text(&renderer, theme) && probe_present(&renderer, theme) &&
               (!capture || capture(name, theme));
    };
    u32 stage = __LINE__;
    bool valid = show_overlay({}, L"shortcuts-overlay.bmp");
    stage = __LINE__;
    valid = valid && show_overlay(utf8_literal("voice"), L"shortcuts-overlay-search.bmp") &&
            overlay.scene.row_count == 3;
    theme = kLightTheme;
    stage = __LINE__;
    valid = valid && show_overlay({}, L"shortcuts-overlay-light.bmp");
    stage = __LINE__;
    valid = valid && ui_shortcuts_scene_scroll(&overlay.scene, 400) && renderer_rebuild_content_text(&renderer, theme) &&
            probe_present(&renderer, theme) && (!capture || capture(L"shortcuts-overlay-light-scrolled.bmp", theme));
    renderer.shortcuts_scene = nullptr;
    theme = kDefaultTheme;

    // Keybinds settings page.
    stage = __LINE__;
    valid = valid && ui_page_registry_initialize(&page.registry, {page.pages, 1});
    for (u32 pass = 0; valid && pass < 2; ++pass) {
        const u32 count = project_page(pass == 1);
        page.content = {utf8_literal("Keybinds"), {}, {page.controls, count}, pass + 1u};
        page.content.presentation = page.presentation;
        if (pass == 0)
            valid = ui_page_registry_register(&page.registry, {{1, 1}, utf8_literal("Keybinds"), &page.content}).ok() &&
                    ui_page_registry_select(&page.registry, {1, 1}).ok();
        stage = __LINE__;
        valid = valid && ui_settings_scene_update(&page.scene, &page.registry, viewport);
        if (!valid)
            break;
        renderer_bind_settings_scene(&renderer, &page.scene);
        ui_settings_scene_set_focus_visible(&page.scene, false);
        stage = __LINE__;
        valid = renderer_rebuild_content_text(&renderer, theme) && probe_present(&renderer, theme) &&
                (!capture || capture(pass ? L"keybinds-settings-draft.bmp" : L"keybinds-settings.bmp", theme));
        if (valid && pass == 0) {
            ui_settings_scene_scroll(&page.scene, false, 640);
            stage = __LINE__;
            valid = renderer_rebuild_content_text(&renderer, theme) && probe_present(&renderer, theme) &&
                    (!capture || capture(L"keybinds-settings-defaults.bmp", theme));
            ui_settings_scene_scroll(&page.scene, false, -640);
        }
    }
    if (!valid)
        log_u64(LogLevel::error, "probe", "shortcuts_probe_failure_line", stage);
    log_write(valid ? LogLevel::info : LogLevel::error, "probe",
              valid ? "shortcuts_overlay_and_keybinds_settings_passed" : "shortcuts_probe_failed");
    return valid;
}
} // namespace maicord
