#include "probe_present.hpp"
#include "maicord/app/shell/shortcut_registry.hpp"
#include "maicord/ui/shell/shortcuts_scene.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
namespace {
// The keyboard shortcuts overlay through the renderer. The row projection is src/app/shell/shortcuts.cpp's: the
// probe does not link the application owners. Listing and scene layout are tests/app/shell/shortcut_registry.cpp's.
constexpr u32 kRows = kUiShortcutsMaximumRows;
constexpr u8 kModifiers[]{shortcut_ctrl, shortcut_alt, shortcut_shift};
struct Overlay {
    UiShortcutsScene scene;
    UiShortcutsContent content;
    UiShortcutsRow rows[kRows];
    ShortcutListingRow listing[kRows];
    char keys[kRows][2][24];
};
Overlay overlay;

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
} // namespace

bool shortcuts_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    if (!renderer.content_text || !renderer.dpi)
        return false;
    struct Restore {
        D3D11Renderer& renderer;
        const UiShortcutsScene* old_shortcuts;
        const UiWidgetManager* focus;
        ~Restore() noexcept {
            renderer.shortcuts_scene = old_shortcuts;
            renderer.focus_owner = focus;
            renderer.retained_presentation.valid = false;
            ui_shortcuts_scene_release(&overlay.scene);
        }
    } restore{renderer, renderer.shortcuts_scene, renderer.focus_owner};
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
    if (!valid)
        log_u64(LogLevel::error, "probe", "shortcuts_probe_failure_line", stage);
    log_write(valid ? LogLevel::info : LogLevel::error, "probe",
              valid ? "shortcuts_overlay_passed" : "shortcuts_probe_failed");
    return valid;
}
} // namespace maicord
