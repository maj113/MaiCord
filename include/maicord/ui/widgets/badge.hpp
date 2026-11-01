#pragma once
#include "maicord/ui/text/style.hpp"
#include "maicord/ui/theme/theme.hpp"
#include "maicord/ui/core/widget_manager.hpp"
namespace maicord {
struct PaintStream;
struct UiTextCommandStream;
struct UiTextMeasureCache;
struct UiTextOutput;
// All badge consumers use the same label, icon slot and padding.
struct UiBadgeLayout {
    UiRect outer, text, icon;
    bool clipped;
};
UiBadgeLayout ui_badge_layout(UiTextMeasureCache* cache, const Utf8View& label, const UiRect& line, bool icon,
                              u32 flags = text_style_subtext | text_style_bold) noexcept;
// A compact badge is the chip of a reply or command header: 16 DIPs tall with
// 4-DIP side padding, a 12-DIP leading mark and the muted fill, no border.
inline constexpr float kUiBadgeCompactHeight = 16.0f;
inline constexpr float kUiBadgeCompactPadding = 4.0f;
inline constexpr float kUiBadgeCompactIcon = 12.0f;
inline constexpr float kUiBadgeCompactIconGap = 2.0f;
// The widest the mark and label may grow.
inline constexpr float kUiBadgeCompactMaximumContent = 70.0f;
UiBadgeLayout ui_badge_compact_layout(UiTextMeasureCache* cache, const Utf8View& label, const UiRect& line,
                                      bool icon, u32 flags = text_style_subtext | text_style_bold) noexcept;
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
UiBadgeLayout ui_badge_widget_layout(UiTextMeasureCache* cache, const UiBadgeWidget& badge,
                                     const UiRect& line) noexcept;
UiBadgeLayout ui_badge_geometry(const UiBadgeWidget&, const UiWidgetManager&) noexcept;
bool ui_badge_paint(const UiBadgeWidget&, const UiWidgetManager&, const Theme&, PaintStream*) noexcept;
bool ui_badge_text(const UiBadgeWidget&, const UiWidgetManager&, const Theme&,
                   UiTextCommandStream*) noexcept;
bool ui_badge_text_output(const UiBadgeWidget& b, const UiWidgetManager& manager, const Theme& theme,
                   UiTextOutput* text) noexcept;
} // namespace maicord
