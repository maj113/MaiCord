#include "maicord/ui/text_composition.hpp"
#include "maicord/ui/paint.hpp"
namespace maicord {
bool text_composition_paint_mark(PaintStream* stream, const TextCompositionMark& mark,
    const UiRect& clip, const Color& fallback) noexcept {
    const auto rectangle = ui_rect_intersection(mark.rectangle, clip);
    if (!ui_rect_has_area(rectangle)) return true;
    const auto& span = mark.attributes;
    if (span.color_flags & text_composition_color_background) {
        constexpr float scale = 1.0f / 255;
        const Color color{((span.background_rgb >> 16) & 255) * scale,
            ((span.background_rgb >> 8) & 255) * scale, (span.background_rgb & 255) * scale, 1};
        if (!paint_stream_push(stream, rectangle, color)) return false;
    }
    return text_composition_paint_line(stream, rectangle, span, fallback);
}
bool text_composition_paint_line(PaintStream* stream, const UiRect& rectangle,
    const TextCompositionSpan& span, const Color& fallback) noexcept {
    if (span.line_style == TextCompositionLineStyle::none || !ui_rect_has_area(rectangle)) return true;
    constexpr float inverse = 1.0f / 255;
    const Color color = (span.color_flags & text_composition_color_line) ?
        Color{static_cast<float>((span.line_rgb >> 16) & 255) * inverse,
            static_cast<float>((span.line_rgb >> 8) & 255) * inverse,
            static_cast<float>(span.line_rgb & 255) * inverse, 1} : fallback;
    const float thickness = span.bold_line ? 2.0f : 1.0f;
    const float left = static_cast<float>(static_cast<i32>(rectangle.left));
    const float right = static_cast<float>(static_cast<i32>(rectangle.right + 0.999f));
    const float bottom = static_cast<float>(static_cast<i32>(rectangle.bottom - 1));
    if (right <= left || bottom < rectangle.top) return true;
    if (span.line_style == TextCompositionLineStyle::solid)
        return paint_stream_push(stream, left, bottom - thickness + 1, right, bottom + 1, color);
    const float segment = span.line_style == TextCompositionLineStyle::dash ? 5.0f : 2.0f;
    const float step = span.line_style == TextCompositionLineStyle::dash ? 8.0f : 4.0f;
    u32 part = 0;
    for (float x = left; x < right; x += step, ++part) {
        const float y = span.line_style == TextCompositionLineStyle::squiggle && (part & 1) ? bottom - thickness : bottom;
        if (!paint_stream_push(stream, x, y - thickness + 1, minimum(right, x + segment), y + 1, color)) return false;
    }
    return true;
}
}
