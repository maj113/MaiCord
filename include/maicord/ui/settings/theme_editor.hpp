#pragma once
#include "maicord/ui/theme/background_theme.hpp"
#include "maicord/ui/core/widget_storage.hpp"
#include "maicord/ui/text/presentation.hpp"
#include "maicord/ui/core/paint.hpp"
#include "maicord/ui/layout/scrollbar.hpp"

namespace maicord {
enum class UiThemeEditorElement : u8 {
    root,
    panel,
    body,
    title,
    close,
    dark,
    light,
    gradient,
    stop0,
    stop1,
    stop2,
    stop3,
    stop4,
    add,
    remove,
    sv,
    hue,
    hex,
    direction,
    intensity,
    surprise,
    reset,
    apply,
    effect_label,
    effect_none,
    effect_stars,
    effect_comets,
    effect_both,
    // Installed (package) effects: step through them, the one in use by name, and its knobs.
    effect_previous,
    effect_installed,
    effect_next,
    effect_knobs,
    speed,
    strength,
    density,
    scrollbar,
    count
};
inline constexpr u32 kUiThemeEditorElements = static_cast<u32>(UiThemeEditorElement::count);
// The elements dragged or stepped as a value (the colour field, hue and the sliders).
constexpr bool ui_theme_editor_range(UiThemeEditorElement e) noexcept {
    return e == UiThemeEditorElement::sv || e == UiThemeEditorElement::hue ||
           e == UiThemeEditorElement::direction || e == UiThemeEditorElement::intensity ||
           e == UiThemeEditorElement::speed || e == UiThemeEditorElement::strength ||
           e == UiThemeEditorElement::density;
}
struct UiThemeEditorStyle {
    float width, height, padding, gap, radius, control_radius;
};
constexpr UiThemeEditorStyle ui_theme_editor_style() noexcept {
    return {420, 640, 20, 12, 16, 8};
}
struct UiThemeEditorScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    UiWidgetHandle elements[kUiThemeEditorElements];
    BackgroundTheme draft;
    BackgroundEffectSettings effect_profiles[kBackgroundEffectProfileCount];
    UiThemeEditorStyle style;
    UiRect panel, viewport;
    u32 revision, generation;
    u8 selected_stop;
    float hue, saturation, value;
    char hex[8], direction_text[4], intensity_text[5];
    char speed_text[5], strength_text[5], density_text[5];
    u8 direction_bytes, intensity_bytes;
    u8 speed_bytes, strength_bytes, density_bytes;
    bool active, save_failed, light_mode;
    // Installed effects, set by the owner before each update: how many, the one in use (empty: a built-in
    // effect), and whether it declares knobs. The row only lays out while there is one.
    Utf8View package_name;
    u32 package_count;
    bool package_knobs, packages_laid_out;
};
static_assert(sizeof(UiThemeEditorScene) +
                  kUiThemeEditorElements * (sizeof(UiWidget) + sizeof(UiLayoutNode)) <=
              32 * 1024);
bool ui_theme_editor_current(const UiThemeEditorScene&) noexcept;
bool ui_theme_editor_update(UiThemeEditorScene*, const BackgroundTheme&, u8 selected_stop, UiRect viewport,
                            const UiThemeEditorStyle* style = nullptr,
                            const BackgroundEffectSettings* profiles = nullptr) noexcept;
void ui_theme_editor_close(UiThemeEditorScene*) noexcept;
void ui_theme_editor_release(UiThemeEditorScene*) noexcept;
const UiLayoutNode* ui_theme_editor_node(const UiThemeEditorScene&, UiThemeEditorElement) noexcept;
// The element a widget handle names; count when none.
UiThemeEditorElement ui_theme_editor_element(const UiThemeEditorScene&, UiWidgetHandle) noexcept;
UiThemeEditorElement ui_theme_editor_hit_test(const UiThemeEditorScene&, UiPoint) noexcept;
bool ui_theme_editor_focus(UiThemeEditorScene*, UiThemeEditorElement) noexcept;
bool ui_theme_editor_move_focus(UiThemeEditorScene*, bool reverse) noexcept;
bool ui_theme_editor_scroll(UiThemeEditorScene*, float delta) noexcept;
UiScrollbarGeometry ui_theme_editor_scrollbar(const UiThemeEditorScene&) noexcept;
bool ui_theme_editor_pointer(UiThemeEditorScene*, UiThemeEditorElement, UiPoint, BackgroundTheme*) noexcept;
bool ui_theme_editor_key(UiThemeEditorScene*, UiThemeEditorElement, u32 virtual_key,
                         BackgroundTheme*) noexcept;
bool ui_theme_editor_action(UiThemeEditorScene*, UiThemeEditorElement, u32 random_seed,
                            BackgroundTheme*) noexcept;
bool ui_theme_editor_paint(const UiThemeEditorScene&, const Theme&, PaintStream*) noexcept;
bool ui_theme_editor_text_output(const UiThemeEditorScene&, const Theme&, UiTextOutput*) noexcept;
} // namespace maicord
