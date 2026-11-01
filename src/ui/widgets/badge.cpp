#include "maicord/ui/widgets/badge.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/widgets/icon.hpp"
#include "maicord/ui/core/paint.hpp"
#include "maicord/ui/text/measure_cache.hpp"
#include "maicord/ui/text/presentation.hpp"
namespace maicord {
UiBadgeLayout ui_badge_layout(UiTextMeasureCache* cache, const Utf8View& label, const UiRect& line, bool icon,
                              u32 flags) noexcept {
    UiTextMeasureValue value{};
    const TextStyleSpan span{0, label.bytes, flags};
    float width = label.bytes * 7.f;
    if (cache &&
        ui_text_measure_cache_measure(cache,
                                      {{0x4241444745, hash64_bytes(kHash64Seed, label.data, label.bytes), 0,
                                        static_cast<u16>(flags), 0},
                                       label,
                                       {&span, 1},
                                       {256, 40},
                                       true},
                                      &value)
            .ok())
        width = value.size.width;
    width = static_cast<float>(static_cast<u32>(width + .999f)) + 2;
    const float cy = (line.top + line.bottom) * .5f;
    UiBadgeLayout result{};
    result.outer = {maximum(line.left, line.right - width - 12 - (icon ? 20 : 0)), cy - 10, line.right, cy + 10};
    result.text = {result.outer.left + 6 + (icon ? 20 : 0), cy - 10, result.outer.right - 6, cy + 10};
    if (icon)
        result.icon = {result.outer.left + 6, cy - 8, result.outer.left + 22, cy + 8};
    result.clipped = ui_rect_width(result.text) + .01f < width;
    return result;
}
UiBadgeLayout ui_badge_compact_layout(UiTextMeasureCache* cache, const Utf8View& label, const UiRect& line,
                                      bool icon, u32 flags) noexcept {
    UiTextMeasureValue value{};
    const TextStyleSpan span{0, label.bytes, flags};
    float width = label.bytes * 6.f;
    if (cache &&
        ui_text_measure_cache_measure(cache,
                                      {{0x4241444743, hash64_bytes(kHash64Seed, label.data, label.bytes), 0,
                                        static_cast<u16>(flags), 0},
                                       label,
                                       {&span, 1},
                                       {256, 40},
                                       true},
                                      &value)
            .ok())
        width = value.size.width;
    const float mark = icon ? kUiBadgeCompactIcon + kUiBadgeCompactIconGap : 0.0f;
    const float content = minimum(kUiBadgeCompactMaximumContent, mark + static_cast<float>(static_cast<u32>(width + .999f)));
    const float cy = (line.top + line.bottom) * .5f;
    UiBadgeLayout result{};
    result.outer = {line.left, cy - kUiBadgeCompactHeight * .5f,
                    minimum(line.right, line.left + content + 2 * kUiBadgeCompactPadding),
                    cy + kUiBadgeCompactHeight * .5f};
    result.text = {result.outer.left + kUiBadgeCompactPadding + mark, result.outer.top,
                   result.outer.right - kUiBadgeCompactPadding, result.outer.bottom};
    if (icon)
        result.icon = {result.outer.left + kUiBadgeCompactPadding, cy - kUiBadgeCompactIcon * .5f,
                       result.outer.left + kUiBadgeCompactPadding + kUiBadgeCompactIcon,
                       cy + kUiBadgeCompactIcon * .5f};
    result.clipped = ui_rect_width(result.text) + .01f < width;
    return result;
}
UiBadgeLayout ui_badge_widget_layout(UiTextMeasureCache* cache, const UiBadgeWidget& badge,
                                     const UiRect& line) noexcept {
    return badge.compact ? ui_badge_compact_layout(cache, badge.label, line, badge.leading, badge.style)
                         : ui_badge_layout(cache, badge.label, line, badge.leading, badge.style);
}
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
