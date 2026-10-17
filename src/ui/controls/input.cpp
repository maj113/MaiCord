#include "maicord/ui/controls/settings_controls.hpp"

namespace maicord {

bool ui_input_shell_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect field, UiControlState state,
                          bool error) noexcept {
    const auto& c = tokens.color;
    const float radius = tokens.metric.control_radius, alpha = state.enabled ? 1.f : .5f;
    const Color border = error ? c.critical_text : state.hovered ? c.border_normal : c.input_border;
    bool ok = paint_stream_push_rounded_border(paint, field.left, field.top, field.right, field.bottom,
                                               ui_settings_color_alpha(c.input_background, alpha),
                                               ui_settings_color_alpha(border, alpha), radius, 1);
    // Focus draws a 2-DIP brand outline over the border.
    if (ok && state.focused)
        ok = paint_stream_push_rounded_border(paint, field.left, field.top, field.right, field.bottom, {}, c.brand,
                                              radius, 2);
    return ok;
}

UiRect ui_select_text_rect(const UiSettingsTokens& tokens, UiRect field) noexcept {
    return {field.left + 12, field.top, field.right - tokens.metric.control_height, field.bottom};
}

bool ui_select_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect field, bool open,
                     UiControlState state) noexcept {
    if (!ui_input_shell_paint(paint, tokens, field, {state.enabled, state.hovered, false, state.focused || open}))
        return false;
    const float icon = 20, cx = field.right - 18, cy = (field.top + field.bottom) * .5f;
    return ui_icon_paint(paint, open ? UiIcon::chevron_up : UiIcon::chevron_down,
                         {cx - icon * .5f, cy - icon * .5f, cx + icon * .5f, cy + icon * .5f},
                         ui_settings_color_alpha(tokens.color.text_strong, state.enabled ? 1.f : .5f));
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
    const float alpha = state.enabled ? 1.f : .5f;
    const float radius = tokens.metric.control_radius;
    bool ok = paint_stream_push_rounded_border(paint, rect.left, rect.top, rect.right, rect.bottom,
                                               ui_settings_color_alpha(fill, alpha),
                                               ui_settings_color_alpha(border, alpha), radius, 1);
    if (ok && state.focused)
        ok = paint_stream_push_rounded_border(paint, rect.left - 3, rect.top - 3, rect.right + 3, rect.bottom + 3,
                                              {}, c.focus_ring, radius + 3, 2);
    return ok;
}

Color ui_settings_button_text(const UiSettingsTokens& tokens, UiSettingsButtonVariant variant,
                              UiControlState state) noexcept {
    const auto& c = tokens.color;
    const Color color = variant == UiSettingsButtonVariant::primary || variant == UiSettingsButtonVariant::critical
                            ? c.on_brand
                        : variant == UiSettingsButtonVariant::critical_secondary ? c.critical_text
                                                                                 : c.text_strong;
    return ui_settings_color_alpha(color, state.enabled ? 1.f : .5f);
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
