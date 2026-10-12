#include "maicord/ui/controls/settings_controls.hpp"

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
    const float alpha = state.enabled ? 1.f : .5f;
    const float radius = ui_rect_width(indicator) * .5f;
    const Color fill = selected ? (state.hovered ? c.brand_hover : c.brand) : c.input_background;
    const Color border = selected ? c.border_subtle : state.hovered ? c.radio_border_hover : c.radio_border;
    bool ok = paint_stream_push_rounded_border(paint, indicator.left, indicator.top, indicator.right,
                                               indicator.bottom, ui_settings_color_alpha(fill, alpha),
                                               ui_settings_color_alpha(border, alpha), radius, 2);
    if (ok && selected) {
        const float dot = ui_rect_width(indicator) * .4f;
        const float cx = (indicator.left + indicator.right) * .5f, cy = (indicator.top + indicator.bottom) * .5f;
        ok = paint_stream_push_rounded(paint, cx - dot * .5f, cy - dot * .5f, cx + dot * .5f, cy + dot * .5f,
                                       ui_settings_color_alpha(c.on_brand, alpha), dot * .5f);
    }
    if (ok && state.focused)
        ok = paint_stream_push_rounded_border(paint, indicator.left - 3, indicator.top - 3, indicator.right + 3,
                                              indicator.bottom + 3, {}, c.focus_ring, radius + 3, 2);
    return ok;
}

} // namespace maicord
