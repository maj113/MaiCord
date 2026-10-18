#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/icon.hpp"

namespace maicord {

UiRect ui_settings_ring_rect(UiRect shape, float gap, float width) noexcept {
    const float out = gap + width;
    return {shape.left - out, shape.top - out, shape.right + out, shape.bottom + out};
}

bool ui_settings_ring_paint(PaintStream* paint, UiRect shape, float radius, float gap, float width,
                            Color color) noexcept {
    const auto ring = ui_settings_ring_rect(shape, gap, width);
    return paint_stream_push_rounded_border(paint, ring.left, ring.top, ring.right, ring.bottom, {}, color,
                                            radius + gap + width, width);
}

bool ui_settings_focus_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect shape,
                             float radius) noexcept {
    return ui_settings_ring_paint(paint, shape, radius, tokens.metric.focus_gap, tokens.metric.focus_width,
                                  tokens.color.focus_ring);
}

bool ui_input_shell_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect field, UiControlState state,
                          bool error) noexcept {
    const auto& c = tokens.color;
    const float radius = tokens.metric.control_radius, alpha = state.enabled ? 1.f : tokens.disabled_alpha;
    const Color border = error           ? c.critical_text
                         : state.pressed ? c.border_normal
                         : state.hovered ? c.border_normal
                                         : c.input_border;
    bool ok = paint_stream_push_rounded_border(paint, field.left, field.top, field.right, field.bottom,
                                               ui_settings_color_alpha(c.input_background, alpha),
                                               ui_settings_color_alpha(border, alpha), radius, 1);
    if (ok && state.focused && state.enabled)
        ok = paint_stream_push_rounded_border(paint, field.left, field.top, field.right, field.bottom, {},
                                              error ? c.critical_text : c.brand, radius, 2);
    return ok;
}

// Select text and chevron share the field's inner padding: the chevron sits
// in a square end cell as tall as the field.
UiRect ui_select_text_rect(const UiSettingsTokens& tokens, UiRect field) noexcept {
    return {field.left + tokens.metric.field_padding, field.top, field.right - ui_rect_height(field), field.bottom};
}

bool ui_select_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect field, bool open,
                     UiControlState state, bool error) noexcept {
    if (!ui_input_shell_paint(paint, tokens, field,
                              {state.enabled, state.hovered, state.pressed, state.focused || open}, error))
        return false;
    const float icon = tokens.metric.nav_icon + 2, cell = ui_rect_height(field);
    const float cx = field.right - cell * .5f, cy = (field.top + field.bottom) * .5f;
    return ui_icon_paint(paint, open ? UiIcon::chevron_up : UiIcon::chevron_down,
                         {cx - icon * .5f, cy - icon * .5f, cx + icon * .5f, cy + icon * .5f},
                         ui_settings_color_alpha(tokens.color.text_strong, state.enabled ? 1.f : tokens.disabled_alpha));
}

float ui_settings_button_width(const UiSettingsTokens& tokens, UiSettingsButtonSize size,
                               float label_width) noexcept {
    const bool compact = size == UiSettingsButtonSize::compact;
    const float padding = compact ? tokens.metric.button_padding - 4 : tokens.metric.button_padding;
    return maximum(compact ? 60.f : 100.f, label_width + padding * 2);
}

bool ui_settings_button_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect rect,
                              UiSettingsButtonVariant variant, UiControlState state) noexcept {
    const auto& c = tokens.color;
    Color fill{}, border{};
    switch (variant) {
    case UiSettingsButtonVariant::primary:
        fill = state.pressed ? c.brand_active : state.hovered ? c.brand_hover : c.brand;
        border = c.button_border;
        break;
    case UiSettingsButtonVariant::critical:
        fill = state.pressed ? ui_settings_color_mix(c.critical_hover, {0, 0, 0, 1}, .1f)
               : state.hovered ? c.critical_hover
                               : c.critical;
        border = c.button_border;
        break;
    default:
        fill = state.pressed ? c.mod_strong : state.hovered ? c.mod_normal : c.mod_subtle;
        border = ui_settings_color_alpha(c.border_subtle, .33f);
        break;
    }
    const float alpha = state.enabled ? 1.f : tokens.disabled_alpha;
    const float radius = tokens.metric.control_radius;
    bool ok = paint_stream_push_rounded_border(paint, rect.left, rect.top, rect.right, rect.bottom,
                                               ui_settings_color_alpha(fill, alpha),
                                               ui_settings_color_alpha(border, alpha), radius, 1);
    if (ok && state.focused)
        ok = ui_settings_focus_paint(paint, tokens, rect, radius);
    return ok;
}

Color ui_settings_button_text(const UiSettingsTokens& tokens, UiSettingsButtonVariant variant,
                              UiControlState state) noexcept {
    const auto& c = tokens.color;
    const Color color = variant == UiSettingsButtonVariant::primary || variant == UiSettingsButtonVariant::critical
                            ? c.on_brand
                        : variant == UiSettingsButtonVariant::critical_secondary ? c.critical_text
                                                                                 : c.text_strong;
    return ui_settings_color_alpha(color, state.enabled ? 1.f : tokens.disabled_alpha);
}

bool ui_settings_shadow_paint(PaintStream* paint, UiRect rect, float radius, float spread, float alpha) noexcept {
    // Three widening layers approximate a blurred drop shadow offset downward.
    for (u32 i = 0; i < 3; ++i) {
        const float grow = spread * (i + 1) / 3.f, offset = spread * .5f;
        if (!paint_stream_push_rounded(paint, rect.left - grow, rect.top - grow + offset, rect.right + grow,
                                       rect.bottom + grow + offset, {0, 0, 0, alpha / 3.f}, radius + grow))
            return false;
    }
    return true;
}

} // namespace maicord
