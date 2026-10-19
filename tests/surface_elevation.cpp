#include "maicord/app/client_theme.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/text_form.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord {
namespace {
bool same(Color a, Color b) noexcept {
    return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
}
bool close_to(float a, float b) noexcept {
    return a - b < .0005f && b - a < .0005f;
}
Theme projected(ClientTheme kind, u8 intensity = 40) noexcept {
    auto preferences = client_preferences_defaults();
    preferences.theme = kind;
    preferences.gradient = background_theme_defaults();
    preferences.gradient.intensity = intensity;
    return client_theme_projection(preferences);
}
Color composite(Color top, Color bottom) noexcept {
    const float a = top.alpha;
    return {top.red * a + bottom.red * (1 - a), top.green * a + bottom.green * (1 - a),
            top.blue * a + bottom.blue * (1 - a), 1};
}
bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    const auto valid = utf8_validate(request.text);
    if (!valid.valid || request.constraint.width <= 0)
        return false;
    const float natural = static_cast<float>(valid.codepoints) * 8;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 8));
    const u32 lines = request.no_wrap ? 1u : maximum(1u, (valid.codepoints + columns - 1) / columns);
    *value = {{minimum(natural, request.constraint.width), static_cast<float>(lines) * 20}, 16, lines};
    return true;
}
// The first quad of an elevation surface carries the gradient-material marker
// and, in reserved_y, the visible gradient share the elevation selected.
bool gradient_material(const PaintQuad& quad, const Theme& theme, UiElevation elevation) noexcept {
    return quad.border_width == -7 && close_to(quad.reserved_y, ui_elevation_gradient_share(theme, elevation));
}
} // namespace

