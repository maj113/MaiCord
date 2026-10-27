#include "maicord/ui/badge.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/icon.hpp"
namespace maicord {
UiBadgeLayout ui_badge_geometry(const UiBadgeWidget& badge, const UiWidgetManager& manager) noexcept {
    const auto* node = ui_widget_manager_layout_node(manager, badge.owner);
    if (!node)
        return {};
    UiBadgeLayout r{};
    r.outer = ui_rect_has_area(badge.rectangle) ? badge.rectangle : node->rectangle;
    const float cy = (r.outer.top + r.outer.bottom) * .5f;
    r.text = {r.outer.left + 6 + (badge.leading ? 20 : 0), r.outer.top, r.outer.right - 6, r.outer.bottom};
    if (badge.leading)
        r.icon = {r.outer.left + 6, cy - 7, r.outer.left + 20, cy + 7};
    return r;
}
bool ui_badge_paint(const UiBadgeWidget& b, const UiWidgetManager& manager, const Theme& theme,
                    PaintStream* paint) noexcept {
    const auto* n = ui_widget_manager_layout_node(manager, b.owner);
    if (!n)
        return false;
    const auto r = ui_badge_geometry(b, manager);
    if (!paint_stream_push_clip(paint, n->clip_rectangle))
        return false;
    auto fill = b.accent ? theme.tokens.emphasis.fill : Color{};
    fill.alpha *= b.opacity;
    bool ok = paint_stream_push_rounded_border(paint, r.outer, fill, ui_theme_color_alpha(theme.tokens.border.strong, b.opacity), 7, 1);
    if (ok && b.leading && b.marker.alpha)
        ok = paint_stream_push_rounded(paint, r.icon, b.marker, 7);
    return paint_stream_pop_clip(paint) && ok;
}
bool ui_badge_text_output(const UiBadgeWidget& b, const UiWidgetManager& manager, const Theme& theme,
                          UiTextOutput* text) noexcept {
    const auto* n = ui_widget_manager_layout_node(manager, b.owner);
    if (!n)
        return false;
    const auto r = ui_badge_geometry(b, manager);
    auto color = b.accent ? ui_surface_foreground(theme, theme.tokens.emphasis.fill) : theme.text_primary;
    color.alpha *= b.opacity;
    return ui_text_output_push(text, {b.label,
                                      {&manager, b.owner},
                                      r.text,
                                      n->clip_rectangle,
                                      color,
                                      b.style,
                                      UiTextAlignment::start,
                                      UiTextAlignment::center,
                                      UiTextWrapping::no_wrap,
                                      UiTextOverflow::ellipsis});
}
} // namespace maicord

namespace maicord {
bool ui_badge_text(const UiBadgeWidget& b, const UiWidgetManager& manager, const Theme& theme,
                   UiTextCommandStream* text) noexcept {
    UiTextOutput output{text, nullptr};
    return ui_badge_text_output(b, manager, theme, &output);
}
} // namespace maicord
