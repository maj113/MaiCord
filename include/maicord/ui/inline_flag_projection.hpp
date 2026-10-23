#pragma once
#include "maicord/ui/regional_flag.hpp"
#include "maicord/ui/text.hpp"

namespace maicord {
inline constexpr u32 kUiInlineFlagCapacity = 127;
static_assert(kUiInlineFlagCapacity * 2 + 1 <= kTextStyleSpanMaximum);
struct UiInlineFlagProjection {
    RegionalFlagRange flags[kUiInlineFlagCapacity];
    // Each flag can have a preceding styled gap, followed by one final gap.
    TextStyleSpan styles[kUiInlineFlagCapacity * 2 + 1];
    u32 count = 0, style_count = 0;
};
// Plain native labels retain their exact UTF-8/UTF-16 document. Only supported
// flags receive object cells; code and already projected emoji stay unchanged.
// Failure publishes no partial suppression, so capacity fallback remains text.
inline bool ui_inline_flag_project(Utf8View text, u32 base_flags, UiInlineFlagProjection* output) noexcept {
    if (!output)
        return false;
    output->count = output->style_count = 0;
    if (!utf8_validate(text).valid)
        return false;
    if (base_flags & (text_style_monospace | text_style_custom_emoji))
        return true;
    u32 cursor = 0, previous = 0, count = 0, style_count = 0;
    RegionalFlagRange range{};
    while (regional_flag_next(text, &cursor, &range)) {
        if (count == kUiInlineFlagCapacity)
            return false;
        // Native style spans carry changes only; an explicit zero-flags span
        // is invalid. Unstyled gaps inherit the label's normal base face.
        if (base_flags && range.first_byte > previous)
            output->styles[style_count++] = {previous, range.first_byte - previous, base_flags};
        output->flags[count++] = range;
        output->styles[style_count++] = {range.first_byte, range.end_byte - range.first_byte,
                                         base_flags | text_style_custom_emoji};
        previous = range.end_byte;
    }
    if (count && base_flags && previous < text.bytes)
        output->styles[style_count++] = {previous, text.bytes - previous, base_flags};
    output->count = count;
    output->style_count = style_count;
    return true;
}
} // namespace maicord