void test_surface_elevation(void (*check)(bool, const char*) noexcept) noexcept {
    const Theme gradients[]{projected(ClientTheme::custom_gradient), projected(ClientTheme::aurora),
                            projected(ClientTheme::sunset), projected(ClientTheme::ocean),
                            projected(ClientTheme::custom_light_gradient),
                            projected(ClientTheme::custom_gradient, 0),
                            projected(ClientTheme::custom_light_gradient, 100)};
    const Theme flats[]{kDefaultTheme, kAmoledTheme, kLightTheme};

    // Surface style resolution per elevation.
    bool ladder = true, materials = true;
    for (const auto& theme : gradients) {
        ladder = ladder && ui_background_active(theme);
        float previous = 0;
        for (u32 i = 0; i < kUiElevationCount; ++i) {
            const auto elevation = static_cast<UiElevation>(i);
            const auto style = ui_surface_style(theme, elevation);
            const float share = ui_elevation_gradient_share(theme, elevation);
            ladder = ladder && share > 0 && share >= previous;
            materials = materials && style.mode == UiBackgroundSurfaceMode::gradient &&
                        style.border_tone == UiBackgroundBorderTone::palette && style.border_width > 0;
            previous = share;
        }
        ladder = ladder && ui_elevation_gradient_share(theme, UiElevation::popover) >
                               ui_elevation_gradient_share(theme, UiElevation::banner);
    }
    check(ladder && materials,
          "surface elevation: gradient overlays use the gradient material with a palette edge and gradient "
          "contrast never falls as elevation rises (dark, light, every intensity)");
    bool flat = true;
    for (const auto& theme : flats) {
        for (u32 i = 0; i < kUiElevationCount; ++i) {
            const auto elevation = static_cast<UiElevation>(i);
            const auto style = ui_surface_style(theme, elevation);
            flat = flat && style.mode == UiBackgroundSurfaceMode::tinted &&
                   style.border_tone == UiBackgroundBorderTone::semantic &&
                   ui_elevation_gradient_share(theme, elevation) == 0 && style.fill.alpha == 1;
        }
        flat = flat && same(ui_surface_style(theme, UiElevation::popover).fill, theme.base_lower) &&
               same(ui_surface_style(theme, UiElevation::popover).border, theme.interactive) &&
               same(ui_surface_style(theme, UiElevation::dialog).border, theme.tokens.border.strong) &&
               same(ui_surface_style(theme, UiElevation::tooltip).fill, theme.base_lowest) &&
               same(ui_surface_style(theme, UiElevation::banner).fill, theme.surface_high);
    }
    check(flat, "surface elevation: flat themes keep each role's opaque palette fill and semantic border");

    PaintStream paint{};
    const bool stream = paint_stream_initialize(&paint);
    check(stream, "surface elevation: paint stream");
    if (!stream)
        return;
    const Theme& gradient = gradients[0];
    bool painted = true;
    for (u32 i = 0; i < kUiElevationCount; ++i) {
        const auto elevation = static_cast<UiElevation>(i);
        // Popovers and dialogs push their shadow plates first.
        const u32 plate = ui_surface_casts_shadow(elevation) ? kUiSurfaceShadowLayers : 0;
        painted = painted && paint_stream_reset(&paint, 400, 300) &&
                  ui_surface_paint(&paint, {10, 10, 200, 120}, gradient, elevation, 8) && paint.count == plate + 1 &&
                  gradient_material(paint.quads[plate], gradient, elevation) && paint.quads[plate].reserved_x < 0 &&
                  paint_stream_reset(&paint, 400, 300) &&
                  ui_surface_paint(&paint, {10, 10, 200, 120}, kDefaultTheme, elevation, 8) &&
                  paint.quads[plate].border_width == 1 &&
                  same(paint.quads[plate].fill, ui_surface_style(kDefaultTheme, elevation).fill);
    }
    check(painted, "surface elevation: every role paints its tier's gradient share or its flat fill");
    // Popovers and dialogs cast a drop shadow drawn before the plate, larger
    // than it and offset downward, on light, dark and gradient themes alike
    // (dark uses the denser shadow token and a longer drop); cards, banners and
    // tooltips cast none.
    bool shadows = paint_stream_reset(&paint, 400, 300) &&
                   ui_surface_paint(&paint, {10, 10, 200, 120}, kLightTheme, UiElevation::popover, 8) &&
                   paint.count == kUiSurfaceShadowLayers + 1 && paint.quads[0].left < 10 &&
                   paint.quads[0].bottom > 120 + kUiSurfaceShadowOffset && paint.quads[0].fill.alpha < .1f &&
                   paint.quads[kUiSurfaceShadowLayers].left == 10 &&
                   same(paint.quads[kUiSurfaceShadowLayers].fill, ui_surface_style(kLightTheme, UiElevation::popover).fill);
    const UiElevation shadowless[]{UiElevation::card, UiElevation::banner, UiElevation::tooltip};
    for (const auto elevation : shadowless)
        shadows = shadows && paint_stream_reset(&paint, 400, 300) &&
                  ui_surface_paint(&paint, {10, 10, 200, 120}, kLightTheme, elevation, 8) && paint.count == 1 &&
                  paint_stream_reset(&paint, 400, 300) &&
                  ui_surface_paint(&paint, {10, 10, 200, 120}, kDefaultTheme, elevation, 8) && paint.count == 1;
    const Theme* darks[]{&kDefaultTheme, &kAmoledTheme, &gradients[0], &gradients[1]};
    for (const Theme* dark : darks) {
        const auto dialog = ui_surface_shadow_color(*dark, UiElevation::dialog);
        shadows = shadows && dark->appearance != ThemeAppearance::light && paint_stream_reset(&paint, 400, 300) &&
                  ui_surface_paint(&paint, {10, 10, 200, 120}, *dark, UiElevation::dialog, 8) &&
                  paint.count == kUiSurfaceShadowLayers + 1 && paint.quads[0].left < 10 &&
                  paint.quads[0].bottom > 120 + kUiSurfaceShadowOffset * 1.5f &&
                  same(paint.quads[0].fill, dialog) && dialog.red == 0 && dialog.alpha > .07f &&
                  dialog.alpha > ui_surface_shadow_color(kLightTheme, UiElevation::dialog).alpha &&
                  ui_surface_shadow_color(*dark, UiElevation::popover).alpha < dialog.alpha &&
                  paint.quads[kUiSurfaceShadowLayers].left == 10;
    }
    shadows = shadows && paint_stream_reset(&paint, 400, 300) &&
              ui_surface_paint(&paint, {10, 10, 200, 120}, gradients[4], UiElevation::popover, 8) &&
              paint.count == kUiSurfaceShadowLayers + 1;
    check(shadows, "surface elevation: popovers and dialogs cast a drop shadow on light, dark and gradient themes "
                   "(denser on dark); cards, banners and tooltips none");

    // Popup menus (context menus, autocomplete, pickers): popover elevation by
    // default, without any per-menu opt-in.
    PopupItem items[3]{};
    items[0] = {utf8_literal("Reply"), 1, popup_item_enabled};
    items[1] = {utf8_literal("Copy Text"), 2, popup_item_enabled};
    items[2] = {utf8_literal("Delete Message"), 3, popup_item_enabled | popup_item_danger};
    PopupRowWidget rows[3]{};
    UiWidget widgets[8]{};
    UiLayoutNode nodes[8]{};
    PopupScene popup{};
    const bool opened = popup_scene_initialize(&popup, {items, 3}, {rows, 3}, {widgets, 8}, {nodes, 8}) &&
                        popup_scene_open(&popup, PopupKind::context_menu, 7, {items, 3}, {100, 100, 101, 101},
                                         {0, 0, 800, 600}, 220)
                            .ok();
    check(opened && popup.panel_material == PopupPanelMaterial::tinted,
          "surface elevation: context menu opens without the old gradient opt-in");
    check(opened && paint_stream_reset(&paint, 800, 600) && popup_presentation_build(popup, gradient, &paint) &&
              paint.count > kUiSurfaceShadowLayers &&
              gradient_material(paint.quads[kUiSurfaceShadowLayers], gradient, UiElevation::popover) &&
              paint_stream_reset(&paint, 800, 600) && popup_presentation_build(popup, kDefaultTheme, &paint) &&
              same(paint.quads[kUiSurfaceShadowLayers].fill, kDefaultTheme.base_lower) &&
              paint.quads[kUiSurfaceShadowLayers].border_width == 1,
          "surface elevation: context menu panel uses the popover material (gradient) and flat panel (plain)");
    popup_scene_close(&popup);

    // Confirmation dialogs ("Log out?") share the dialog elevation.
    TextFormScene form{};
    UiTextMeasureRecord records[32]{};
    UiTextMeasureCache cache{};
    TextFormContent content{};
    content.title = utf8_literal("Log Out");
    content.context = utf8_literal("Are you sure you want to log out?");
    content.cancel = utf8_literal("Cancel");
    content.accept = utf8_literal("Log Out");
    content.can_accept = content.can_cancel = content.confirmation_only = true;
    const bool form_ready = ui_text_measure_cache_initialize(&cache, {records, 32}, measure, nullptr) &&
                            text_form_update(&form, content, {0, 0, 640, 480}, &cache) && form.active;
    bool dialog = false, primary = false;
    if (form_ready && paint_stream_reset(&paint, 640, 480) && text_form_paint(form, {}, gradient, &paint)) {
        for (u32 i = 0; i < paint.count; ++i)
            dialog = dialog || gradient_material(paint.quads[i], gradient, UiElevation::dialog);
        for (u32 i = 0; i < paint.count; ++i)
            primary = primary || same(paint.quads[i].fill, gradient.tokens.emphasis.fill);
    }
    check(form_ready && dialog && primary,
          "surface elevation: confirmation dialog panel uses the dialog material and its primary action the "
          "gradient emphasis fill");
    text_form_release(&form);
    paint_stream_release(&paint);

    // Accent tokens: gradient derivation with accessible contrast.
    bool derived = true;
    for (const auto& theme : gradients) {
        const auto& e = theme.tokens.emphasis;
        const auto floating = theme.tokens.background.floating, primary_bg = theme.tokens.background.primary;
        Color boosted = e.selected;
        boosted.alpha = minimum(1.f, e.selected.alpha * kUiThemeEmphasisElevationBoost);
        const Color top = ui_emphasis_selected(theme, UiElevation::tooltip);
        derived = derived && !same(e.fill, theme.accent) && !same(e.outline, theme.accent) &&
                  ui_theme_contrast_ratio(e.text, e.fill) >= 4.5f &&
                  ui_theme_contrast_ratio(e.outline, floating) >= 3 &&
                  ui_theme_contrast_ratio(e.outline, primary_bg) >= 3 &&
                  ui_theme_contrast_ratio(theme.tokens.text.normal, composite(boosted, floating)) >= 4.5f &&
                  ui_theme_contrast_ratio(theme.tokens.text.normal, composite(boosted, primary_bg)) >= 4.5f &&
                  same(theme.tokens.border.focus, e.outline) && top.alpha <= boosted.alpha + .0001f &&
                  ui_emphasis_selected(theme, UiElevation::popover).alpha >=
                      ui_emphasis_selected(theme, UiElevation::card).alpha;
        UiThemeContrastWarning warnings[16]{};
        const u32 count = ui_theme_contrast_warnings(theme, warnings, 16);
        for (u32 i = 0; i < minimum(count, 16u); ++i)
            derived = derived && warnings[i].foreground != UiThemeToken::emphasis_text &&
                      warnings[i].foreground != UiThemeToken::emphasis_outline;
    }
    check(derived, "accent tokens: gradient themes derive fill/text/outline/selected/focus from the palette and "
                   "pass the 4.5:1 label, 3:1 indicator and selected-text contrast checks");
    bool mentions = true;
    for (const auto& theme : gradients) {
        const auto& mention = theme.tokens.mention;
        const auto& defaults = theme.appearance == ThemeAppearance::light ? kLightTheme : kDefaultTheme;
        mentions = mentions && same(mention.bar, theme.tokens.emphasis.outline) &&
                   same(mention.text, defaults.tokens.mention.text) &&
                   same(mention.background,
                        ui_theme_color_alpha(mention.bar, defaults.tokens.mention.background.alpha)) &&
                   same(mention.inline_background, defaults.tokens.mention.inline_background) &&
                   ui_theme_contrast_ratio(theme.tokens.text.normal,
                                           composite(mention.background, theme.tokens.background.primary)) >= 4.5f;
    }
    check(mentions, "mention tokens: gradient message highlights follow the theme hue, retain their opacity "
                    "and readable message text, and leave inline styles untouched");
    // Hover/selected glass and the neutral state/fill tokens follow the theme hue.
    const auto aligned = [](Color first, Color second) noexcept {
        const auto x = background_color_oklab(first), y = background_color_oklab(second);
        const double cross = x.a * y.b - x.b * y.a;
        return x.a * y.a + x.b * y.b > 0 && cross * cross < .04 * (x.a * x.a + x.b * x.b) * (y.a * y.a + y.b * y.b);
    };
    const auto tone = [](Color c) noexcept { return .2126f * c.red + .7152f * c.green + .0722f * c.blue; };
    bool glass = true;
    for (const auto& theme : gradients) {
        const auto& defaults = theme.appearance == ThemeAppearance::light ? kLightTheme : kDefaultTheme;
        const auto& state = theme.tokens.state;
        const Color sources[]{theme.surface_high, theme.interactive, theme.interactive_hover};
        for (const Color source : sources) {
            const Color hover = ui_background_hover_color(theme, source, .28f);
            glass = glass && close_to(hover.alpha, source.alpha * .28f) && aligned(hover, state.tint) &&
                    tone(hover) - tone(source) < .01f && tone(source) - tone(hover) < .01f;
        }
        glass = glass && aligned(state.tint, theme.tokens.emphasis.outline) &&
                state.hover.alpha == defaults.tokens.state.hover.alpha &&
                state.pressed.alpha == defaults.tokens.state.pressed.alpha &&
                theme.tokens.fill.subtle.alpha == defaults.tokens.fill.subtle.alpha &&
                theme.tokens.fill.muted.alpha == defaults.tokens.fill.muted.alpha &&
                aligned(state.hover, state.tint) && aligned(state.pressed, state.tint) &&
                aligned(theme.tokens.fill.subtle, state.tint) && aligned(theme.tokens.fill.muted, state.tint);
    }
    for (const auto& theme : flats)
        glass = glass && same(theme.tokens.state.tint, theme.interactive) &&
                same(ui_background_hover_color(theme, theme.surface_high, .2f), theme.surface_high);
    check(glass, "state tokens: gradient hover/selected glass keeps its tone and opacity but takes the theme "
                 "hue, as do the state and fill overlays; flat themes keep their opaque neutrals");
    Theme translucent = kDefaultTheme;
    translucent.accent.alpha = .5f;
    translucent = ui_theme_resolve_defaults(translucent);
    translucent.background = gradient.background;
    const auto original_mentions = translucent.tokens.mention;
    check(ui_theme_resolve_emphasis(&translucent) &&
              translucent.tokens.mention.background.alpha == original_mentions.background.alpha &&
              translucent.tokens.mention.bar.alpha == original_mentions.bar.alpha &&
              translucent.tokens.mention.text.alpha == original_mentions.text.alpha &&
              translucent.tokens.mention.inline_background.alpha == original_mentions.inline_background.alpha,
          "mention tokens: gradient adaptation preserves translucent palette opacity for message highlights");
    // The hue follows the most chromatic stop (default ramp: violet 0x6935b8).
    const auto stop = background_color_oklab(background_color_rgb24(0x6935b8));
    const auto fill = background_color_oklab(gradient.tokens.emphasis.fill);
    check(stop.a * fill.a + stop.b * fill.b > 0 &&
              (stop.a * fill.b - stop.b * fill.a) * (stop.a * fill.b - stop.b * fill.a) <
                  .04 * (stop.a * stop.a + stop.b * stop.b) * (fill.a * fill.a + fill.b * fill.b),
          "accent tokens: emphasis hue follows the gradient's most chromatic stop");
    bool plain = true;
    for (auto theme : flats) {
        const Theme before = theme;
        plain = plain && same(theme.tokens.emphasis.fill, theme.accent) &&
                same(theme.tokens.emphasis.outline, theme.accent) &&
                same(theme.tokens.emphasis.text, theme.tokens.text.on_accent) &&
                same(theme.tokens.border.focus, theme.accent) && !ui_theme_resolve_emphasis(&theme) &&
                bytes_equal(&before, &theme, sizeof(Theme)) &&
                same(ui_emphasis_selected(theme, UiElevation::tooltip), theme.tokens.emphasis.selected) &&
                same(ui_settings_tokens(theme).color.brand, theme.accent);
    }
    check(plain, "accent tokens: flat themes keep their accent and resolution leaves them untouched");
    Theme authored = gradients[0];
    const Color custom{.9f, .2f, .3f, 1};
    authored.tokens.emphasis.fill = custom;
    authored.tokens.mention.bar = custom;
    authored.tokens.mention.text = custom;
    authored.tokens.mention.background = ui_theme_color_alpha(custom, .3f);
    authored.tokens.mention.inline_background = ui_theme_color_alpha(custom, .4f);
    Theme again = authored;
    check(ui_theme_resolve_emphasis(&again) && same(again.tokens.emphasis.fill, custom) &&
              bytes_equal(&again, &authored, sizeof(Theme)),
          "accent tokens: resolution is idempotent and preserves authored overrides");
    Theme grey = projected(ClientTheme::custom_gradient);
    grey.background.color_count = 2;
    grey.background.colors[0] = 0x303030;
    grey.background.colors[1] = 0x808080;
    grey.background.colors[2] = 0;
    UiThemeEmphasis unused{};
    check(!ui_theme_gradient_emphasis(grey, &unused), "accent tokens: an achromatic gradient keeps the accent");
    const auto settings = ui_settings_tokens(gradient);
    check(same(settings.color.brand, gradient.tokens.emphasis.fill) &&
              same(settings.color.focus_ring, gradient.tokens.border.focus) &&
              same(settings.color.nav_selected, gradient.tokens.emphasis.selected) &&
              ui_theme_contrast_ratio(settings.color.on_brand, settings.color.brand_hover) >= 4.5f,
          "accent tokens: switches, sliders, radios, nav selection and focus rings follow the emphasis tokens");
}
} // namespace maicord
