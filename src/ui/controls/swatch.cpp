#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/icon.hpp"

namespace maicord {
namespace {
constexpr float kLabelHeight = 32;
} // namespace

float ui_swatch_label_height() noexcept {
    return kLabelHeight;
}

u32 ui_swatch_columns(const UiSettingsTokens& tokens, float width) noexcept {
    const float pitch = tokens.metric.swatch + tokens.metric.swatch_gap + 12;
    return maximum(1u, static_cast<u32>((width + tokens.metric.swatch_gap) / pitch));
}

UiRect ui_swatch_rect(const UiSettingsTokens& tokens, UiRect field, u32 index, u32 columns) noexcept {
    columns = maximum(1u, columns);
    const float size = tokens.metric.swatch, pitch_x = size + tokens.metric.swatch_gap + 12;
    const float pitch_y = size + kLabelHeight + tokens.metric.swatch_gap * .5f;
    const float left = field.left + (index % columns) * pitch_x + 6, top = field.top + (index / columns) * pitch_y;
    return {left, top, left + size, top + size};
}

bool ui_swatch_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect swatch, Color fill,
                     const BackgroundTheme* gradient, bool selected, UiControlState state) noexcept {
    const auto& c = tokens.color;
    const float radius = tokens.metric.control_radius;
    const float alpha = state.enabled ? 1.f : .4f;
    bool ok = gradient && background_theme_valid(*gradient)
                  ? paint_stream_push_palette_gradient(paint, swatch, *gradient, radius)
                  : paint_stream_push_rounded(paint, swatch.left, swatch.top, swatch.right, swatch.bottom,
                                              ui_settings_color_alpha(fill, alpha), radius);
    const Color ring = state.hovered && !selected ? c.text_subtle : ui_settings_color_alpha(c.text_subtle, .5f);
    ok = ok && paint_stream_push_rounded_border(paint, swatch.left, swatch.top, swatch.right, swatch.bottom, {},
                                                ui_settings_color_alpha(ring, alpha), radius, 1);
    if (ok && (selected || state.focused))
        ok = paint_stream_push_rounded_border(paint, swatch.left - 4, swatch.top - 4, swatch.right + 4,
                                              swatch.bottom + 4, {}, selected ? c.brand : c.focus_ring,
                                              radius + 4, selected ? 3.f : 2.f);
    // Keyboard focus on the selected tile sits outside its selection ring.
    if (ok && selected && state.focused)
        ok = paint_stream_push_rounded_border(paint, swatch.left - 8, swatch.top - 8, swatch.right + 8,
                                              swatch.bottom + 8, {}, c.focus_ring, radius + 8, 2);
    if (ok && selected) {
        const float badge = 20, x = swatch.right - badge + 6, y = swatch.top - 6;
        ok = paint_stream_push_rounded_border(paint, x, y, x + badge, y + badge, c.on_brand, c.brand, badge * .5f,
                                              0) &&
             paint_stream_push_rounded(paint, x + 2, y + 2, x + badge - 2, y + badge - 2, c.brand,
                                       badge * .5f - 2) &&
             ui_icon_paint(paint, UiIcon::check, {x + 4, y + 4, x + badge - 4, y + badge - 4}, c.on_brand);
    }
    return ok;
}

} // namespace maicord
