#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"

namespace maicord {
namespace {
float option_height(const UiSettingsTokens& tokens) noexcept {
    return maximum(tokens.metric.radio_size, kUiSettingsLabelLine) + 4;
}
} // namespace

float ui_radio_group_height(const UiSettingsTokens& tokens, u32 count) noexcept {
    return count ? count * option_height(tokens) + (count - 1) * tokens.metric.radio_spacing : 0;
}

UiRect ui_radio_option(const UiSettingsTokens& tokens, UiRect group, u32 index) noexcept {
    const float top = group.top + index * (option_height(tokens) + tokens.metric.radio_spacing);
    return {group.left, top, group.right, top + option_height(tokens)};
}

UiRect ui_radio_indicator(const UiSettingsTokens& tokens, UiRect option) noexcept {
    const float size = tokens.metric.radio_size;
    const float top = (option.top + option.bottom - size) * .5f;
    return {option.left, top, option.left + size, top + size};
}

UiRect ui_radio_label(const UiSettingsTokens& tokens, UiRect option) noexcept {
    return {option.left + tokens.metric.radio_size + tokens.metric.radio_gap, option.top, option.right,
            option.bottom};
}

u32 ui_radio_hit(const UiSettingsTokens& tokens, UiRect group, u32 count, float x, float y) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (ui_rect_contains(ui_radio_option(tokens, group, i), x, y))
            return i;
    return ~0u;
}

bool ui_radio_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect indicator, bool selected,
                    UiControlState state) noexcept {
    const auto& c = tokens.color;
    const float alpha = state.enabled ? 1.f : tokens.disabled_alpha;
    const float radius = ui_rect_width(indicator) * .5f;
    const bool pressed = state.pressed && state.enabled, hovered = state.hovered && state.enabled;
    const Color fill = selected ? (pressed ? c.brand_active : hovered ? c.brand_hover : c.brand)
                       : pressed ? c.mod_normal
                       : hovered ? c.mod_subtle
                                 : c.input_background;
    const Color border = selected ? c.border_subtle : pressed || hovered ? c.radio_border_hover : c.radio_border;
    bool ok = paint_stream_push_rounded_border(paint, indicator.left, indicator.top, indicator.right,
                                               indicator.bottom, ui_settings_color_alpha(fill, alpha),
                                               ui_settings_color_alpha(border, alpha), radius, 2);
    if (ok && selected) {
        // The dot is concentric with the ring: inset by (size - dot) / 2.
        const float dot = ui_rect_width(indicator) * .4f, inset = (ui_rect_width(indicator) - dot) * .5f;
        ok = paint_stream_push_rounded(paint, indicator.left + inset, indicator.top + inset, indicator.right - inset,
                                       indicator.bottom - inset, ui_settings_color_alpha(c.on_brand, alpha),
                                       ui_inner_radius(radius, inset));
    }
    if (ok && state.focused)
        ok = ui_settings_focus_paint(paint, tokens, indicator, radius);
    return ok;
}

} // namespace maicord
