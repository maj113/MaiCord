#pragma once
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/text_presentation.hpp"
namespace maicord {
struct PaintStream;
// All badge consumers use the same label, icon slot and padding.
struct UiBadgeLayout {
    UiRect outer, text, icon;
    bool clipped;
};
inline UiBadgeLayout ui_badge_layout(UiTextMeasureCache* cache, const Utf8View& label, const UiRect& line, bool icon,
                                     u32 flags = text_style_subtext | text_style_bold) noexcept {
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
// A compact badge is the chip of a reply or command header: 16 DIPs tall with
// 4-DIP side padding, a 12-DIP leading mark and the muted fill, no border.
inline constexpr float kUiBadgeCompactHeight = 16.0f;
inline constexpr float kUiBadgeCompactPadding = 4.0f;
inline constexpr float kUiBadgeCompactIcon = 12.0f;
inline constexpr float kUiBadgeCompactIconGap = 2.0f;
// The widest the mark and label may grow.
inline constexpr float kUiBadgeCompactMaximumContent = 70.0f;
inline UiBadgeLayout ui_badge_compact_layout(UiTextMeasureCache* cache, const Utf8View& label, const UiRect& line,
                                             bool icon, u32 flags = text_style_subtext | text_style_bold) noexcept {
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
// A badge binds its geometry, label and leading mark to one widget. Paint and
// text use the same insets. A compact accent badge is the APP tag beside a
// bot's name: the brand fill, white text, and a leading check when verified.
struct UiBadgeWidget {
    UiWidgetHandle owner;
    Utf8View label;
    Color marker;
    u32 style;
    bool leading, accent;
    UiRect rectangle;
    float opacity = 1;
    bool compact = false;
    // The pointer is over the badge (a compact chip lightens).
    bool hovered = false;
};
// The APP tag of a bot with these public flags (VERIFIED_BOT, 1 << 16, leads
// with the check); the caller sets its owner and rectangle.
constexpr bool ui_app_badge_verified(u64 public_flags) noexcept {
    return (public_flags & (1ull << 16)) != 0;
}
inline void ui_app_badge(UiBadgeWidget* badge, u64 public_flags) noexcept {
    badge->label = utf8_literal("APP");
    badge->style = text_style_subtext | text_style_bold;
    badge->leading = ui_app_badge_verified(public_flags);
    badge->accent = badge->compact = true;
}
// The 16 DIP check of a verified APP tag at outer, 2 DIPs into its padding;
// the client's tooltip on it reads "Verified App".
constexpr UiRect ui_app_badge_check_rectangle(const UiRect& outer) noexcept {
    const float cy = (outer.top + outer.bottom) * .5f;
    return {outer.left + kUiBadgeCompactPadding - 2, cy - 8, outer.left + kUiBadgeCompactPadding + 14, cy + 8};
}
// That check of a laid-out badge (its rectangle), or empty when it has none.
inline UiRect ui_app_badge_check(const UiBadgeWidget& badge) noexcept {
    return badge.compact && badge.accent && badge.leading ? ui_app_badge_check_rectangle(badge.rectangle) : UiRect{};
}
inline constexpr Utf8View kUiAppBadgeVerifiedHint = utf8_literal("Verified App");
// A badge's natural layout in line: compact badges start at its left, others
// end at its right.
inline UiBadgeLayout ui_badge_widget_layout(UiTextMeasureCache* cache, const UiBadgeWidget& badge,
                                            const UiRect& line) noexcept {
    return badge.compact ? ui_badge_compact_layout(cache, badge.label, line, badge.leading, badge.style)
                         : ui_badge_layout(cache, badge.label, line, badge.leading, badge.style);
}
UiBadgeLayout ui_badge_geometry(const UiBadgeWidget&, const UiWidgetManager&) noexcept;
bool ui_badge_paint(const UiBadgeWidget&, const UiWidgetManager&, const Theme&, PaintStream*) noexcept;
bool ui_badge_text(const UiBadgeWidget&, const UiWidgetManager&, const Theme&,
                   UiTextCommandStream*) noexcept;
bool ui_badge_text_output(const UiBadgeWidget& b, const UiWidgetManager& manager, const Theme& theme,
                   UiTextOutput* text) noexcept;
} // namespace maicord
