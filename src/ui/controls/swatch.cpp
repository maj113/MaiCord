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

u32 ui_swatch_columns(const UiSettingsLayoutTokens& tokens, float width) noexcept {
    const float pitch = tokens.metric.swatch + tokens.metric.swatch_gap + 12;
    return maximum(1u, static_cast<u32>((width + tokens.metric.swatch_gap) / pitch));
}

UiRect ui_swatch_rect(const UiSettingsLayoutTokens& tokens, const UiRect& field, u32 index, u32 columns) noexcept {
    columns = maximum(1u, columns);
    const float size = tokens.metric.swatch, pitch_x = size + tokens.metric.swatch_gap + 12;
    const float pitch_y = size + kLabelHeight + tokens.metric.swatch_gap * .5f;
    const float left = field.left + (index % columns) * pitch_x + 6, top = field.top + (index / columns) * pitch_y;
    return {left, top, left + size, top + size};
}

bool ui_swatch_paint(PaintStream* paint, const UiSettingsTokens& tokens, const UiRect& swatch, const Color& fill,
                     const BackgroundTheme* gradient, bool selected, UiControlState state) noexcept {
    const auto& c = tokens.color;
    const auto& m = tokens.metric;
    const float radius = m.control_radius;
    const float alpha = state.enabled ? 1.f : tokens.disabled_alpha;
    bool ok = gradient && background_theme_valid(*gradient)
                  ? paint_stream_push_palette_gradient(paint, swatch, *gradient, radius)
                  : paint_stream_push_rounded(paint, swatch, ui_settings_color_alpha(fill, alpha), radius);
    const Color edge = state.hovered && !selected ? c.text_subtle : ui_settings_color_alpha(c.text_subtle, .5f);
    ok = ok && paint_stream_push_rounded_border(paint, swatch, {}, ui_settings_color_alpha(edge, alpha), radius, 1);
    // Selection is a brand ring around the tile; keyboard focus rings the
    // selection (or the bare tile) from outside. All rings are concentric.
    const float outside = selected ? m.ring_gap + m.ring_width : 0;
    if (ok && selected)
        ok = ui_settings_ring_paint(paint, swatch, radius, m.ring_gap, m.ring_width,
                                    ui_settings_color_alpha(c.brand, alpha));
    if (ok && state.focused)
        ok = ui_settings_ring_paint(paint, swatch, radius, outside + m.focus_gap, m.focus_width, c.focus_ring);
    if (ok && selected) {
        // The check badge sits over the tile's top end corner.
        const float badge = 20, x = swatch.right - badge + 6, y = swatch.top - 6, inset = m.ring_width;
        ok = paint_stream_push_rounded(paint, x, y, x + badge, y + badge, c.on_brand, badge * .5f) &&
             paint_stream_push_rounded(paint, x + inset, y + inset, x + badge - inset, y + badge - inset, c.brand,
                                       ui_inner_radius(badge * .5f, inset)) &&
             ui_icon_paint(paint, UiIcon::check, {x + 4, y + 4, x + badge - 4, y + badge - 4}, c.on_brand);
    }
    return ok;
}

} // namespace maicord
