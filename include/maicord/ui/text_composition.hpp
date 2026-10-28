#pragma once

#include "maicord/core/base.hpp"
#include "maicord/ui/geometry.hpp"

namespace maicord {

// Renderer-neutral projection of a native text-service display attribute.
// Positions are interpreted by the owner of the containing view: the native
// store uses UTF-16 ACP units, while LoginView uses visible codepoint indices.
enum class TextCompositionLineStyle : u8 {
    none,
    solid,
    dot,
    dash,
    squiggle,
};

enum class TextCompositionAttribute : u8 {
    input,
    target_converted,
    converted,
    target_not_converted,
    input_error,
    fixed_converted,
    other,
};

enum TextCompositionColorFlag : u8 {
    text_composition_color_none = 0,
    text_composition_color_text = 1 << 0,
    text_composition_color_background = 1 << 1,
    text_composition_color_line = 1 << 2,
};

struct TextCompositionSpan {
    u32 start;
    u32 end;
    u32 text_rgb;
    u32 background_rgb;
    u32 line_rgb;
    TextCompositionLineStyle line_style;
    TextCompositionAttribute attribute;
    u8 color_flags;
    bool bold_line;
};

inline constexpr u32 kTextCompositionSpanCapacity = 48;
struct TextCompositionMark { UiRect rectangle; TextCompositionSpan attributes; };

struct PaintStream;
struct Color;
bool text_composition_paint_line(PaintStream* stream, const UiRect& rectangle,
    const TextCompositionSpan& span, const Color& fallback) noexcept;
bool text_composition_paint_mark(PaintStream* stream, const TextCompositionMark& mark,
    const UiRect& clip, const Color& fallback) noexcept;

} // namespace maicord
