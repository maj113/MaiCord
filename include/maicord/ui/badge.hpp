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
// A badge binds its geometry, label and leading mark to one widget. Paint and
// text use the same insets.
struct UiBadgeWidget {
    UiWidgetHandle owner;
    Utf8View label;
    Color marker;
    u32 style;
    bool leading, accent;
    UiRect rectangle;
    float opacity = 1;
};
UiBadgeLayout ui_badge_geometry(const UiBadgeWidget&, const UiWidgetManager&) noexcept;
bool ui_badge_paint(const UiBadgeWidget&, const UiWidgetManager&, const Theme&, PaintStream*) noexcept;
bool ui_badge_text(const UiBadgeWidget&, const UiWidgetManager&, const Theme&,
                   UiTextCommandStream*) noexcept;
} // namespace maicord

namespace maicord {
bool ui_badge_text_output(const UiBadgeWidget& b, const UiWidgetManager& manager, const Theme& theme,
                   UiTextOutput* text) noexcept;
} // namespace maicord
