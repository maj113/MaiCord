#include "maicord/ui/badge.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/icon.hpp"
namespace maicord {
UiBadgeLayout ui_badge_geometry(const UiBadgeWidget& badge, const UiWidgetManager& manager) noexcept {
    const auto* node = ui_widget_manager_layout_node(manager, badge.owner);
    if (!node)
        return {};
    UiBadgeLayout r{};
    r.outer = ui_rect_has_area(badge.rectangle) ? badge.rectangle : node->rectangle;
    const float cy = (r.outer.top + r.outer.bottom) * .5f;
    if (badge.compact) {
        const float mark = badge.leading ? kUiBadgeCompactIcon + kUiBadgeCompactIconGap : 0.0f;
        r.text = {r.outer.left + kUiBadgeCompactPadding + mark, r.outer.top, r.outer.right - kUiBadgeCompactPadding,
                  r.outer.bottom};
        if (badge.leading && badge.accent)
            r.icon = ui_app_badge_check_rectangle(r.outer);
        else if (badge.leading)
            r.icon = {r.outer.left + kUiBadgeCompactPadding, cy - kUiBadgeCompactIcon * .5f,
                      r.outer.left + kUiBadgeCompactPadding + kUiBadgeCompactIcon, cy + kUiBadgeCompactIcon * .5f};
        return r;
    }
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
    if (b.compact) {
        // The muted chip fill without a border; accent is the APP tag: the
        // brand fill and, for a verified app, a check in the label's colour.
        bool ok = paint_stream_push_rounded(
            paint, r.outer,
            ui_theme_color_alpha(b.accent ? theme.tokens.accent.app_badge : theme.tokens.fill.muted, b.opacity),
            theme.scale.radius_sm);
        if (ok && b.hovered)
            ok = paint_stream_push_rounded(paint, r.outer, theme.tokens.state.hover, theme.scale.radius_sm);
        if (ok && b.leading && b.accent)
            ok = ui_icon_paint(paint, UiIcon::check, r.icon, ui_theme_color_alpha(theme.tokens.text.on_accent, b.opacity));
        else if (ok && b.leading && b.marker.alpha)
            ok = paint_stream_push_rounded(paint, r.icon, b.marker, 7);
        return paint_stream_pop_clip(paint) && ok;
    }
    bool ok = paint_stream_push_rounded_border(paint, r.outer, Color{},
                                               ui_theme_color_alpha(theme.tokens.border.strong, b.opacity), 7, 1);
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
    auto color = b.accent ? theme.tokens.text.on_accent : theme.text_primary;
    color.alpha *= b.opacity;
    return ui_text_output_push(text, {b.label,
                                      {&manager, b.owner},
                                      r.text,
                                      n->clip_rectangle,
                                      color,
                                      b.style,
                                      UiTextAlignment::start,
                                      UiTextAlignment::center,
                                      TextWrapping::no_wrap,
                                      TextOverflow::ellipsis});
}
bool ui_badge_text(const UiBadgeWidget& b, const UiWidgetManager& manager, const Theme& theme,
                   UiTextCommandStream* text) noexcept {
    UiTextOutput output{text, nullptr};
    return ui_badge_text_output(b, manager, theme, &output);
}
} // namespace maicord
